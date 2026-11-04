#pragma once

#include "cli/clipboard.hpp"
#include "cli/markdown.hpp"
#include "docs_search.hpp"

#include <cstddef>
#include <iostream>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <vector>

namespace docs {

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
	KEY_WHEEL_UP,
	KEY_WHEEL_DOWN,
};

constexpr std::size_t WHEEL_SCROLL_LINES = 3;

extern MouseEvent g_mouse_event;

struct TerminalSize {
	int rows = 24;
	int columns = 80;
};

TerminalSize GetTerminalSize();

std::string PlainTitle(const std::string &title);
std::string TruncatePlain(const std::string &text, std::size_t max_width);

struct VisibleQuery {
	std::string text;
	//! Width of the part of the query before the cursor.
	std::size_t cursor_width = 0;
};

VisibleQuery FitQueryToWidth(const std::string &query, std::size_t cursor, std::size_t max_width);

void AppendPageContent(PagerPage &page, const SearchResult &result, bool color, bool raw, int columns);
void AppendPageHeader(PagerPage &page, bool color, int columns);

bool IsExternalLink(const std::string &url);
std::string ExternalLinkTarget(const std::string &url);
bool OpenUrl(const std::string &url);

class RawMode {
public:
	RawMode();
	~RawMode();

	RawMode(const RawMode &) = delete;
	RawMode &operator=(const RawMode &) = delete;

private:
	struct termios original_ = {};
	bool active_ = false;
};

class PagerScreen {
public:
	explicit PagerScreen(bool mouse = false);
	~PagerScreen();

	PagerScreen(const PagerScreen &) = delete;
	PagerScreen &operator=(const PagerScreen &) = delete;
};

bool WaitForInput(int milliseconds);
int ReadKey();

std::size_t PreviousCodepoint(const std::string &text, std::size_t position);
std::size_t NextCodepoint(const std::string &text, std::size_t position);

void Draw(const std::vector<PagerPage> &pages, std::size_t index, std::size_t &scroll, bool color,
          const std::string &page_query, bool internal, const std::string &custom_status,
          std::size_t selected_line, const cli::TextLink *selected_link, bool answer,
          std::size_t selected_toc, const TextSelection *selection);

bool RunInternalPage(const char *label, const SearchResult &result, bool color, bool raw,
                     bool *regenerate = nullptr);

} // namespace docs
