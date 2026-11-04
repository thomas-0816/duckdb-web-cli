#pragma once

#include "cli/pager_page.hpp"

#include <cstddef>

namespace docs {

enum class PagerClickAction {
	None,
	Redraw,
	Quit,
	EditQuery,
	OpenLink,
};

struct PagerClick {
	PagerClickAction action = PagerClickAction::None;
	cli::TextLink link;
};

//! Resolve a mouse click against the tab bar, the outline, or a link in the
//! rendered page. The caller applies the returned action, which keeps the
//! navigation and link-opening logic out of the mouse handling itself.
PagerClick HandlePagerClick(const PagerPage &page, int row, std::size_t column, bool internal, bool search,
                            int header_rows, std::size_t available, std::size_t max_scroll,
                            std::size_t &scroll, std::size_t &index, std::size_t &selected_toc,
                            std::size_t &selected_toc_scroll, bool &edit_query, std::size_t &resume_index);

} // namespace docs
