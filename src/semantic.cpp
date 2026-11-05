#include "semantic.hpp"

#include "arg.h"
#include "docs_storage.hpp"
#include "download.h"
#include "platform.hpp"

#include <hnswlib/hnswlib.h>
#include <llama.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace docs {

int GpuLayers() {
	if (auto *value = std::getenv("DOCS_GPU_LAYERS")) {
		if (*value) {
			return std::atoi(value);
		}
	}
	return -1;
}

namespace {

constexpr const char *SEMANTIC_MAGIC = "DDBSEM1\n";
constexpr uint32_t SEMANTIC_VERSION = 1;
constexpr const char *BGE_QUERY_PREFIX = "Represent this sentence for searching relevant passages: ";
constexpr const char *EMBEDDING_MODEL_HF_REPO = "ggml-org/bge-small-en-v1.5-Q8_0-GGUF:Q8_0";
const SemanticModel SEMANTIC_MODEL = {"bge-small-en-v1.5-q8_0"};
// The shipped index records this SHA-1. The model download is pinned to a fixed
// HF revision, so the model is not rehashed on every start.
constexpr const char *SEMANTIC_MODEL_REVISION = "embedded:03e4cfd53ed82c28044a2087f21c153483ecda9f";
constexpr int EMBEDDING_CONTEXT_TOKENS = 512;
constexpr int EMBEDDING_BATCH_TOKENS = 2048;
constexpr int EMBEDDING_UBATCH_TOKENS = 2048;
constexpr int EMBEDDING_MAX_SEQUENCES = 16;

SemanticProgressCallback progress_callback = nullptr;
void *progress_callback_user = nullptr;

//! llama.cpp otherwise grabs every hardware thread, which makes the first
//! search feel like it freezes the whole machine. Four threads keep the rest
//! of the system responsive while the GPU does most of the embedding work.
constexpr int EMBEDDING_THREADS = 4;

int SemanticThreads() {
	return EMBEDDING_THREADS;
}

struct SemanticRecord {
	uint8_t type = 0;
	uint32_t page_id = 0;
};

bool FileExists(const std::string &path) {
	std::error_code error;
	return std::filesystem::exists(path, error) && !error;
}

std::string JoinPath(const std::string &base, const std::string &relative) {
	if (base.empty()) {
		return relative;
	}
	if (base.back() == '/' || base.back() == '\\') {
		return base + relative;
	}
	return base + "/" + relative;
}

std::string SanitizeModelId(const std::string &id) {
	std::string result;
	for (auto c : id) {
		if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_') {
			result.push_back(c);
		} else {
			result.push_back('-');
		}
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Embedding model
//===--------------------------------------------------------------------===//

void SilentLlamaLog(enum ggml_log_level, const char *, void *) {
}

void ConfigureLlamaBackend() {
	// With CPU-only inference, do not initialize the GPU backend at all. This
	// keeps the Vulkan driver out of the picture in containers and on machines
	// that expose a virtual GPU that cannot run inference.
	if (GpuLayers() == 0) {
#if defined(_WIN32)
		_putenv_s("GGML_DISABLE_VULKAN", "1");
#else
		setenv("GGML_DISABLE_VULKAN", "1", 1);
#endif
	}
	llama_log_set(SilentLlamaLog, nullptr);
	llama_backend_init();
}

std::string EmbeddingModelPath() {
	// Use the Hugging Face cache that llama.cpp's downloader uses, so the model
	// is shared with the llama CLI and other tools.
	return common_download_resolve_path(EMBEDDING_MODEL_HF_REPO);
}

std::string EnsureEmbeddingModelFile() {
	auto path = EmbeddingModelPath();
	if (!path.empty() && FileExists(path)) {
		return path;
	}

	fprintf(stderr, "docs: downloading %s...\n", EMBEDDING_MODEL_HF_REPO);
	fflush(stderr);

	common_params params;
	params.model.hf_repo = EMBEDDING_MODEL_HF_REPO;
	try {
		auto handler = common_models_handler_init(params, LLAMA_EXAMPLE_DOWNLOAD);
		common_models_handler_apply(handler, params);
	} catch (const std::exception &failure) {
		throw std::runtime_error("Could not download " + std::string(EMBEDDING_MODEL_HF_REPO) + ": " +
		                         failure.what());
	}

	if (params.model.path.empty() || !FileExists(params.model.path)) {
		throw std::runtime_error("Could not download " + std::string(EMBEDDING_MODEL_HF_REPO));
	}
	return params.model.path;
}

void NormalizeVector(float *vector, int dim) {
	double norm = 0;
	for (int i = 0; i < dim; i++) {
		auto value = vector[i];
		norm += static_cast<double>(value) * value;
	}
	if (norm > 0) {
		auto scale = static_cast<float>(1.0 / std::sqrt(norm));
		for (int i = 0; i < dim; i++) {
			vector[i] *= scale;
		}
	}
}

class Embedder {
public:
	Embedder() {
		EnsureLlamaBackend();
		path_ = EnsureEmbeddingModelFile();

		llama_model_params model_params = llama_model_default_params();
		model_params.n_gpu_layers = GpuLayers();
		if (model_params.n_gpu_layers == 0) {
			// Keep the model on the CPU and do not initialize a GPU backend.
			// GitHub's macOS runners expose a paravirtual Metal device that
			// crashes even when no layers are offloaded.
			model_params.split_mode = LLAMA_SPLIT_MODE_NONE;
			model_params.main_gpu = -1;
		}

		model_ = llama_model_load_from_file(path_.c_str(), model_params);
		if (!model_) {
			throw std::runtime_error("Could not load the embedding model");
		}

		max_length_ = llama_model_n_ctx_train(model_);
		if (max_length_ <= 0) {
			max_length_ = EMBEDDING_CONTEXT_TOKENS;
		}
		// Never ask for more context than the model was trained with.
		max_length_ = std::min(max_length_, EMBEDDING_CONTEXT_TOKENS);

		llama_context_params context_params = llama_context_default_params();
		context_params.n_ctx = static_cast<uint32_t>(max_length_);
		context_params.n_batch = static_cast<uint32_t>(EMBEDDING_BATCH_TOKENS);
		context_params.n_ubatch = static_cast<uint32_t>(EMBEDDING_UBATCH_TOKENS);
		context_params.n_seq_max = static_cast<uint32_t>(EMBEDDING_MAX_SEQUENCES);
		context_params.n_threads = SemanticThreads();
		context_params.n_threads_batch = SemanticThreads();
		context_params.embeddings = true;
		ctx_ = llama_init_from_model(model_, context_params);
		if (!ctx_) {
			llama_model_free(model_);
			model_ = nullptr;
			throw std::runtime_error("Could not create an embedding context for the embedding model");
		}

		dim_ = llama_model_n_embd_out(model_);
		if (dim_ <= 0) {
			throw std::runtime_error("Embedding model reported an invalid dimension");
		}
	}

	~Embedder() {
		if (ctx_) {
			llama_free(ctx_);
		}
		if (model_) {
			llama_model_free(model_);
		}
	}

	Embedder(const Embedder &) = delete;
	Embedder &operator=(const Embedder &) = delete;

	int Dim() const {
		return dim_;
	}

	const std::string &Path() const {
		return path_;
	}

	void Embed(const std::vector<std::string> &texts, std::vector<float> &output,
	           SemanticProgressCallback progress = nullptr, void *progress_user = nullptr) {
		output.clear();
		if (texts.empty()) {
			return;
		}
		const auto total = texts.size();
		const auto dimensions = static_cast<std::size_t>(dim_);
		output.resize(total * dimensions);

		std::vector<std::vector<llama_token>> tokenized;
		tokenized.reserve(total);
		for (auto &text : texts) {
			tokenized.push_back(Tokenize(text));
		}

		auto batch = llama_batch_init(EMBEDDING_BATCH_TOKENS, 0, EMBEDDING_MAX_SEQUENCES);
		std::size_t done = 0;
		std::size_t next = 0;
		try {
			while (next < total) {
				// llama.cpp keeps per-sequence state between calls. Embeddings do
				// not need it, so start every batch from a clean state.
				llama_memory_clear(llama_get_memory(ctx_), true);

				auto first = next;
				int tokens_in_batch = 0;
				int sequences = 0;
				std::array<int, EMBEDDING_MAX_SEQUENCES> first_token{};
				while (next < total && sequences < EMBEDDING_MAX_SEQUENCES) {
					auto &tokens = tokenized[next];
					auto count = static_cast<int>(tokens.size());
					if (sequences > 0 && tokens_in_batch + count > EMBEDDING_BATCH_TOKENS) {
						break;
					}
					first_token[sequences] = tokens_in_batch;
					for (int i = 0; i < count; i++) {
						auto position = tokens_in_batch + i;
						batch.token[position] = tokens[static_cast<std::size_t>(i)];
						batch.pos[position] = i;
						batch.n_seq_id[position] = 1;
						batch.seq_id[position][0] = sequences;
						batch.logits[position] = 0;
					}
					tokens_in_batch += count;
					sequences++;
					next++;
				}
				batch.n_tokens = tokens_in_batch;

				auto status = llama_decode(ctx_, batch);
				if (status != 0) {
					throw std::runtime_error("Embedding inference failed with status " + std::to_string(status));
				}

				auto pooling = llama_pooling_type(ctx_);
				for (int sequence = 0; sequence < sequences; sequence++) {
					const float *embedding = nullptr;
					if (pooling == LLAMA_POOLING_TYPE_NONE) {
						// Models without a pooling type still expose token-level
						// embeddings; the first token is the CLS vector.
						embedding = llama_get_embeddings_ith(ctx_, first_token[sequence]);
					} else {
						embedding = llama_get_embeddings_seq(ctx_, sequence);
					}
					if (!embedding) {
						throw std::runtime_error("Embedding model did not return a vector");
					}
					auto target = output.data() + (first + static_cast<std::size_t>(sequence)) * dimensions;
					std::memcpy(target, embedding, dimensions * sizeof(float));
					// An inner-product HNSW index ranks by cosine similarity only
					// when every vector has unit length.
					NormalizeVector(target, dim_);
				}

				done += static_cast<std::size_t>(sequences);
				if (progress) {
					progress(done, total, progress_user);
				}
			}
		} catch (...) {
			llama_batch_free(batch);
			throw;
		}
		llama_batch_free(batch);
	}

private:
	std::vector<llama_token> Tokenize(const std::string &text) const {
		const auto *vocab = llama_model_get_vocab(model_);
		std::vector<llama_token> tokens(static_cast<std::size_t>(max_length_) + 2);
		auto count = llama_tokenize(vocab, text.c_str(), static_cast<int32_t>(text.size()), tokens.data(),
		                            static_cast<int32_t>(tokens.size()), true, true);
		if (count < 0) {
			tokens.resize(static_cast<std::size_t>(-count));
			count = llama_tokenize(vocab, text.c_str(), static_cast<int32_t>(text.size()), tokens.data(),
			                       static_cast<int32_t>(tokens.size()), true, true);
		}
		if (count < 0) {
			throw std::runtime_error("Could not tokenize semantic document");
		}
		tokens.resize(static_cast<std::size_t>(count));
		if (tokens.empty()) {
			// WPM normally emits [CLS]/[SEP]; keep a fallback for empty input.
			tokens.push_back(llama_vocab_bos(vocab));
			tokens.push_back(llama_vocab_sep(vocab));
		}
		if (static_cast<int>(tokens.size()) > max_length_) {
			tokens.resize(static_cast<std::size_t>(max_length_));
			tokens.back() = llama_vocab_sep(vocab);
		}
		return tokens;
	}

	std::string path_;
	llama_model *model_ = nullptr;
	llama_context *ctx_ = nullptr;
	int dim_ = 384;
	int max_length_ = 512;
};

//===--------------------------------------------------------------------===//
// Index metadata
//===--------------------------------------------------------------------===//

void AppendU8(std::string &output, uint8_t value) {
	output.push_back(static_cast<char>(value));
}

void AppendU16(std::string &output, uint16_t value) {
	AppendU8(output, static_cast<uint8_t>(value & 0xff));
	AppendU8(output, static_cast<uint8_t>((value >> 8) & 0xff));
}

void AppendU32(std::string &output, uint32_t value) {
	AppendU8(output, static_cast<uint8_t>(value & 0xff));
	AppendU8(output, static_cast<uint8_t>((value >> 8) & 0xff));
	AppendU8(output, static_cast<uint8_t>((value >> 16) & 0xff));
	AppendU8(output, static_cast<uint8_t>((value >> 24) & 0xff));
}

void AppendString(std::string &output, const std::string &value) {
	AppendU32(output, static_cast<uint32_t>(value.size()));
	output += value;
}

class Reader {
public:
	explicit Reader(const std::string &data) : data_(data) {
	}

	uint8_t U8() {
		Require(1);
		return static_cast<uint8_t>(data_[position_++]);
	}

	uint16_t U16() {
		Require(2);
		uint16_t result = static_cast<uint8_t>(data_[position_]) |
		                  (static_cast<uint16_t>(static_cast<uint8_t>(data_[position_ + 1])) << 8);
		position_ += 2;
		return result;
	}

	uint32_t U32() {
		Require(4);
		uint32_t result = static_cast<uint8_t>(data_[position_]) |
		                  (static_cast<uint32_t>(static_cast<uint8_t>(data_[position_ + 1])) << 8) |
		                  (static_cast<uint32_t>(static_cast<uint8_t>(data_[position_ + 2])) << 16) |
		                  (static_cast<uint32_t>(static_cast<uint8_t>(data_[position_ + 3])) << 24);
		position_ += 4;
		return result;
	}

	std::string String() {
		auto length = U32();
		Require(length);
		std::string result = data_.substr(position_, length);
		position_ += length;
		return result;
	}

	void Skip(std::size_t amount) {
		Require(amount);
		position_ += amount;
	}

private:
	void Require(std::size_t amount) const {
		if (position_ + amount > data_.size()) {
			throw std::runtime_error("Invalid semantic index metadata");
		}
	}

	const std::string &data_;
	std::size_t position_ = 0;
};

std::string BuildMetadata(const SemanticModel &model, int dim, const std::string &fingerprint,
                          const std::vector<SemanticRecord> &records) {
	std::string output;
	output.reserve(128 + records.size() * 8);
	output += SEMANTIC_MAGIC;
	AppendU32(output, SEMANTIC_VERSION);
	AppendU32(output, static_cast<uint32_t>(dim));
	AppendString(output, model.id);
	AppendString(output, fingerprint);
	AppendU32(output, static_cast<uint32_t>(records.size()));
	for (auto &record : records) {
		AppendU8(output, record.type);
		AppendU8(output, 0);
		AppendU16(output, 0);
		AppendU32(output, record.page_id);
	}
	return output;
}

bool ParseMetadata(const std::string &data, const SemanticModel &model, int dim, const std::string &fingerprint,
                   std::vector<SemanticRecord> &records) {
	if (data.size() < 28 || std::memcmp(data.data(), SEMANTIC_MAGIC, 8) != 0) {
		return false;
	}
	try {
		Reader reader(data);
		reader.Skip(8);
		if (reader.U32() != SEMANTIC_VERSION || reader.U32() != static_cast<uint32_t>(dim)) {
			return false;
		}
		if (reader.String() != model.id || reader.String() != fingerprint) {
			return false;
		}
		auto count = reader.U32();
		records.clear();
		records.reserve(count);
		for (uint32_t i = 0; i < count; i++) {
			SemanticRecord record;
			record.type = reader.U8();
			(void)reader.U8();
			(void)reader.U16();
			record.page_id = reader.U32();
			records.push_back(record);
		}
		return true;
	} catch (...) {
		return false;
	}
}

void WriteFileAtomic(const std::string &path, const std::string &data) {
	auto temporary = path + ".tmp." + std::to_string(ProcessId());
	{
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output) {
			throw std::runtime_error("Could not write " + temporary);
		}
		if (!data.empty()) {
			output.write(data.data(), static_cast<std::streamsize>(data.size()));
		}
		output.close();
		if (!output) {
			std::remove(temporary.c_str());
			throw std::runtime_error("Could not write " + temporary);
		}
	}
	std::error_code error;
	std::filesystem::rename(temporary, path, error);
	if (error) {
		std::remove(temporary.c_str());
		throw std::runtime_error("Could not replace " + path + ": " + error.message());
	}
}

std::string ReadTextFile(const std::string &path) {
	std::ifstream input(path, std::ios::binary);
	if (!input) {
		throw std::runtime_error("Could not open " + path);
	}
	input.seekg(0, std::ios::end);
	auto size = input.tellg();
	if (size < 0) {
		throw std::runtime_error("Could not read " + path);
	}
	std::string data(static_cast<std::size_t>(size), '\0');
	input.seekg(0, std::ios::beg);
	if (!data.empty()) {
		input.read(&data[0], static_cast<std::streamsize>(data.size()));
	}
	if (!input) {
		throw std::runtime_error("Could not read " + path);
	}
	return data;
}

//===--------------------------------------------------------------------===//
// HNSW index
//===--------------------------------------------------------------------===//

struct SemanticIndex {
	std::unique_ptr<hnswlib::InnerProductSpace> space;
	std::unique_ptr<hnswlib::HierarchicalNSW<float>> index;
	std::vector<SemanticRecord> records;
	std::size_t ef_search = 128;
};

struct TerminalProgress {
	std::size_t last_percent = 101;
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

void PrintTerminalProgress(std::size_t done, std::size_t total, void *user) {
	if (!IsTerminal(stderr)) {
		return;
	}
	auto state = static_cast<TerminalProgress *>(user);
	auto percent = total ? (done * 100) / total : 100;
	if (percent == state->last_percent) {
		return;
	}
	state->last_percent = percent;
	auto elapsed =
	    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - state->start).count();
	auto minutes = elapsed / 60;
	auto seconds = elapsed % 60;
	fprintf(stderr, "\rdocs: embedding chunks %zu/%zu (%zu%%, %lld:%02lld elapsed)", done, total, percent,
	        static_cast<long long>(minutes), static_cast<long long>(seconds));
	if (done >= total) {
		fputc('\n', stderr);
	}
	fflush(stderr);
}

std::unique_ptr<SemanticIndex> BuildIndex(const SemanticModel &model, const std::string &fingerprint,
                                          const std::vector<SemanticDocument> &documents,
                                          const std::string &index_path, const std::string &meta_path,
                                          Embedder &embedder) {
	std::vector<std::string> texts;
	texts.reserve(documents.size());
	std::vector<SemanticRecord> records;
	records.reserve(documents.size());
	for (auto &document : documents) {
		texts.push_back(document.text);
		records.push_back({document.type, document.page_id});
	}

	std::vector<float> vectors;
	TerminalProgress terminal;
	auto progress = progress_callback ? progress_callback : PrintTerminalProgress;
	auto progress_user = progress_callback ? progress_callback_user : &terminal;
	if (IsTerminal(stderr)) {
		fprintf(stderr, "docs: building semantic index for %s (%zu chunks, %d threads)...\n", model.id.c_str(),
		        documents.size(), SemanticThreads());
	}
	embedder.Embed(texts, vectors, progress, progress_user);
	if (vectors.size() != documents.size() * static_cast<std::size_t>(embedder.Dim())) {
		throw std::runtime_error("Embedding count does not match the semantic document count");
	}

	auto result = std::make_unique<SemanticIndex>();
	result->records = records;
	result->space = std::make_unique<hnswlib::InnerProductSpace>(embedder.Dim());
	result->index = std::make_unique<hnswlib::HierarchicalNSW<float>>(
	    result->space.get(), documents.size(), 16, 200);
	result->index->setEf(result->ef_search);

	for (std::size_t i = 0; i < documents.size(); i++) {
		result->index->addPoint(vectors.data() + i * static_cast<std::size_t>(embedder.Dim()),
		                        static_cast<hnswlib::labeltype>(i));
	}

	auto temporary = index_path + ".tmp." + std::to_string(ProcessId());
	try {
		result->index->saveIndex(temporary);
		std::error_code error;
		std::filesystem::rename(temporary, index_path, error);
		if (error) {
			throw std::runtime_error("Could not replace " + index_path + ": " + error.message());
		}
		WriteFileAtomic(meta_path, BuildMetadata(model, embedder.Dim(), fingerprint, records));
	} catch (...) {
		std::remove(temporary.c_str());
		throw;
	}
	return result;
}

std::unique_ptr<SemanticIndex> LoadOrBuildIndex(const SemanticModel &model, const std::string &fingerprint,
                                               const std::vector<SemanticDocument> &documents,
                                               const std::string &index_path, const std::string &meta_path,
                                               Embedder &embedder) {
	if (FileExists(index_path) && FileExists(meta_path)) {
		try {
			auto metadata = ReadTextFile(meta_path);
			std::vector<SemanticRecord> records;
			if (ParseMetadata(metadata, model, embedder.Dim(), fingerprint, records) &&
			    records.size() == documents.size()) {
				auto result = std::make_unique<SemanticIndex>();
				result->records = std::move(records);
				result->space = std::make_unique<hnswlib::InnerProductSpace>(embedder.Dim());
				result->index = std::make_unique<hnswlib::HierarchicalNSW<float>>(
				    result->space.get(), index_path, false, documents.size());
				result->index->setEf(result->ef_search);
				return result;
			}
		} catch (...) { // NOLINT(bugprone-empty-catch)
			// A truncated or incompatible index is rebuilt below.
		}
	}
	return BuildIndex(model, fingerprint, documents, index_path, meta_path, embedder);
}

//===--------------------------------------------------------------------===//
// Engine
//===--------------------------------------------------------------------===//

struct Engine {
	std::unique_ptr<Embedder> embedder;
	std::unique_ptr<SemanticIndex> index;
	std::string fingerprint;
};

Engine &GetEngine() {
	static Engine engine;
	return engine;
}

} // namespace

