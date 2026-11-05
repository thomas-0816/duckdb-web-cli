#include "cli/markdown.hpp"

#include "md4c.h"

#include <algorithm>
#include <cctype>
#include <clocale>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <wchar.h>

namespace docs {
namespace cli {
namespace {

constexpr const char *RESET = "\x1b[0m";
constexpr const char *BOLD = "\x1b[1m";
constexpr const char *DIM = "\x1b[2m";
constexpr const char *ITALIC = "\x1b[3m";
constexpr const char *UNDERLINE = "\x1b[4m";
constexpr const char *STRIKE = "\x1b[9m";

constexpr int INTERNAL_LINK_COLOR = 75;
constexpr int EXTERNAL_LINK_COLOR = 208;

constexpr std::size_t MAX_CELL_WIDTH = 48;

struct Style {
	bool bold = false;
	bool italic = false;
	bool underline = false;
	bool strike = false;
	bool dim = false;
	int fg = -1;
	int bg = -1;
};

struct Cell {
	std::string text;
	MD_ALIGN align = MD_ALIGN_DEFAULT;
};

struct TableRow {
	std::vector<Cell> cells;
	bool header = false;
};

struct ListPart {
	std::string marker;
	std::string continuation;
	bool pending = true;
};

struct ListState {
	bool ordered = false;
	bool tight = true;
	std::uint64_t counter = 1;
};

std::string Spaces(std::size_t count) {
	return std::string(count, ' ');
}

bool IsExternalLink(const std::string &href) {
	auto scheme_end = href.find("://");
	std::size_t host_start = 0;
	if (scheme_end != std::string::npos) {
		auto scheme = href.substr(0, scheme_end);
		for (auto &c : scheme) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		if (scheme != "http" && scheme != "https") {
			return true;
		}
		host_start = scheme_end + 3;
	} else if (href.compare(0, 2, "//") == 0) {
		host_start = 2;
	} else {
		// Relative paths and bare fragments stay inside the documentation.
		auto colon = href.find(':');
		auto slash = href.find('/');
		return colon != std::string::npos && (slash == std::string::npos || colon < slash);
	}

	auto host_end = href.find_first_of("/?#", host_start);
	auto authority = href.substr(host_start, host_end == std::string::npos ? std::string::npos : host_end - host_start);
	auto user_info = authority.rfind('@');
	if (user_info != std::string::npos) {
		authority = authority.substr(user_info + 1);
	}
	auto port = authority.rfind(':');
	if (port != std::string::npos && authority.find(']') == std::string::npos) {
		authority.resize(port);
	}
	for (auto &c : authority) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return authority != "duckdb.org" && authority != "www.duckdb.org";
}

bool IsEscape(const std::string &text, std::size_t pos) {
	return pos < text.size() && static_cast<unsigned char>(text[pos]) == 0x1b;
}

//! Return the position just after an ANSI escape sequence starting at `pos`.
std::size_t SkipEscape(const std::string &text, std::size_t pos) {
	std::size_t i = pos + 1;
	if (i >= text.size()) {
		return i;
	}
	if (text[i] == '[') {
		i++;
		while (i < text.size()) {
			auto c = static_cast<unsigned char>(text[i]);
			i++;
			if (c >= 0x40 && c <= 0x7e) {
				break;
			}
		}
		return i;
	}
	if (text[i] == ']') {
		i++;
		while (i < text.size()) {
			if (text[i] == '\a') {
				return i + 1;
			}
			if (text[i] == 0x1b && i + 1 < text.size() && text[i + 1] == '\\') {
				return i + 2;
			}
			i++;
		}
		return i;
	}
	return std::min(i + 1, text.size());
}

std::size_t DecodeUtf8(const std::string &text, std::size_t pos, unsigned int &codepoint) {
	auto c = static_cast<unsigned char>(text[pos]);
	if (c < 0x80) {
		codepoint = c;
		return 1;
	}
	std::size_t length = 0;
	if ((c & 0xe0) == 0xc0) {
		length = 2;
		codepoint = c & 0x1f;
	} else if ((c & 0xf0) == 0xe0) {
		length = 3;
		codepoint = c & 0x0f;
	} else if ((c & 0xf8) == 0xf0) {
		length = 4;
		codepoint = c & 0x07;
	} else {
		codepoint = 0xfffd;
		return 1;
	}
	if (pos + length > text.size()) {
		codepoint = 0xfffd;
		return 1;
	}
	for (std::size_t i = 1; i < length; i++) {
		auto next = static_cast<unsigned char>(text[pos + i]);
		if ((next & 0xc0) != 0x80) {
			codepoint = 0xfffd;
			return 1;
		}
		codepoint = (codepoint << 6) | (next & 0x3f);
	}
	return length;
}

std::size_t DisplayWidth(const std::string &text) {
	std::size_t width = 0;
	std::size_t i = 0;
	while (i < text.size()) {
		if (IsEscape(text, i)) {
			i = SkipEscape(text, i);
			continue;
		}
		auto c = static_cast<unsigned char>(text[i]);
		if (c < 0x20 || c == 0x7f) {
			i++;
			continue;
		}
		unsigned int codepoint = 0;
		auto length = DecodeUtf8(text, i, codepoint);
		int cell_width = wcwidth(static_cast<wchar_t>(codepoint));
		width += cell_width > 0 ? static_cast<std::size_t>(cell_width) : 1;
		i += length;
	}
	return width;
}

std::string TruncateVisible(const std::string &text, std::size_t max_width, bool color) {
	if (DisplayWidth(text) <= max_width) {
		return text;
	}
	std::string result;
	std::size_t width = 0;
	std::size_t i = 0;
	while (i < text.size()) {
		if (IsEscape(text, i)) {
			auto next = SkipEscape(text, i);
			result.append(text, i, next - i);
			i = next;
			continue;
		}
		unsigned int codepoint = 0;
		auto length = DecodeUtf8(text, i, codepoint);
		int cell_width = wcwidth(static_cast<wchar_t>(codepoint));
		auto add = cell_width > 0 ? static_cast<std::size_t>(cell_width) : 1;
		if (width + add + 1 > max_width) {
			break;
		}
		result.append(text, i, length);
		width += add;
		i += length;
	}
	result += "\xe2\x80\xa6";
	if (color) {
		result += RESET;
	}
	return result;
}

void AppendUtf8(std::string &out, unsigned int codepoint) {
	if (codepoint <= 0x7f) {
		out += static_cast<char>(codepoint);
	} else if (codepoint <= 0x7ff) {
		out += static_cast<char>(0xc0 | (codepoint >> 6));
		out += static_cast<char>(0x80 | (codepoint & 0x3f));
	} else if (codepoint <= 0xffff) {
		out += static_cast<char>(0xe0 | (codepoint >> 12));
		out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
		out += static_cast<char>(0x80 | (codepoint & 0x3f));
	} else {
		out += static_cast<char>(0xf0 | (codepoint >> 18));
		out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
		out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
		out += static_cast<char>(0x80 | (codepoint & 0x3f));
	}
}

std::string DecodeEntity(const std::string &entity) {
	if (entity.size() < 3 || entity.front() != '&' || entity.back() != ';') {
		return entity;
	}
	auto body = entity.substr(1, entity.size() - 2);
	if (!body.empty() && body.front() == '#') {
		bool hex = body.size() > 1 && (body[1] == 'x' || body[1] == 'X');
		auto digits = body.substr(hex ? 2 : 1);
		if (digits.empty()) {
			return entity;
		}
		unsigned int value = 0;
		for (auto c : digits) {
			unsigned int digit;
			if (c >= '0' && c <= '9') {
				digit = static_cast<unsigned int>(c - '0');
			} else if (hex && c >= 'a' && c <= 'f') {
				digit = static_cast<unsigned int>(c - 'a' + 10);
			} else if (hex && c >= 'A' && c <= 'F') {
				digit = static_cast<unsigned int>(c - 'A' + 10);
			} else {
				return entity;
			}
			value = value * (hex ? 16 : 10) + digit;
			if (value > 0x10ffff) {
				return entity;
			}
		}
		std::string result;
		AppendUtf8(result, value);
		return result;
	}

	if (body == "amp") {
		return "&";
	}
	if (body == "lt") {
		return "<";
	}
	if (body == "gt") {
		return ">";
	}
	if (body == "quot") {
		return "\"";
	}
	if (body == "apos") {
		return "'";
	}
	if (body == "nbsp") {
		return "\xc2\xa0";
	}
	if (body == "ndash") {
		return "\xe2\x80\x93";
	}
	if (body == "mdash") {
		return "\xe2\x80\x94";
	}
	if (body == "hellip") {
		return "\xe2\x80\xa6";
	}
	if (body == "rarr") {
		return "\xe2\x86\x92";
	}
	if (body == "larr") {
		return "\xe2\x86\x90";
	}
	if (body == "times") {
		return "\xc3\x97";
	}
	if (body == "copy") {
		return "\xc2\xa9";
	}
	if (body == "reg") {
		return "\xc2\xae";
	}
	if (body == "trade") {
		return "\xe2\x84\xa2";
	}
	return entity;
}

void StripHeadingAnchor(std::string &heading) {
	auto marker = heading.rfind(" {#");
	if (marker == std::string::npos) {
		return;
	}
	auto close = heading.find('}', marker);
	if (close == std::string::npos) {
		return;
	}
	heading.erase(marker, close - marker + 1);
	while (!heading.empty() && (heading.back() == ' ' || heading.back() == '\t')) {
		heading.pop_back();
	}
}

std::string PrepareMarkdown(const std::string &markdown) {
	std::string result;
	result.reserve(markdown.size());
	bool in_fence = false;
	char fence_char = 0;
	std::size_t fence_length = 0;
	std::size_t pos = 0;
	while (pos < markdown.size()) {
		auto end = markdown.find('\n', pos);
		if (end == std::string::npos) {
			end = markdown.size();
		}
		std::string line = markdown.substr(pos, end - pos);
		std::size_t indent = 0;
		while (indent < line.size() && indent < 4 && line[indent] == ' ') {
			indent++;
		}

		std::size_t run = indent;
		while (run < line.size() && (line[run] == '`' || line[run] == '~')) {
			run++;
		}
		bool is_fence = run - indent >= 3 && (run == line.size() || line[run] == ' ');
		if (is_fence) {
			char marker = line[indent];
			if (!in_fence) {
				in_fence = true;
				fence_char = marker;
				fence_length = run - indent;
			} else if (marker == fence_char && run - indent >= fence_length) {
				in_fence = false;
			}
		} else if (!in_fence && indent < line.size() && line[indent] == '#') {
			StripHeadingAnchor(line);
		}

		result += line;
		if (end < markdown.size()) {
			result += '\n';
		}
		pos = end + 1;
	}
	return result;
}

std::string LowerAscii(const std::string &text) {
	std::string result = text;
	for (auto &c : result) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return result;
}

bool IsSqlWordStart(unsigned char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}

bool IsSqlWordChar(unsigned char c) {
	return IsSqlWordStart(c) || (c >= '0' && c <= '9');
}

bool IsSqlKeyword(const std::string &word) {
	static const std::unordered_set<std::string> keywords = {
	    "abort",
	    "all",
	    "alter",
	    "analyze",
	    "and",
	    "anti",
	    "any",
	    "as",
	    "asc",
	    "asof",
	    "attach",
	    "authorization",
	    "begin",
	    "between",
	    "by",
	    "call",
	    "case",
	    "cast",
	    "check",
	    "checkpoint",
	    "collate",
	    "commit",
	    "conflict",
	    "constraint",
	    "copy",
	    "create",
	    "cross",
	    "current",
	    "database",
	    "default",
	    "delete",
	    "desc",
	    "describe",
	    "detach",
	    "distinct",
	    "do",
	    "drop",
	    "else",
	    "end",
	    "escape",
	    "except",
	    "exclude",
	    "exists",
	    "explain",
	    "export",
	    "extension",
	    "false",
	    "fetch",
	    "filter",
	    "first",
	    "following",
	    "for",
	    "force",
	    "foreign",
	    "from",
	    "full",
	    "glob",
	    "grant",
	    "group",
	    "groups",
	    "having",
	    "if",
	    "ilike",
	    "import",
	    "in",
	    "index",
	    "inner",
	    "insert",
	    "install",
	    "intersect",
	    "into",
	    "is",
	    "join",
	    "key",
	    "last",
	    "lateral",
	    "left",
	    "like",
	    "limit",
	    "load",
	    "local",
	    "locked",
	    "macro",
	    "map",
	    "materialized",
	    "natural",
	    "next",
	    "not",
	    "nothing",
	    "nowait",
	    "null",
	    "nulls",
	    "of",
	    "offset",
	    "on",
	    "only",
	    "or",
	    "order",
	    "outer",
	    "over",
	    "partition",
	    "password",
	    "persistent",
	    "pivot",
	    "positional",
	    "pragma",
	    "preceding",
	    "primary",
	    "qualify",
	    "range",
	    "recursive",
	    "references",
	    "replace",
	    "reset",
	    "returning",
	    "revoke",
	    "right",
	    "rollback",
	    "row",
	    "rows",
	    "sample",
	    "schema",
	    "secret",
	    "select",
	    "semi",
	    "sequence",
	    "session",
	    "set",
	    "show",
	    "similar",
	    "skip",
	    "some",
	    "start",
	    "struct",
	    "summarize",
	    "table",
	    "tablesample",
	    "temp",
	    "temporary",
	    "then",
	    "to",
	    "transaction",
	    "trigger",
	    "true",
	    "truncate",
	    "try_cast",
	    "type",
	    "unbounded",
	    "union",
	    "unique",
	    "unknown",
	    "uninstall",
	    "unpivot",
	    "update",
	    "use",
	    "using",
	    "vacuum",
	    "values",
	    "view",
	    "when",
	    "where",
	    "window",
	    "with",
	    "zone",
	};
	return keywords.find(word) != keywords.end();
}

bool IsSqlType(const std::string &word) {
	static const std::unordered_set<std::string> types = {
	    "bigint",
	    "bit",
	    "blob",
	    "bool",
	    "boolean",
	    "bpchar",
	    "bytea",
	    "char",
	    "date",
	    "datetime",
	    "dec",
	    "decimal",
	    "double",
	    "enum",
	    "float",
	    "float4",
	    "float8",
	    "hugeint",
	    "int",
	    "int1",
	    "int2",
	    "int4",
	    "int8",
	    "integer",
	    "int128",
	    "interval",
	    "json",
	    "list",
	    "long",
	    "mediumint",
	    "numeric",
	    "real",
	    "serial",
	    "short",
	    "smallint",
	    "string",
	    "text",
	    "time",
	    "timestamp",
	    "timestamptz",
	    "timetz",
	    "tinyint",
	    "ubigint",
	    "uinteger",
	    "uhugeint",
	    "union",
	    "usmallint",
	    "utinyint",
	    "uuid",
	    "varbinary",
	    "varchar",
	    "variant",
	    "void",
	};
	return types.find(word) != types.end();
}

bool IsSqlNameContext(const std::string &word) {
	// After these keywords a parenthesised word is a table or type name, not a
	// function call.
	static const std::unordered_set<std::string> contexts = {
	    "table",
	    "view",
	    "index",
	    "type",
	    "macro",
	    "sequence",
	    "schema",
	    "into",
	    "function",
	};
	return contexts.find(word) != contexts.end();
}

class Renderer {
  public:
	explicit Renderer(bool color, std::vector<MarkdownHeading> *headings = nullptr,
	                  ExternalLinkPredicate is_external = ExternalLinkPredicate())
	    : color_(color), headings_(headings), is_external_(std::move(is_external)) {
		styles_.push_back(Style{});
	}

