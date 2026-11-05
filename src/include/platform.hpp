#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <wchar.h>

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace docs {

//! Width of a Unicode code point in terminal cells.
inline int CodepointWidth(unsigned int codepoint) {
#if defined(_WIN32)
	// The Windows CRT has no wcwidth. Assume one cell, as llama.cpp's console
	// code does.
	(void)codepoint;
	return 1;
#else
	return wcwidth(static_cast<wchar_t>(codepoint));
#endif
}

inline unsigned long ProcessId() {
#if defined(_WIN32)
	return static_cast<unsigned long>(_getpid());
#else
	return static_cast<unsigned long>(getpid());
#endif
}

inline bool IsTerminal(std::FILE *stream) {
#if defined(_WIN32)
	return _isatty(_fileno(stream)) != 0;
#else
	return isatty(fileno(stream)) != 0;
#endif
}

#if defined(_WIN32)
bool CopyToWindowsClipboard(const std::string &text);
#else
bool WriteToCommand(const std::vector<std::string> &command, const std::string &text);
#if !defined(__APPLE__)
bool WriteToX11(const std::string &text);
#endif
#endif

//! Copy text to the system clipboard using the platform-native mechanism.
inline void CopyToSystemClipboard(const std::string &text) {
#if defined(_WIN32)
	CopyToWindowsClipboard(text);
#elif defined(__APPLE__)
	WriteToCommand({"pbcopy"}, text);
#else
	bool copied = false;
	if (std::getenv("WAYLAND_DISPLAY")) {
		copied = WriteToCommand({"wl-copy"}, text);
	}
	if (!copied) {
		WriteToX11(text);
	}
#endif
}

} // namespace docs
