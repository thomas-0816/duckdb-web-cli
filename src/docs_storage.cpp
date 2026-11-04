#include "docs_storage.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

#include "download.h"

namespace docs {
namespace {

constexpr const char *LOCAL_MAGIC = "DDBIDX2\n";
constexpr uint32_t LOCAL_VERSION = 1;
constexpr const char *VOCAB_MAGIC = "DDBVOC1\n";
constexpr uint32_t VOCAB_VERSION = 1;

//===--------------------------------------------------------------------===//
// SHA-1 (RFC 3174)
//===--------------------------------------------------------------------===//

class Sha1 {
public:
	Sha1() {
		state_[0] = 0x67452301u;
		state_[1] = 0xEFCDAB89u;
		state_[2] = 0x98BADCFEu;
		state_[3] = 0x10325476u;
		state_[4] = 0xC3D2E1F0u;
	}

	void Update(const void *data, std::size_t length) {
		auto bytes = static_cast<const unsigned char *>(data);
		total_bytes_ += length;
		while (length > 0) {
			auto take = std::min(length, sizeof(buffer_) - buffer_size_);
			std::memcpy(buffer_ + buffer_size_, bytes, take);
			buffer_size_ += take;
			bytes += take;
			length -= take;
			if (buffer_size_ == sizeof(buffer_)) {
				Transform(buffer_);
				buffer_size_ = 0;
			}
		}
	}

	std::string Final() {
		auto bit_length = total_bytes_ * 8;
		unsigned char pad = 0x80;
		Update(&pad, 1);
		unsigned char zero = 0;
		while (buffer_size_ != 56) {
			Update(&zero, 1);
		}
		unsigned char length_bytes[8];
		for (int i = 0; i < 8; i++) {
			length_bytes[7 - i] = static_cast<unsigned char>((bit_length >> (i * 8)) & 0xff);
		}
		Update(length_bytes, 8);

		std::string result(20, '\0');
		for (int word = 0; word < 5; word++) {
			result[word * 4 + 0] = static_cast<char>((state_[word] >> 24) & 0xff);
			result[word * 4 + 1] = static_cast<char>((state_[word] >> 16) & 0xff);
			result[word * 4 + 2] = static_cast<char>((state_[word] >> 8) & 0xff);
			result[word * 4 + 3] = static_cast<char>(state_[word] & 0xff);
		}
		return result;
	}

private:
	static uint32_t RotateLeft(uint32_t value, int bits) {
		return (value << bits) | (value >> (32 - bits));
	}

