#pragma once

#include <cstdio>

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace docs {

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

} // namespace docs
