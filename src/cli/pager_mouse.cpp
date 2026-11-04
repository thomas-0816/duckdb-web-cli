#include "cli/pager_mouse.hpp"

#include <iostream>

namespace docs {
namespace {

bool g_mouse_captured = false;

} // namespace

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

} // namespace docs
