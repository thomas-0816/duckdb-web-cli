#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace docs {
namespace cli {

//! A heading in a rendered Markdown document.
struct MarkdownHeading {
	//! Zero-based line number of the heading in the rendered text.
	std::size_t line = 0;
	//! Markdown heading level (1 for `#`, 2 for `##`, ...).
	unsigned int level = 0;
	//! Rendered heading text, possibly containing ANSI escape sequences.
	std::string text;
};

//! Returns true when a link target should be shown and opened as external.
using ExternalLinkPredicate = std::function<bool(const std::string &)>;

//! Render a Markdown document for a terminal.
//!
//! Formatting is always applied (headings, lists, tables, code blocks, links).
//! When `color` is true, ANSI colors, text styles, and OSC 8 hyperlinks are
//! emitted as well. When `headings` is not null, the headings found in the
//! document and their rendered line numbers are appended to it. When
//! `is_external` is set, it decides the color of each link; otherwise links to
//! duckdb.org are treated as internal and all other links as external.
std::string RenderMarkdown(const std::string &markdown, bool color,
                           std::vector<MarkdownHeading> *headings = nullptr,
                           ExternalLinkPredicate is_external = ExternalLinkPredicate());

//! Width of a string in terminal cells, ignoring ANSI escape sequences.
std::size_t VisibleWidth(const std::string &text);

//! A clickable span of text on a rendered line.
struct TextLink {
	//! First screen column of the link (zero based).
	std::size_t start = 0;
	//! Screen column just after the link (zero based, exclusive).
	std::size_t end = 0;
	//! Target URL.
	std::string url;
};

//! Find the OSC 8 hyperlinks in a rendered line and their screen columns.
std::vector<TextLink> ExtractLinks(const std::string &text);

//! Split a line into screen-width chunks, preserving ANSI styles and links.
std::vector<std::string> WrapAnsi(const std::string &text, std::size_t width);

//! Length of the longest prefix of `text` that ends on a complete UTF-8 code
//! point. An incomplete trailing sequence is not counted.
std::size_t Utf8CompleteLength(const std::string &text);

} // namespace cli
} // namespace docs
