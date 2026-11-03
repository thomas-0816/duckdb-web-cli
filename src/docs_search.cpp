#include "docs_search.hpp"

#include "docs_storage.hpp"
#include "semantic.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace docs {
namespace {

struct Section {
	std::string title;
	std::string anchor;
	uint32_t level = 0;
	uint32_t start_line = 0;
	uint32_t line_count = 0;
	uint32_t body_len = 0;
	bool has_body = false;
};

struct Page {
	std::string anchor;
	std::string title;
	uint32_t start_line = 0;
	uint32_t end_line = 0;
	std::vector<uint32_t> sections;
};

struct Corpus {
	std::string data;
	std::vector<std::string> lines;
	std::vector<Section> sections;
	std::vector<Page> pages;
	std::vector<uint32_t> section_pages;
	std::unordered_map<std::string, uint32_t> path_pages;
};

struct IndexReader {
	const std::string &data;
	size_t position = 0;

	explicit IndexReader(const std::string &data_p) : data(data_p) {
	}

	void Require(size_t amount) {
		if (position + amount > data.size()) {
			throw std::runtime_error("Invalid embedded search index: unexpected end of file");
		}
	}

	uint8_t U8() {
		Require(1);
		return static_cast<uint8_t>(data[position++]);
	}

	uint16_t U16() {
		Require(2);
		uint16_t result = static_cast<uint8_t>(data[position]) |
		                   (static_cast<uint16_t>(static_cast<uint8_t>(data[position + 1])) << 8);
		position += 2;
		return result;
	}

	uint32_t U32() {
		Require(4);
		uint32_t result = static_cast<uint8_t>(data[position]) |
		                  (static_cast<uint32_t>(static_cast<uint8_t>(data[position + 1])) << 8) |
		                  (static_cast<uint32_t>(static_cast<uint8_t>(data[position + 2])) << 16) |
		                  (static_cast<uint32_t>(static_cast<uint8_t>(data[position + 3])) << 24);
		position += 4;
		return result;
	}

	double F64() {
		uint64_t bits = static_cast<uint64_t>(U32()) | (static_cast<uint64_t>(U32()) << 32);
		double result = 0;
		std::memcpy(&result, &bits, sizeof(result));
		return result;
	}

	std::string String() {
		auto length = U32();
		Require(length);
		std::string result = data.substr(position, length);
		position += length;
		return result;
	}

