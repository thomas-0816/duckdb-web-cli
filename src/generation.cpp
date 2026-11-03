#include "generation.hpp"

#include "chat.h"
#include "docs_storage.hpp"

#include <llama.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace docs {
namespace {

constexpr const char *ANSWER_MODEL_FILE = "MiniCPM5-1B-Q4_K_M.gguf";
constexpr const char *ANSWER_MODEL_URL =
    "https://huggingface.co/openbmb/MiniCPM5-1B-GGUF/resolve/main/MiniCPM5-1B-Q4_K_M.gguf";
constexpr const char *ANSWER_MODEL_DOWNLOAD_SIZE = "656 MB";
constexpr int ANSWER_CONTEXT_TOKENS = 16384;
constexpr int ANSWER_MAX_OUTPUT_TOKENS = 2048;
constexpr int ANSWER_BATCH_TOKENS = 2048;
constexpr int ANSWER_UBATCH_TOKENS = 512;
constexpr int ANSWER_THREADS = 8;

constexpr float ANSWER_TEMPERATURE = 0.9f;

constexpr const char *ANSWER_SYSTEM_PROMPT =
    "You answer questions about DuckDB. Use only the documentation excerpts supplied in the user message, "
    "the SQL-92 standard, and the SQL-99 standard. Do not use any other background knowledge. "
    "Think through the excerpts briefly, then give a direct, concise answer. If the excerpts and those "
    "standards do not contain the answer, say so instead of guessing. Mention the page titles you used "
    "when that helps the reader.";

struct AnswerEngine {
	llama_model *model = nullptr;
	llama_context *context = nullptr;
	common_chat_templates_ptr templates;
	int context_tokens = ANSWER_CONTEXT_TOKENS;
};

AnswerEngine &GetAnswerEngine() {
	static AnswerEngine engine;
	return engine;
}

void SilenceLlamaLog(enum ggml_log_level, const char *, void *) {
}

void EnsureLlamaBackend() {
	static const bool initialized = [] {
		llama_log_set(SilenceLlamaLog, nullptr);
		llama_backend_init();
		return true;
	}();
	(void)initialized;
}

std::string JoinPath(const std::string &base, const std::string &name) {
	if (base.empty()) {
		return name;
	}
	if (base.back() == '/' || base.back() == '\\') {
		return base + name;
	}
	return base + "/" + name;
}

std::string AnswerModelPath() {
	auto &paths = GetDataPaths();
	std::vector<std::string> candidates = {
	    JoinPath(paths.binary_dir, ANSWER_MODEL_FILE),
	    JoinPath(paths.binary_dir, std::string("data/") + ANSWER_MODEL_FILE),
	    ANSWER_MODEL_FILE,
	};
	for (auto &candidate : candidates) {
		std::error_code error;
		if (std::filesystem::exists(candidate, error) && !error) {
			return candidate;
		}
	}
	return candidates.front();
}

bool ReportLoadProgress(float progress, void *user) {
	auto callback = static_cast<const AnswerLoadProgress *>(user);
	if (callback && *callback) {
		(*callback)(std::max(0.0f, std::min(1.0f, progress)), "Loading the model weights...");
	}
	return true;
}

void EnsureAnswerModelFile(const std::string &model_path, const AnswerLoadProgress &progress) {
	std::error_code error;
	if (std::filesystem::exists(model_path, error) && !error) {
		return;
	}

	auto status = std::string("Downloading ") + ANSWER_MODEL_FILE + " (about " + ANSWER_MODEL_DOWNLOAD_SIZE + ")...";
	if (progress) {
		progress(0.0f, status);
	}
	int last_percent = -1;
	DownloadFile(ANSWER_MODEL_URL, model_path, [&](uint64_t done, uint64_t total) {
		if (!progress || total == 0) {
			return;
		}
		auto fraction = static_cast<float>(static_cast<double>(done) / static_cast<double>(total));
		auto percent = static_cast<int>(std::max(0.0f, std::min(1.0f, fraction)) * 100.0f + 0.5f);
		if (percent != last_percent) {
			last_percent = percent;
			progress(fraction, status);
		}
	});
	if (progress) {
		progress(0.0f, "Download complete; loading the model weights...");
	}
}

std::vector<llama_token> Tokenize(const llama_vocab *vocab, const std::string &text) {
	std::vector<llama_token> tokens(text.size() + 16);
	auto count = llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()), tokens.data(),
	                            static_cast<int32_t>(tokens.size()), false, true);
	if (count < 0) {
		tokens.resize(static_cast<std::size_t>(-count));
		count = llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()), tokens.data(),
		                       static_cast<int32_t>(tokens.size()), false, true);
	}
	if (count < 0) {
		throw std::runtime_error("Could not tokenize the answer prompt");
	}
	tokens.resize(static_cast<std::size_t>(count));
	return tokens;
}

