#include "cli/clipboard.hpp"

#include "cli/markdown.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace docs {
namespace {

std::string Base64(const std::string &input) {
	static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string output;
	output.reserve((input.size() + 2) / 3 * 4);
	std::size_t i = 0;
	while (i + 2 < input.size()) {
		unsigned int value = (static_cast<unsigned char>(input[i]) << 16) |
		                     (static_cast<unsigned char>(input[i + 1]) << 8) |
		                     static_cast<unsigned char>(input[i + 2]);
		output.push_back(alphabet[(value >> 18) & 63]);
		output.push_back(alphabet[(value >> 12) & 63]);
		output.push_back(alphabet[(value >> 6) & 63]);
		output.push_back(alphabet[value & 63]);
		i += 3;
	}
	if (i < input.size()) {
		unsigned int value = static_cast<unsigned char>(input[i]) << 16;
		if (i + 1 < input.size()) {
			value |= static_cast<unsigned char>(input[i + 1]) << 8;
		}
		output.push_back(alphabet[(value >> 18) & 63]);
		output.push_back(alphabet[(value >> 12) & 63]);
		output.push_back(i + 1 < input.size() ? alphabet[(value >> 6) & 63] : '=');
		output.push_back('=');
	}
	return output;
}

#if !defined(_WIN32)
//! Temporarily ignore SIGPIPE while writing to a helper process that may exit
//! before reading all of its input.
class SigpipeGuard {
  public:
	SigpipeGuard() {
		struct sigaction action = {};
		action.sa_handler = SIG_IGN;
		sigemptyset(&action.sa_mask);
		active_ = sigaction(SIGPIPE, &action, &previous_) == 0;
	}
	~SigpipeGuard() {
		if (active_) {
			sigaction(SIGPIPE, &previous_, nullptr);
		}
	}
	SigpipeGuard(const SigpipeGuard &) = delete;
	SigpipeGuard &operator=(const SigpipeGuard &) = delete;

  private:
	struct sigaction previous_ = {};
	bool active_ = false;
};

//! Run `command` and write `text` to its standard input. Returns true when the
//! command ran and exited successfully.
bool WriteToCommand(const std::vector<std::string> &command, const std::string &text) {
	if (command.empty()) {
		return false;
	}
	std::vector<char *> argv;
	argv.reserve(command.size() + 1);
	for (auto &argument : command) {
		argv.push_back(const_cast<char *>(argument.c_str()));
	}
	argv.push_back(nullptr);

	int pipe_fds[2];
	if (pipe(pipe_fds) != 0) {
		return false;
	}
	pid_t pid = fork();
	if (pid < 0) {
		close(pipe_fds[0]);
		close(pipe_fds[1]);
		return false;
	}
	if (pid == 0) {
		dup2(pipe_fds[0], STDIN_FILENO);
		close(pipe_fds[0]);
		close(pipe_fds[1]);
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > STDERR_FILENO) {
				close(devnull);
			}
		}
		execvp(argv[0], argv.data());
		_exit(127);
	}
	close(pipe_fds[0]);
	{
		SigpipeGuard guard;
		std::size_t written = 0;
		while (written < text.size()) {
			auto chunk = write(pipe_fds[1], text.data() + written, text.size() - written);
			if (chunk <= 0) {
				break;
			}
			written += static_cast<std::size_t>(chunk);
		}
	}
	close(pipe_fds[1]);
	int status = 0;
	if (waitpid(pid, &status, 0) != pid) {
		return false;
	}
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

//! Write the text to an X11 clipboard tool. Both CLIPBOARD and PRIMARY are set
//! so middle-click paste works as well. The tools detach themselves and keep
//! owning the selection after this process exits.
bool WriteToX11(const std::string &text) {
	if (!std::getenv("DISPLAY")) {
		return false;
	}
	if (WriteToCommand({"xclip", "-selection", "clipboard"}, text)) {
		WriteToCommand({"xclip", "-selection", "primary"}, text);
		return true;
	}
	if (WriteToCommand({"xsel", "--clipboard", "--input"}, text)) {
		WriteToCommand({"xsel", "--primary", "--input"}, text);
		return true;
	}
	return false;
}
#endif

#if defined(_WIN32)
bool CopyToWindowsClipboard(const std::string &text) {
	int wide_length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
	if (wide_length <= 0) {
		return false;
	}
	std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
	if (MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), wide_length) !=
	    wide_length) {
		return false;
	}
	HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t));
	if (!memory) {
		return false;
	}
	auto *destination = static_cast<wchar_t *>(GlobalLock(memory));
	if (!destination) {
		GlobalFree(memory);
		return false;
	}
	std::memcpy(destination, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
	GlobalUnlock(memory);

	if (!OpenClipboard(nullptr)) {
		GlobalFree(memory);
		return false;
	}
	EmptyClipboard();
	if (!SetClipboardData(CF_UNICODETEXT, memory)) {
		CloseClipboard();
		GlobalFree(memory);
		return false;
	}
	CloseClipboard();
	return true;
}
#endif

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

bool g_mouse_captured = false;

} // namespace

void SetClipboard(const std::string &text) {
	if (text.empty()) {
		return;
	}
	std::string copy = text;
	if (copy.back() != '\n') {
		copy.push_back('\n');
	}
	auto encoded = Base64(copy);
	// OSC 52 works in kitty, WezTerm, foot and similar terminals. Konsole
	// ignores it, so each platform also uses its native clipboard below.
	std::fputs("\x1b]52;c;", stdout);
	std::fputs(encoded.c_str(), stdout);
	std::fputc('\a', stdout);
	std::fputs("\x1b]52;p;", stdout);
	std::fputs(encoded.c_str(), stdout);
	std::fputc('\a', stdout);
	std::fflush(stdout);

#if defined(_WIN32)
	CopyToWindowsClipboard(copy);
#elif defined(__APPLE__)
	WriteToCommand({"pbcopy"}, copy);
#else
	bool copied = false;
	if (std::getenv("WAYLAND_DISPLAY")) {
		copied = WriteToCommand({"wl-copy"}, copy);
	}
	if (!copied) {
		WriteToX11(copy);
	}
#endif
}

void SetMouseCapture(bool enabled) {
	if (enabled == g_mouse_captured) {
		return;
	}
	g_mouse_captured = enabled;
	if (enabled) {
		// Report button presses, releases, and drags with SGR coordinates. The
		// drag reports let the pager implement its own text selection instead
		// of relying on the terminal's selection while the mouse is captured.
		std::cout << "\x1b[?1000h\x1b[?1002h\x1b[?1006h";
	} else {
		std::cout << "\x1b[?1006l\x1b[?1002l\x1b[?1000l";
	}
	std::cout << std::flush;
}

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

PagerClick HandlePagerClick(const PagerPage &page, int row, std::size_t column, bool internal, bool search,
                            int header_rows, std::size_t available, std::size_t max_scroll,
                            std::size_t &scroll, std::size_t &index, std::size_t &selected_toc,
                            std::size_t &selected_toc_scroll, bool &edit_query, std::size_t &resume_index) {
	PagerClick result;
	if (row == 0) {
		if (internal) {
			// The Back label in the first header line.
			result.action = PagerClickAction::Back;
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