	void Skip(size_t amount) {
		Require(amount);
		position += amount;
	}
};

char LowerChar(char c) {
	if (c >= 'A' && c <= 'Z') {
		return static_cast<char>(c - 'A' + 'a');
	}
	return c;
}

//===--------------------------------------------------------------------===//
// Local corpus
//===--------------------------------------------------------------------===//

std::vector<std::string> SplitLines(const std::string &text) {
	std::vector<std::string> lines;
	size_t start = 0;
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

std::string BlogDateToPath(const std::string &label) {
	if (label == "blog") {
		return "/news/";
	}
	auto rest = label.substr(5);
	if (rest.size() < 11 || rest[4] != '-' || rest[7] != '-' || rest[10] != '-') {
		return "/news/";
	}
	return "/" + rest.substr(0, 4) + "/" + rest.substr(5, 2) + "/" + rest.substr(8, 2) + "/" + rest.substr(11);
}

std::string AnchorToPath(const std::string &anchor) {
	auto separator = anchor.find("::");
	auto label = anchor.substr(0, separator);

	if (label.compare(0, 5, "docs:") == 0) {
		auto path = label.substr(5);
		std::replace(path.begin(), path.end(), ':', '/');
		return "/docs/" + path;
	}
	if (label.compare(0, 5, "blog:") == 0) {
		return BlogDateToPath(label);
	}
	if (label.find(':') != std::string::npos) {
		auto path = label;
		std::replace(path.begin(), path.end(), ':', '/');
		return "/docs/2.0/" + path;
	}
	return "/docs/2.0/" + label;
}

void BuildPages(Corpus &corpus) {
	std::unordered_map<std::string, uint32_t> page_ids;
	corpus.section_pages.resize(corpus.sections.size(), 0);

	for (idx_t section_id = 0; section_id < corpus.sections.size(); section_id++) {
		auto &section = corpus.sections[section_id];
		auto key = section.anchor.empty() ? std::string() : AnchorToPath(section.anchor);

		uint32_t page_id = 0;
		if (key.empty()) {
			if (corpus.pages.empty()) {
				Page page;
				page.title = section.title;
				page.start_line = section.start_line;
				corpus.pages.push_back(std::move(page));
			}
			page_id = static_cast<uint32_t>(corpus.pages.size() - 1);
		} else {
			auto existing = page_ids.find(key);
			if (existing == page_ids.end()) {
				Page page;
				page.anchor = key;
				page.title = section.title;
				page.start_line = section.start_line;
				page_id = static_cast<uint32_t>(corpus.pages.size());
				corpus.pages.push_back(std::move(page));
				page_ids.emplace(key, page_id);
			} else {
				page_id = existing->second;
			}
		}

		corpus.section_pages[section_id] = page_id;
		corpus.pages[page_id].sections.push_back(static_cast<uint32_t>(section_id));
	}

	for (idx_t i = 0; i < corpus.pages.size(); i++) {
		corpus.pages[i].end_line =
		    i + 1 < corpus.pages.size() ? corpus.pages[i + 1].start_line : static_cast<uint32_t>(corpus.lines.size());
	}

	// Some pages only group child sections and have no body of their own, such
	// as "## Parquet Files" immediately followed by "### Reading and Writing
	// Parquet Files". Give those pages the content of their first child page so
	// opening them from a search result does not show an empty page.
	auto page_level = [&](idx_t page_id) {
		auto &page = corpus.pages[page_id];
		if (page.sections.empty()) {
			return uint32_t(0);
		}
		return corpus.sections[page.sections.front()].level;
	};
	for (idx_t page_id = 0; page_id < corpus.pages.size(); page_id++) {
		auto &page = corpus.pages[page_id];
		bool has_body = false;
		auto first_line = std::min<std::size_t>(static_cast<std::size_t>(page.start_line) + 1, corpus.lines.size());
		auto last_line = std::min<std::size_t>(page.end_line, corpus.lines.size());
		for (auto line = first_line; line < last_line && !has_body; line++) {
			has_body = corpus.lines[line].find_first_not_of(" \t\r\n") != std::string::npos;
		}
		if (has_body) {
			continue;
		}
		auto level = page_level(page_id);
		for (idx_t child_id = page_id + 1; child_id < corpus.pages.size(); child_id++) {
			if (page_level(child_id) <= level) {
				break;
			}
			page.end_line = corpus.pages[child_id].end_line;
			break;
		}
	}

	for (idx_t page_id = 0; page_id < corpus.pages.size(); page_id++) {
		auto &page = corpus.pages[page_id];
		if (page.anchor.empty()) {
			continue;
		}
		corpus.path_pages.emplace(page.anchor, static_cast<uint32_t>(page_id));
	}

	// The published index uses the canonical query-syntax URL, while the
	// Markdown export still files this page under sql/samples.
	auto samples = corpus.path_pages.find("/docs/2.0/sql/samples");
	if (samples != corpus.path_pages.end()) {
		corpus.path_pages.emplace("/docs/2.0/sql/query_syntax/samples", samples->second);
	}
}

Corpus LoadCorpus(const std::string &data, const std::string &index) {
	static const char MAGIC[] = "DDBIDX2\n";
	if (index.size() < 40 || std::memcmp(index.data(), MAGIC, 8) != 0) {
		throw std::runtime_error("Invalid docs search index: bad magic");
	}

	Corpus corpus;
	corpus.data = data;
	corpus.lines = SplitLines(corpus.data);

	IndexReader reader(index);
	// The header stores the SHA-1 of the source Markdown after the magic.
	reader.position = 8 + 20;
	auto version = reader.U32();
	if (version != 1) {
		throw std::runtime_error("Unsupported docs search index version");
	}
	auto section_count = reader.U32();
	auto term_count = reader.U32();

	corpus.sections.reserve(section_count);
	for (idx_t i = 0; i < section_count; i++) {
		Section section;
		section.title = reader.String();
		section.anchor = reader.String();
		section.level = reader.U8();
		section.has_body = reader.U8() != 0;
		(void)reader.U16();
		section.start_line = reader.U32();
		section.line_count = reader.U32();
		section.body_len = reader.U32();
		(void)reader.U32();
		corpus.sections.push_back(std::move(section));
	}

	// Older builds stored a local inverted index here. Only the section
	// metadata is used now, so any term data is skipped.
	for (idx_t i = 0; i < term_count; i++) {
		(void)reader.String();
		auto posting_count = reader.U32();
		reader.Skip(static_cast<size_t>(posting_count) * 4);
	}

	BuildPages(corpus);
	return corpus;
}

const Corpus &GetManualCorpus() {
	static const Corpus corpus = [] {
		auto &paths = GetDataPaths();
		auto files = LoadCorpusFiles("docs", paths.docs_markdown, paths.docs_index, paths.docs_data);
		return LoadCorpus(files.data, files.index);
	}();
	return corpus;
}

const Corpus &GetBlogCorpus() {
	static const Corpus corpus = [] {
		auto &paths = GetDataPaths();
		auto files = LoadCorpusFiles("blog", paths.blog_markdown, paths.blog_index, paths.blog_data);
		return LoadCorpus(files.data, files.index);
	}();
	return corpus;
}

//===--------------------------------------------------------------------===//
// Page rendering
//===--------------------------------------------------------------------===//

std::string AnchorToUrl(const std::string &anchor) {
	auto separator = anchor.find("::");
	auto url = "https://duckdb.org" + AnchorToPath(anchor);
	if (separator == std::string::npos) {
		return url;
	}

	auto sub_anchor = anchor.substr(separator + 2);
	std::string slug;
	bool last_dash = false;
	for (auto raw : sub_anchor) {
		char c = LowerChar(raw);
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
			slug += c;
			last_dash = c == '-';
		} else if (c == ' ') {
			if (!last_dash) {
				slug += '-';
				last_dash = true;
			}
		}
	}
	while (!slug.empty() && slug.back() == '-') {
		slug.pop_back();
	}
	if (!slug.empty()) {
		url += "#" + slug;
	}
	return url;
}

bool HasUrlScheme(const std::string &href) {
	if (href.empty() || !std::isalpha(static_cast<unsigned char>(href[0]))) {
		return false;
	}
	for (size_t i = 1; i < href.size(); i++) {
		char c = href[i];
		if (c == ':') {
			return true;
		}
		if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '-' && c != '.') {
			return false;
		}
	}
	return false;
}

