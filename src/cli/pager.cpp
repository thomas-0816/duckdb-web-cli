#include "cli/pager.hpp"

#include "cli/markdown.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <utility>
#include <vector>

#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace docs {
namespace {

constexpr const char *RESET = "\x1b[0m";
constexpr const char *BOLD = "\x1b[1m";
constexpr const char *DIM = "\x1b[2m";
constexpr const char *HEADER_COLOR = "\x1b[1;38;5;39m";

enum : int {
	KEY_UP = 1000,
	KEY_DOWN,
	KEY_LEFT,
	KEY_RIGHT,
	KEY_PGUP,
	KEY_PGDN,
	KEY_HOME,
	KEY_END,
	KEY_DELETE,
	KEY_MOUSE,
};

struct MouseEvent {
	int button = 0;
	int column = 0;
	int row = 0;
	bool press = false;
};

MouseEvent g_mouse_event;

struct TerminalSize {
	int rows = 24;
	int columns = 80;
};

TerminalSize GetTerminalSize() {
	TerminalSize size;
	struct winsize window = {};
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) == 0) {
		if (window.ws_row > 0) {
			size.rows = window.ws_row;
		}
		if (window.ws_col > 0) {
			size.columns = window.ws_col;
		}
	}
	return size;
}

std::string PlainTitle(const std::string &title) {
	std::string result;
	bool pending_space = false;
	for (auto c : title) {
		if (c == '`') {
			continue;
		}
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			pending_space = !result.empty();
			continue;
		}
		if (pending_space) {
			result.push_back(' ');
			pending_space = false;
		}
		result.push_back(c);
	}
	return result;
}

std::string TruncatePlain(const std::string &text, std::size_t max_width) {
	if (max_width == 0) {
		return std::string();
	}
	if (cli::VisibleWidth(text) <= max_width) {
		return text;
	}
	std::string result;
	std::size_t position = 0;
	while (position < text.size()) {
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
		auto candidate = result + text.substr(position, length);
		if (cli::VisibleWidth(candidate) + 1 > max_width) {
			break;
		}
		result = std::move(candidate);
		position += length;
	}
	return result + "\xe2\x80\xa6";
}

std::string TailToWidth(const std::string &text, std::size_t max_width) {
	if (max_width == 0) {
		return std::string();
	}
	if (cli::VisibleWidth(text) <= max_width) {
		return text;
	}
	std::vector<std::pair<std::size_t, std::size_t>> characters;
	std::size_t position = 0;
	while (position < text.size()) {
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
		characters.emplace_back(position, length);
		position += length;
	}

	std::string result;
	std::size_t width = 0;
	for (auto entry = characters.rbegin(); entry != characters.rend(); ++entry) {
		auto chunk = text.substr(entry->first, entry->second);
		auto chunk_width = cli::VisibleWidth(chunk);
		if (width + chunk_width > max_width) {
			break;
		}
		result = chunk + result;
		width += chunk_width;
	}
	return result;
}

struct VisibleQuery {
	std::string text;
	//! Width of the part of the query before the cursor.
	std::size_t cursor_width = 0;
};

//! Return the part of a query that fits into max_width columns while keeping
//! the cursor visible. Text hidden on the left is replaced by an ellipsis.
VisibleQuery FitQueryToWidth(const std::string &query, std::size_t cursor, std::size_t max_width) {
	VisibleQuery result;
	if (max_width == 0) {
		return result;
	}
	cursor = std::min(cursor, query.size());
	auto before = query.substr(0, cursor);
	auto after = query.substr(cursor);
	if (cli::VisibleWidth(before) > max_width) {
		auto tail_width = max_width > 1 ? max_width - 1 : 0;
		result.text = "\xe2\x80\xa6" + TailToWidth(before, tail_width);
	} else {
		result.text = before;
	}
	result.cursor_width = cli::VisibleWidth(result.text);
	auto remaining = max_width > result.cursor_width ? max_width - result.cursor_width : 0;
	if (remaining > 0 && !after.empty()) {
		result.text += TruncatePlain(after, remaining);
	}
	return result;
}

struct Tab {
	std::string plain;
	std::string styled;
	std::size_t width = 0;
};

void StyleTab(Tab &tab, bool active, bool color) {
	if (color) {
		tab.styled = (active ? std::string("\x1b[1;7m") : std::string(DIM)) + tab.plain + RESET;
	} else {
		tab.styled = active ? "[" + tab.plain + "]" : tab.plain;
	}
	tab.width = cli::VisibleWidth(tab.plain);
}

Tab MakeTab(std::size_t position, const std::string &title, bool active, bool color) {
	Tab tab;
	tab.plain = std::to_string(position + 1) + " " + PlainTitle(title);
	StyleTab(tab, active, color);
	return tab;
}

struct TabHit {
	std::size_t first = 0;
	std::size_t last = 0;
	std::size_t tab = 0;
};

struct TabBarLayout {
	std::string text;
	std::vector<TabHit> hits;
};

