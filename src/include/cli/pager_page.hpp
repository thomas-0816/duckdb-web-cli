#pragma once

#include "cli/markdown.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace docs {

struct TabHit {
	std::size_t first = 0;
	std::size_t last = 0;
	std::size_t tab = 0;
};

struct PagerPage {
	std::vector<std::string> header;
	std::vector<std::string> content;
	std::vector<std::vector<cli::TextLink>> content_links;
	std::vector<TabHit> tab_hits;
	//! Raw Markdown source of the page, copied by `c`.
	std::string source;
	//! Section headings shown in the outline on the right side.
	struct TocEntry {
		std::string text;
		std::size_t line = 0;
	};
	std::vector<TocEntry> toc;
	//! Number of columns used by the rendered page content.
	std::size_t content_width = 0;
	//! Width of the outline itself, excluding the gap before it. Zero when the
	//! outline is hidden.
	std::size_t toc_width = 0;
	//! First column of the outline (zero based).
	std::size_t toc_column = 0;
	double score = 0;
};

constexpr std::size_t TOC_GAP = 2;
//! Keep the outline clear of the terminal's right edge.
constexpr std::size_t TOC_RIGHT_MARGIN = 1;
constexpr std::size_t TOC_TOP_MARGIN = 1;
constexpr std::size_t TOC_MIN_WIDTH = 12;
constexpr std::size_t TOC_MAX_WIDTH = 30;
constexpr std::size_t TOC_MIN_CONTENT_WIDTH = 30;
constexpr int TOC_MIN_TERMINAL_COLUMNS = 70;

//! Index of the outline entry for the section at the top of the viewport.
inline std::size_t TocActiveIndex(const PagerPage &page, std::size_t scroll) {
	if (page.toc.empty()) {
		return 0;
	}
	std::size_t active = 0;
	for (std::size_t i = 0; i < page.toc.size(); i++) {
		if (page.toc[i].line > scroll + 1) {
			break;
		}
		active = i;
	}
	return active;
}

//! First outline entry that fits into `rows`, keeping the active entry visible.
inline std::size_t TocFirstVisible(const PagerPage &page, std::size_t active, std::size_t rows) {
	if (page.toc.size() <= rows) {
		return 0;
	}
	auto first = active > rows / 2 ? active - rows / 2 : 0;
	return std::min(first, page.toc.size() - rows);
}

} // namespace docs