	std::string Render(const std::string &markdown) {
		std::setlocale(LC_CTYPE, "");
		auto prepared = PrepareMarkdown(markdown);
		MD_PARSER parser = {};
		parser.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS | MD_FLAG_UNDERLINE;
		parser.enter_block = [](MD_BLOCKTYPE type, void *detail, void *userdata) {
			return static_cast<Renderer *>(userdata)->EnterBlock(type, detail);
		};
		parser.leave_block = [](MD_BLOCKTYPE type, void *detail, void *userdata) {
			return static_cast<Renderer *>(userdata)->LeaveBlock(type, detail);
		};
		parser.enter_span = [](MD_SPANTYPE type, void *detail, void *userdata) {
			return static_cast<Renderer *>(userdata)->EnterSpan(type, detail);
		};
		parser.leave_span = [](MD_SPANTYPE type, void *detail, void *userdata) {
			return static_cast<Renderer *>(userdata)->LeaveSpan(type, detail);
		};
		parser.text = [](MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *userdata) {
			return static_cast<Renderer *>(userdata)->Text(type, text, size);
		};

		if (md_parse(prepared.data(), static_cast<MD_SIZE>(prepared.size()), &parser, this) != 0) {
			// Parsing is best effort: return whatever was rendered so far.
		}
		if (!at_line_start_) {
			body_ += '\n';
		}
		while (!body_.empty() && body_.back() == '\n') {
			body_.pop_back();
		}
		if (!body_.empty()) {
			body_ += '\n';
		}
		return body_;
	}