std::string ResolveLinkTarget(const std::string &href, const std::string &page_path) {
	if (href.empty()) {
		return href;
	}
	if (href[0] == '#') {
		auto target = href.substr(1);
		if (target.compare(0, 5, "docs:") == 0 || target.compare(0, 5, "blog:") == 0) {
			return AnchorToUrl(target);
		}
		// A bare fragment refers to a heading on the current page.
		return "https://duckdb.org" + page_path + href;
	}
	if (href.compare(0, 2, "//") == 0) {
		return "https:" + href;
	}
	if (href[0] == '/') {
		return "https://duckdb.org" + href;
	}
	if (HasUrlScheme(href)) {
		return href;
	}

	// Relative paths in the Markdown export (mostly images) are written
	// relative to the documentation root, so resolve them from the site root.
	auto relative = href;
	while (relative.compare(0, 3, "../") == 0) {
		relative = relative.substr(3);
	}
	if (relative.compare(0, 2, "./") == 0) {
		relative = relative.substr(2);
	}
	if (relative.empty()) {
		return href;
	}
	return "https://duckdb.org/" + relative;
}

void ReplaceInternalAnchors(std::string &text, const std::string &page_path) {
	size_t position = 0;
	while (true) {
		auto link = text.find("](", position);
		if (link == std::string::npos) {
			break;
		}
		// Find the matching ')' so destinations that contain balanced
		// parentheses (for example Wikipedia URLs) are kept intact.
		size_t close = std::string::npos;
		int depth = 0;
		bool escaped = false;
		for (size_t i = link + 2; i < text.size(); i++) {
			char c = text[i];
			if (escaped) {
				escaped = false;
				continue;
			}
			if (c == '\\') {
				escaped = true;
			} else if (c == '\n') {
				break;
			} else if (c == '(') {
				depth++;
			} else if (c == ')') {
				if (depth == 0) {
					close = i;
					break;
				}
				depth--;
			}
		}
		if (close == std::string::npos) {
			break;
		}
		auto href = text.substr(link + 2, close - (link + 2));
		auto resolved = ResolveLinkTarget(href, page_path);
		if (resolved != href) {
			text.replace(link + 2, close - (link + 2), resolved);
			position = link + 2 + resolved.size();
		} else {
			position = close + 1;
		}
	}
}

std::string TrimBlankLines(const std::string &input) {
	size_t start = 0;
	while (start < input.size() && input[start] == '\n') {
		start++;
	}
	size_t end = input.size();
	while (end > start && input[end - 1] == '\n') {
		end--;
	}
	return input.substr(start, end - start);
}

