#include "cli/pager_answer.hpp"

#include "cli/pager.hpp"
#include "cli/pager_internal.hpp"

#include "generation.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace docs {
namespace {

std::string ComposeLiveAnswer(const std::string &reasoning, const std::string &answer) {
	std::string content;
	if (!reasoning.empty()) {
		content += "### Reasoning\n\n";
		content += reasoning;
		if (content.back() != '\n') {
			content += '\n';
		}
		content += '\n';
	}
	content += "### Answer\n\n";
	if (answer.empty()) {
		content += "*Waiting for the answer...*";
	} else {
		content += answer;
	}
	return content;
}

std::string ComposeAnswerContent(const std::string &reasoning, const std::string &answer, bool cancelled,
                                 const std::string &error) {
	std::string content;
	if (!reasoning.empty()) {
		content += "### Reasoning\n\n";
		content += reasoning;
		if (content.back() != '\n') {
			content += '\n';
		}
		content += '\n';
	}
	content += "### Answer\n\n";
	if (!answer.empty()) {
		content += answer;
		if (content.back() != '\n') {
			content += '\n';
		}
	} else if (error.empty() && !cancelled) {
		content += "The model did not return an answer.\n";
	}
	if (cancelled) {
		content += "\n*[stopped]*\n";
	}
	if (!error.empty()) {
		content += "\n**Error:** " + error + "\n";
	}
	return content;
}

std::string EscapeMarkdownLinkText(const std::string &text) {
	std::string result;
	result.reserve(text.size());
	for (auto c : text) {
		if (c == '\\' || c == '[' || c == ']') {
			result.push_back('\\');
		}
		result.push_back(c);
	}
	return result;
}

void DrawAnswerLoading(const char *label, float progress, const std::string &message, bool color) {
	auto terminal = GetTerminalSize();
	int columns = std::max(terminal.columns, 20);
	auto width_limit = static_cast<std::size_t>(std::max(columns - 1, 1));
	auto percent = static_cast<int>(std::lround(std::max(0.0f, std::min(1.0f, progress)) * 100.0f));

	std::string output = "\x1b[H";
	if (color) {
		output += BOLD;
	}
	output += label;
	output += "> ";
	if (color) {
		output += RESET;
	}
	output += "Answer model";
	output += "\x1b[K\n";

	if (color) {
		output += DIM;
	}
	output += message;
	if (color) {
		output += RESET;
	}
	output += "\x1b[K\n";

	auto bar_width = static_cast<std::size_t>(std::max(10, std::min(44, columns - 12)));
	auto filled = static_cast<std::size_t>(percent) * bar_width / 100;
	std::string bar = "[";
	bar.append(filled, '#');
	bar.append(bar_width - filled, ' ');
	bar += "] " + std::to_string(percent) + "%";
	output += TruncatePlain(bar, width_limit);
	output += "\x1b[K\n\x1b[J";
	std::cout << output << std::flush;
}

struct BuiltAnswerPage {
	PagerPage page;
	std::size_t answer_lines = 0;
};

BuiltAnswerPage BuildAnswerPage(const std::string &reasoning, const std::string &answer, const std::string &query,
                                bool color, int columns) {
	auto width_limit = static_cast<std::size_t>(std::max(columns - 1, 1));
	auto title = PlainTitle(query);
	auto prefix = std::string("Answer | ");
	auto title_room = width_limit > prefix.size() ? width_limit - prefix.size() : 0;
	title = TruncatePlain(title, title_room);
	std::string label = "Answer";
	if (color) {
		label = std::string("\x1b[1;7m") + label + RESET;
	}
	PagerPage page;
	page.header.push_back(label + (title.empty() ? std::string() : " | " + title));
	AppendPageHeader(page, color, columns);
	SearchResult result;
	result.content = ComposeLiveAnswer(reasoning, answer);
	result.title = "Answer: " + query;
	AppendPageContent(page, result, color, false, columns);
	BuiltAnswerPage built;
	built.answer_lines = page.content.size();
	built.page = std::move(page);
	return built;
}

struct AnswerDrawResult {
	std::size_t header_rows = 0;
	std::size_t available = 0;
	std::size_t scroll = 0;
	std::size_t max_scroll = 0;
	std::size_t content_width = 0;
	bool has_toc = false;
	std::size_t toc_column = 0;
	std::vector<std::string> content;
	std::vector<std::vector<cli::TextLink>> content_links;
};

AnswerDrawResult DrawAnswerPage(const std::string &reasoning, const std::string &answer, const std::string &query,
                                bool color, const std::string &status = "Answering  |  Esc stop  |  Ctrl+c Quit",
                                const TextSelection *selection = nullptr) {
	auto size = GetTerminalSize();
	auto built = BuildAnswerPage(reasoning, answer, query, color, size.columns);
	auto header_rows = built.page.header.size();
	auto available = static_cast<std::size_t>(std::max(std::max(size.rows, 3) - static_cast<int>(header_rows) - 1, 1));
	auto visible = std::min(built.page.content.size(), available);
	std::size_t scroll = built.answer_lines > available ? built.answer_lines - available : 0;
	auto max_scroll = built.page.content.size() > visible ? built.page.content.size() - visible : 0;
	scroll = std::min(scroll, max_scroll);

	AnswerDrawResult result;
	result.header_rows = header_rows;
	result.available = available;
	result.scroll = scroll;
	result.max_scroll = max_scroll;
	result.content_width = built.page.content_width;
	result.has_toc = built.page.toc_width > 0 && !built.page.toc.empty();
	result.toc_column = built.page.toc_column;
	result.content = built.page.content;
	result.content_links = built.page.content_links;

	std::vector<PagerPage> pages;
	pages.push_back(std::move(built.page));
	Draw(pages, 0, scroll, color, std::string(), true, status, std::size_t(-1), nullptr, false,
	     std::size_t(-1), selection);
	return result;
}

struct SplitAnswerLayout {
	std::size_t list_rows = 1;
	std::size_t first = 0;
	std::size_t list_header_start = 2;
	std::size_t header_rows = 4;
};

SplitAnswerLayout MakeSplitAnswerLayout(std::size_t entry_count, std::size_t selected, int terminal_rows,
                                        bool show_correction) {
	SplitAnswerLayout layout;
	auto rows = std::max(terminal_rows, 8);
	auto correction_rows = show_correction ? 1 : 0;
	auto fixed = 4 + correction_rows;
	auto remaining = static_cast<std::size_t>(std::max(rows - fixed, 2));
	layout.list_rows = std::min(entry_count, std::max<std::size_t>(1, remaining / 2));
	if (layout.list_rows >= remaining) {
		layout.list_rows = remaining > 1 ? remaining - 1 : 1;
	}
	if (selected >= layout.list_rows) {
		layout.first = selected - layout.list_rows + 1;
	}
	layout.list_header_start = 2 + static_cast<std::size_t>(correction_rows);
	layout.header_rows = layout.list_header_start + layout.list_rows + 1;
	return layout;
}

struct SplitAnswerPage {
	PagerPage page;
	SplitAnswerLayout layout;
};

SplitAnswerPage BuildSplitAnswerPage(const char *label, const std::string &query,
                                     const std::vector<SearchResult> &results, std::size_t selected, bool color,
                                     const std::string &reasoning, const std::string &answer, bool cancelled,
                                     const std::string &error, bool final, int terminal_rows, int columns,
                                     const std::string &loading = std::string()) {
	auto show_correction = !results.empty() && !results.front().corrected_query.empty() && terminal_rows >= 6;
	SplitAnswerPage built;
	built.layout = MakeSplitAnswerLayout(results.size() + 2, selected, terminal_rows, show_correction);
	auto &page = built.page;

	auto width_limit = static_cast<std::size_t>(std::max(columns - 1, 1));
	auto label_text = std::string(label) + "> ";
	auto label_width = cli::VisibleWidth(label_text);
	auto query_room = width_limit > label_width ? width_limit - label_width : 0;
	auto visible_query = FitQueryToWidth(query, query.size(), query_room);
	if (color) {
		page.header.push_back(std::string(BOLD) + label + RESET + "> " + visible_query.text);
	} else {
		page.header.push_back(label_text + visible_query.text);
	}
	if (show_correction) {
		auto correction = TruncatePlain("corrected to: " + results.front().corrected_query, width_limit);
		page.header.push_back(color ? std::string(DIM) + correction + RESET : correction);
	}
	page.header.push_back(std::string());

	auto answer_index = results.size();
	auto quit_index = answer_index + 1;
	for (std::size_t row = 0; row < built.layout.list_rows; row++) {
		auto position = built.layout.first + row;
		std::string line;
		bool active = position == selected;
		if (position < results.size()) {
			line = std::to_string(position + 1) + ". " + PlainTitle(results[position].title);
		} else if (position == answer_index) {
			line = std::to_string(position + 1) + ". Answer with LLM";
		} else if (position == quit_index) {
			line = std::to_string(position + 1) + ". Quit";
		}
		line = TruncatePlain(line, width_limit);
		if (active) {
			if (color) {
				line.insert(0, "\x1b[7m");
				line += RESET;
			} else {
				line.insert(0, "> ");
			}
		}
		page.header.push_back(std::move(line));
	}
	auto rule = std::string(width_limit, '-');
	page.header.push_back(color ? std::string(DIM) + rule + RESET : rule);

	SearchResult body;
	if (!loading.empty()) {
		body.content = "\n" + loading + "\n";
	} else {
		body.content = final ? ComposeAnswerContent(reasoning, answer, cancelled, error)
		                     : ComposeLiveAnswer(reasoning, answer);
	}
	body.title = "Answer: " + query;
	AppendPageContent(page, body, color, false, columns);
	return built;
}

struct SplitAnswerView {
	std::size_t scroll = 0;
	bool follow_tail = true;
};

struct SplitDrawResult {
	SplitAnswerLayout layout;
	std::vector<std::vector<cli::TextLink>> content_links;
	std::size_t header_rows = 0;
	std::size_t available = 0;
	std::size_t max_scroll = 0;
	std::size_t content_width = 0;
	bool has_toc = false;
	std::size_t toc_column = 0;
	std::vector<std::string> content;
};

SplitDrawResult DrawSplitAnswerPage(const char *label, const std::string &query, std::size_t query_cursor,
                                    const std::vector<SearchResult> &results, std::size_t selected, bool color,
                                    const std::string &reasoning, const std::string &answer, bool cancelled,
                                    const std::string &error, bool final, SplitAnswerView &view,
                                    const std::string &status, const std::string &loading = std::string(),
                                    const TextSelection *selection = nullptr) {
	auto size = GetTerminalSize();
	auto rows = std::max(size.rows, 8);
	auto columns = std::max(size.columns, 20);
	auto built = BuildSplitAnswerPage(label, query, results, selected, color, reasoning, answer, cancelled, error,
	                                  final, rows, columns, loading);
	auto header_rows = built.page.header.size();
	auto available = static_cast<std::size_t>(std::max(rows - static_cast<int>(header_rows) - 1, 1));
	auto visible = std::min(built.page.content.size(), available);
	auto max_scroll = built.page.content.size() > visible ? built.page.content.size() - visible : 0;

	if (view.follow_tail) {
		view.scroll = max_scroll;
	}
	view.scroll = std::min(view.scroll, max_scroll);

	auto content_links = std::move(built.page.content_links);
	SplitDrawResult result;
	result.layout = built.layout;
	result.header_rows = header_rows;
	result.available = available;
	result.max_scroll = max_scroll;
	result.content_width = built.page.content_width;
	result.has_toc = built.page.toc_width > 0 && !built.page.toc.empty();
	result.toc_column = built.page.toc_column;
	result.content = built.page.content;
	std::vector<PagerPage> pages;
	pages.push_back(std::move(built.page));
	Draw(pages, 0, view.scroll, color, std::string(), true, status, std::size_t(-1), nullptr, false,
	     std::size_t(-1), selection);

	// Keep the cursor on the query line so the prompt stays editable.
	auto width_limit = static_cast<std::size_t>(std::max(columns - 1, 1));
	auto label_text = std::string(label) + "> ";
	auto label_width = cli::VisibleWidth(label_text);
	auto query_room = width_limit > label_width ? width_limit - label_width : 0;
	auto visible_query = FitQueryToWidth(query, query_cursor, query_room);
	auto cursor_column = std::min<std::size_t>(label_width + visible_query.cursor_width + 1,
	                                           static_cast<std::size_t>(columns));
	std::cout << "\x1b[1;" << cursor_column << "H\x1b[?25h" << std::flush;

	result.content_links = std::move(content_links);
	return result;
}

} // namespace

