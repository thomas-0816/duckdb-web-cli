#pragma once

#include "docs_search.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace docs {

enum class AnswerAction {
	None,
	Done,
	Restart,
	Quit,
	OpenResult,
	EditQuery,
};

//! Stream an answer below the still-active search result list. The selected
//! list entry is updated in place; returning OpenResult asks the caller to open
//! that result. Typing returns EditQuery with the key stored in pending_key so
//! the caller can apply it to the prompt.
AnswerAction ShowAnswerWithResults(const char *label, const std::string &query,
                                   const std::vector<SearchResult> &results,
                                   const std::vector<SearchResult> &search_results, std::size_t &selected,
                                   std::size_t &query_cursor, bool color, int &pending_key);

//! Show the model-loading progress below the still-visible result list.
void DrawSplitAnswerLoading(const char *label, const std::string &query, std::size_t query_cursor,
                            const std::vector<SearchResult> &results, std::size_t selected, bool color,
                            float progress, const std::string &message);

} // namespace docs
