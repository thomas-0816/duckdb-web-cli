#pragma once

#include "cli/run.hpp"
#include "docs_search.hpp"

#include <string>
#include <vector>

namespace docs {

//! Build a Markdown list of the source pages behind an answer. Pages that
//! contributed more than one excerpt are linked once.
std::string BuildAnswerSources(const std::vector<SearchResult> &results);

//! Stream an answer for `query` in the interactive answer view, preparing the
//! model first when needed. The caller must be on a terminal. Returns true when
//! the user asked to quit the application.
bool RunAnswer(const char *label, const std::string &query, const std::vector<SearchResult> &results, bool color);

//! Show search results one page at a time on an interactive terminal.
//!
//! Up/Down (or j/k) scroll within the current result, Left/Right (or p/n)
//! switch between results, `?` edits the search query, `/` finds text inside
//! the current page (`n`/`p` move to the next/previous match), and Ctrl+c quits.
//! The function assumes stdin and stdout are terminals.
int RunPager(const char *label, SearchFunction search, idx_t limit, const std::string &query,
             const std::vector<SearchResult> &results, bool color, bool raw, std::size_t start_index = 0);

//! Interactive search line for when no query was given.
//!
//! Every edit re-runs the search. Up/Down selects a result, Enter opens the
//! pager, and Esc or Ctrl+c quits. The function assumes stdin and stdout are
//! terminals.
int RunLiveSearch(const char *label, SearchFunction search, idx_t limit, bool color, bool raw);

} // namespace docs