	int EnterBlock(MD_BLOCKTYPE type, void *detail) {
		switch (type) {
		case MD_BLOCK_DOC:
			at_line_start_ = true;
			break;
		case MD_BLOCK_QUOTE:
			if (quote_depth_ == 0) {
				BeginBlock();
			}
			quote_depth_++;
			at_line_start_ = true;
			break;
		case MD_BLOCK_UL: {
			auto *info = static_cast<MD_BLOCK_UL_DETAIL *>(detail);
			if (list_depth_ == 0) {
				BeginBlock();
			}
			ListState state;
			state.ordered = false;
			state.tight = !info || info->is_tight;
			lists_.push_back(state);
			list_depth_++;
			break;
		}
		case MD_BLOCK_OL: {
			auto *info = static_cast<MD_BLOCK_OL_DETAIL *>(detail);
			if (list_depth_ == 0) {
				BeginBlock();
			}
			ListState state;
			state.ordered = true;
			state.tight = !info || info->is_tight;
			state.counter = info ? info->start : 1;
			lists_.push_back(state);
			list_depth_++;
			break;
		}
		case MD_BLOCK_LI:
			EnterListItem(detail);
			break;
		case MD_BLOCK_HR:
			BeginBlock();
			StartLine();
			EmitColor(240);
			for (int i = 0; i < 60; i++) {
				AppendRaw("\xe2\x94\x80");
			}
			EmitReset();
			NewLine();
			FinishBlock();
			break;
		case MD_BLOCK_H:
			EnterHeading(detail);
			break;
		case MD_BLOCK_CODE:
			BeginBlock();
			in_code_ = true;
			code_line_started_ = false;
			code_sql_ = IsSqlFence(detail);
			code_buffer_.clear();
			at_line_start_ = true;
			break;
		case MD_BLOCK_HTML:
			BeginBlock();
			in_html_ = true;
			in_html_tag_ = false;
			break;
		case MD_BLOCK_P:
			if (!in_table_ && list_depth_ == 0 && quote_depth_ == 0) {
				BeginBlock();
			}
			break;
		case MD_BLOCK_TABLE:
			EnterTable(detail);
			break;
		case MD_BLOCK_THEAD:
			in_thead_ = true;
			break;
		case MD_BLOCK_TBODY:
			in_thead_ = false;
			break;
		case MD_BLOCK_TR:
			current_row_.clear();
			break;
		case MD_BLOCK_TH:
			EnterCell(detail, true);
			break;
		case MD_BLOCK_TD:
			EnterCell(detail, false);
			break;
		}
		return 0;
	}