std::string BuildAnswerSources(const std::vector<SearchResult> &results) {
	std::string content = "### Sources\n\n";
	std::vector<std::string> seen;
	std::size_t shown = 0;
	for (auto &result : results) {
		if (!result.url.empty()) {
			if (std::find(seen.begin(), seen.end(), result.url) != seen.end()) {
				continue;
			}
			seen.push_back(result.url);
		}
		shown++;
		content += std::to_string(shown) + ". ";
		if (!result.url.empty()) {
			content += "[" + EscapeMarkdownLinkText(result.title) + "](" + result.url + ")";
		} else {
			content += result.title;
		}
		content += "\n";
	}
	if (shown == 0) {
		content += "*No source pages available.*\n";
	}
	return content;
}

void DrawSplitAnswerLoading(const char *label, const std::string &query, std::size_t query_cursor,
                            const std::vector<SearchResult> &results, std::size_t selected, bool color,
                            float progress, const std::string &message) {
	auto size = GetTerminalSize();
	auto columns = std::max(size.columns, 20);
	auto percent = static_cast<int>(std::lround(std::max(0.0f, std::min(1.0f, progress)) * 100.0f));
	auto bar_width = static_cast<std::size_t>(std::max(10, std::min(44, columns - 12)));
	auto filled = static_cast<std::size_t>(percent) * bar_width / 100;
	std::string bar = "[";
	bar.append(filled, '#');
	bar.append(bar_width - filled, ' ');
	bar += "] " + std::to_string(percent) + "%";

	SplitAnswerView view;
	DrawSplitAnswerPage(label, query, query_cursor, results, selected, color, std::string(), std::string(), false,
	                    std::string(), false, view, "Preparing the answer model  |  Please wait",
	                    message + "\n" + bar);
}

