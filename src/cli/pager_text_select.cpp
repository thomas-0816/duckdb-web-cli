#include "cli/pager_text_select.hpp"

#include "cli/markdown.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace docs {
namespace {

struct SelectionBounds {
	std::size_t start_row = 0;
	std::size_t start_col = 0;
	std::size_t end_row = 0;
	std::size_t end_col = 0;
};

SelectionBounds NormalizeSelection(const TextSelection &selection) {
	SelectionBounds bounds;
	bool forward = selection.anchor_row < selection.focus_row ||
	               (selection.anchor_row == selection.focus_row && selection.anchor_col <= selection.focus_col);
	if (forward) {
		bounds.start_row = selection.anchor_row;
		bounds.start_col = selection.anchor_col;
		bounds.end_row = selection.focus_row;
		bounds.end_col = selection.focus_col;
	} else {
		bounds.start_row = selection.focus_row;
		bounds.start_col = selection.focus_col;
		bounds.end_row = selection.anchor_row;
		bounds.end_col = selection.anchor_col;
	}
	return bounds;
}

std::size_t Utf8LengthAt(const std::string &text, std::size_t position) {
	auto byte = static_cast<unsigned char>(text[position]);
	std::size_t length = 1;
	if ((byte & 0xe0) == 0xc0) {
		length = 2;
	} else if ((byte & 0xf0) == 0xe0) {
		length = 3;
	} else if ((byte & 0xf8) == 0xf0) {
		length = 4;
	}
	if (position + length > text.size()) {
		length = 1;
	}
	return length;
}

//! Byte index in `text` for the character that starts at `column`.
std::size_t PlainIndexForColumn(const std::string &text, std::size_t column) {
	std::size_t position = 0;
	std::size_t width = 0;
	while (position < text.size()) {
		auto length = Utf8LengthAt(text, position);
		auto chunk = text.substr(position, length);
		auto chunk_width = cli::VisibleWidth(chunk);
		if (width + chunk_width > column) {
			break;
		}
		width += chunk_width;
		position += length;
	}
	return position;
}

std::string SlicePlainColumns(const std::string &text, std::size_t start, std::size_t end) {
	if (end <= start) {
		return std::string();
	}
	auto begin = PlainIndexForColumn(text, start);
	auto finish = PlainIndexForColumn(text, end);
	if (finish <= begin) {
		return std::string();
	}
	return text.substr(begin, finish - begin);
}

} // namespace

PlainLine MakePlainLine(const std::string &line) {
	PlainLine plain;
	plain.offsets.reserve(line.size());
	std::size_t position = 0;
	while (position < line.size()) {
		auto byte = static_cast<unsigned char>(line[position]);
		if (byte == 0x1b) {
			position++;
			if (position < line.size() && line[position] == '[') {
				position++;
				while (position < line.size()) {
					auto end = static_cast<unsigned char>(line[position++]);
					if (end >= 0x40 && end <= 0x7e) {
						break;
					}
				}
			} else if (position < line.size() && line[position] == ']') {
				position++;
				while (position < line.size()) {
					if (line[position] == '\x07') {
						position++;
						break;
					}
					if (line[position] == '\x1b' && position + 1 < line.size() && line[position + 1] == '\\') {
						position += 2;
						break;
					}
					position++;
				}
			} else if (position < line.size()) {
				position++;
			}
			continue;
		}
		if (byte == '\r') {
			position++;
			continue;
		}
		plain.text.push_back(line[position]);
		plain.offsets.push_back(position);
		position++;
	}
	return plain;
}

std::string SelectionText(const std::vector<std::string> &content, std::size_t content_width,
                          const TextSelection &selection) {
	if (!selection.active || content.empty()) {
		return std::string();
	}
	auto bounds = NormalizeSelection(selection);
	if (bounds.start_row >= content.size()) {
		return std::string();
	}
	bounds.end_row = std::min(bounds.end_row, content.size() - 1);
	std::string result;
	for (auto row = bounds.start_row; row <= bounds.end_row; row++) {
		auto plain = MakePlainLine(content[row]);
		auto from = row == bounds.start_row ? bounds.start_col : 0;
		auto to = row == bounds.end_row ? bounds.end_col : content_width;
		if (row > bounds.start_row) {
			result.push_back('\n');
		}
		result += SlicePlainColumns(plain.text, from, to);
	}
	return result;
}