std::string BuildPageContent(const Corpus &corpus, const Page &page) {
	auto start = static_cast<idx_t>(page.start_line);
	auto end = std::min<idx_t>(page.end_line, corpus.lines.size());
	std::string result;
	for (auto line = start; line < end; line++) {
		if (!result.empty()) {
			result += '\n';
		}
		result += corpus.lines[line];
	}
	result = TrimBlankLines(result);
	ReplaceInternalAnchors(result, page.anchor);
	return result;
}

std::string PercentDecode(const std::string &text) {
	std::string result;
	result.reserve(text.size());
	for (std::size_t i = 0; i < text.size(); i++) {
		if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
		    std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
			auto hex = [](char c) {
				if (c >= '0' && c <= '9') {
					return c - '0';
				}
				return (c >= 'a' ? c - 'a' : c - 'A') + 10;
			};
			result.push_back(static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2])));
			i += 2;
		} else {
			result.push_back(text[i]);
		}
	}
	return result;
}

bool EndsWith(const std::string &text, const std::string &suffix) {
	return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

//! Extract the path of a duckdb.org URL, or accept an absolute path.
bool DuckDbPathFromUrl(const std::string &url, std::string &path) {
	std::size_t host_start = 0;
	auto scheme = url.find("://");
	if (scheme != std::string::npos) {
		host_start = scheme + 3;
	} else if (url.compare(0, 2, "//") == 0) {
		host_start = 2;
	} else if (!url.empty() && url[0] == '/') {
		path = url;
	} else {
		return false;
	}

	if (host_start > 0) {
		auto path_start = url.find('/', host_start);
		auto host = url.substr(host_start, path_start == std::string::npos ? std::string::npos : path_start - host_start);
		auto port = host.find(':');
		if (port != std::string::npos) {
			host.resize(port);
		}
		if (host != "duckdb.org" && host != "www.duckdb.org") {
			return false;
		}
		path = path_start == std::string::npos ? "/" : url.substr(path_start);
	}

	path = PercentDecode(path);
	auto cut = path.find_first_of("?#");
	if (cut != std::string::npos) {
		path.resize(cut);
	}
	if (EndsWith(path, ".html")) {
		path.resize(path.size() - 5);
	} else if (EndsWith(path, ".htm")) {
		path.resize(path.size() - 4);
	} else if (EndsWith(path, ".md")) {
		path.resize(path.size() - 3);
	}
	while (path.size() > 1 && path.back() == '/') {
		path.pop_back();
	}
	return !path.empty();
}

//! Map versioned or unversioned docs paths to the bundled documentation.
std::string NormalizeDocsPath(const std::string &path) {
	const std::string prefix = "/docs/";
	if (path.compare(0, prefix.size(), prefix) != 0) {
		return path;
	}
	auto rest = path.substr(prefix.size());
	auto separator = rest.find('/');
	auto first = rest.substr(0, separator);
	if (first == "2.0") {
		return path;
	}
	bool version_alias = first == "current" || first == "stable" || first == "lts";
	bool version_number = !first.empty() && std::isdigit(static_cast<unsigned char>(first[0])) != 0;
	if (version_alias || version_number) {
		auto tail = separator == std::string::npos ? std::string() : rest.substr(separator + 1);
		return "/docs/2.0/" + tail;
	}
	// Paths written without a version segment also live under the current docs.
	return "/docs/2.0/" + rest;
}

bool LookupPathInCorpus(const Corpus &corpus, const std::string &path, bool blog, SearchResult &result) {
	auto page_entry = corpus.path_pages.find(path);
	if (page_entry == corpus.path_pages.end()) {
		return false;
	}
	auto &page = corpus.pages[page_entry->second];
	result.content = BuildPageContent(corpus, page);
	result.score = 0;
	result.title = blog ? "Blog: " + page.title : page.title;
	return true;
}

bool IsDatedBlogPath(const std::string &path) {
	// Blog pages look like /2026/10/21/announcing-duckdb-200.
	if (path.size() < 13 || path[0] != '/' || path[5] != '/' || path[8] != '/' || path[11] != '/') {
		return false;
	}
	static const idx_t DIGITS[] = {1, 2, 3, 4, 6, 7, 9, 10};
	for (auto index : DIGITS) {
		if (path[index] < '0' || path[index] > '9') {
			return false;
		}
	}
	return true;
}

std::vector<SearchResult> LatestBlogPosts(const Corpus &blog, idx_t limit) {
	std::vector<const Page *> pages;
	for (auto &page : blog.pages) {
		if (IsDatedBlogPath(page.anchor)) {
			pages.push_back(&page);
		}
	}
	std::sort(pages.begin(), pages.end(),
	          [](const Page *left, const Page *right) { return left->anchor > right->anchor; });
	if (pages.size() > limit) {
		pages.resize(limit);
	}

	std::vector<SearchResult> results;
	results.reserve(pages.size());
	for (auto page : pages) {
		results.push_back({BuildPageContent(blog, *page), 0.0, "Blog: " + page->title});
	}
	return results;
}

//===--------------------------------------------------------------------===//
// Semantic search
//===--------------------------------------------------------------------===//

std::string CleanMarkdownForEmbedding(const std::string &line) {
	// Drop link destinations and image markup; keep the human-readable label so
	// that prose and code remain close to what a reader sees.
	std::string result;
	result.reserve(line.size());
	for (std::size_t position = 0; position < line.size();) {
		if (line[position] == '!' && position + 1 < line.size() && line[position + 1] == '[') {
			auto close = line.find(']', position + 2);
			if (close != std::string::npos && close + 1 < line.size() && line[close + 1] == '(') {
				auto end = line.find(')', close + 2);
				if (end != std::string::npos) {
					position = end + 1;
					continue;
				}
			}
		}
		if (line[position] == '[') {
			auto close = line.find(']', position + 1);
			if (close != std::string::npos && close + 1 < line.size() && line[close + 1] == '(') {
				auto end = line.find(')', close + 2);
				if (end != std::string::npos) {
					result += line.substr(position + 1, close - position - 1);
					position = end + 1;
					continue;
				}
			}
		}
		if (line[position] == '{' && position + 1 < line.size() && line[position + 1] == '#') {
			// Drop the explicit anchor that follows a heading, for example
			// {#docs:2.0:data:parquet:tips}.
			auto end = line.find('}', position + 2);
			if (end != std::string::npos) {
				position = end + 1;
				continue;
			}
		}
		if (line[position] == '`') {
			position++;
			continue;
		}
		result.push_back(line[position++]);
	}
	while (!result.empty() && (result.back() == ' ' || result.back() == '\t')) {
		result.pop_back();
	}
	std::size_t start = 0;
	while (start < result.size() && (result[start] == ' ' || result[start] == '\t')) {
		start++;
	}
	return result.substr(start);
}

std::string TrimMarkdownText(const std::string &text) {
	std::size_t start = 0;
	while (start < text.size() && (text[start] == '\n' || text[start] == ' ' || text[start] == '\t')) {
		start++;
	}
	std::size_t end = text.size();
	while (end > start && (text[end - 1] == '\n' || text[end - 1] == ' ' || text[end - 1] == '\t')) {
		end--;
	}
	return text.substr(start, end - start);
}

std::vector<std::string> ChunkSemanticText(const std::string &prefix, const std::string &body) {
	constexpr std::size_t MAX_CHARS = 1800;
	constexpr std::size_t OVERLAP = 200;

	auto trimmed = TrimMarkdownText(body);
	if (trimmed.empty()) {
		return {prefix};
	}
	if (prefix.size() + 1 + trimmed.size() <= MAX_CHARS) {
		return {prefix + "\n" + trimmed};
	}

	auto budget = MAX_CHARS > prefix.size() + 1 ? MAX_CHARS - prefix.size() - 1 : MAX_CHARS / 2;
	std::vector<std::string> chunks;
	std::size_t start = 0;
	while (start < trimmed.size()) {
		auto end = std::min(trimmed.size(), start + budget);
		if (end < trimmed.size()) {
			auto newline = trimmed.rfind('\n', end);
			if (newline != std::string::npos && newline > start + budget / 2) {
				end = newline;
			}
		}
		auto chunk = TrimMarkdownText(trimmed.substr(start, end - start));
		if (!chunk.empty()) {
			chunks.push_back(prefix + "\n" + chunk);
		}
		if (end >= trimmed.size()) {
			break;
		}
		start = end > OVERLAP ? end - OVERLAP : end;
	}
	return chunks;
}

void BuildSemanticDocuments(const Corpus &corpus, uint8_t type, std::vector<SemanticDocument> &documents) {
	// Embed a whole page at a time. Pages are the unit that is returned to the
	// user anyway, and grouping consecutive sections avoids thousands of tiny
	// chunks whose padding dominates the embedding cost.
	for (auto &page : corpus.pages) {
		auto start_line = std::min<std::size_t>(static_cast<std::size_t>(page.start_line) + 1, corpus.lines.size());
		auto end_line = std::min<std::size_t>(page.end_line, corpus.lines.size());

		std::string body;
		for (auto line = start_line; line < end_line; line++) {
			auto cleaned = CleanMarkdownForEmbedding(corpus.lines[line]);
			if (cleaned.empty()) {
				continue;
			}
			if (!body.empty()) {
				body += '\n';
			}
			body += cleaned;
		}

		for (auto &chunk : ChunkSemanticText(page.title, body)) {
			documents.push_back({std::move(chunk), type, static_cast<uint32_t>(&page - corpus.pages.data())});
		}
	}
}

struct ScoredPage {
	uint32_t page_id;
	uint8_t type;
	double score;
};

struct SemanticCorpusCache {
	const Corpus *manual = nullptr;
	const Corpus *blog = nullptr;
	std::vector<SemanticDocument> documents;
	std::string fingerprint;
	std::unordered_map<std::string, uint32_t> vocabulary;
};

//! Split text into lowercase ASCII words. Non-ASCII bytes act as separators,
//! which is enough for English documentation and keeps the tokenizer small.
std::vector<std::string> TokenizeWords(const std::string &text, std::size_t min_length) {
	std::vector<std::string> words;
	std::string word;
	word.reserve(32);
	auto flush = [&]() {
		if (word.size() >= min_length) {
			words.push_back(word);
		}
		word.clear();
	};
	for (auto raw : text) {
		auto c = static_cast<unsigned char>(raw);
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
			word.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
		} else {
			flush();
		}
	}
	flush();
	return words;
}