	int LeaveBlock(MD_BLOCKTYPE type, void *detail) {
		(void)detail;
		switch (type) {
		case MD_BLOCK_QUOTE:
			quote_depth_--;
			if (!at_line_start_) {
				NewLine();
			}
			if (quote_depth_ == 0) {
				FinishBlock();
			}
			break;
		case MD_BLOCK_UL:
		case MD_BLOCK_OL:
			if (!at_line_start_) {
				NewLine();
			}
			if (list_depth_ > 0) {
				list_depth_--;
			}
			if (!lists_.empty()) {
				lists_.pop_back();
			}
			if (list_depth_ == 0) {
				FinishBlock();
			}
			break;
		case MD_BLOCK_LI:
			if (!at_line_start_) {
				NewLine();
			}
			if (!indents_.empty()) {
				indents_.pop_back();
			}
			at_line_start_ = true;
			break;
		case MD_BLOCK_H:
			LeaveHeading();
			break;
		case MD_BLOCK_CODE:
			if (code_sql_) {
				if (!at_line_start_) {
					EmitReset();
					NewLine();
				}
				WriteSqlBlock(code_buffer_);
				code_buffer_.clear();
				code_sql_ = false;
			} else {
				if (code_line_started_) {
					EmitReset();
					code_line_started_ = false;
				}
				if (!at_line_start_) {
					NewLine();
				}
			}
			in_code_ = false;
			FinishBlock();
			break;
		case MD_BLOCK_HTML:
			if (!at_line_start_) {
				NewLine();
			}
			in_html_ = false;
			FinishBlock();
			break;
		case MD_BLOCK_P:
			FinishBlock();
			break;
		case MD_BLOCK_TABLE:
			RenderTable();
			in_table_ = false;
			FinishBlock();
			break;
		case MD_BLOCK_TR: {
			TableRow row;
			row.cells = std::move(current_row_);
			row.header = in_thead_;
			rows_.push_back(std::move(row));
			current_row_.clear();
			break;
		}
		case MD_BLOCK_TH:
		case MD_BLOCK_TD:
			LeaveCell(detail);
			break;
		default:
			break;
		}
		return 0;
	}

	int EnterSpan(MD_SPANTYPE type, void *detail) {
		switch (type) {
		case MD_SPAN_EM:
			PushStyle([](Style &style) { style.italic = true; });
			break;
		case MD_SPAN_STRONG:
			PushStyle([](Style &style) { style.bold = true; });
			break;
		case MD_SPAN_DEL:
			PushStyle([](Style &style) { style.strike = true; });
			break;
		case MD_SPAN_U:
			PushStyle([](Style &style) { style.underline = true; });
			break;
		case MD_SPAN_CODE:
			PushStyle([](Style &style) {
				style.fg = 222;
				style.bg = 236;
			});
			break;
		case MD_SPAN_A:
			EnterLink(detail);
			break;
		case MD_SPAN_IMG:
			EnterImage(detail);
			break;
		default:
			break;
		}
		return 0;
	}

	int LeaveSpan(MD_SPANTYPE type, void *detail) {
		(void)detail;
		switch (type) {
		case MD_SPAN_EM:
		case MD_SPAN_STRONG:
		case MD_SPAN_DEL:
		case MD_SPAN_U:
		case MD_SPAN_CODE:
			PopStyle();
			break;
		case MD_SPAN_A:
			LeaveLink();
			break;
		case MD_SPAN_IMG:
			LeaveImage();
			break;
		default:
			break;
		}
		return 0;
	}

	int Text(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size) {
		switch (type) {
		case MD_TEXT_NULLCHAR:
			AppendRaw("\xef\xbf\xbd");
			break;
		case MD_TEXT_BR:
			if (InlineContext()) {
				AppendRaw(" ");
			} else {
				NewLine();
			}
			break;
		case MD_TEXT_SOFTBR:
			if (in_code_) {
				WriteCode(text, size);
			} else {
				AppendRaw(" ");
			}
			break;
		case MD_TEXT_ENTITY:
			AppendInline(DecodeEntity(std::string(text, size)));
			break;
		case MD_TEXT_CODE:
			if (in_code_) {
				WriteCode(text, size);
			} else {
				AppendInline(text, size);
			}
			break;
		case MD_TEXT_HTML:
			AppendHtml(text, size);
			break;
		case MD_TEXT_LATEXMATH:
			AppendInline(text, size);
			break;
		case MD_TEXT_NORMAL:
		default:
			if (in_code_) {
				WriteCode(text, size);
			} else if (InlineContext()) {
				AppendInline(text, size);
			} else {
				WriteText(text, size);
			}
			break;
		}
		return 0;
	}

  private:
	bool InlineContext() const {
		return in_cell_ || in_heading_ || in_link_ || in_image_;
	}

	void Emit(const std::string &text) {
		*sink_ += text;
	}

	void AppendRaw(const char *text) {
		*sink_ += text;
	}

	void AppendRaw(const std::string &text) {
		*sink_ += text;
	}

	void AppendRaw(const char *text, std::size_t size) {
		sink_->append(text, size);
	}

	void EmitColor(int color) {
		if (!color_) {
			return;
		}
		sink_->append("\x1b[38;5;");
		sink_->append(std::to_string(color));
		sink_->push_back('m');
	}

	void EmitReset() {
		if (color_) {
			*sink_ += RESET;
		}
	}

	void EmitStyle() {
		if (!color_) {
			return;
		}
		*sink_ += RESET;
		auto &style = styles_.back();
		if (style.bold) {
			*sink_ += BOLD;
		}
		if (style.dim) {
			*sink_ += DIM;
		}
		if (style.italic) {
			*sink_ += ITALIC;
		}
		if (style.underline) {
			*sink_ += UNDERLINE;
		}
		if (style.strike) {
			*sink_ += STRIKE;
		}
		if (style.fg >= 0 || style.bg >= 0) {
			sink_->append("\x1b[");
			if (style.fg >= 0) {
				sink_->append("38;5;");
				sink_->append(std::to_string(style.fg));
			}
			if (style.bg >= 0) {
				if (style.fg >= 0) {
					sink_->push_back(';');
				}
				sink_->append("48;5;");
				sink_->append(std::to_string(style.bg));
			}
			sink_->push_back('m');
		}
	}

	template <class Modifier>
	void PushStyle(Modifier modifier) {
		styles_.push_back(styles_.back());
		modifier(styles_.back());
		EmitStyle();
	}

