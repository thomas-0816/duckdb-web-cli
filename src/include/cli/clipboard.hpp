#pragma once

#include "cli/markdown.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace docs {

//! Copy text to the system clipboard.
void SetClipboard(const std::string &text);

//! Mouse event as reported by the terminal in SGR mode.
struct MouseEvent {
	int button = 0;
	int column = 0;
	int row = 0;
	bool press = false;
};

//! Enable or disable terminal mouse reporting.
void SetMouseCapture(bool enabled);

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

enum class PagerClickAction {
	None,
	Redraw,
	Quit,
	Back,
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

//! Visible bytes of a rendered line plus the byte offset each one has in the
//! original string. ANSI escape sequences are skipped so searches and text
//! selection only ever work on what the user can see.
struct PlainLine {
	std::string text;
	std::vector<std::size_t> offsets;
};

PlainLine MakePlainLine(const std::string &line);

//! Text selected with the mouse. Rows are absolute content rows and columns
//! are screen columns inside the content pane (the outline is not selectable).
struct TextSelection {
	bool active = false;
	bool dragging = false;
	std::size_t anchor_row = 0;
	std::size_t anchor_col = 0;
	std::size_t focus_row = 0;
	std::size_t focus_col = 0;
};

std::string SelectionText(const std::vector<std::string> &content, std::size_t content_width,
                          const TextSelection &selection);

std::string HighlightSelection(const std::string &line, const TextSelection &selection,
                               std::size_t row, std::size_t content_width);

//! Geometry shared by the views that support mouse text selection.
struct PagerMouseContext {
	int header_rows = 0;
	std::size_t available = 0;
	std::size_t scroll = 0;
	std::size_t content_rows = 0;
	std::size_t content_width = 0;
	bool has_toc = false;
	std::size_t toc_column = 0;
	std::size_t toc_top_margin = 0;
};

struct PagerMouseState {
	TextSelection selection;
	bool pressed = false;
	int press_row = 0;
	std::size_t press_column = 0;
};

enum class PagerMouseAction {
	None,
	Redraw,
	Copy,
	Click,
};

struct PagerMouseResult {
	PagerMouseAction action = PagerMouseAction::None;
	int click_row = 0;
	std::size_t click_column = 0;
};

//! Shared mouse handling for the pager views.
//!
//! A press inside the selectable content starts a selection and is reported as
//! a redraw. Dragging updates the focus and reports a redraw. Releasing a drag
//! reports a copy but leaves the highlight visible until the next press. A
//! press in a non-selectable area (tab bar, outline, list, header) is reported
//! immediately as a click so the caller can navigate. A press-and-release in
//! the content without dragging is also reported as a click on release, which
//! lets a drag select text instead of following a link.
PagerMouseResult HandlePagerMouse(const PagerMouseContext &context, PagerMouseState &state,
                                  const MouseEvent &event);

} // namespace docs