AnswerAction ShowAnswerWithResults(const char *label, const std::string &query,
                                   const std::vector<SearchResult> &results,
                                   const std::vector<SearchResult> &search_results, std::size_t &selected,
                                   std::size_t &query_cursor, bool color, int &pending_key) {
	std::string reasoning;
	std::string answer;
	std::string reasoning_pending;
	std::string answer_pending;
	bool answer_started = false;
	bool cancelled = false;
	std::string error;
	AnswerAction action = AnswerAction::None;
	bool stop = false;

	auto entry_count = search_results.size() + 2;
	auto answer_index = search_results.size();
	auto quit_index = answer_index + 1;
	selected = std::min(selected, entry_count - 1);

	SplitAnswerView view;
	SplitDrawResult current;
	PagerMouseState mouse;
	bool final = false;
	auto draw = [&](const std::string &status) {
		current = DrawSplitAnswerPage(label, query, query_cursor, search_results, selected, color, reasoning, answer,
		                              cancelled, error, final, view, status, std::string(), &mouse.selection);
	};

	auto open_link = [&](const cli::TextLink &link) {
		SearchResult target;
		auto url = ExternalLinkTarget(link.url);
		if (!IsExternalLink(link.url) && LookupPageByUrl(url, target)) {
			if (RunInternalPage(label, target, color, false)) {
				action = AnswerAction::Quit;
				stop = true;
			}
		} else {
			OpenUrl(url);
		}
	};

	auto scroll_answer = [&](int direction, std::size_t amount) {
		if (direction < 0) {
			view.scroll = view.scroll > amount ? view.scroll - amount : 0;
			view.follow_tail = false;
		} else {
			view.scroll = std::min(current.max_scroll, view.scroll + amount);
			if (view.scroll >= current.max_scroll) {
				view.follow_tail = true;
			}
		}
	};

	auto process_key = [&](int key, bool generating) -> bool {
		switch (key) {
		case 3:
			action = AnswerAction::Quit;
			stop = true;
			return false;
		case 27:
			if (generating) {
				cancelled = true;
			} else {
				action = AnswerAction::Quit;
			}
			stop = true;
			return false;
		case '\r':
		case '\n':
			if (selected == answer_index) {
				action = AnswerAction::Restart;
				stop = true;
				return false;
			}
			if (selected < search_results.size()) {
				action = AnswerAction::OpenResult;
				stop = true;
			} else if (selected >= quit_index) {
				action = AnswerAction::Quit;
				stop = true;
			}
			return false;
		case KEY_UP:
			selected = selected == 0 ? entry_count - 1 : selected - 1;
			return true;
		case KEY_DOWN:
			selected = (selected + 1) % entry_count;
			return true;
		case KEY_WHEEL_UP:
			scroll_answer(-1, WHEEL_SCROLL_LINES);
			return true;
		case KEY_WHEEL_DOWN:
			scroll_answer(1, WHEEL_SCROLL_LINES);
			return true;
		case KEY_PGUP:
			scroll_answer(-1, current.available);
			return true;
		case KEY_PGDN:
			scroll_answer(1, current.available);
			return true;
		case KEY_HOME:
			view.scroll = 0;
			view.follow_tail = false;
			return true;
		case KEY_END:
			view.scroll = current.max_scroll;
			view.follow_tail = true;
			return true;
		case KEY_RESIZE:
			return true;
		case KEY_LEFT:
			query_cursor = PreviousCodepoint(query, query_cursor);
			return true;
		case KEY_RIGHT:
			query_cursor = NextCodepoint(query, query_cursor);
			return true;
		case KEY_MOUSE: {
			PagerMouseContext context;
			context.header_rows = static_cast<int>(current.header_rows);
			context.available = current.available;
			context.scroll = view.scroll;
			context.content_rows = current.content.size();
			context.content_width = current.content_width;
			context.has_toc = current.has_toc;
			context.toc_column = current.toc_column;
			context.toc_top_margin = TOC_TOP_MARGIN;
			auto mouse_result = HandlePagerMouse(context, mouse, g_mouse_event);

			auto list_start = static_cast<int>(current.layout.list_header_start);
			auto list_end = list_start + static_cast<int>(current.layout.list_rows);
			switch (mouse_result.action) {
			case PagerMouseAction::Copy: {
				auto text = SelectionText(current.content, current.content_width, mouse.selection);
				if (!text.empty()) {
					SetClipboard(text);
				}
				return true;
			}
			case PagerMouseAction::Redraw:
				if (mouse.selection.dragging) {
					// Stop following the tail while the user is selecting text.
					view.follow_tail = false;
				}
				return true;
			case PagerMouseAction::Click: {
				if (mouse_result.click_row >= list_start && mouse_result.click_row < list_end) {
					auto position = current.layout.first +
					                static_cast<std::size_t>(mouse_result.click_row - list_start);
					if (position < entry_count) {
						selected = position;
						if (position < search_results.size()) {
							action = AnswerAction::OpenResult;
							stop = true;
						} else if (position >= quit_index) {
							action = AnswerAction::Quit;
							stop = true;
						}
						return true;
					}
					return false;
				}
				auto content_row = mouse_result.click_row - static_cast<int>(current.header_rows);
				if (content_row >= 0 && static_cast<std::size_t>(content_row) < current.available) {
					auto line = view.scroll + static_cast<std::size_t>(content_row);
					if (line < current.content_links.size()) {
						for (auto &link : current.content_links[line]) {
							if (mouse_result.click_column >= link.start &&
							    mouse_result.click_column < link.end) {
								open_link(link);
								return true;
							}
						}
					}
				}
				return true;
			}
			case PagerMouseAction::None:
				return false;
			}
			return false;
		}
		default:
			// Text editing returns to the prompt with the key replayed, so the
			// query stays editable while the result list is shown.
			if (key == 8 || key == 23 || key == KEY_DELETE ||
			    (key >= 32 && key < 1000)) {
				cancelled = cancelled || generating;
				pending_key = key;
				action = AnswerAction::EditQuery;
				stop = true;
			}
			return false;
		}
	};

	auto handle_pending_input = [&](const std::string &status, bool generating) -> bool {
		bool redraw = false;
		if (ResizePending()) {
			ClearResizePending();
			redraw = true;
		}
		while (!stop && WaitForInput(0)) {
			if (process_key(ReadKey(), generating)) {
				redraw = true;
			}
		}
		if (redraw && !stop) {
			draw(status);
		}
		return !stop;
	};

	draw("Reading the top 10 matching blocks  |  Please wait");
	try {
		GenerateAnswer(
		    query, results,
		    [&](const std::string &piece, bool is_reasoning) {
			    if (!is_reasoning && !answer_started) {
				    answer_started = true;
				    reasoning.clear();
				    reasoning_pending.clear();
			    }
			    auto &pending = is_reasoning ? reasoning_pending : answer_pending;
			    auto &target = is_reasoning ? reasoning : answer;
			    pending += piece;
			    auto complete = cli::Utf8CompleteLength(pending);
			    if (complete > 0) {
				    target.append(pending, 0, complete);
				    pending.erase(0, complete);
			    }
			    auto status = is_reasoning
			                      ? " Thinking  |  Up/Down  |  Enter restart  |  Esc stop"
			                      : "Answering  |  Up/Down  |  Enter restart  |  Esc stop";
			    if (!handle_pending_input(status, true)) {
				    return false;
			    }
			    if (complete > 0) {
				    draw(status);
			    }
			    return true;
		    },
		    [&](std::size_t done, std::size_t total) {
			    if (stop) {
				    return;
			    }
			    auto percent = total == 0 ? 100 : static_cast<int>((done * 100 + total - 1) / total);
			    constexpr std::size_t BAR_WIDTH = 20;
			    std::string bar(BAR_WIDTH, ' ');
			    std::fill_n(bar.begin(), static_cast<std::size_t>(percent) * BAR_WIDTH / 100, '#');
			    auto status = "Processing prompt [" + bar + "] " + std::to_string(percent) + "%";
			    if (!handle_pending_input(status, true)) {
				    return;
			    }
			    draw(status);
		    });
	} catch (const std::exception &exception) {
		error = exception.what();
	}

	if (!reasoning_pending.empty()) {
		reasoning += reasoning_pending;
	}
	if (!answer_pending.empty()) {
		answer += answer_pending;
	}
	if (error.empty() && !cancelled && answer.empty() && !reasoning.empty()) {
		answer = std::move(reasoning);
		reasoning.clear();
	}

	if (action == AnswerAction::Quit) {
		return AnswerAction::Quit;
	}
	if (action == AnswerAction::OpenResult) {
		return AnswerAction::OpenResult;
	}
	if (action == AnswerAction::EditQuery) {
		return AnswerAction::EditQuery;
	}
	if (action == AnswerAction::Restart) {
		return AnswerAction::Restart;
	}

	final = true;
	if (view.follow_tail) {
		view.follow_tail = false;
		view.scroll = 0;
	}
	auto status = "Answer  |  Enter restart  |  Up/Down  |  PgUp/PgDn scroll  |  Ctrl+c Quit";
	draw(status);
	while (true) {
		auto key = ReadKey();
		bool redraw = process_key(key, false);
		if (stop) {
			break;
		}
		if (redraw) {
			draw(status);
		}
	}

	if (action == AnswerAction::Quit) {
		return AnswerAction::Quit;
	}
	if (action == AnswerAction::OpenResult) {
		return AnswerAction::OpenResult;
	}
	if (action == AnswerAction::EditQuery) {
		return AnswerAction::EditQuery;
	}
	if (action == AnswerAction::Restart) {
		return AnswerAction::Restart;
	}
	return AnswerAction::Done;
}