//! Damerau-Levenshtein distance (optimal string alignment) with early exit.
uint32_t BoundedDistance(const std::string &left, const std::string &right, uint32_t max_distance) {
	std::vector<uint32_t> previous(right.size() + 1);
	std::vector<uint32_t> current(right.size() + 1);
	std::vector<uint32_t> previous_previous(right.size() + 1);
	for (std::size_t j = 0; j <= right.size(); j++) {
		previous[j] = static_cast<uint32_t>(j);
	}
	for (std::size_t i = 1; i <= left.size(); i++) {
		current[0] = static_cast<uint32_t>(i);
		auto row_minimum = current[0];
		for (std::size_t j = 1; j <= right.size(); j++) {
			auto cost = left[i - 1] == right[j - 1] ? 0u : 1u;
			auto value = std::min({previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost});
			if (i > 1 && j > 1 && left[i - 1] == right[j - 2] && left[i - 2] == right[j - 1]) {
				value = std::min(value, previous_previous[j - 2] + 1);
			}
			current[j] = value;
			row_minimum = std::min(row_minimum, value);
		}
		if (row_minimum > max_distance) {
			return max_distance + 1;
		}
		previous_previous.swap(previous);
		previous.swap(current);
	}
	return previous[right.size()];
}

std::string CorrectToken(const std::string &token, const std::unordered_map<std::string, uint32_t> &vocabulary) {
	if (token.size() < 3 || vocabulary.find(token) != vocabulary.end()) {
		return token;
	}
	auto max_distance = token.size() >= 6 ? 2u : 1u;
	const std::string *best = nullptr;
	auto best_distance = max_distance + 1;
	uint32_t best_frequency = 0;
	for (auto &entry : vocabulary) {
		auto &candidate = entry.first;
		if (candidate.empty() || candidate[0] != token[0] || entry.second < 2) {
			continue;
		}
		auto difference = candidate.size() > token.size() ? candidate.size() - token.size() : token.size() - candidate.size();
		if (difference > max_distance) {
			continue;
		}
		auto distance = BoundedDistance(token, candidate, max_distance);
		if (distance > max_distance) {
			continue;
		}
		if (distance < best_distance || (distance == best_distance && entry.second > best_frequency)) {
			best = &candidate;
			best_distance = distance;
			best_frequency = entry.second;
		}
	}
	return best ? *best : token;
}

