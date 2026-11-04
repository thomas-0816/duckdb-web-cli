#include "cli/pager_click.hpp"

#include <algorithm>

namespace docs {

PagerClick HandlePagerClick(const PagerPage &page, int row, std::size_t column, bool internal, bool search,
                            int header_rows, std::size_t available, std::size_t max_scroll,
                            std::size_t &scroll, std::size_t &index, std::size_t &selected_toc,
                            std::size_t &selected_toc_scroll, bool &edit_query, std::size_t &resume_index) {
	PagerClick result;
	if (row == 0) {
		if (internal) {
			// The Back label in the first header line.
			result.action = PagerClickAction::Quit;
			return result;
		}
		bool hit = false;
		for (auto &tab : page.tab_hits) {
			if (column < tab.first || column >= tab.last) {
				continue;
			}
			hit = true;
			if (tab.tab == 0) {
				if (search) {
					edit_query = true;
					resume_index = index;
					result.action = PagerClickAction::EditQuery;
				}
			} else {
				index = tab.tab - 1;
				selected_toc = std::size_t(-1);
				result.action = PagerClickAction::Redraw;
			}
			break;
		}
		if (!hit) {
			result.action = PagerClickAction::None;
		}
		return result;
	}
	auto content_row = row - header_rows;
	if (content_row < 0 || static_cast<std::size_t>(content_row) >= available) {
		result.action = PagerClickAction::None;
		return result;
	}
	auto toc_rows = available > TOC_TOP_MARGIN ? available - TOC_TOP_MARGIN : 0;
	if (toc_rows > 0 && page.toc_width > 0 && !page.toc.empty() && column >= page.toc_column &&
	    static_cast<std::size_t>(content_row) >= TOC_TOP_MARGIN) {
		auto active = TocActiveIndex(page, scroll);
		auto first = TocFirstVisible(page, active, toc_rows);
		auto toc_index = first + static_cast<std::size_t>(content_row) - TOC_TOP_MARGIN;
		if (toc_index < page.toc.size()) {
			scroll = std::min(page.toc[toc_index].line, max_scroll);
			selected_toc = toc_index;
			selected_toc_scroll = scroll;
			result.action = PagerClickAction::Redraw;
		}
		return result;
	}
	auto line = scroll + static_cast<std::size_t>(content_row);
	if (line < page.content_links.size()) {
		for (auto &link : page.content_links[line]) {
			if (column >= link.start && column < link.end) {
				result.action = PagerClickAction::OpenLink;
				result.link = link;
				return result;
			}
		}
	}
	return result;
}

} // namespace docs