TabBarLayout MakeTabBar(const std::vector<SearchResult> &results, std::size_t index, int columns, bool color) {
	auto width_limit = static_cast<std::size_t>(std::max(columns - 1, 1));
	std::vector<Tab> tabs;
	tabs.reserve(results.size() + 1);
	Tab search_tab;
	search_tab.plain = "Search";
	for (auto &result : results) {
		if (!result.corrected_query.empty()) {
			search_tab.plain = "Search: " + result.corrected_query;
			break;
		}
	}
	StyleTab(search_tab, false, color);
	tabs.push_back(std::move(search_tab));
	for (std::size_t i = 0; i < results.size(); i++) {
		tabs.push_back(MakeTab(i, results[i].title, i == index, color));
	}
	if (tabs.empty()) {
		return {};
	}
	auto active = index + 1;

	auto make_range = [&](std::size_t first, std::size_t last) {
		TabBarLayout layout;
		std::size_t column = 0;
		if (first > 0) {
			layout.text += "\xe2\x80\xa6 ";
			column += 2;
		}
		for (auto i = first; i <= last; i++) {
			if (i > first) {
				layout.text += " | ";
				column += 3;
			}
			auto start = column;
			layout.text += tabs[i].styled;
			column += tabs[i].width;
			layout.hits.push_back({start, column, i});
		}
		if (last + 1 < tabs.size()) {
			layout.text += " \xe2\x80\xa6";
		}
		return layout;
	};
	auto width = [&](std::size_t first, std::size_t last) {
		return cli::VisibleWidth(make_range(first, last).text);
	};

	if (width(0, tabs.size() - 1) <= width_limit) {
		return make_range(0, tabs.size() - 1);
	}

	// The active title has to fit even if it is the only tab we can show.
	auto room = width_limit;
	if (active > 0) {
		room = room > 2 ? room - 2 : 0;
	}
	if (active + 1 < tabs.size()) {
		room = room > 2 ? room - 2 : 0;
	}
	if (tabs[active].width > room) {
		tabs[active].plain = TruncatePlain(tabs[active].plain, room);
		StyleTab(tabs[active], true, color);
	}

	std::size_t first = active;
	std::size_t last = active;
	bool grow_right = true;
	while (true) {
		bool grew = false;
		for (int attempt = 0; attempt < 2 && !grew; attempt++) {
			bool right = attempt == 0 ? grow_right : !grow_right;
			if (right) {
				if (last + 1 < tabs.size() && width(first, last + 1) <= width_limit) {
					last++;
					grew = true;
				}
			} else if (first > 0 && width(first - 1, last) <= width_limit) {
				first--;
				grew = true;
			}
		}
		if (!grew) {
			break;
		}
		grow_right = !grow_right;
	}
	return make_range(first, last);
}

struct PagerPage {
	std::vector<std::string> header;
	std::vector<std::string> content;
	std::vector<std::vector<cli::TextLink>> content_links;
	std::vector<TabHit> tab_hits;
	double score = 0;
};

void AppendPageContent(PagerPage &page, const SearchResult &result, bool color, bool raw, int columns) {
	auto content = raw ? result.content : cli::RenderMarkdown(result.content, color);
	std::size_t start = 0;
	auto wrap_width = static_cast<std::size_t>(std::max(columns, 1));
	while (start < content.size()) {
		auto end = content.find('\n', start);
		if (end == std::string::npos) {
			end = content.size();
		}
		auto chunks = cli::WrapAnsi(content.substr(start, end - start), wrap_width);
		for (auto &chunk : chunks) {
			page.content_links.push_back(cli::ExtractLinks(chunk));
			page.content.push_back(std::move(chunk));
		}
		start = end + 1;
	}
	// Leave one blank line after the page so the last line is never flush
	// against the status bar.
	if (page.content.empty() || !page.content.back().empty()) {
		page.content.push_back(std::string());
		page.content_links.push_back({});
	}
}

void AppendPageHeader(PagerPage &page, bool color, int columns) {
	auto rule_width = static_cast<std::size_t>(std::max(columns - 1, 1));
	auto rule = std::string(rule_width, '-');
	page.header.push_back(color ? std::string(DIM) + rule + RESET : rule);
}

PagerPage BuildPage(const std::vector<SearchResult> &results, std::size_t index, bool color, bool raw, int columns) {
	PagerPage page;
	page.score = results[index].score;
	auto tab_bar = MakeTabBar(results, index, columns, color);
	page.header.push_back(std::move(tab_bar.text));
	page.tab_hits = std::move(tab_bar.hits);
	AppendPageHeader(page, color, columns);
	AppendPageContent(page, results[index], color, raw, columns);
	return page;
}

PagerPage BuildInternalPage(const SearchResult &result, bool color, bool raw, int columns) {
	PagerPage page;
	page.score = 0;
	auto width_limit = static_cast<std::size_t>(std::max(columns - 1, 1));
	auto title = PlainTitle(result.title);
	auto prefix = std::string("[Back] | ");
	auto title_room = width_limit > prefix.size() ? width_limit - prefix.size() : 0;
	title = TruncatePlain(title, title_room);
	std::string back = "[Back]";
	if (color) {
		back = std::string("\x1b[1;7m") + back + RESET;
	}
	page.header.push_back(back + " | " + title);
	AppendPageHeader(page, color, columns);
	AppendPageContent(page, result, color, raw, columns);
	return page;
}

//! Open a web or mail link with the desktop's default handler.
//!
//! The command runs in a detached grandchild so the pager keeps ownership of
//! the terminal, and the URL is passed as an argument rather than through a
//! shell. Only well-known schemes are accepted so clicking arbitrary rendered
//! text can never execute a local command.
bool OpenUrl(const std::string &url) {
	auto separator = url.find(':');
	if (separator == std::string::npos) {
		return false;
	}
	std::string scheme = url.substr(0, separator);
	for (auto &c : scheme) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	if (scheme != "http" && scheme != "https" && scheme != "mailto") {
		return false;
	}

	auto child = fork();
	if (child < 0) {
		return false;
	}
	if (child == 0) {
		// Detach from the pager session and let the opener outlive us.
		setsid();
		auto grandchild = fork();
		if (grandchild != 0) {
			_exit(grandchild < 0 ? 1 : 0);
		}
		auto null_fd = open("/dev/null", O_RDWR);
		if (null_fd >= 0) {
			dup2(null_fd, STDIN_FILENO);
			dup2(null_fd, STDOUT_FILENO);
			dup2(null_fd, STDERR_FILENO);
			if (null_fd > STDERR_FILENO) {
				close(null_fd);
			}
		}
#if defined(__APPLE__)
		execlp("open", "open", url.c_str(), static_cast<char *>(nullptr));
#else
		execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char *>(nullptr));
		execlp("gio", "gio", "open", url.c_str(), static_cast<char *>(nullptr));
#endif
		_exit(127);
	}
	int status = 0;
	while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
	}
	return true;
}