//! Correct unknown query words against the documentation vocabulary. If no word
//! changes, the original query is kept exactly as typed.
std::string CorrectQuery(const std::string &query, const std::unordered_map<std::string, uint32_t> &vocabulary) {
	if (vocabulary.empty()) {
		return query;
	}
	auto words = TokenizeWords(query, 1);
	bool changed = false;
	std::string corrected;
	for (auto &word : words) {
		auto replacement = CorrectToken(word, vocabulary);
		changed = changed || replacement != word;
		if (!corrected.empty()) {
			corrected += ' ';
		}
		corrected += replacement;
	}
	return changed ? corrected : query;
}

//! Small exact-term boost for keyword-style queries. Semantic scores stay in
//! charge; this only promotes pages that contain the corrected query words.
double KeywordBoost(const Corpus &corpus, const Page &page, const std::vector<std::string> &terms) {
	std::unordered_map<std::string, uint32_t> counts;
	for (auto &term : terms) {
		counts.emplace(term, 0);
	}

	auto title_words = TokenizeWords(page.title, 1);
	std::size_t title_hits = 0;
	for (auto &term : terms) {
		if (std::find(title_words.begin(), title_words.end(), term) != title_words.end()) {
			title_hits++;
		}
	}

	auto start = std::min<std::size_t>(static_cast<std::size_t>(page.start_line) + 1, corpus.lines.size());
	auto end = std::min<std::size_t>(page.end_line, corpus.lines.size());
	std::size_t body_length = 0;
	for (auto line = start; line < end; line++) {
		body_length += corpus.lines[line].size();
		for (auto &word : TokenizeWords(corpus.lines[line], 1)) {
			auto entry = counts.find(word);
			if (entry != counts.end()) {
				entry->second++;
			}
		}
	}

	double boost = 0;
	for (auto &term : terms) {
		auto density = static_cast<double>(counts[term]) / (1.0 + static_cast<double>(body_length) / 500.0);
		boost += 0.12 * std::min(1.0, density);
	}
	if (title_hits > 0 && !title_words.empty()) {
		auto coverage = static_cast<double>(title_hits) / static_cast<double>(title_words.size());
		boost += 0.20 * (0.5 + 0.5 * std::min(1.0, coverage / 0.5));
	}
	return terms.empty() ? 0.0 : boost / static_cast<double>(terms.size());
}