bool ShowAnswer(const char *label, const std::string &query, const std::vector<SearchResult> &results, bool color) {
	std::string reasoning;
	std::string answer;
	std::string reasoning_pending;
	std::string answer_pending;
	bool answer_started = false;
	bool cancelled = false;
	bool quit = false;
	bool restart = false;
	std::string error;
	AnswerDrawResult current;
	PagerMouseState mouse;
	auto draw = [&](const std::string &status) {
		current = DrawAnswerPage(reasoning, answer, query, color, status, &mouse.selection);
	};

	auto handle_mouse = [&](int key, const std::string &status) -> bool {
		if (key != KEY_MOUSE) {
			return false;
		}
		PagerMouseContext context;
		context.header_rows = static_cast<int>(current.header_rows);
		context.available = current.available;
		context.scroll = current.scroll;
		context.content_rows = current.content.size();
		context.content_width = current.content_width;
		context.has_toc = current.has_toc;
		context.toc_column = current.toc_column;
		context.toc_top_margin = TOC_TOP_MARGIN;
		auto result = HandlePagerMouse(context, mouse, g_mouse_event);
		if (result.action == PagerMouseAction::None) {
			return false;
		}
		if (result.action == PagerMouseAction::Copy) {
			auto text = SelectionText(current.content, current.content_width, mouse.selection);
			if (!text.empty()) {
				SetClipboard(text);
			}
		}
		draw(status);
		return true;
	};

	while (true) {
		reasoning.clear();
		answer.clear();
		reasoning_pending.clear();
		answer_pending.clear();
		answer_started = false;
		cancelled = false;
		restart = false;
		error.clear();

		draw("Reading the top 10 matching blocks  |  Please wait");
		try {
			GenerateAnswer(query, results, [&](const std::string &piece, bool is_reasoning) {
				if (!is_reasoning && !answer_started) {
					// The model has stopped thinking. Drop the reasoning from the live
					// view so the answer starts at the top of the page.
					answer_started = true;
					reasoning.clear();
					reasoning_pending.clear();
				}
				auto &pending = is_reasoning ? reasoning_pending : answer_pending;
				auto &target = is_reasoning ? reasoning : answer;
				pending += piece;
				auto complete = cli::Utf8CompleteLength(pending);
				if (complete > 0) {
					target.append(pending, 0, complete);
					pending.erase(0, complete);
				}
				auto status = is_reasoning
				                  ? " Thinking  |  Esc stop  |  Enter restart  |  Ctrl+c Quit"
				                  : "Answering  |  Esc stop  |  Enter restart  |  Ctrl+c Quit";
				if (ResizePending()) {
					ClearResizePending();
					draw(status);
				}
				while (WaitForInput(0)) {
					auto key = ReadKey();
					if (handle_mouse(key, status)) {
						continue;
					}
					if (key == 27) {
						cancelled = true;
						return false;
					}
					if (key == 3) {
						quit = true;
						return false;
					}
					if (key == '\r' || key == '\n') {
						restart = true;
						return false;
					}
				}
				if (complete > 0) {
					draw(status);
				}
				return true; }, [&](std::size_t done, std::size_t total) {
				auto percent = total == 0 ? 100 : static_cast<int>((done * 100 + total - 1) / total);
				constexpr std::size_t BAR_WIDTH = 20;
				std::string bar(BAR_WIDTH, ' ');
				std::fill_n(bar.begin(), static_cast<std::size_t>(percent) * BAR_WIDTH / 100, '#');
				auto status = "Processing prompt [" + bar + "] " + std::to_string(percent) + "%";
				draw(status); });
		} catch (const std::exception &exception) {
			error = exception.what();
		}

		if (quit) {
			return true;
		}
		if (restart) {
			continue;
		}
		if (!reasoning_pending.empty()) {
			reasoning += reasoning_pending;
		}
		if (!answer_pending.empty()) {
			answer += answer_pending;
		}
		if (error.empty() && !cancelled && answer.empty() && !reasoning.empty()) {
			// The model stopped without a thinking end tag, so the text it produced
			// is the answer rather than a separate reasoning block.
			answer = std::move(reasoning);
			reasoning.clear();
		}

		auto content = ComposeAnswerContent(reasoning, answer, cancelled, error);

		SearchResult result;
		result.content = content;
		result.title = "Answer: " + query;
		bool regenerate = false;
		auto done = RunInternalPage(label, result, color, false, &regenerate);
		if (!regenerate) {
			return done;
		}
	}
}

bool RunAnswer(const char *label, const std::string &query, const std::vector<SearchResult> &results, bool color) {
	PagerScreen screen(true);
	RawMode raw_mode;
	if (!AnswerModelLoaded()) {
		int last_percent = -1;
		DrawAnswerLoading(label, 0.0f, "Preparing the answer model...", color);
		PrepareAnswerModel([&](float progress, const std::string &status) {
			auto percent = static_cast<int>(std::lround(progress * 100.0f));
			if (percent != last_percent) {
				last_percent = percent;
				DrawAnswerLoading(label, progress, status, color);
			}
		});
	}
	return ShowAnswer(label, query, results, color);
}

} // namespace docs