	void PopStyle() {
		if (styles_.size() > 1) {
			styles_.pop_back();
		}
		EmitStyle();
	}

	void BeginBlock() {
		if (sink_ != &body_ || body_.empty()) {
			at_line_start_ = true;
			return;
		}
		if (body_.back() != '\n') {
			body_ += '\n';
		}
		if (list_depth_ == 0 && quote_depth_ == 0) {
			if (body_.size() < 2 || body_[body_.size() - 2] != '\n') {
				body_ += '\n';
			}
		}
		at_line_start_ = true;
	}

	void FinishBlock() {
		if (sink_ != &body_ || body_.empty()) {
			return;
		}
		if (body_.back() != '\n') {
			body_ += '\n';
		}
		if (list_depth_ == 0 && quote_depth_ == 0) {
			if (body_.size() < 2 || body_[body_.size() - 2] != '\n') {
				body_ += '\n';
			}
		}
		at_line_start_ = true;
	}

	void NewLine() {
		if (sink_ != &body_) {
			AppendRaw(" ");
			return;
		}
		if (!at_line_start_) {
			body_ += '\n';
		}
		at_line_start_ = true;
	}

	void StartLine() {
		if (sink_ != &body_ || !at_line_start_) {
			return;
		}
		for (int i = 0; i < quote_depth_; i++) {
			EmitColor(244);
			*sink_ += "\xe2\x94\x82 ";
			EmitReset();
		}
		for (auto &part : indents_) {
			if (part.pending) {
				*sink_ += part.marker;
				part.pending = false;
			} else {
				*sink_ += part.continuation;
			}
		}
		at_line_start_ = false;
	}

	void WriteText(const MD_CHAR *text, MD_SIZE size) {
		for (MD_SIZE i = 0; i < size; i++) {
			auto c = static_cast<unsigned char>(text[i]);
			if (c == '\n') {
				NewLine();
			} else if (c == '\t') {
				StartLine();
				*sink_ += "    ";
			} else if (c == '\r' || c == 0x1b || c < 0x20 || c == 0x7f) {
				continue;
			} else {
				StartLine();
				sink_->push_back(static_cast<char>(c));
			}
		}
	}

	void AppendInline(const char *text, std::size_t size) {
		for (std::size_t i = 0; i < size; i++) {
			auto c = static_cast<unsigned char>(text[i]);
			if (c == '\n' || c == '\r' || c == '\t') {
				sink_->push_back(' ');
			} else if (c == 0x1b || c < 0x20 || c == 0x7f) {
				continue;
			} else {
				sink_->push_back(static_cast<char>(c));
			}
		}
	}

	void AppendInline(const std::string &text) {
		AppendInline(text.data(), text.size());
	}

	void AppendHtml(const char *text, std::size_t size) {
		for (std::size_t i = 0; i < size; i++) {
			auto c = static_cast<unsigned char>(text[i]);
			if (in_html_tag_) {
				if (c == '>') {
					in_html_tag_ = false;
				}
				continue;
			}
			if (c == '<') {
				in_html_tag_ = true;
				continue;
			}
			if (c == '\n') {
				if (InlineContext()) {
					sink_->push_back(' ');
				} else {
					NewLine();
				}
			} else if (c == '\r' || c == 0x1b || c == 0x7f || c < 0x20) {
				continue;
			} else if (c == '\t') {
				StartLine();
				sink_->push_back(' ');
			} else {
				StartLine();
				sink_->push_back(static_cast<char>(c));
			}
		}
	}

	void WriteCode(const MD_CHAR *text, MD_SIZE size) {
		if (code_sql_) {
			for (MD_SIZE i = 0; i < size; i++) {
				auto c = static_cast<unsigned char>(text[i]);
				if (c != '\r') {
					code_buffer_.push_back(static_cast<char>(c));
				}
			}
			return;
		}
		for (MD_SIZE i = 0; i < size; i++) {
			auto c = static_cast<unsigned char>(text[i]);
			if (c == '\n') {
				if (code_line_started_) {
					EmitReset();
				}
				NewLine();
				code_line_started_ = false;
			} else if (c == '\r') {
				continue;
			} else {
				if (!code_line_started_) {
					StartLine();
					EmitColor(240);
					*sink_ += "\xe2\x94\x82 ";
					EmitColor(252);
					code_line_started_ = true;
				}
				if (c == '\t') {
					*sink_ += "    ";
				} else if (c == 0x1b || c < 0x20 || c == 0x7f) {
					continue;
				} else {
					sink_->push_back(static_cast<char>(c));
				}
			}
		}
	}

	bool IsSqlFence(void *detail) {
		auto *info = static_cast<MD_BLOCK_CODE_DETAIL *>(detail);
		if (!info || !info->lang.text || info->lang.size == 0) {
			return false;
		}
		auto lang = LowerAscii(std::string(info->lang.text, info->lang.size));
		if (!lang.empty() && lang[0] == '.') {
			lang = lang.substr(1);
		}
		return lang == "sql" || lang == "plsql" || lang == "duckdb";
	}