std::string TokenToPiece(const llama_vocab *vocab, llama_token token) {
	std::array<char, 256> buffer{};
	auto count = llama_token_to_piece(vocab, token, buffer.data(), static_cast<int32_t>(buffer.size()), 0, false);
	if (count < 0) {
		std::vector<char> larger(static_cast<std::size_t>(-count));
		count = llama_token_to_piece(vocab, token, larger.data(), static_cast<int32_t>(larger.size()), 0, false);
		if (count < 0) {
			return {};
		}
		return std::string(larger.data(), static_cast<std::size_t>(count));
	}
	return std::string(buffer.data(), static_cast<std::size_t>(count));
}

std::string BuildUserContent(const std::vector<SearchResult> &blocks, const std::string &question) {
	std::string message = "DuckDB documentation excerpts:\n";
	for (std::size_t i = 0; i < blocks.size(); i++) {
		message += "\n--- Excerpt " + std::to_string(i + 1) + ": " + blocks[i].title + " ---\n";
		message += blocks[i].content;
		if (message.empty() || message.back() != '\n') {
			message += '\n';
		}
	}
	message += "\nQuestion:\n" + question + "\n";
	return message;
}

std::size_t Utf8BoundaryBefore(const std::string &text, std::size_t position) {
	if (position >= text.size()) {
		return text.size();
	}
	while (position > 0 && (static_cast<unsigned char>(text[position]) & 0xC0) == 0x80) {
		position--;
	}
	return position;
}

struct PromptRender {
	std::vector<llama_token> tokens;
	std::string thinking_start_tag;
	std::vector<std::string> thinking_end_tags;
};

common_chat_params ApplyAnswerTemplate(const common_chat_templates *templates, const std::string &question,
                                       const std::vector<SearchResult> &blocks) {
	common_chat_msg system;
	system.role = "system";
	system.content = ANSWER_SYSTEM_PROMPT;

	common_chat_msg user;
	user.role = "user";
	user.content = BuildUserContent(blocks, question);

	common_chat_templates_inputs inputs;
	inputs.use_jinja = true;
	inputs.messages = {system, user};
	inputs.add_generation_prompt = true;
	inputs.enable_thinking = true;
	return common_chat_templates_apply(templates, inputs);
}

PromptRender RenderAnswerPrompt(const AnswerEngine &engine, const std::string &question,
                                const std::vector<SearchResult> &blocks) {
	auto fitted = blocks;
	auto *vocab = llama_model_get_vocab(engine.model);
	auto context_tokens = static_cast<std::size_t>(engine.context_tokens);
	auto reserve = static_cast<std::size_t>(ANSWER_MAX_OUTPUT_TOKENS) + 8;
	auto budget = context_tokens > reserve ? context_tokens - reserve : 0;
	while (true) {
		auto params = ApplyAnswerTemplate(engine.templates.get(), question, fitted);
		auto text = params.prompt.str();
		auto tokens = Tokenize(vocab, text);
		if (tokens.size() <= budget) {
			PromptRender result;
			result.tokens = std::move(tokens);
			result.thinking_start_tag = params.thinking_start_tag;
			result.thinking_end_tags = params.thinking_end_tags;
			return result;
		}
		if (fitted.size() > 1) {
			fitted.pop_back();
			continue;
		}
		if (fitted.empty()) {
			throw std::runtime_error("The answer prompt does not fit in the model context");
		}
		auto &content = fitted.back().content;
		if (content.empty()) {
			throw std::runtime_error("The answer prompt does not fit in the model context");
		}
		auto over = tokens.size() - budget;
		auto remove = std::max<std::size_t>(1, over * 4);
		auto keep = remove >= content.size() ? 0 : content.size() - remove;
		content.resize(Utf8BoundaryBefore(content, keep));
	}
}

