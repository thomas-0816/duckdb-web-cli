#pragma once

#include "cli/run.hpp"
#include "docs_search.hpp"

#include <string>
#include <vector>

namespace docs {

//! Show search results one page at a time on an interactive terminal.
//!
//! Up/Down (or j/k) scroll within the current result, Left/Right (or p/n)
//! switch between results, `?` edits the search query, `/` finds text inside
//! the current page (`n`/`p` move to the next/previous match), and q quits. The
//! function assumes stdin and stdout are terminals.
int RunPager(const char *label, SearchFunction search, idx_t limit, const std::string &query,
             const std::vector<SearchResult> &results, bool color, bool raw, std::size_t start_index = 0);

//! Interactive search line for when no query was given.
//!
//! Every edit re-runs the search. Up/Down selects a result, Enter opens the
//! pager, and Esc quits. The function assumes stdin and stdout are terminals.
int RunLiveSearch(const char *label, SearchFunction search, idx_t limit, bool color, bool raw);

} // namespace docs