class RawMode {
public:
	RawMode() {
		if (tcgetattr(STDIN_FILENO, &original_) != 0) {
			return;
		}
		auto raw = original_;
		raw.c_lflag &= ~(ICANON | ECHO | ISIG);
		raw.c_iflag &= ~(IXON | ICRNL);
		raw.c_cc[VMIN] = 1;
		raw.c_cc[VTIME] = 0;
		active_ = tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0;
	}

	~RawMode() {
		if (active_) {
			tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
		}
	}

private:
	struct termios original_ = {};
	bool active_ = false;
};

class PagerScreen {
public:
	explicit PagerScreen(bool mouse = false) : mouse_(mouse) {
		// Always take over the whole terminal: enter the alternate screen,
		// hide the cursor and start from a blank page.
		std::cout << "\x1b[?1049h\x1b[?25l\x1b[H\x1b[2J";
		if (mouse_) {
			// Report button presses and releases with SGR coordinates.
			std::cout << "\x1b[?1000h\x1b[?1006h";
		}
		std::cout << std::flush;
	}

	~PagerScreen() {
		if (mouse_) {
			std::cout << "\x1b[?1006l\x1b[?1000l";
		}
		std::cout << "\x1b[?25h\x1b[?1049l" << std::flush;
	}

private:
	bool mouse_ = false;
};

bool WaitForInput(int milliseconds) {
	struct pollfd descriptor = {STDIN_FILENO, POLLIN, 0};
	return poll(&descriptor, 1, milliseconds) > 0;
}

int ReadByte() {
	unsigned char byte = 0;
	if (read(STDIN_FILENO, &byte, 1) != 1) {
		return -1;
	}
	return byte;
}

int ReadKey() {
	int key = ReadByte();
	if (key < 0) {
		return 'q';
	}
	if (key != 27) {
		return key;
	}
	if (!WaitForInput(30)) {
		return 27;
	}
	int second = ReadByte();
	if (second != '[' && second != 'O') {
		return 27;
	}
	if (second == 'O') {
		int third = ReadByte();
		if (third == 'A') {
			return KEY_UP;
		}
		if (third == 'B') {
			return KEY_DOWN;
		}
		if (third == 'C') {
			return KEY_RIGHT;
		}
		if (third == 'D') {
			return KEY_LEFT;
		}
		if (third == 'H') {
			return KEY_HOME;
		}
		if (third == 'F') {
			return KEY_END;
		}
		return 0;
	}

	std::string sequence;
	while (WaitForInput(20)) {
		int next = ReadByte();
		if (next < 0) {
			break;
		}
		sequence.push_back(static_cast<char>(next));
		if ((next >= 'A' && next <= 'Z') || (next >= 'a' && next <= 'z') || next == '~') {
			break;
		}
	}
	if (!sequence.empty() && sequence[0] == '<') {
		auto action = sequence.back();
		if (action == 'M' || action == 'm') {
			int values[3] = {0, 0, 0};
			int count = 0;
			std::size_t position = 1;
			while (position + 1 < sequence.size() && count < 3) {
				if (sequence[position] < '0' || sequence[position] > '9') {
					position++;
					continue;
				}
				int value = 0;
				while (position + 1 < sequence.size() && sequence[position] >= '0' &&
				       sequence[position] <= '9') {
					value = value * 10 + (sequence[position] - '0');
					position++;
				}
				values[count++] = value;
				if (position + 1 < sequence.size() && sequence[position] == ';') {
					position++;
				}
			}
			if (count == 3) {
				auto button = values[0];
				if ((button & 64) != 0) {
					if (button == 64) {
						return KEY_UP;
					}
					if (button == 65) {
						return KEY_DOWN;
					}
					return 0;
				}
				g_mouse_event.button = button;
				g_mouse_event.column = values[1] > 0 ? values[1] - 1 : 0;
				g_mouse_event.row = values[2] > 0 ? values[2] - 1 : 0;
				g_mouse_event.press = action == 'M';
				return KEY_MOUSE;
			}
		}
		return 0;
	}
	if (sequence == "A") {
		return KEY_UP;
	}
	if (sequence == "B") {
		return KEY_DOWN;
	}
	if (sequence == "C") {
		return KEY_RIGHT;
	}
	if (sequence == "D") {
		return KEY_LEFT;
	}
	if (sequence == "H" || sequence == "1~" || sequence == "7~") {
		return KEY_HOME;
	}
	if (sequence == "F" || sequence == "4~" || sequence == "8~") {
		return KEY_END;
	}
	if (sequence == "3~") {
		return KEY_DELETE;
	}
	if (sequence == "5~") {
		return KEY_PGUP;
	}
	if (sequence == "6~") {
		return KEY_PGDN;
	}
	return 0;
}

//! Visible bytes of a rendered line plus the byte offset each one has in the
//! original string. ANSI escape sequences are skipped so searches only ever
//! match what the user can see.
struct PlainLine {
	std::string text;
	std::vector<std::size_t> offsets;
};

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

char LowerAsciiChar(char c) {
	if (c >= 'A' && c <= 'Z') {
		return static_cast<char>(c - 'A' + 'a');
	}
	return c;
}

