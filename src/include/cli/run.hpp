#pragma once

#include "docs_search.hpp"

#include <string>
#include <vector>

namespace docs {

using SearchFunction = std::vector<SearchResult> (*)(const std::string &, idx_t);

//! One search mode: the label shown in headers and status lines, plus the
//! function that performs the search.
struct SearchMode {
	const char *label;
	SearchFunction search;
};

//! Run the command-line front end. The search covers the documentation and the
//! blog together.
int RunCli(const char *program, const SearchMode &mode, int argc, char **argv);

} // namespace docs