	void Transform(const unsigned char *block) {
		uint32_t words[80];
		for (int i = 0; i < 16; i++) {
			words[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
			           (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
			           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
			           static_cast<uint32_t>(block[i * 4 + 3]);
		}
		for (int i = 16; i < 80; i++) {
			words[i] = RotateLeft(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
		}

		auto a = state_[0];
		auto b = state_[1];
		auto c = state_[2];
		auto d = state_[3];
		auto e = state_[4];
		for (int i = 0; i < 80; i++) {
			uint32_t f = 0;
			uint32_t k = 0;
			if (i < 20) {
				f = (b & c) | ((~b) & d);
				k = 0x5A827999u;
			} else if (i < 40) {
				f = b ^ c ^ d;
				k = 0x6ED9EBA1u;
			} else if (i < 60) {
				f = (b & c) | (b & d) | (c & d);
				k = 0x8F1BBCDCu;
			} else {
				f = b ^ c ^ d;
				k = 0xCA62C1D6u;
			}
			auto temp = RotateLeft(a, 5) + f + e + k + words[i];
			e = d;
			d = c;
			c = RotateLeft(b, 30);
			b = a;
			a = temp;
		}

		state_[0] += a;
		state_[1] += b;
		state_[2] += c;
		state_[3] += d;
		state_[4] += e;
	}

	uint32_t state_[5];
	uint64_t total_bytes_ = 0;
	unsigned char buffer_[64];
	std::size_t buffer_size_ = 0;
};

std::string Sha1Digest(const std::string &data) {
	Sha1 sha;
	sha.Update(data.data(), data.size());
	return sha.Final();
}

std::string Sha1Hex(const std::string &data) {
	static const char HEX[] = "0123456789abcdef";
	auto digest = Sha1Digest(data);
	std::string result;
	result.reserve(digest.size() * 2);
	for (auto byte : digest) {
		auto value = static_cast<unsigned char>(byte);
		result.push_back(HEX[value >> 4]);
		result.push_back(HEX[value & 0xf]);
	}
	return result;
}

//===--------------------------------------------------------------------===//
// File helpers
//===--------------------------------------------------------------------===//

bool FileExists(const std::string &path) {
	std::error_code error;
	return std::filesystem::exists(path, error) && !error;
}

std::string FirstExisting(const std::vector<std::string> &paths) {
	for (auto &path : paths) {
		if (FileExists(path)) {
			return path;
		}
	}
	return paths.empty() ? std::string() : paths.front();
}

bool FileHasContent(const std::string &path) {
	std::error_code error;
	auto size = std::filesystem::file_size(path, error);
	return !error && size > 0;
}

std::string FirstWithContent(const std::vector<std::string> &paths) {
	for (auto &path : paths) {
		if (FileHasContent(path)) {
			return path;
		}
	}
	return FirstExisting(paths);
}

std::string ReadFile(const std::string &path) {
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

void EnsureParentDirectory(const std::string &path) {
	std::error_code error;
	auto parent = std::filesystem::path(path).parent_path();
	if (parent.empty()) {
		return;
	}
	std::filesystem::create_directories(parent, error);
	if (error) {
		throw std::runtime_error("Could not create " + parent.string() + ": " + error.message());
	}
}

void WriteFileIfChanged(const std::string &path, const std::string &data) {
	if (FileExists(path)) {
		if (ReadFile(path) == data) {
			return;
		}
	}
	EnsureParentDirectory(path);

	auto temporary = path + ".tmp." + std::to_string(static_cast<long>(getpid()));
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
	if (std::rename(temporary.c_str(), path.c_str()) != 0) {
		std::remove(temporary.c_str());
		throw std::runtime_error("Could not replace " + path);
	}
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

std::string ExecutableDirectory() {
	std::vector<char> buffer(4096);
	while (true) {
		auto length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
		if (length < 0) {
			throw std::runtime_error("Could not determine the location of the docs binary");
		}
		if (static_cast<std::size_t>(length) < buffer.size() - 1) {
			buffer[static_cast<std::size_t>(length)] = '\0';
			std::string path(buffer.data());
			auto slash = path.rfind('/');
			if (slash == std::string::npos) {
				return ".";
			}
			return path.substr(0, slash);
		}
		buffer.resize(buffer.size() * 2);
	}
}

std::string CacheDirectory(const std::string &binary_dir) {
	// Keep generated files next to the binary so an installation is fully
	// self-contained and there is no per-user state in the temporary directory.
	auto directory = std::filesystem::path(binary_dir);

	std::error_code error;
	std::filesystem::create_directories(directory, error);
	if (error) {
		throw std::runtime_error("Could not create " + directory.string() + ": " + error.message());
	}
	return directory.string();
}

//===--------------------------------------------------------------------===//
// Little-endian serialization
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

void AppendF64(std::string &output, double value) {
	uint64_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	AppendU32(output, static_cast<uint32_t>(bits & 0xffffffffu));
	AppendU32(output, static_cast<uint32_t>(bits >> 32));
}

void AppendString(std::string &output, const std::string &value) {
	AppendU32(output, static_cast<uint32_t>(value.size()));
	output += value;
}

//===--------------------------------------------------------------------===//
// Local Markdown sections
//===--------------------------------------------------------------------===//

struct SectionMeta {
	std::string title;
	std::string anchor;
	uint32_t level = 0;
	bool has_body = false;
	uint32_t start_line = 0;
	uint32_t end_line = 0;
	uint32_t line_count = 0;
	uint32_t body_len = 0;
};

std::string NormalizeMarkdown(const std::string &text) {
	std::string result;
	result.reserve(text.size());
	for (std::size_t i = 0; i < text.size(); i++) {
		if (text[i] == '\r') {
			result.push_back('\n');
			if (i + 1 < text.size() && text[i + 1] == '\n') {
				i++;
			}
		} else {
			result.push_back(text[i]);
		}
	}
	return result;
}

std::vector<std::string> SplitLines(const std::string &text) {
	std::vector<std::string> lines;
	std::size_t start = 0;
	while (true) {
		auto end = text.find('\n', start);
		if (end == std::string::npos) {
			lines.push_back(text.substr(start));
			break;
		}
		lines.push_back(text.substr(start, end - start));
		start = end + 1;
	}
	return lines;
}

std::string LStripSpaces(const std::string &line) {
	std::size_t start = 0;
	while (start < line.size() && line[start] == ' ') {
		start++;
	}
	return line.substr(start);
}

bool IsFence(const std::string &line) {
	return line.size() >= 3 && ((line[0] == '`' && line[1] == '`' && line[2] == '`') ||
	                           (line[0] == '~' && line[1] == '~' && line[2] == '~'));
}

std::string TrimHorizontalWhitespace(std::string value) {
	while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
		value.pop_back();
	}
	std::size_t start = 0;
	while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
		start++;
	}
	return value.substr(start);
}

bool ParseHeading(const std::string &line, SectionMeta &section) {
	std::size_t position = 0;
	while (position < line.size() && line[position] == ' ') {
		position++;
	}
	if (position > 3) {
		return false;
	}

	auto hash_start = position;
	while (position < line.size() && line[position] == '#') {
		position++;
	}
	auto level = position - hash_start;
	if (level < 1 || level > 6) {
		return false;
	}
	if (position >= line.size() || (line[position] != ' ' && line[position] != '\t')) {
		return false;
	}
	while (position < line.size() && (line[position] == ' ' || line[position] == '\t')) {
		position++;
	}

	auto title = line.substr(position);
	while (!title.empty() && (title.back() == ' ' || title.back() == '\t')) {
		title.pop_back();
	}

	std::string anchor;
	if (!title.empty() && title.back() == '}') {
		auto marker = title.rfind("{#");
		if (marker != std::string::npos) {
			auto inner = title.substr(marker + 2, title.size() - marker - 3);
			if (inner.find('}') == std::string::npos) {
				anchor = inner;
				title = TrimHorizontalWhitespace(title.substr(0, marker));
			}
		}
	}

	section.title = std::move(title);
	section.anchor = std::move(anchor);
	section.level = static_cast<uint32_t>(level);
	return true;
}

bool HasNonWhitespace(const std::string &text) {
	for (auto raw : text) {
		auto byte = static_cast<unsigned char>(raw);
		if (byte >= 0x80 || !std::isspace(byte)) {
			return true;
		}
	}
	return false;
}

std::vector<SectionMeta> SplitSections(const std::string &text) {
	auto lines = SplitLines(text);
	std::vector<SectionMeta> sections;
	bool in_fence = false;
	char fence_char = '`';

	for (std::size_t line_number = 0; line_number < lines.size(); line_number++) {
		auto stripped = LStripSpaces(lines[line_number]);
		if (in_fence) {
			if (stripped.size() >= 3 && stripped[0] == fence_char && stripped[1] == fence_char &&
			    stripped[2] == fence_char) {
				in_fence = false;
			}
			continue;
		}
		if (IsFence(stripped)) {
			in_fence = true;
			fence_char = stripped[0];
			continue;
		}

		SectionMeta section;
		if (!ParseHeading(lines[line_number], section)) {
			continue;
		}
		if (!sections.empty()) {
			sections.back().end_line = static_cast<uint32_t>(line_number);
		}
		section.start_line = static_cast<uint32_t>(line_number);
		section.end_line = static_cast<uint32_t>(lines.size());
		sections.push_back(std::move(section));
	}

	for (auto &section : sections) {
		std::string body;
		for (auto line = section.start_line + 1; line < section.end_line; line++) {
			if (line > section.start_line + 1) {
				body.push_back('\n');
			}
			body += lines[line];
		}
		section.body_len = static_cast<uint32_t>(body.size());
		section.has_body = HasNonWhitespace(body);
		section.line_count = section.end_line - section.start_line;
	}
	return sections;
}

std::string BuildLocalIndex(const std::vector<SectionMeta> &sections, const std::string &source_hash) {
	std::string output;
	output.reserve(64 + sections.size() * 64);
	output += LOCAL_MAGIC;
	output += source_hash;
	AppendU32(output, LOCAL_VERSION);
	AppendU32(output, static_cast<uint32_t>(sections.size()));
	AppendU32(output, 0);
	for (auto &section : sections) {
		AppendString(output, section.title);
		AppendString(output, section.anchor);
		AppendU8(output, static_cast<uint8_t>(section.level));
		AppendU8(output, section.has_body ? 1 : 0);
		AppendU16(output, 0);
		AppendU32(output, section.start_line);
		AppendU32(output, section.line_count);
		AppendU32(output, section.body_len);
		AppendU32(output, 0);
	}
	return output;
}

} // namespace

std::string Sha1HexDigest(const std::string &data) {
	return Sha1Hex(data);
}

const DataPaths &GetDataPaths() {
	static const DataPaths paths = [] {
		DataPaths result;
		result.binary_dir = ExecutableDirectory();
		result.docs_markdown = FirstWithContent({JoinPath(result.binary_dir, "duckdb-docs.md")});
		result.blog_markdown = FirstWithContent({JoinPath(result.binary_dir, "duckdb-blog.md")});
		result.cache_dir = CacheDirectory(result.binary_dir);
		result.docs_data = JoinPath(result.cache_dir, "duckdb-docs-data");
		result.docs_index = JoinPath(result.cache_dir, "duckdb-docs-index");
		result.blog_data = JoinPath(result.cache_dir, "duckdb-blog-data");
		result.blog_index = JoinPath(result.cache_dir, "duckdb-blog-index");
		result.vocabulary = JoinPath(result.cache_dir, "duckdb-vocabulary");
		return result;
	}();
	return paths;
}

namespace {

struct CorpusDownload {
	const char *file_name;
	const char *url;
};

constexpr CorpusDownload CORPUS_DOWNLOADS[] = {
    {"duckdb-docs.md", "https://blobs.duckdb.org/docs/duckdb-docs.md"},
    {"duckdb-blog.md", "https://blobs.duckdb.org/docs/duckdb-blog.md"},
};

std::string DownloadUrl(const std::string &url) {
	common_remote_params params;
	params.headers.push_back({"User-Agent", "duckdb-docs-search/1.0"});
	params.timeout = 600;

	std::pair<long, std::vector<char>> response;
	try {
		response = common_remote_get_content(url, params);
	} catch (const std::exception &error) {
		throw std::runtime_error("Could not download " + url + ": " + error.what());
	}
	if (response.first < 200 || response.first >= 300) {
		throw std::runtime_error("Could not download " + url + ": HTTP status " +
		                         std::to_string(response.first));
	}
	if (response.second.empty()) {
		throw std::runtime_error("Could not download " + url + ": empty response");
	}
	return std::string(response.second.begin(), response.second.end());
}

std::string FormatSize(std::size_t bytes) {
	std::ostringstream output;
	output << std::fixed << std::setprecision(1);
	if (bytes >= 1024 * 1024) {
		output << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MB";
	} else if (bytes >= 1024) {
		output << (static_cast<double>(bytes) / 1024.0) << " KB";
	} else {
		output << bytes << " bytes";
	}
	return output.str();
}

class ByteProgressCallback : public common_download_callback {
public:
	explicit ByteProgressCallback(const std::function<void(uint64_t, uint64_t)> &progress)
	    : progress_(progress) {
	}

	void on_start(const common_download_progress &progress) override {
		Report(progress);
	}
	void on_update(const common_download_progress &progress) override {
		Report(progress);
	}
	void on_done(const common_download_progress &progress, bool ok) override {
		if (ok) {
			Report(progress);
		}
	}

private:
	void Report(const common_download_progress &progress) {
		if (progress_) {
			progress_(static_cast<uint64_t>(progress.downloaded), static_cast<uint64_t>(progress.total));
		}
	}

	const std::function<void(uint64_t, uint64_t)> &progress_;
};

} // namespace

void DownloadFile(const std::string &url, const std::string &destination,
                  const std::function<void(uint64_t done, uint64_t total)> &progress) {
	std::error_code error;
	auto parent = std::filesystem::path(destination).parent_path();
	if (!parent.empty()) {
		std::filesystem::create_directories(parent, error);
		if (error) {
			throw std::runtime_error("Could not create " + parent.string() + ": " + error.message());
		}
	}

	ByteProgressCallback callback(progress);
	common_download_opts opts;
	opts.headers.push_back({"User-Agent", "duckdb-docs-search/1.0"});
	opts.callback = &callback;

	// The shared downloader writes to a temporary file and only replaces the
	// destination when the transfer completes, so an interrupted download never
	// looks finished.
	auto status = common_download_file_single(url, destination, opts);
	if (status == 304) {
		return;
	}
	if (status < 0) {
		throw std::runtime_error("Could not download " + url);
	}
	if (status < 200 || status >= 300) {
		throw std::runtime_error("Could not download " + url + ": HTTP status " + std::to_string(status));
	}
}

std::vector<std::string> EnsureCorpusFiles(bool force) {
	auto &paths = GetDataPaths();
	std::vector<std::string> downloaded;

	for (auto &target : CORPUS_DOWNLOADS) {
		auto adjacent = JoinPath(paths.binary_dir, target.file_name);
		auto nested = JoinPath(paths.binary_dir, std::string("data/") + target.file_name);
		if (!force && (FileHasContent(adjacent) || FileHasContent(nested))) {
			continue;
		}

		// Replace the file that the corpus loader will actually read. If only
		// data/<name> exists, update it there instead of creating a second copy
		// next to the binary that GetDataPaths() has already resolved past.
		auto destination = !FileHasContent(adjacent) && FileHasContent(nested) ? nested : adjacent;

		// Show the source before the transfer starts so a slow connection is
		// visibly making progress instead of appearing to hang.
		std::cerr << "Downloading " << target.file_name << " from " << target.url << "\n";
		std::cerr.flush();

		auto data = DownloadUrl(target.url);
		WriteFileIfChanged(destination, data);
		downloaded.push_back(destination);

		std::cerr << "Downloaded " << target.file_name << " (" << FormatSize(data.size()) << ") to "
		          << destination << "\n";
	}
	return downloaded;
}

CorpusFiles LoadCorpusFiles(const std::string &name, const std::string &markdown_path,
                            const std::string &index_path, const std::string &data_path) {
	(void)name;
	auto markdown = ReadFile(markdown_path);
	auto normalized = NormalizeMarkdown(markdown);
	auto source_hash = Sha1Digest(markdown);

	if (FileExists(index_path) && FileExists(data_path)) {
		auto index = ReadFile(index_path);
		if (index.size() >= 28 && std::memcmp(index.data(), LOCAL_MAGIC, 8) == 0 &&
		    std::memcmp(index.data() + 8, source_hash.data(), source_hash.size()) == 0) {
			auto data = ReadFile(data_path);
			if (data == normalized) {
				return {std::move(data), std::move(index)};
			}
		}
	}

	auto sections = SplitSections(normalized);
	auto index = BuildLocalIndex(sections, source_hash);
	WriteFileIfChanged(data_path, normalized);
	WriteFileIfChanged(index_path, index);
	return {std::move(normalized), std::move(index)};
}

namespace {

std::unordered_map<std::string, uint32_t> ParseVocabulary(const std::string &blob,
                                                          const std::string &source_hash) {
	constexpr std::size_t HEADER_SIZE = 8 + 40 + 4 + 4;
	if (blob.size() < HEADER_SIZE || std::memcmp(blob.data(), VOCAB_MAGIC, 8) != 0 ||
	    blob.compare(8, 40, source_hash) != 0) {
		return {};
	}
	std::size_t position = 48;
	auto read_u32 = [&](uint32_t &value) {
		if (position + 4 > blob.size()) {
			return false;
		}
		value = static_cast<uint8_t>(blob[position]) |
		        (static_cast<uint32_t>(static_cast<uint8_t>(blob[position + 1])) << 8) |
		        (static_cast<uint32_t>(static_cast<uint8_t>(blob[position + 2])) << 16) |
		        (static_cast<uint32_t>(static_cast<uint8_t>(blob[position + 3])) << 24);
		position += 4;
		return true;
	};
	uint32_t version = 0;
	uint32_t count = 0;
	if (!read_u32(version) || version != VOCAB_VERSION || !read_u32(count)) {
		return {};
	}

	std::unordered_map<std::string, uint32_t> vocabulary;
	vocabulary.reserve(count);
	for (uint32_t entry = 0; entry < count; entry++) {
		uint32_t length = 0;
		uint32_t frequency = 0;
		if (!read_u32(length) || position + length > blob.size()) {
			return {};
		}
		std::string term = blob.substr(position, length);
		position += length;
		if (!read_u32(frequency)) {
			return {};
		}
		vocabulary.emplace(std::move(term), frequency);
	}
	return vocabulary;
}

void CountVocabulary(const std::string &text, std::unordered_map<std::string, uint32_t> &vocabulary) {
	std::string word;
	word.reserve(32);
	auto flush = [&]() {
		if (word.size() >= 3) {
			vocabulary[word]++;
		}
		word.clear();
	};
	for (auto raw : text) {
		auto c = static_cast<unsigned char>(raw);
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
			word.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
		} else {
			flush();
		}
	}
	flush();
}

} // namespace

std::unordered_map<std::string, uint32_t> LoadVocabulary(const std::string &cache_path,
                                                         const std::string &source_hash,
                                                         const std::vector<std::string> &sources) {
	if (FileExists(cache_path)) {
		auto cached = ParseVocabulary(ReadFile(cache_path), source_hash);
		if (!cached.empty()) {
			return cached;
		}
	}

	std::unordered_map<std::string, uint32_t> vocabulary;
	for (auto &source : sources) {
		CountVocabulary(source, vocabulary);
	}

	std::vector<std::pair<std::string, uint32_t>> entries(vocabulary.begin(), vocabulary.end());
	std::sort(entries.begin(), entries.end(),
	          [](const std::pair<std::string, uint32_t> &left, const std::pair<std::string, uint32_t> &right) {
		          return left.first < right.first;
	          });

	std::string blob;
	blob.reserve(64 + entries.size() * 24);
	blob += VOCAB_MAGIC;
	blob += source_hash;
	AppendU32(blob, VOCAB_VERSION);
	AppendU32(blob, static_cast<uint32_t>(entries.size()));
	for (auto &entry : entries) {
		AppendString(blob, entry.first);
		AppendU32(blob, entry.second);
	}
	WriteFileIfChanged(cache_path, blob);
	return vocabulary;
}

} // namespace docs