std::vector<std::pair<std::size_t, std::size_t>> FindPlainMatches(const std::string &text, const std::string &query) {
	std::vector<std::pair<std::size_t, std::size_t>> matches;
	if (query.empty() || query.size() > text.size()) {
		return matches;
	}
	std::size_t position = 0;
	while (position + query.size() <= text.size()) {
		bool found = true;
		for (std::size_t i = 0; i < query.size(); i++) {
			if (LowerAsciiChar(text[position + i]) != LowerAsciiChar(query[i])) {
				found = false;
				break;
			}
		}
		if (found) {
			matches.emplace_back(position, position + query.size());
			position += query.size();
		} else {
			position++;
		}
	}
	return matches;
}

bool HasPlainMatch(const std::string &line, const std::string &query) {
	auto plain = MakePlainLine(line);
	return !FindPlainMatches(plain.text, query).empty();
}

std::string HighlightPlainMatches(const std::string &line, const PlainLine &plain,
                                  const std::vector<std::pair<std::size_t, std::size_t>> &matches) {
	if (matches.empty()) {
		return line;
	}
	std::string result;
	std::size_t copied = 0;
	for (auto &match : matches) {
		auto start = plain.offsets[match.first];
		auto end = plain.offsets[match.second - 1] + 1;
		// Keep any multi-byte UTF-8 character intact at the end of a match.
		while (end < line.size() && (static_cast<unsigned char>(line[end]) & 0xc0) == 0x80) {
			end++;
		}
		if (start < copied) {
			continue;
		}
		result.append(line, copied, start - copied);
		result += "\x1b[7m";
		result.append(line, start, end - start);
		result += "\x1b[27m";
		copied = end;
	}
	result.append(line, copied, std::string::npos);
	return result;
}

std::size_t FindMatchLine(const PagerPage &page, const std::string &query, std::size_t start) {
	if (query.empty() || page.content.empty()) {
		return page.content.size();
	}
	start = std::min(start, page.content.size());
	for (std::size_t i = start; i < page.content.size(); i++) {
		if (HasPlainMatch(page.content[i], query)) {
			return i;
		}
	}
	for (std::size_t i = 0; i < start; i++) {
		if (HasPlainMatch(page.content[i], query)) {
			return i;
		}
	}
	return page.content.size();
}

std::size_t FindPreviousMatchLine(const PagerPage &page, const std::string &query, std::size_t before) {
	if (query.empty() || page.content.empty()) {
		return page.content.size();
	}
	before = std::min(before, page.content.size());
	for (std::size_t i = before; i-- > 0;) {
		if (HasPlainMatch(page.content[i], query)) {
			return i;
		}
	}
	for (std::size_t i = page.content.size(); i-- > before;) {
		if (HasPlainMatch(page.content[i], query)) {
			return i;
		}
	}
	return page.content.size();
}

std::string MakeStatus(std::size_t index, std::size_t total, std::size_t top, std::size_t visible,
                       std::size_t count, int columns, const std::string &page_query, bool internal = false) {
	std::string status;
	if (!internal) {
		status += "Result " + std::to_string(index + 1) + "/" + std::to_string(total);
	}
	if (count > 0) {
		auto last = std::min(count, top + visible);
		if (!status.empty()) {
			status += "  |  ";
		}
		status += "Lines " + std::to_string(top + 1) + "-" + std::to_string(last) + "/" + std::to_string(count);
	}
	if (!page_query.empty()) {
		status += "  /" + page_query;
	}
	if (!internal) {
		status += "  |  ESC Search";
	}
	status += "  |  / Find-in-page";
	status += "  |  Up/Down scroll";
	if (!internal) {
		status += "  |  Left/Right result";
	} else {
		status += "  |  Esc back";
	}
	status += "  |  q quit";
	auto limit = static_cast<std::size_t>(std::max(columns - 1, 1));
	if (status.size() > limit) {
		status.resize(limit);
	}
	return status;
}

void Draw(const std::vector<PagerPage> &pages, std::size_t index, std::size_t &scroll, bool color,
          const std::string &page_query = std::string(), bool internal = false) {
	auto size = GetTerminalSize();
	int rows = std::max(size.rows, 3);
	int columns = std::max(size.columns, 20);

	auto &page = pages[index];
	auto header_rows = static_cast<int>(page.header.size());
	std::size_t available =
	    static_cast<std::size_t>(std::max(rows - header_rows - 1, 1));
	std::size_t visible = std::min(page.content.size(), available);
	auto max_scroll = page.content.size() > visible ? page.content.size() - visible : 0;
	if (scroll > max_scroll) {
		scroll = max_scroll;
	}

	std::string output;
	output += "\x1b[H";
	for (auto &line : page.header) {
		output += line;
		output += "\x1b[K\n";
	}

	// Fill the whole screen. When the page is shorter than the terminal the
	// remaining rows stay blank and the status line sits on the last row.
	for (std::size_t row = 0; row < available; row++) {
		if (scroll + row < page.content.size()) {
			auto &line = page.content[scroll + row];
			if (color && !page_query.empty()) {
				auto plain = MakePlainLine(line);
				auto matches = FindPlainMatches(plain.text, page_query);
				output += HighlightPlainMatches(line, plain, matches);
			} else {
				output += line;
			}
		}
		output += "\x1b[K\n";
	}

	if (color) {
		output += "\x1b[7m";
	}
	auto status = MakeStatus(index, pages.size(), scroll, visible, page.content.size(), columns, page_query, internal);
	auto status_limit = static_cast<std::size_t>(std::max(columns - 1, 1));
	if (status.size() < status_limit) {
		status.append(status_limit - status.size(), ' ');
	}
	output += status;
	if (color) {
		output += RESET;
	}
	output += "\x1b[K\x1b[?25l";
	std::cout << output << std::flush;
}