class ReasoningSplitter {
public:
	ReasoningSplitter(const std::vector<std::string> &end_tags, const AnswerTokenCallback &callback)
	    : callback_(callback) {
		for (auto &tag : end_tags) {
			if (!tag.empty()) {
				end_tags_.push_back(tag);
			}
		}
		reasoning_ = !end_tags_.empty();
	}

	bool Feed(const std::string &piece) {
		buffer_ += piece;
		return Drain(false);
	}

	bool Finish() {
		return Drain(true);
	}

private:
	bool Emit(const std::string &text, bool reasoning) {
		return text.empty() || callback_(text, reasoning);
	}

	bool EmitAndClear(bool reasoning) {
		auto text = std::move(buffer_);
		buffer_.clear();
		return Emit(text, reasoning);
	}

	std::size_t HeldSuffix() const {
		std::size_t hold = 0;
		for (auto &tag : end_tags_) {
			auto max = std::min(buffer_.size(), tag.size() - 1);
			for (auto length = max; length > hold; length--) {
				if (buffer_.compare(buffer_.size() - length, length, tag, 0, length) == 0) {
					hold = length;
					break;
				}
			}
		}
		return hold;
	}

	bool Drain(bool final) {
		while (!buffer_.empty()) {
			if (!reasoning_) {
				return EmitAndClear(false);
			}
			std::size_t match_position = std::string::npos;
			std::size_t match_length = 0;
			for (auto &tag : end_tags_) {
				auto position = buffer_.find(tag);
				if (position != std::string::npos && (match_position == std::string::npos || position < match_position)) {
					match_position = position;
					match_length = tag.size();
				}
			}
			if (match_position != std::string::npos) {
				if (!Emit(buffer_.substr(0, match_position), true)) {
					return false;
				}
				buffer_.erase(0, match_position + match_length);
				reasoning_ = false;
				continue;
			}
			if (final) {
				return EmitAndClear(true);
			}
			auto hold = HeldSuffix();
			if (buffer_.size() > hold) {
				auto count = buffer_.size() - hold;
				if (!Emit(buffer_.substr(0, count), true)) {
					return false;
				}
				buffer_.erase(0, count);
			}
			return true;
		}
		return true;
	}

	const AnswerTokenCallback &callback_;
	std::vector<std::string> end_tags_;
	std::string buffer_;
	bool reasoning_ = false;
};

void DecodePrompt(llama_context *context, const std::vector<llama_token> &tokens,
                  const AnswerPromptProgress &progress) {
	if (progress) {
		progress(0, tokens.size());
	}
	for (std::size_t position = 0; position < tokens.size();) {
		auto remaining = tokens.size() - position;
		auto count = static_cast<int32_t>(std::min<std::size_t>(remaining, ANSWER_BATCH_TOKENS));
		auto batch = llama_batch_get_one(const_cast<llama_token *>(tokens.data() + position), count);
		auto status = llama_decode(context, batch);
		if (status != 0) {
			throw std::runtime_error("The answer model failed to read the prompt (status " +
			                         std::to_string(status) + ")");
		}
		position += static_cast<std::size_t>(count);
		if (progress) {
			progress(position, tokens.size());
		}
	}
}

} // namespace

bool AnswerModelAvailable() {
	std::error_code error;
	return std::filesystem::exists(AnswerModelPath(), error) && !error;
}

bool AnswerModelLoaded() {
	auto &engine = GetAnswerEngine();
	return engine.model != nullptr && engine.context != nullptr && engine.templates != nullptr;
}