SemanticCorpusCache &GetSemanticCorpusCache(const Corpus *manual, const Corpus *blog) {
	static SemanticCorpusCache cache;

	if (cache.manual != manual || cache.blog != blog) {
		cache.manual = manual;
		cache.blog = blog;
		cache.documents.clear();
		if (manual) {
			BuildSemanticDocuments(*manual, 0, cache.documents);
		}
		if (blog) {
			BuildSemanticDocuments(*blog, 1, cache.documents);
		}
		cache.fingerprint = "semantic-v2";
		if (manual) {
			cache.fingerprint += "|docs:" + Sha1HexDigest(manual->data);
		}
		if (blog) {
			cache.fingerprint += "|blog:" + Sha1HexDigest(blog->data);
		}
		std::vector<std::string> sources;
		if (manual) {
			sources.push_back(manual->data);
		}
		if (blog) {
			sources.push_back(blog->data);
		}
		cache.vocabulary =
		    LoadVocabulary(GetDataPaths().vocabulary, Sha1HexDigest(cache.fingerprint), sources);
	}
	return cache;
}

std::vector<SearchResult> SearchSemanticCorpora(const Corpus *manual, const Corpus *blog, const std::string &query,
                                                idx_t limit, uint8_t mode) {
	auto &cache = GetSemanticCorpusCache(manual, blog);
	auto corrected_query = CorrectQuery(query, cache.vocabulary);
	auto correction = corrected_query != query ? corrected_query : std::string();

	auto wanted = [&](uint8_t type) { return mode == 2 || type == mode; };
	auto candidates = std::max<idx_t>(limit * 16, 128);
	candidates = std::min<idx_t>(candidates, 2000);
	auto hits = SearchSemantic(cache.documents, cache.fingerprint, corrected_query, candidates);

	std::unordered_map<uint64_t, double> page_scores;
	for (auto &hit : hits) {
		if (!wanted(hit.type)) {
			continue;
		}
		auto corpus = hit.type == 1 ? blog : manual;
		if (!corpus || hit.page_id >= corpus->pages.size()) {
			continue;
		}
		auto key = (static_cast<uint64_t>(hit.type) << 32) | hit.page_id;
		auto existing = page_scores.find(key);
		if (existing == page_scores.end()) {
			page_scores.emplace(key, hit.score);
		} else {
			existing->second = std::max(existing->second, static_cast<double>(hit.score));
		}
	}

	auto query_terms = TokenizeWords(corrected_query, 3);
	auto use_keyword_boost = query_terms.size() > 0 && query_terms.size() <= 3;

	std::vector<ScoredPage> ranked;
	ranked.reserve(page_scores.size());
	for (auto &entry : page_scores) {
		auto corpus = static_cast<uint8_t>(entry.first >> 32) == 1 ? blog : manual;
		auto page_id = static_cast<uint32_t>(entry.first & 0xffffffff);
		auto score = entry.second;
		if (use_keyword_boost && corpus && page_id < corpus->pages.size()) {
			auto boost = KeywordBoost(*corpus, corpus->pages[page_id], query_terms);
			score = std::min(1.0, score + boost);
		}
		ranked.push_back({page_id, static_cast<uint8_t>(entry.first >> 32), score});
	}
	std::sort(ranked.begin(), ranked.end(), [&](const ScoredPage &left, const ScoredPage &right) {
		if (left.score != right.score) {
			return left.score > right.score;
		}
		if (left.type != right.type) {
			return left.type < right.type;
		}
		auto left_corpus = left.type == 1 ? blog : manual;
		auto right_corpus = right.type == 1 ? blog : manual;
		return left_corpus->pages[left.page_id].start_line < right_corpus->pages[right.page_id].start_line;
	});
	if (ranked.size() > limit) {
		ranked.resize(limit);
	}

	std::vector<SearchResult> results;
	results.reserve(ranked.size());
	for (auto &entry : ranked) {
		auto corpus = entry.type == 1 ? blog : manual;
		auto &page = corpus->pages[entry.page_id];
		auto title = entry.type == 1 ? "Blog: " + page.title : page.title;
		results.push_back({BuildPageContent(*corpus, page), entry.score, std::move(title), correction});
	}
	return results;
}

