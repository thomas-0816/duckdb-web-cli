#pragma once

#include <string>
#include <vector>

namespace docs {
namespace cli {

//! Render a Markdown document for a terminal.
//!
//! Formatting is always applied (headings, lists, tables, code blocks, links).
//! When `color` is true, ANSI colors, text styles, and OSC 8 hyperlinks are
//! emitted as well.
std::string RenderMarkdown(const std::string &markdown, bool color);

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