void PrepareAnswerModel(const AnswerLoadProgress &progress) {
	auto &engine = GetAnswerEngine();
	if (engine.model && engine.context && engine.templates) {
		if (progress) {
			progress(1.0f, "The model is ready");
		}
		return;
	}
	auto model_path = AnswerModelPath();
	EnsureAnswerModelFile(model_path, progress);
	if (progress) {
		progress(0.0f, "Loading the model weights...");
	}
	EnsureLlamaBackend();

	llama_model_params model_params = llama_model_default_params();
	// Offload as many layers as the active backend supports. The CPU backend is
	// used automatically when no GPU is available.
	model_params.n_gpu_layers = -1;
	model_params.progress_callback = ReportLoadProgress;
	model_params.progress_callback_user_data = const_cast<AnswerLoadProgress *>(&progress);

	auto model = llama_model_load_from_file(model_path.c_str(), model_params);
	if (!model) {
		throw std::runtime_error("Could not load the answer model from " + model_path);
	}

	auto trained_context = llama_model_n_ctx_train(model);
	auto context_tokens = trained_context > 0 ? std::min(trained_context, ANSWER_CONTEXT_TOKENS) : ANSWER_CONTEXT_TOKENS;

	llama_context_params context_params = llama_context_default_params();
	context_params.n_ctx = static_cast<uint32_t>(context_tokens);
	context_params.n_batch = static_cast<uint32_t>(ANSWER_BATCH_TOKENS);
	context_params.n_ubatch = static_cast<uint32_t>(ANSWER_UBATCH_TOKENS);
	context_params.n_seq_max = 1;
	context_params.n_threads = ANSWER_THREADS;
	context_params.n_threads_batch = ANSWER_THREADS;
	context_params.embeddings = false;

	auto context = llama_init_from_model(model, context_params);
	if (!context) {
		llama_model_free(model);
		throw std::runtime_error("Could not create a context for the answer model");
	}

	common_chat_templates_ptr templates;
	try {
		templates = common_chat_templates_init(model, "");
	} catch (...) {
		llama_free(context);
		llama_model_free(model);
		throw;
	}
	if (!templates) {
		llama_free(context);
		llama_model_free(model);
		throw std::runtime_error("Could not read the chat template from the answer model");
	}

	engine.model = model;
	engine.context = context;
	engine.templates = std::move(templates);
	engine.context_tokens = context_tokens;
	if (progress) {
		progress(1.0f, "The model is ready");
	}
}

void GenerateAnswer(const std::string &question, const std::vector<SearchResult> &blocks,
                    const AnswerTokenCallback &callback, const AnswerPromptProgress &prompt_progress) {
	if (!callback) {
		return;
	}
	PrepareAnswerModel();

	auto &engine = GetAnswerEngine();
	auto *vocab = llama_model_get_vocab(engine.model);
	auto prompt = RenderAnswerPrompt(engine, question, blocks);

	auto *memory = llama_get_memory(engine.context);
	if (memory) {
		llama_memory_clear(memory, true);
	}

	auto sampler_params = llama_sampler_chain_default_params();
	auto *sampler = llama_sampler_chain_init(sampler_params);
	llama_sampler_chain_add(sampler, llama_sampler_init_temp(ANSWER_TEMPERATURE));
	llama_sampler_chain_add(sampler, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

	ReasoningSplitter splitter(prompt.thinking_end_tags, callback);
	try {
		DecodePrompt(engine.context, prompt.tokens, prompt_progress);
		for (int generated = 0; generated < ANSWER_MAX_OUTPUT_TOKENS; generated++) {
			auto token = llama_sampler_sample(sampler, engine.context, -1);
			if (llama_vocab_is_eog(vocab, token)) {
				break;
			}
			auto piece = TokenToPiece(vocab, token);
			if (!piece.empty() && !splitter.Feed(piece)) {
				break;
			}
			auto batch = llama_batch_get_one(&token, 1);
			auto status = llama_decode(engine.context, batch);
			if (status != 0) {
				throw std::runtime_error("The answer model stopped with status " + std::to_string(status));
			}
		}
		if (!splitter.Finish()) {
			// The callback asked to stop while flushing the tail.
		}
	} catch (...) {
		llama_sampler_free(sampler);
		throw;
	}
	llama_sampler_free(sampler);
}

} // namespace docs
