#include "cli/run.hpp"

#include "cli/markdown.hpp"
#include "cli/pager.hpp"
#include "docs_search.hpp"

#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

namespace docs {
namespace {

constexpr idx_t DEFAULT_LIMIT = 10;
constexpr idx_t MAX_LIMIT = 1000;

constexpr const char *RESET = "\x1b[0m";
constexpr const char *BOLD = "\x1b[1m";
constexpr const char *DIM = "\x1b[2m";
constexpr const char *HEADER_COLOR = "\x1b[1;38;5;39m";

enum class ColorMode { AUTO, ALWAYS, NEVER };

bool ParseLimit(const std::string &text, idx_t &result) {
	if (text.empty()) {
		return false;
	}
	idx_t value = 0;
	for (auto c : text) {
		if (c < '0' || c > '9') {
			return false;
		}
		value = value * 10 + static_cast<idx_t>(c - '0');
		if (value > MAX_LIMIT) {
			return false;
		}
	}
	result = value;
	return true;
}

void PrintUsage(const char *program) {
	std::cerr << "Usage: " << program << " [options] [query] [N]\n";
	std::cerr << "\n";
	std::cerr << "Run without a query on a terminal to open live search.\n";
	std::cerr << "By default, searches the documentation and the blog.\n";
	std::cerr << "\n";
	std::cerr << "Options:\n";
	std::cerr << "  -l, --limit N          Maximum number of results (default 10, max 1000)\n";
	std::cerr << "      --color[=WHEN]     Colorize output: auto (default), always, never\n";
	std::cerr << "      --no-color         Disable colors (same as --color=never)\n";
	std::cerr << "      --raw              Print the original Markdown without rendering\n";
	std::cerr << "      --no-pager         Print every result instead of paging interactively\n";
	std::cerr << "  -h, --help             Show this help\n";
}

bool ParseColorMode(const std::string &value, ColorMode &mode) {
	if (value == "auto") {
		mode = ColorMode::AUTO;
		return true;
	}
	if (value == "always" || value == "yes" || value == "on") {
		mode = ColorMode::ALWAYS;
		return true;
	}
	if (value == "never" || value == "no" || value == "off") {
		mode = ColorMode::NEVER;
		return true;
	}
	return false;
}

bool UseColor(ColorMode mode) {
	if (mode == ColorMode::NEVER) {
		return false;
	}
	if (mode == ColorMode::ALWAYS) {
		return true;
	}
	auto no_color = std::getenv("NO_COLOR");
	if (no_color && *no_color) {
		return false;
	}
	auto force = std::getenv("CLICOLOR_FORCE");
	if (force && *force && std::string(force) != "0") {
		return true;
	}
	return isatty(STDOUT_FILENO) != 0;
}

std::string Join(const std::vector<std::string> &parts) {
	std::string result;
	for (auto &part : parts) {
		if (!result.empty()) {
			result += ' ';
		}
		result += part;
	}
	return result;
}

} // namespace

int RunCli(const char *program, const SearchMode &mode, int argc, char **argv) {
	idx_t limit = DEFAULT_LIMIT;
	bool limit_set = false;
	bool raw = false;
	bool no_pager = false;
	ColorMode color_mode = ColorMode::AUTO;
	std::vector<std::string> positional;

	for (int i = 1; i < argc; i++) {
		std::string argument = argv[i];
		if (argument == "--") {
			for (i++; i < argc; i++) {
				positional.emplace_back(argv[i]);
			}
			break;
		}
		if (argument == "-h" || argument == "--help") {
			PrintUsage(program);
			return 0;
		}
		if (argument == "-l" || argument == "--limit") {
			if (i + 1 >= argc) {
				std::cerr << program << ": missing value for " << argument << "\n";
				return 2;
			}
			idx_t parsed = 0;
			if (!ParseLimit(argv[++i], parsed) || parsed == 0) {
				std::cerr << program << ": invalid limit '" << argv[i] << "'\n";
				return 2;
			}
			limit = parsed;
			limit_set = true;
			continue;
		}
		if (argument.compare(0, 8, "--limit=") == 0) {
			idx_t parsed = 0;
			auto value = argument.substr(8);
			if (!ParseLimit(value, parsed) || parsed == 0) {
				std::cerr << program << ": invalid limit '" << value << "'\n";
				return 2;
			}
			limit = parsed;
			limit_set = true;
			continue;
		}
		if (argument == "--color") {
			color_mode = ColorMode::ALWAYS;
			continue;
		}
		if (argument.compare(0, 8, "--color=") == 0) {
			auto value = argument.substr(8);
			if (!ParseColorMode(value, color_mode)) {
				std::cerr << program << ": invalid color mode '" << value << "'\n";
				return 2;
			}
			continue;
		}
		if (argument == "--no-color") {
			color_mode = ColorMode::NEVER;
			continue;
		}
		if (argument == "--raw" || argument == "--plain") {
			raw = true;
			continue;
		}
		if (argument == "--no-pager" || argument == "--all") {
			no_pager = true;
			continue;
		}
		positional.push_back(std::move(argument));
	}

	// Also accept "query" N as a shorthand for the limit.
	if (!limit_set && positional.size() >= 2) {
		idx_t parsed = 0;
		if (ParseLimit(positional.back(), parsed) && parsed > 0) {
			limit = parsed;
			positional.pop_back();
		}
	}

	auto query = Join(positional);
	bool color = UseColor(color_mode);
	auto label = mode.label;
	if (query.empty()) {
		if (!no_pager && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
			try {
				// Load the corpora and build the caches before taking over the
				// terminal, so errors and the first search are not delayed.
				(void)mode.search(std::string(), 1);
				PrepareSemanticSearch(std::string(), true);
			} catch (const std::exception &exception) {
				std::cerr << program << ": " << exception.what() << "\n";
				return 1;
			}
			return RunLiveSearch(label, mode.search, limit, color, raw);
		}
		PrintUsage(program);
		return 2;
	}

	// Long natural-language queries use the semantic index. Build it before
	// running the search so the progress indicator is visible from the start.
	try {
		PrepareSemanticSearch(query);
	} catch (const std::exception &exception) {
		std::cerr << program << ": semantic index unavailable: " << exception.what() << "\n";
	}

	std::vector<SearchResult> results;
	try {
		results = mode.search(query, limit);
	} catch (const std::exception &exception) {
		std::cerr << program << ": " << exception.what() << "\n";
		return 1;
	}

	if (results.empty()) {
		if (color) {
			std::cout << BOLD << "No " << label << " results for '" << query << "'." << RESET << "\n";
		} else {
			std::cout << "No " << label << " results for '" << query << "'.\n";
		}
		return 0;
	}

	bool interactive = !no_pager && results.size() > 1 && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
	if (interactive) {
		return RunPager(label, mode.search, limit, query, results, color, raw, 0);
	}

	const std::string rule(78, '-');
	for (idx_t i = 0; i < results.size(); i++) {
		auto &result = results[i];
		std::ostringstream details;
		details << "(" << (i + 1) << '/' << results.size() << ", score " << std::fixed << std::setprecision(2)
		        << result.score << ")";
		auto detail_text = details.str();
		if (color) {
			std::cout << HEADER_COLOR << label << RESET << "   " << BOLD << query << RESET << "   " << DIM
			          << detail_text << RESET << "\n";
			if (i == 0 && !result.corrected_query.empty()) {
				std::cout << DIM << "corrected to: " << result.corrected_query << RESET << "\n\n";
			}
			std::cout << DIM << rule << RESET << "\n";
		} else {
			std::cout << label << "   " << query << "   " << detail_text << "\n";
			if (i == 0 && !result.corrected_query.empty()) {
				std::cout << "corrected to: " << result.corrected_query << "\n\n";
			}
			std::cout << rule << "\n";
		}

		if (raw) {
			std::cout << result.content;
			if (result.content.empty() || result.content.back() != '\n') {
				std::cout << '\n';
			}
		} else {
			auto rendered = cli::RenderMarkdown(result.content, color);
			if (rendered.empty()) {
				std::cout << '\n';
			} else {
				std::cout << rendered;
			}
		}
		// Every page ends with an empty line, including the last one.
		std::cout << '\n';
	}
	return 0;
}

} // namespace docs