struct LiveSearchLayout {
	std::string correction;
	bool show_correction = false;
	std::size_t list_start = 1;
	std::size_t list_rows = 1;
};

LiveSearchLayout MakeLiveSearchLayout(const std::vector<SearchResult> &results, int terminal_rows) {
	LiveSearchLayout layout;
	if (!results.empty()) {
		layout.correction = results.front().corrected_query;
	}
	layout.show_correction = !layout.correction.empty() && terminal_rows >= 5;
	auto rows = std::max(terminal_rows, layout.show_correction ? 5 : 4);
	auto extra_rows = layout.show_correction ? 1 : 0;
	layout.list_start = 2 + extra_rows;
	layout.list_rows = static_cast<std::size_t>(std::max(rows - 3 - extra_rows, 1));
	return layout;
}

void DrawLiveSearch(const char *label, const std::string &query, std::size_t cursor,
                    const std::vector<SearchResult> &results, std::size_t selected, bool color,
                    const std::string &error, bool from_pager) {
	auto terminal = GetTerminalSize();
	int columns = std::max(terminal.columns, 20);
	auto width_limit = static_cast<std::size_t>(std::max(columns - 1, 1));

	std::string output = "\x1b[H";
	if (color) {
		output += BOLD;
	}
	output += label;
	output += "> ";
	if (color) {
		output += RESET;
	}

	auto prefix_width = cli::VisibleWidth(label) + 2;
	auto query_room = width_limit > prefix_width ? width_limit - prefix_width : 0;
	auto visible_query = FitQueryToWidth(query, cursor, query_room);
	output += visible_query.text;
	output += "\x1b[K\n";

	auto live_layout = MakeLiveSearchLayout(results, terminal.rows);
	if (live_layout.show_correction) {
		auto correction_line = TruncatePlain("corrected to: " + live_layout.correction, width_limit);
		output += color ? std::string(DIM) + correction_line + RESET : correction_line;
		output += "\x1b[K\n";
	}
	// Blank line between the query input (including any correction) and the
	// result list.
	output += "\x1b[K\n";

	std::size_t list_rows = live_layout.list_rows;
	std::size_t first = 0;
	if (selected >= list_rows) {
		first = selected - list_rows + 1;
	}
	auto entry_count = results.size() + 1;
	for (std::size_t row = 0; row < list_rows; row++) {
		auto position = first + row;
		std::string line;
		bool active = false;
		if (position < results.size()) {
			line = std::to_string(position + 1) + ". " + PlainTitle(results[position].title);
			line = TruncatePlain(line, width_limit);
			active = position == selected;
		} else if (position < entry_count) {
			line = std::to_string(position + 1) + ". Quit";
			active = position == selected;
		}
		if (active) {
			if (color) {
				output += "\x1b[7m";
			} else {
				line = "> " + line;
			}
		}
		output += line;
		if (active && color) {
			output += RESET;
		}
		output += "\x1b[K\n";
	}

	std::string status;
	if (!error.empty()) {
		status = "Error: " + error;
	} else if (query.empty()) {
		status = std::string("Type to search ") + label + "  |  Esc quit";
	} else if (results.empty()) {
		status = std::string("No ") + label + " results  |  Esc quit";
	} else {
		status = std::to_string(results.size()) + (results.size() == 1 ? " result" : " results") +
		         (from_pager ? "  |  Up/Down select  |  Enter apply  |  Esc quit"
		                     : "  |  Up/Down select  |  Enter open  |  Esc quit");
	}
	auto status_limit = static_cast<std::size_t>(std::max(columns - 1, 1));
	status = TruncatePlain(status, status_limit);
	if (status.size() < status_limit) {
		status.append(status_limit - status.size(), ' ');
	}
	if (color) {
		output += "\x1b[7m";
	}
	output += status;
	if (color) {
		output += RESET;
	}
	output += "\x1b[K";
	std::cout << output << std::flush;

	auto cursor_column = prefix_width + visible_query.cursor_width + 1;
	cursor_column = std::min<std::size_t>(cursor_column, static_cast<std::size_t>(columns));
	std::cout << "\x1b[1;" << cursor_column << "H\x1b[?25h" << std::flush;
}

void DrawFindPrompt(const std::string &query, std::size_t cursor, bool color) {
	auto size = GetTerminalSize();
	int rows = std::max(size.rows, 3);
	int columns = std::max(size.columns, 20);
	auto limit = static_cast<std::size_t>(std::max(columns - 1, 1));
	auto query_room = limit > 1 ? limit - 1 : 0;
	auto visible_query = FitQueryToWidth(query, cursor, query_room);

	std::string prompt = "/" + visible_query.text;

	std::string output = "\x1b[" + std::to_string(rows) + ";1H";
	if (color) {
		output += "\x1b[7m";
	}
	output += prompt;
	if (color) {
		output += RESET;
	}
	output += "\x1b[K";

	auto cursor_column =
	    std::min<std::size_t>(1 + visible_query.cursor_width + 1, static_cast<std::size_t>(columns));
	output += "\x1b[" + std::to_string(rows) + ";" + std::to_string(cursor_column) + "H\x1b[?25h";
	std::cout << output << std::flush;
}

} // namespace