std::string HighlightSelection(const std::string &line, const TextSelection &selection,
                               std::size_t row, std::size_t content_width) {
	if (!selection.active) {
		return line;
	}
	auto bounds = NormalizeSelection(selection);
	if (row < bounds.start_row || row > bounds.end_row) {
		return line;
	}
	auto from = row == bounds.start_row ? bounds.start_col : 0;
	auto to = row == bounds.end_row ? bounds.end_col : content_width;
	auto plain = MakePlainLine(line);
	auto begin = PlainIndexForColumn(plain.text, from);
	auto end = PlainIndexForColumn(plain.text, to);
	if (end <= begin) {
		return line;
	}
	auto begin_byte = begin < plain.offsets.size() ? plain.offsets[begin] : line.size();
	auto end_byte = end < plain.offsets.size() ? plain.offsets[end] : line.size();

	// Splice reverse video into the already-styled line. Every escape sequence
	// inside the selection re-asserts reverse video, because syntax colors end
	// with a reset that would otherwise stop the highlight halfway through the
	// selected text.
	std::string result;
	result.reserve(line.size() + 16);
	bool selected = false;
	std::size_t position = 0;
	while (position < line.size()) {
		if (!selected && position == begin_byte) {
			result += "\x1b[7m";
			selected = true;
		}
		if (selected && position == end_byte) {
			result += "\x1b[27m";
			selected = false;
		}
		auto byte = static_cast<unsigned char>(line[position]);
		if (byte == 0x1b) {
			auto sequence_start = position;
			position++;
			if (position < line.size() && line[position] == '[') {
				position++;
				while (position < line.size()) {
					auto terminator = static_cast<unsigned char>(line[position++]);
					if (terminator >= 0x40 && terminator <= 0x7e) {
						break;
					}
				}
			} else if (position < line.size() && line[position] == ']') {
				position++;
				while (position < line.size()) {
					if (line[position] == '\x07') {
						position++;
						break;
					}
					if (line[position] == '\x1b' && position + 1 < line.size() &&
					    line[position + 1] == '\\') {
						position += 2;
						break;
					}
					position++;
				}
			} else if (position < line.size()) {
				position++;
			}
			result.append(line, sequence_start, position - sequence_start);
			if (selected) {
				result += "\x1b[7m";
			}
			continue;
		}
		result.push_back(line[position++]);
	}
	if (selected) {
		result += "\x1b[27m";
	}
	return result;
}

PagerMouseResult HandlePagerMouse(const PagerMouseContext &context, PagerMouseState &state,
                                  const MouseEvent &event) {
	auto row = event.row;
	auto column = static_cast<std::size_t>(std::max(event.column, 0));
	bool left = (event.button & 3) == 0;
	bool motion = (event.button & 32) != 0;
	bool release = !event.press;

	if (left && motion) {
		if (state.selection.active) {
			auto content_row = row - context.header_rows;
			if (content_row >= 0 && static_cast<std::size_t>(content_row) < context.available) {
				auto line = context.scroll + static_cast<std::size_t>(content_row);
				if (line < context.content_rows) {
					state.selection.focus_row = line;
					state.selection.focus_col =
					    std::min(column, context.content_width > 0 ? context.content_width - 1 : 0);
					state.selection.dragging = true;
					return {PagerMouseAction::Redraw};
				}
			}
		}
		return {};
	}

	if (left && release) {
		if (state.selection.active && state.selection.dragging) {
			// Keep the selection active so the highlight remains visible after
			// the copy. The next mouse press starts a new selection or click.
			state.selection.dragging = false;
			state.pressed = false;
			return {PagerMouseAction::Copy};
		}
		if (state.pressed) {
			PagerMouseResult result;
			result.action = PagerMouseAction::Click;
			result.click_row = state.press_row;
			result.click_column = state.press_column;
			state.pressed = false;
			state.selection = {};
			return result;
		}
		return {};
	}

	if (!event.press || !left) {
		return {};
	}

	// Any new press clears the previous selection.
	state.pressed = false;
	state.selection = {};
	auto content_row = row - context.header_rows;
	if (content_row >= 0 && static_cast<std::size_t>(content_row) < context.available) {
		auto line = context.scroll + static_cast<std::size_t>(content_row);
		bool in_toc = context.has_toc && column >= context.toc_column &&
		              static_cast<std::size_t>(content_row) >= context.toc_top_margin;
		if (!in_toc && line < context.content_rows && column < context.content_width) {
			state.selection.active = true;
			state.selection.anchor_row = state.selection.focus_row = line;
			state.selection.anchor_col = state.selection.focus_col =
			    std::min(column, context.content_width > 0 ? context.content_width - 1 : 0);
			state.pressed = true;
			state.press_row = row;
			state.press_column = column;
			return {PagerMouseAction::Redraw};
		}
	}

	PagerMouseResult result;
	result.action = PagerMouseAction::Click;
	result.click_row = row;
	result.click_column = column;
	return result;
}

} // namespace docs
