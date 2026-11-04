#pragma once

namespace docs {

//! Mouse event as reported by the terminal in SGR mode.
struct MouseEvent {
	int button = 0;
	int column = 0;
	int row = 0;
	bool press = false;
};

//! Enable or disable terminal mouse reporting.
void SetMouseCapture(bool enabled);

} // namespace docs
