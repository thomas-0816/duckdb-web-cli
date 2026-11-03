#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace docs {

//! Locations of the source files that ship next to the binary and of the
//! generated cache files.
struct DataPaths {
	std::string binary_dir;
	std::string docs_markdown;
	std::string blog_markdown;
	std::string cache_dir;
	std::string docs_data;
	std::string docs_index;
	std::string blog_data;
	std::string blog_index;
	std::string vocabulary;
};

const DataPaths &GetDataPaths();

struct CorpusFiles {
	std::string data;
	std::string index;
};

//! Load a Markdown corpus and its section metadata, rebuilding the cached
//! files in the temporary directory when the source changed.
CorpusFiles LoadCorpusFiles(const std::string &name, const std::string &markdown_path,
                            const std::string &index_path, const std::string &data_path);

//! Hex-encoded SHA-1 of a byte string, used to key generated cache files.
std::string Sha1HexDigest(const std::string &data);

//! Load the term vocabulary used for query spelling correction. The cache is
//! rebuilt from the given Markdown sources when the fingerprint changes.
std::unordered_map<std::string, uint32_t> LoadVocabulary(const std::string &cache_path,
                                                         const std::string &source_hash,
                                                         const std::vector<std::string> &sources);

} // namespace docs
