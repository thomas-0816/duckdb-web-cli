#pragma once

#include "cli/pager_mouse.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace docs {

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