namespace {

struct QueryEdit {
	bool accepted = false;
	std::string query;
	std::size_t cursor = 0;
	std::vector<SearchResult> results;
	std::size_t selected = 0;
};

struct FindEdit {
	bool accepted = false;
	std::string query;
	std::size_t cursor = 0;
};

std::size_t PreviousCodepoint(const std::string &text, std::size_t position) {
	if (position == 0) {
		return 0;
	}
	position--;
	while (position > 0 && (static_cast<unsigned char>(text[position]) & 0xc0) == 0x80) {
		position--;
	}
	return position;
}

std::size_t NextCodepoint(const std::string &text, std::size_t position) {
	if (position >= text.size()) {
		return text.size();
	}
	position++;
	while (position < text.size() && (static_cast<unsigned char>(text[position]) & 0xc0) == 0x80) {
		position++;
	}
	return position;
}

void EraseCodepointBefore(std::string &text, std::size_t &cursor) {
	if (cursor == 0) {
		return;
	}
	auto start = PreviousCodepoint(text, cursor);
	text.erase(start, cursor - start);
	cursor = start;
}

void EraseCodepointAt(std::string &text, std::size_t cursor) {
	if (cursor >= text.size()) {
		return;
	}
	auto end = NextCodepoint(text, cursor);
	text.erase(cursor, end - cursor);
}

void EraseWordBefore(std::string &text, std::size_t &cursor) {
	auto end = cursor;
	while (end > 0 && text[end - 1] == ' ') {
		end--;
	}
	auto start = end;
	while (start > 0 && text[start - 1] != ' ') {
		start = PreviousCodepoint(text, start);
	}
	if (start < cursor) {
		text.erase(start, cursor - start);
		cursor = start;
	}
}

FindEdit EditFind(const std::string &initial_query, bool color) {
	FindEdit edit;
	edit.query = initial_query;
	edit.cursor = edit.query.size();
	while (true) {
		DrawFindPrompt(edit.query, edit.cursor, color);
		int key = ReadKey();
		if (key < 0 || key == 3 || key == 27) {
			return edit;
		}
		if (key == '\r' || key == '\n') {
			edit.accepted = true;
			return edit;
		}
		if (key == KEY_LEFT) {
			edit.cursor = PreviousCodepoint(edit.query, edit.cursor);
			continue;
		}
		if (key == KEY_RIGHT) {
			edit.cursor = NextCodepoint(edit.query, edit.cursor);
			continue;
		}
		if (key == KEY_HOME || key == 1) {
			edit.cursor = 0;
			continue;
		}
		if (key == KEY_END || key == 5) {
			edit.cursor = edit.query.size();
			continue;
		}
		if (key == KEY_DELETE) {
			EraseCodepointAt(edit.query, edit.cursor);
			continue;
		}
		if (key == 127 || key == 8) {
			EraseCodepointBefore(edit.query, edit.cursor);
			continue;
		}
		if (key == 21) {
			edit.query.clear();
			edit.cursor = 0;
			continue;
		}
		if (key == 23) {
			EraseWordBefore(edit.query, edit.cursor);
			continue;
		}
		if (key >= 32 && key < 1000) {
			edit.query.insert(edit.cursor, 1, static_cast<char>(key));
			edit.cursor++;
		}
	}
}

QueryEdit EditQuery(const char *label, SearchFunction search, idx_t limit, const std::string &initial_query,
                    const std::vector<SearchResult> &initial_results, std::size_t initial_selected, bool color,
                    bool from_pager) {
	QueryEdit edit;
	edit.query = initial_query;
	edit.cursor = edit.query.size();
	edit.results = initial_results;
	// The last entry is always the "Quit" action, so the selection can point
	// one past the search results.
	edit.selected = std::min(initial_selected, edit.results.size());
	std::string error;
	// An empty search shows the latest blog posts, so the initial screen must
	// be populated even before the first keystroke.
	bool dirty = initial_results.empty();

	{
		PagerScreen screen(true);
		RawMode raw_mode;

		while (true) {
			if (dirty) {
				edit.results.clear();
				edit.selected = 0;
				error.clear();
				try {
					edit.results = search(edit.query, limit);
				} catch (const std::exception &exception) {
					error = exception.what();
				}
				dirty = false;
			}

			DrawLiveSearch(label, edit.query, edit.cursor, edit.results, edit.selected, color, error, from_pager);
			int key = ReadKey();
			if (key < 0) {
				break;
			}
			if (key == 3 || key == 4 || key == 27) {
				break;
			}
			if (key == KEY_MOUSE) {
				if (g_mouse_event.press && (g_mouse_event.button & 3) == 0 &&
				    (g_mouse_event.button & (32 | 64)) == 0) {
					auto terminal = GetTerminalSize();
					auto layout = MakeLiveSearchLayout(edit.results, terminal.rows);
					auto row = g_mouse_event.row;
					if (row >= static_cast<int>(layout.list_start) &&
					    row < static_cast<int>(layout.list_start + layout.list_rows)) {
						std::size_t first = 0;
						if (edit.selected >= layout.list_rows) {
							first = edit.selected - layout.list_rows + 1;
						}
						auto position =
						    first + static_cast<std::size_t>(row - static_cast<int>(layout.list_start));
						if (position < edit.results.size()) {
							edit.selected = position;
							edit.accepted = true;
							return edit;
						}
						if (position == edit.results.size()) {
							// The Quit row was clicked.
							return edit;
						}
					}
				}
				continue;
			}
			if (key == '\r' || key == '\n') {
				if (edit.selected == edit.results.size()) {
					// The Quit row was activated.
					break;
				}
				edit.accepted = true;
				return edit;
			}
			if (key == KEY_UP) {
				auto entry_count = edit.results.size() + 1;
				edit.selected = (edit.selected + entry_count - 1) % entry_count;
				continue;
			}
			if (key == KEY_DOWN) {
				auto entry_count = edit.results.size() + 1;
				edit.selected = (edit.selected + 1) % entry_count;
				continue;
			}
			if (key == KEY_LEFT) {
				edit.cursor = PreviousCodepoint(edit.query, edit.cursor);
				continue;
			}
			if (key == KEY_RIGHT) {
				edit.cursor = NextCodepoint(edit.query, edit.cursor);
				continue;
			}
			if (key == KEY_HOME || key == 1) {
				edit.cursor = 0;
				continue;
			}
			if (key == KEY_END || key == 5) {
				edit.cursor = edit.query.size();
				continue;
			}
			if (key == KEY_DELETE) {
				if (edit.cursor < edit.query.size()) {
					EraseCodepointAt(edit.query, edit.cursor);
					dirty = true;
				}
				continue;
			}
			if (key == 127 || key == 8) {
				if (edit.cursor > 0) {
					EraseCodepointBefore(edit.query, edit.cursor);
					dirty = true;
				}
				continue;
			}
			if (key == 21) {
				if (!edit.query.empty()) {
					edit.query.clear();
					edit.cursor = 0;
					dirty = true;
				}
				continue;
			}
			if (key == 23) {
				if (edit.cursor > 0) {
					EraseWordBefore(edit.query, edit.cursor);
					dirty = true;
				}
				continue;
			}
			if (key >= 32 && key < 1000) {
				edit.query.insert(edit.cursor, 1, static_cast<char>(key));
				edit.cursor++;
				dirty = true;
			}
		}
	}
	return edit;
}

//! Show a page opened from a link in the pager. Returns true when the user
//! asked to quit the whole application; Esc returns to the previous page.
bool RunInternalPage(const char *label, const SearchResult &result, bool color, bool raw) {
	std::vector<PagerPage> pages;
	auto size = GetTerminalSize();
	pages.push_back(BuildInternalPage(result, color, raw, size.columns));
	std::size_t scroll = 0;
	std::string page_query;
	Draw(pages, 0, scroll, color, page_query, true);

	while (true) {
		int key = ReadKey();
		bool redraw = true;
		auto terminal = GetTerminalSize();
		auto &page = pages[0];
		auto header_rows = static_cast<int>(page.header.size());
		auto available =
		    static_cast<std::size_t>(std::max(std::max(terminal.rows, 3) - header_rows - 1, 1));
		auto visible = std::min(page.content.size(), available);
		auto max_scroll = page.content.size() > visible ? page.content.size() - visible : 0;

		switch (key) {
		case 'q':
		case 'Q':
		case 3:
			return true;
		case 27:
			return false;
		case '/': {
			auto find = EditFind(std::string(), color);
			if (find.accepted) {
				std::size_t start = scroll;
				if (!find.query.empty()) {
					if (find.query != page_query) {
						page_query = find.query;
					} else {
						start = scroll + 1;
					}
				} else if (!page_query.empty()) {
					// An empty query repeats the previous search.
					start = scroll + 1;
				} else {
					redraw = false;
					break;
				}
				auto match = FindMatchLine(page, page_query, start);
				if (match < page.content.size()) {
					scroll = match;
				}
			}
			break;
		}
		case KEY_UP:
		case 'k':
			if (scroll > 0) {
				scroll--;
			}
			break;
		case KEY_DOWN:
		case 'j':
			if (scroll < max_scroll) {
				scroll++;
			}
			break;
		case KEY_PGUP:
		case 'b':
			scroll = scroll > visible ? scroll - visible : 0;
			break;
		case KEY_PGDN:
		case ' ':
			scroll = std::min(max_scroll, scroll + visible);
			break;
		case KEY_HOME:
		case 'g':
			scroll = 0;
			break;
		case KEY_END:
		case 'G':
			scroll = max_scroll;
			break;
		case 'n':
			if (!page_query.empty()) {
				auto match = FindMatchLine(page, page_query, scroll + 1);
				if (match < page.content.size()) {
					scroll = match;
				}
			} else {
				redraw = false;
			}
			break;
		case 'p':
			if (!page_query.empty()) {
				auto match = FindPreviousMatchLine(page, page_query, scroll);
				if (match < page.content.size()) {
					scroll = match;
				}
			} else {
				redraw = false;
			}
			break;
		case KEY_MOUSE: {
			if (!g_mouse_event.press || (g_mouse_event.button & 3) != 0 ||
			    (g_mouse_event.button & (32 | 64)) != 0) {
				redraw = false;
				break;
			}
			auto row = g_mouse_event.row;
			auto column = static_cast<std::size_t>(std::max(g_mouse_event.column, 0));
			if (row == 0) {
				// The Back label in the first header line.
				return false;
			}
			auto content_row = row - header_rows;
			if (content_row < 0 || static_cast<std::size_t>(content_row) >= available) {
				redraw = false;
				break;
			}
			auto line = scroll + static_cast<std::size_t>(content_row);
			if (line < page.content_links.size()) {
				for (auto &link : page.content_links[line]) {
					if (column >= link.start && column < link.end) {
						SearchResult target;
						if (LookupPageByUrl(link.url, target)) {
							if (RunInternalPage(label, target, color, raw)) {
								return true;
							}
						} else {
							OpenUrl(link.url);
						}
						break;
					}
				}
			}
			redraw = true;
			break;
		}
		default:
			redraw = false;
			break;
		}

		if (redraw) {
			Draw(pages, 0, scroll, color, page_query, true);
		}
	}
}

} // namespace