//===--------------------------------------------------------------------===//
// Search
//===--------------------------------------------------------------------===//

//! Search one corpus (mode 0), the other corpus (mode 1), or both (mode 2).
//!
//! Every query is answered by the semantic index. Blog results get a "Blog: "
//! title prefix so they stand out in combined results.
std::vector<SearchResult> SearchCorpora(const Corpus *manual, const Corpus *blog, const std::string &query,
                                        idx_t limit, uint8_t mode) {
	if (limit == 0) {
		return {};
	}
	if (!SemanticModelAvailable()) {
		throw std::runtime_error("Semantic search is unavailable: the embedding model could not be loaded");
	}
	return SearchSemanticCorpora(manual, blog, query, limit, mode);
}

} // namespace

bool LookupPageByUrl(const std::string &url, SearchResult &result) {
	std::string path;
	if (!DuckDbPathFromUrl(url, path)) {
		return false;
	}
	if (path.compare(0, 6, "/docs/") == 0 || path == "/docs") {
		auto &manual = GetManualCorpus();
		if (LookupPathInCorpus(manual, path, false, result)) {
			return true;
		}
		auto normalized = NormalizeDocsPath(path);
		if (normalized != path && LookupPathInCorpus(manual, normalized, false, result)) {
			return true;
		}
		return false;
	}

	auto &blog = GetBlogCorpus();
	if (LookupPathInCorpus(blog, path, true, result)) {
		return true;
	}
	auto &manual = GetManualCorpus();
	if (LookupPathInCorpus(manual, path, false, result)) {
		return true;
	}
	auto normalized = NormalizeDocsPath(path);
	return normalized != path && LookupPathInCorpus(manual, normalized, false, result);
}

std::vector<SearchResult> SearchManual(const std::string &query, idx_t limit) {
	auto &manual = GetManualCorpus();
	return SearchCorpora(&manual, nullptr, query, limit, 0);
}

std::vector<SearchResult> SearchBlog(const std::string &query, idx_t limit) {
	auto &blog = GetBlogCorpus();
	return SearchCorpora(nullptr, &blog, query, limit, 1);
}

std::vector<SearchResult> SearchAll(const std::string &query, idx_t limit) {
	auto &blog = GetBlogCorpus();
	if (query.empty()) {
		return LatestBlogPosts(blog, limit);
	}
	auto &manual = GetManualCorpus();
	return SearchCorpora(&manual, &blog, query, limit, 2);
}

void PrepareSemanticSearch(const std::string &query, bool force) {
	(void)query;
	(void)force;
	if (!SemanticModelAvailable()) {
		return;
	}
	auto &manual = GetManualCorpus();
	auto &blog = GetBlogCorpus();
	auto &cache = GetSemanticCorpusCache(&manual, &blog);
	EnsureSemanticIndex(cache.documents, cache.fingerprint);
}

} // namespace docs
