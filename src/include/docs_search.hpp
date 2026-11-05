#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace docs {

using idx_t = std::size_t;

struct SearchResult {
	std::string content;
	double score;
	std::string title;
	//! Corrected query when spelling correction changed the query, empty
	//! otherwise.
	std::string corrected_query;
	//! Canonical page URL when the result comes from a known page, empty
	//! otherwise.
	std::string url;
};

std::vector<SearchResult> SearchManual(const std::string &query, idx_t limit);
std::vector<SearchResult> SearchBlog(const std::string &query, idx_t limit);
std::vector<SearchResult> SearchAll(const std::string &query, idx_t limit);

//! Return the best-matching semantic content blocks instead of whole pages.
//! Each block is one chunk of a page (at most about 1800 characters) and keeps
//! the page title as a prefix. Used as context for answer mode.
std::vector<SearchResult> SearchAnswerBlocks(const std::string &query, idx_t limit);

//! Return one snippet per matching page, ranked exactly like `SearchAll`. Each
//! snippet is the page's best-matching content block (at most about 1800
//! characters) with the page title as a prefix. Used for `--snippets`.
std::vector<SearchResult> SearchAllSnippets(const std::string &query, idx_t limit);

//! Build the semantic index ahead of the first search, with a progress
//! indicator. Called at startup so the query does not sit silently while the
//! embeddings are computed.
void PrepareSemanticSearch(const std::string &query, bool force = false);

//! Find a page in the bundled documentation or blog by a duckdb.org URL.
//!
//! Returns true when the target page is available locally. The result contains
//! the Markdown content and title, ready for the pager. Links to older
//! documentation versions resolve to the bundled version when possible.
bool LookupPageByUrl(const std::string &url, SearchResult &result);

//! Whether a duckdb.org URL can be opened as a bundled page. Used to color
//! links consistently with what opening them will do: links that are not in the
//! corpus open in the browser and are shown as external.
bool IsInternalPageUrl(const std::string &url);

} // namespace docs