	void WriteSqlBlock(const std::string &text) {
		enum class State : std::uint8_t { NORMAL,
			                              BLOCK_COMMENT,
			                              STRING,
			                              QUOTED,
			                              DOLLAR };
		enum : std::uint8_t {
			COLOR_DEFAULT = 252,
			COLOR_GUTTER = 240,
			COLOR_KEYWORD = 81,
			COLOR_TYPE = 180,
			COLOR_FUNCTION = 214,
			COLOR_STRING = 114,
			COLOR_NUMBER = 141,
			COLOR_COMMENT = 242,
			COLOR_OPERATOR = 245,
		};

		State state = State::NORMAL;
		int comment_depth = 0;
		std::string dollar_tag;
		std::string previous_word;
		bool line_started = false;
		int current_color = -2;
		std::size_t i = 0;
		auto size = text.size();

		auto put = [&](char c) {
			if (c == '\t') {
				*sink_ += "    ";
			} else if (c == 0x1b || c == 0x7f || static_cast<unsigned char>(c) < 0x20) {
				// Drop control characters so they cannot disturb the terminal.
			} else {
				sink_->push_back(c);
			}
		};
		auto put_range = [&](std::size_t begin, std::size_t end) {
			for (auto position = begin; position < end; position++) {
				put(text[position]);
			}
		};
		auto set_color = [&](int color) {
			if (color != current_color) {
				EmitColor(color);
				current_color = color;
			}
		};

		while (i < size) {
			char c = text[i];
			if (!line_started) {
				if (c == '\n') {
					NewLine();
					i++;
					continue;
				}
				StartLine();
				EmitColor(COLOR_GUTTER);
				*sink_ += "\xe2\x94\x82 ";
				int color = COLOR_DEFAULT;
				if (state == State::BLOCK_COMMENT) {
					color = COLOR_COMMENT;
				} else if (state == State::QUOTED) {
					color = COLOR_TYPE;
				} else if (state == State::STRING || state == State::DOLLAR) {
					color = COLOR_STRING;
				}
				EmitColor(color);
				current_color = color;
				line_started = true;
			}

			if (c == '\n') {
				EmitReset();
				NewLine();
				line_started = false;
				current_color = -2;
				i++;
				continue;
			}
			if (c == '\r') {
				i++;
				continue;
			}

			if (state == State::BLOCK_COMMENT) {
				if (c == '/' && i + 1 < size && text[i + 1] == '*') {
					comment_depth++;
					put('/');
					put('*');
					i += 2;
				} else if (c == '*' && i + 1 < size && text[i + 1] == '/') {
					comment_depth--;
					put('*');
					put('/');
					i += 2;
					if (comment_depth == 0) {
						state = State::NORMAL;
						set_color(COLOR_DEFAULT);
					}
				} else {
					put(c);
					i++;
				}
				continue;
			}
			if (state == State::STRING) {
				if (c == '\'') {
					if (i + 1 < size && text[i + 1] == '\'') {
						put('\'');
						put('\'');
						i += 2;
					} else {
						put('\'');
						i++;
						state = State::NORMAL;
						set_color(COLOR_DEFAULT);
					}
				} else {
					put(c);
					i++;
				}
				continue;
			}
			if (state == State::QUOTED) {
				if (c == '"') {
					if (i + 1 < size && text[i + 1] == '"') {
						put('"');
						put('"');
						i += 2;
					} else {
						put('"');
						i++;
						state = State::NORMAL;
						set_color(COLOR_DEFAULT);
					}
				} else {
					put(c);
					i++;
				}
				continue;
			}
			if (state == State::DOLLAR) {
				if (i + dollar_tag.size() <= size && text.compare(i, dollar_tag.size(), dollar_tag) == 0) {
					put_range(i, i + dollar_tag.size());
					i += dollar_tag.size();
					state = State::NORMAL;
					set_color(COLOR_DEFAULT);
				} else {
					put(c);
					i++;
				}
				continue;
			}

			if (c == '-' && i + 1 < size && text[i + 1] == '-') {
				set_color(COLOR_COMMENT);
				while (i < size && text[i] != '\n') {
					put(text[i]);
					i++;
				}
				continue;
			}
			if (c == '/' && i + 1 < size && text[i + 1] == '*') {
				set_color(COLOR_COMMENT);
				state = State::BLOCK_COMMENT;
				comment_depth = 1;
				put('/');
				put('*');
				i += 2;
				continue;
			}
			if (c == '\'') {
				set_color(COLOR_STRING);
				state = State::STRING;
				put(c);
				i++;
				continue;
			}
			if (c == '"') {
				set_color(COLOR_TYPE);
				state = State::QUOTED;
				put(c);
				i++;
				continue;
			}
			if (c == '$') {
				auto end = i + 1;
				while (end < size &&
				       (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_')) {
					end++;
				}
				if (end < size && text[end] == '$') {
					dollar_tag = text.substr(i, end - i + 1);
					set_color(COLOR_STRING);
					state = State::DOLLAR;
					put_range(i, end + 1);
					i = end + 1;
					continue;
				}
			}
			if (c >= '0' && c <= '9') {
				set_color(COLOR_NUMBER);
				auto start = i;
				i++;
				while (i < size) {
					auto d = static_cast<unsigned char>(text[i]);
					bool consume = std::isalnum(d) || d == '_' ||
					               (d == '.' && !(i + 1 < size && text[i + 1] == '.')) ||
					               ((d == '+' || d == '-') && i > start &&
					                (text[i - 1] == 'e' || text[i - 1] == 'E'));
					if (!consume) {
						break;
					}
					i++;
				}
				put_range(start, i);
				continue;
			}
			if (IsSqlWordStart(static_cast<unsigned char>(c))) {
				auto start = i;
				i++;
				while (i < size && IsSqlWordChar(static_cast<unsigned char>(text[i]))) {
					i++;
				}
				auto word = LowerAscii(text.substr(start, i - start));
				auto lookahead = i;
				while (lookahead < size && (text[lookahead] == ' ' || text[lookahead] == '\t')) {
					lookahead++;
				}
				if (IsSqlKeyword(word)) {
					set_color(COLOR_KEYWORD);
				} else if (IsSqlType(word)) {
					set_color(COLOR_TYPE);
				} else if (lookahead < size && text[lookahead] == '(' && !IsSqlNameContext(previous_word)) {
					set_color(COLOR_FUNCTION);
				} else {
					set_color(COLOR_DEFAULT);
				}
				put_range(start, i);
				previous_word = word;
				continue;
			}
			set_color(COLOR_OPERATOR);
			put(c);
			i++;
		}
		if (line_started) {
			EmitReset();
		}
	}

	void EnterListItem(void *detail) {
		auto *info = static_cast<MD_BLOCK_LI_DETAIL *>(detail);
		if (!at_line_start_) {
			NewLine();
		}
		ListState *list = lists_.empty() ? nullptr : &lists_.back();
		std::string marker;
		if (info && info->is_task) {
			marker = (info->task_mark == 'x' || info->task_mark == 'X') ? "\xe2\x98\x91 " : "\xe2\x98\x90 ";
		} else if (list && list->ordered) {
			marker = std::to_string(list->counter++) + ". ";
		} else {
			marker = "\xe2\x80\xa2 ";
		}
		ListPart part;
		part.marker = marker;
		part.continuation = Spaces(DisplayWidth(marker));
		part.pending = true;
		indents_.push_back(std::move(part));
		at_line_start_ = true;
	}

	void EnterHeading(void *detail) {
		auto *info = static_cast<MD_BLOCK_H_DETAIL *>(detail);
		BeginBlock();
		in_heading_ = true;
		heading_level_ = info ? info->level : 1;
		heading_buf_.clear();
		heading_prev_sink_ = sink_;
		sink_ = &heading_buf_;
	}

	void LeaveHeading() {
		sink_ = heading_prev_sink_;
		in_heading_ = false;
		StripHeadingAnchor(heading_buf_);
		if (headings_) {
			auto line = static_cast<std::size_t>(std::count(body_.begin(), body_.end(), '\n'));
			headings_->push_back(MarkdownHeading{line, heading_level_, heading_buf_});
		}
		if (color_) {
			switch (heading_level_) {
			case 1:
				AppendRaw("\x1b[1;4;38;5;39m");
				break;
			case 2:
				AppendRaw("\x1b[1;38;5;75m");
				break;
			case 3:
				AppendRaw("\x1b[1;38;5;111m");
				break;
			default:
				AppendRaw(BOLD);
				break;
			}
		}
		StartLine();
		AppendRaw(heading_buf_);
		EmitReset();
		NewLine();
		FinishBlock();
	}

	void EnterLink(void *detail) {
		auto *info = static_cast<MD_SPAN_A_DETAIL *>(detail);
		in_link_ = true;
		link_prev_sink_ = sink_;
		link_buf_.clear();
		sink_ = &link_buf_;
		link_href_ = info ? std::string(info->href.text, info->href.size) : std::string();
		styles_.push_back(styles_.back());
		bool external = is_external_ ? is_external_(link_href_) : IsExternalLink(link_href_);
		styles_.back().fg = external ? EXTERNAL_LINK_COLOR : INTERNAL_LINK_COLOR;
		styles_.back().underline = true;
		EmitStyle();
	}

	void LeaveLink() {
		PopStyle();
		auto text = std::move(link_buf_);
		sink_ = link_prev_sink_;
		in_link_ = false;
		if (!InlineContext()) {
			StartLine();
		}
		// Emit an OSC 8 hyperlink so the label itself is clickable. The target
		// is not printed, which keeps result snippets compact. A leading style
		// reset is emitted before the hyperlink starts: some terminals clear the
		// active link when they see SGR 0.
		bool hyperlink = color_ && !link_href_.empty();
		if (hyperlink && text.compare(0, 4, RESET) == 0) {
			*sink_ += RESET;
			text.erase(0, 4);
		}
		if (hyperlink) {
			*sink_ += "\x1b]8;;";
			for (auto c : link_href_) {
				auto byte = static_cast<unsigned char>(c);
				if (byte >= 0x20 && byte != 0x7f) {
					sink_->push_back(c);
				}
			}
			// BEL is accepted by more terminals than the ST terminator.
			*sink_ += '\a';
		}
		AppendRaw(text);
		if (hyperlink) {
			*sink_ += "\x1b]8;;\a";
		}
		link_href_.clear();
	}

	void EnterImage(void *detail) {
		(void)detail;
		in_image_ = true;
		image_prev_sink_ = sink_;
		image_buf_.clear();
		sink_ = &image_buf_;
	}

	void LeaveImage() {
		auto text = std::move(image_buf_);
		sink_ = image_prev_sink_;
		in_image_ = false;
		if (!InlineContext()) {
			StartLine();
		}
		if (color_) {
			*sink_ += DIM;
		}
		*sink_ += "[image: ";
		*sink_ += text;
		*sink_ += "]";
		if (color_) {
			*sink_ += RESET;
		}
	}

	void EnterTable(void *detail) {
		auto *info = static_cast<MD_BLOCK_TABLE_DETAIL *>(detail);
		BeginBlock();
		in_table_ = true;
		rows_.clear();
		current_row_.clear();
		col_count_ = info ? info->col_count : 0;
		col_align_.assign(std::max<std::size_t>(col_count_, 1), MD_ALIGN_DEFAULT);
		at_line_start_ = true;
	}

	void EnterCell(void *detail, bool header) {
		auto *info = static_cast<MD_BLOCK_TD_DETAIL *>(detail);
		in_cell_ = true;
		cell_prev_sink_ = sink_;
		cell_buf_.clear();
		sink_ = &cell_buf_;
		cell_align_ = info ? info->align : MD_ALIGN_DEFAULT;
		if (header) {
			auto column = current_row_.size();
			if (column < col_align_.size() && cell_align_ != MD_ALIGN_DEFAULT) {
				col_align_[column] = cell_align_;
			}
		}
	}

	void LeaveCell(void *detail) {
		(void)detail;
		Cell cell;
		cell.text = std::move(cell_buf_);
		cell.align = cell_align_;
		sink_ = cell_prev_sink_;
		in_cell_ = false;
		current_row_.push_back(std::move(cell));
	}

	static std::string Pad(const std::string &text, std::size_t width, MD_ALIGN align) {
		auto length = DisplayWidth(text);
		if (length >= width) {
			return text;
		}
		auto padding = width - length;
		if (align == MD_ALIGN_RIGHT) {
			return Spaces(padding) + text;
		}
		if (align == MD_ALIGN_CENTER) {
			auto left = padding / 2;
			return Spaces(left) + text + Spaces(padding - left);
		}
		return text + Spaces(padding);
	}

	void WriteTableLine(const std::string &line) {
		StartLine();
		AppendRaw(line);
		NewLine();
	}

	void RenderTable() {
		if (rows_.empty()) {
			return;
		}
		std::size_t columns = col_count_;
		for (auto &row : rows_) {
			columns = std::max(columns, row.cells.size());
		}
		if (columns == 0) {
			return;
		}
		col_align_.resize(columns, MD_ALIGN_DEFAULT);
		std::vector<std::size_t> widths(columns, 3);
		for (auto &row : rows_) {
			for (std::size_t c = 0; c < row.cells.size(); c++) {
				row.cells[c].text = TruncateVisible(row.cells[c].text, MAX_CELL_WIDTH, color_);
				widths[c] = std::max(widths[c], DisplayWidth(row.cells[c].text));
			}
		}

		auto paint_border = [&](const std::string &text) {
			if (!color_) {
				return text;
			}
			return std::string("\x1b[38;5;240m") + text + RESET;
		};
		auto border = [&](const char *left, const char *middle, const char *right) {
			std::string line = left;
			for (std::size_t c = 0; c < columns; c++) {
				for (std::size_t i = 0; i < widths[c] + 2; i++) {
					line += "\xe2\x94\x80";
				}
				line += c + 1 == columns ? right : middle;
			}
			return paint_border(line);
		};

		WriteTableLine(border("\xe2\x94\x8c", "\xe2\x94\xac", "\xe2\x94\x90"));
		for (std::size_t r = 0; r < rows_.size(); r++) {
			auto &row = rows_[r];
			std::string line = paint_border("\xe2\x94\x82");
			for (std::size_t c = 0; c < columns; c++) {
				std::string text;
				MD_ALIGN align = col_align_[c];
				if (c < row.cells.size()) {
					text = row.cells[c].text;
					if (row.cells[c].align != MD_ALIGN_DEFAULT) {
						align = row.cells[c].align;
					}
				}
				line += " ";
				auto padded = Pad(text, widths[c], align);
				if (row.header && color_) {
					padded.insert(0, BOLD);
					padded += RESET;
				}
				line += padded;
				line += " ";
				line += paint_border("\xe2\x94\x82");
			}
			WriteTableLine(line);
			if (row.header && r + 1 < rows_.size()) {
				WriteTableLine(border("\xe2\x94\x9c", "\xe2\x94\xbc", "\xe2\x94\xa4"));
			}
		}
		WriteTableLine(border("\xe2\x94\x94", "\xe2\x94\xb4", "\xe2\x94\x98"));
	}

	bool color_;
	std::vector<MarkdownHeading> *headings_ = nullptr;
	ExternalLinkPredicate is_external_;
	std::string body_;
	std::string *sink_ = &body_;

	bool at_line_start_ = true;
	int quote_depth_ = 0;
	int list_depth_ = 0;
	std::vector<ListPart> indents_;
	std::vector<ListState> lists_;

	std::vector<Style> styles_;

	bool in_code_ = false;
	bool code_line_started_ = false;
	bool code_sql_ = false;
	std::string code_buffer_;
	bool in_html_ = false;
	bool in_html_tag_ = false;

	bool in_heading_ = false;
	unsigned int heading_level_ = 1;
	std::string heading_buf_;
	std::string *heading_prev_sink_ = nullptr;

	bool in_table_ = false;
	bool in_thead_ = false;
	std::size_t col_count_ = 0;
	std::vector<TableRow> rows_;
	std::vector<Cell> current_row_;
	std::vector<MD_ALIGN> col_align_;
	MD_ALIGN cell_align_ = MD_ALIGN_DEFAULT;
	bool in_cell_ = false;
	std::string cell_buf_;
	std::string *cell_prev_sink_ = nullptr;

	bool in_link_ = false;
	std::string link_href_;
	std::string link_buf_;
	std::string *link_prev_sink_ = nullptr;

	bool in_image_ = false;
	std::string image_buf_;
	std::string *image_prev_sink_ = nullptr;
};

} // namespace

std::size_t Utf8CompleteLength(const std::string &text) {
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
		} else if ((byte & 0x80) != 0) {
			// Standalone continuation byte: keep it rather than stall forever.
			position++;
			continue;
		}
		if (position + length > text.size()) {
			break;
		}
		position += length;
	}
	return position;
}

std::vector<TextLink> ExtractLinks(const std::string &text) {
	std::vector<TextLink> links;
	std::string current_url;
	std::size_t current_start = 0;
	bool current_has_text = false;
	std::size_t column = 0;
	std::size_t i = 0;

	auto finish_link = [&](std::size_t end) {
		if (!current_url.empty() && current_has_text && end > current_start) {
			links.push_back(TextLink{current_start, end, current_url});
		}
		current_url.clear();
		current_has_text = false;
	};

	while (i < text.size()) {
		if (IsEscape(text, i)) {
			auto next = SkipEscape(text, i);
			auto sequence = text.substr(i, next - i);
			// OSC 8 uses the form ESC ] 8 ; params ; URI <terminator>.
			if (sequence.compare(0, 4, "\x1b]8;") == 0) {
				auto body = sequence.substr(4);
				auto separator = body.find(';');
				if (separator != std::string::npos) {
					auto url = body.substr(separator + 1);
					if (!url.empty() && url.back() == '\a') {
						url.pop_back();
					} else if (url.size() >= 2 && url[url.size() - 2] == '\x1b' && url.back() == '\\') {
						url.resize(url.size() - 2);
					}
					finish_link(column);
					current_url = std::move(url);
				}
			}
			i = next;
			continue;
		}
		auto c = static_cast<unsigned char>(text[i]);
		if (c < 0x20 || c == 0x7f) {
			i++;
			continue;
		}
		unsigned int codepoint = 0;
		auto length = DecodeUtf8(text, i, codepoint);
		int cell_width = wcwidth(static_cast<wchar_t>(codepoint));
		auto add = cell_width > 0 ? static_cast<std::size_t>(cell_width) : 1;
		if (!current_url.empty() && !current_has_text) {
			current_start = column;
			current_has_text = true;
		}
		column += add;
		i += length;
	}
	finish_link(column);
	return links;
}

std::size_t VisibleWidth(const std::string &text) {
	return DisplayWidth(text);
}

std::vector<std::string> WrapAnsi(const std::string &text, std::size_t width) {
	if (width == 0) {
		width = 80;
	}
	std::vector<std::string> result;
	std::string chunk;
	std::string active_sgr;
	std::string active_link;
	std::size_t column = 0;
	std::size_t i = 0;

	auto finish_chunk = [&]() {
		if (chunk.find('\x1b') != std::string::npos) {
			chunk += RESET;
		}
		if (!active_link.empty()) {
			chunk += "\x1b]8;;\a";
		}
		result.push_back(chunk);
		chunk.clear();
		column = 0;
		chunk += active_sgr;
		chunk += active_link;
	};

	while (i < text.size()) {
		if (IsEscape(text, i)) {
			auto next = SkipEscape(text, i);
			auto sequence = text.substr(i, next - i);
			if (sequence.size() >= 3 && sequence[0] == '\x1b' && sequence[1] == '[' && sequence.back() == 'm') {
				if (sequence == RESET) {
					active_sgr.clear();
				} else if (sequence.compare(0, 4, "\x1b[0m") == 0) {
					active_sgr = sequence.substr(4);
				} else {
					active_sgr += sequence;
				}
			} else if (sequence.compare(0, 5, "\x1b]8;;") == 0) {
				if (sequence == "\x1b]8;;\x1b\\" || sequence == "\x1b]8;;\a") {
					active_link.clear();
				} else {
					active_link = sequence;
				}
			}
			chunk += sequence;
			i = next;
			continue;
		}

		unsigned int codepoint = 0;
		auto length = DecodeUtf8(text, i, codepoint);
		int cell_width = wcwidth(static_cast<wchar_t>(codepoint));
		auto add = cell_width > 0 ? static_cast<std::size_t>(cell_width) : 1;
		if (column > 0 && column + add > width) {
			finish_chunk();
		}
		chunk.append(text, i, length);
		column += add;
		i += length;
	}

	if (!chunk.empty() || result.empty()) {
		if (chunk.find('\x1b') != std::string::npos) {
			chunk += RESET;
		}
		if (!active_link.empty()) {
			chunk += "\x1b]8;;\a";
		}
		result.push_back(chunk);
	}
	return result;
}

std::string RenderMarkdown(const std::string &markdown, bool color, std::vector<MarkdownHeading> *headings,
                           ExternalLinkPredicate is_external) {
	Renderer renderer(color, headings, std::move(is_external));
	return renderer.Render(markdown);
}

} // namespace cli
} // namespace docs