int RunPager(const char *label, SearchFunction search, idx_t limit, const std::string &query,
             const std::vector<SearchResult> &results, bool color, bool raw, std::size_t start_index) {
	std::string current_query = query;
	std::vector<SearchResult> current_results = results;
	std::size_t current_index = start_index;

	while (true) {
		if (current_results.empty()) {
			return 0;
		}

		bool edit_query = false;
		std::size_t resume_index = current_index;
		std::string page_query;

		{
			auto size = GetTerminalSize();
			std::vector<PagerPage> pages;
			pages.reserve(current_results.size());
			for (std::size_t i = 0; i < current_results.size(); i++) {
				pages.push_back(BuildPage(current_results, i, color, raw, size.columns));
			}

			PagerScreen screen(true);
			RawMode raw_mode;
			std::vector<std::size_t> positions(current_results.size(), 0);
			std::size_t index = std::min(current_index, current_results.size() - 1);
			resume_index = index;
			Draw(pages, index, positions[index], color, page_query);

			while (true) {
				int key = ReadKey();
				bool redraw = true;
				auto terminal = GetTerminalSize();
				auto &page = pages[index];
				auto header_rows = static_cast<int>(page.header.size());
				std::size_t available =
				    static_cast<std::size_t>(std::max(std::max(terminal.rows, 3) - header_rows - 1, 1));
				auto visible = std::min(page.content.size(), available);
				auto max_scroll = page.content.size() > visible ? page.content.size() - visible : 0;

				switch (key) {
				case 'q':
				case 'Q':
				case 3:
					return 0;
				case 27:
					if (search) {
						edit_query = true;
						resume_index = index;
					} else {
						redraw = false;
					}
					break;
				case '/': {
					auto find = EditFind(std::string(), color);
					if (find.accepted) {
						std::size_t start = positions[index];
						if (!find.query.empty()) {
							if (find.query != page_query) {
								page_query = find.query;
							} else {
								start = positions[index] + 1;
							}
						} else if (!page_query.empty()) {
							// An empty query repeats the previous search.
							start = positions[index] + 1;
						} else {
							redraw = false;
							break;
						}
						auto match = FindMatchLine(page, page_query, start);
						if (match < page.content.size()) {
							positions[index] = match;
						}
					}
					break;
				}
				case KEY_UP:
				case 'k':
					if (positions[index] > 0) {
						positions[index]--;
					}
					break;
				case KEY_DOWN:
				case 'j':
					if (positions[index] < max_scroll) {
						positions[index]++;
					}
					break;
				case KEY_PGUP:
				case 'b':
					positions[index] = positions[index] > visible ? positions[index] - visible : 0;
					break;
				case KEY_PGDN:
				case ' ':
					positions[index] = std::min(max_scroll, positions[index] + visible);
					break;
				case KEY_HOME:
				case 'g':
					positions[index] = 0;
					break;
				case KEY_END:
				case 'G':
					positions[index] = max_scroll;
					break;
				case KEY_LEFT:
					if (index == 0) {
						edit_query = true;
						resume_index = index;
					} else {
						index--;
					}
					break;
				case 'p':
					if (!page_query.empty()) {
						auto match = FindPreviousMatchLine(page, page_query, positions[index]);
						if (match < page.content.size()) {
							positions[index] = match;
						}
						break;
					}
					if (index == 0) {
						edit_query = true;
						resume_index = index;
					} else {
						index--;
					}
					break;
				case KEY_RIGHT:
					// Wrap around the result tabs: last -> first.
					index = (index + 1) % current_results.size();
					break;
				case '\r':
				case '\n':
					if (index + 1 >= current_results.size()) {
						edit_query = true;
						resume_index = index;
					} else {
						index++;
					}
					break;
				case 'n':
					if (!page_query.empty()) {
						auto match = FindMatchLine(page, page_query, positions[index] + 1);
						if (match < page.content.size()) {
							positions[index] = match;
						}
						break;
					}
					if (index + 1 >= current_results.size()) {
						edit_query = true;
						resume_index = index;
					} else {
						index++;
					}
					break;
				case KEY_MOUSE: {
					if (!g_mouse_event.press || (g_mouse_event.button & 3) != 0 ||
					    (g_mouse_event.button & (32 | 64)) != 0) {
						redraw = false;
						break;
					}
					auto row = g_mouse_event.row;
					auto column = static_cast<std::size_t>(std::max(g_mouse_event.column, 0));
					if (row == 0) {
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
								} else {
									redraw = false;
								}
							} else {
								index = tab.tab - 1;
							}
							break;
						}
						if (!hit) {
							redraw = false;
						}
						break;
					}
					// A click inside the content area opens the link under the
					// pointer, using the same coordinates as Draw().
					auto content_row = row - header_rows;
					if (content_row < 0 || static_cast<std::size_t>(content_row) >= available) {
						redraw = false;
						break;
					}
					auto line = positions[index] + static_cast<std::size_t>(content_row);
					bool opened = false;
					if (line < page.content_links.size()) {
						for (auto &link : page.content_links[line]) {
							if (column >= link.start && column < link.end) {
								opened = true;
								SearchResult target;
								if (LookupPageByUrl(link.url, target)) {
									if (RunInternalPage(label, target, color, raw)) {
										return 0;
									}
									redraw = true;
								} else {
									OpenUrl(link.url);
									redraw = false;
								}
								break;
							}
						}
					}
					if (!opened) {
						redraw = false;
					}
					break;
				}
				default:
					redraw = false;
					break;
				}

				if (edit_query) {
					break;
				}
				if (redraw) {
					Draw(pages, index, positions[index], color, page_query);
				}
			}
		}

		if (!edit_query) {
			return 0;
		}

		auto edit = EditQuery(label, search, limit, current_query, current_results, resume_index, color, true);
		if (!edit.accepted) {
			// Esc (or Ctrl-C) in the search prompt leaves the application.
			return 0;
		}
		current_query = edit.query;
		current_results = edit.results;
		current_index = edit.selected;
	}
}

int RunLiveSearch(const char *label, SearchFunction search, idx_t limit, bool color, bool raw) {
	auto edit = EditQuery(label, search, limit, std::string(), std::vector<SearchResult>(), 0, color, false);
	if (!edit.accepted || edit.results.empty()) {
		return 0;
	}
	return RunPager(label, search, limit, edit.query, edit.results, color, raw, edit.selected);
}

} // namespace docs