void EnsureLlamaBackend() {
	static const bool initialized = [] {
		ConfigureLlamaBackend();
		return true;
	}();
	(void)initialized;
}

void SetSemanticProgressCallback(SemanticProgressCallback callback, void *user) {
	progress_callback = callback;
	progress_callback_user = user;
}

void EnsureSemanticIndex(const std::vector<SemanticDocument> &documents, const std::string &fingerprint) {
	if (documents.empty()) {
		return;
	}

	auto &paths = GetDataPaths();
	auto base = JoinPath(paths.cache_dir, "semantic-" + SanitizeModelId(SEMANTIC_MODEL.id));
	auto index_path = base + ".hnsw";
	auto meta_path = base + ".meta";

	auto &engine = GetEngine();
	if (!engine.embedder) {
		engine.embedder = std::make_unique<Embedder>();
	}

	auto effective_fingerprint = fingerprint + "|model:" + SEMANTIC_MODEL_REVISION;

	if (!engine.index || engine.fingerprint != effective_fingerprint) {
		engine.index =
		    LoadOrBuildIndex(SEMANTIC_MODEL, effective_fingerprint, documents, index_path, meta_path, *engine.embedder);
		engine.fingerprint = effective_fingerprint;
	}
}

std::vector<SemanticHit> SearchSemantic(const std::vector<SemanticDocument> &documents,
                                        const std::string &fingerprint, const std::string &query,
                                        std::size_t limit) {
	if (documents.empty() || limit == 0) {
		return {};
	}

	EnsureSemanticIndex(documents, fingerprint);

	auto &engine = GetEngine();

	auto query_text = query;
	if (SEMANTIC_MODEL.id.rfind("bge-small-en-v1.5", 0) == 0) {
		query_text = BGE_QUERY_PREFIX + query;
	}
	std::vector<std::string> query_texts = {query_text};
	std::vector<float> query_vector;
	engine.embedder->Embed(query_texts, query_vector);
	if (query_vector.size() != static_cast<std::size_t>(engine.embedder->Dim())) {
		throw std::runtime_error("Could not embed the semantic query");
	}

	auto count = std::min(limit, engine.index->records.size());
	auto neighbours = engine.index->index->searchKnn(query_vector.data(), count);
	std::vector<SemanticHit> hits;
	hits.reserve(count);
	while (!neighbours.empty()) {
		auto entry = neighbours.top();
		neighbours.pop();
		auto label = static_cast<std::size_t>(entry.second);
		if (label >= engine.index->records.size()) {
			continue;
		}
		auto &record = engine.index->records[label];
		SemanticHit hit;
		hit.type = record.type;
		hit.page_id = record.page_id;
		hit.document_id = label;
		hit.score = 1.0f - static_cast<float>(entry.first);
		hits.push_back(hit);
	}
	// HNSW returns the furthest neighbour first.
	std::reverse(hits.begin(), hits.end());
	return hits;
}

} // namespace docs
