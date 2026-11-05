#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <wchar.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <process.h>
#include <windows.h>

#include <algorithm>
#include <ios>
#include <iostream>
#include <streambuf>
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

#if defined(_WIN32)
namespace detail {

inline void EnableVirtualTerminal(DWORD standard_handle) {
	auto handle = GetStdHandle(standard_handle);
	DWORD mode = 0;
	if (handle != INVALID_HANDLE_VALUE && handle != nullptr && GetConsoleMode(handle, &mode)) {
		SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
	}
}

//! Write UTF-8 text to a Windows console.
//!
//! SetConsoleOutputCP(CP_UTF8) only changes the code page used by the C
//! runtime. Bytes that reach the console through WriteFile are still decoded
//! with the console's original code page, so characters such as the registered
//! and trade mark signs or the box-drawing bar come out as mojibake. A console
//! accepts UTF-16 through WriteConsoleW, which is independent of the code
//! page, so convert each complete UTF-8 sequence and write that instead.
class Utf8ConsoleStream : public std::streambuf {
public:
	explicit Utf8ConsoleStream(HANDLE handle) : handle_(handle) {
	}

	//! Flush a trailing sequence that was never completed.
	void Finish() {
		if (!pending_.empty()) {
			WriteConsole(pending_);
			pending_.clear();
		}
	}

protected:
	int_type overflow(int_type character) override {
		if (traits_type::eq_int_type(character, traits_type::eof())) {
			return traits_type::not_eof(character);
		}
		auto byte = traits_type::to_char_type(character);
		if (Write(&byte, 1) != 1) {
			return traits_type::eof();
		}
		return character;
	}

	std::streamsize xsputn(const char *data, std::streamsize count) override {
		if (count <= 0) {
			return 0;
		}
		auto size = static_cast<std::size_t>(count);
		return static_cast<std::streamsize>(Write(data, size));
	}

	int sync() override {
		FlushComplete();
		return 0;
	}

private:
	std::size_t Write(const char *data, std::size_t size) {
		pending_.append(data, size);
		FlushComplete();
		return size;
	}

	//! Return the length of the longest prefix that ends on a character boundary.
	std::size_t CompleteLength() const {
		std::size_t position = 0;
		while (position < pending_.size()) {
			auto length = SequenceLength(position);
			if (length == 0) {
				break;
			}
			position += length;
		}
		return position;
	}

	//! Length of the UTF-8 sequence at `position`, 0 when it is incomplete, or 1
	//! when the byte does not start a valid sequence.
	std::size_t SequenceLength(std::size_t position) const {
		auto byte = static_cast<unsigned char>(pending_[position]);
		std::size_t length = 1;
		if (byte < 0x80) {
			return 1;
		} else if ((byte & 0xe0) == 0xc0) {
			length = 2;
		} else if ((byte & 0xf0) == 0xe0) {
			length = 3;
		} else if ((byte & 0xf8) == 0xf0) {
			length = 4;
		} else {
			return 1;
		}
		if (position + length > pending_.size()) {
			return 0;
		}
		for (std::size_t offset = 1; offset < length; offset++) {
			if ((static_cast<unsigned char>(pending_[position + offset]) & 0xc0) != 0x80) {
				return 1;
			}
		}
		return length;
	}

	void FlushComplete() {
		auto complete = CompleteLength();
		if (complete == 0) {
			return;
		}
		WriteConsole(pending_.substr(0, complete));
		pending_.erase(0, complete);
	}

	void WriteConsole(const std::string &text) {
		if (text.empty()) {
			return;
		}
		auto length = static_cast<int>(text.size());
		auto wide_length = MultiByteToWideChar(CP_UTF8, 0, text.data(), length, nullptr, 0);
		if (wide_length <= 0) {
			return;
		}
		std::vector<wchar_t> wide(static_cast<std::size_t>(wide_length));
		if (MultiByteToWideChar(CP_UTF8, 0, text.data(), length, wide.data(), wide_length) <= 0) {
			return;
		}
		std::size_t offset = 0;
		while (offset < wide.size()) {
			auto remaining = wide.size() - offset;
			auto chunk = std::min<std::size_t>(remaining, 8192);
			// Never split a UTF-16 surrogate pair across two WriteConsoleW calls.
			if (chunk < remaining && chunk > 0 && wide[offset + chunk - 1] >= 0xd800 &&
			    wide[offset + chunk - 1] <= 0xdbff) {
				chunk--;
			}
			DWORD written = 0;
			if (!WriteConsoleW(handle_, wide.data() + offset, static_cast<DWORD>(chunk), &written, nullptr) ||
			    written == 0) {
				return;
			}
			offset += written;
		}
	}

	HANDLE handle_;
	std::string pending_;
};

inline bool IsConsole(DWORD standard_handle) {
	auto handle = GetStdHandle(standard_handle);
	DWORD mode = 0;
	return handle != INVALID_HANDLE_VALUE && handle != nullptr && GetConsoleMode(handle, &mode) != 0;
}

inline Utf8ConsoleStream *InstallConsoleStream(DWORD standard_handle, std::ostream &stream) {
	if (!IsConsole(standard_handle)) {
		return nullptr;
	}
	auto *buffer = new Utf8ConsoleStream(GetStdHandle(standard_handle));
	stream.rdbuf(buffer);
	return buffer;
}

struct ConsoleStreams {
	Utf8ConsoleStream *output = nullptr;
	Utf8ConsoleStream *error = nullptr;
};

inline ConsoleStreams &GetConsoleStreams() {
	static ConsoleStreams streams;
	return streams;
}

} // namespace detail

//! Switch the Windows console to UTF-8 and route the standard streams through it.
inline void InitConsole() {
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);
	detail::EnableVirtualTerminal(STD_OUTPUT_HANDLE);
	detail::EnableVirtualTerminal(STD_ERROR_HANDLE);

	// Take the C++ streams off the C runtime so replacing their buffers below
	// also changes where the pager and the CLI write their output.
	std::ios::sync_with_stdio(false);
	auto &streams = detail::GetConsoleStreams();
	streams.output = detail::InstallConsoleStream(STD_OUTPUT_HANDLE, std::cout);
	streams.error = detail::InstallConsoleStream(STD_ERROR_HANDLE, std::cerr);
}

//! Flush the standard streams, including any trailing partial UTF-8 sequence.
inline void FinishConsole() {
	std::cout.flush();
	std::cerr.flush();
	auto &streams = detail::GetConsoleStreams();
	if (streams.output) {
		streams.output->Finish();
	}
	if (streams.error) {
		streams.error->Finish();
	}
}
#else
inline void InitConsole() {
}

inline void FinishConsole() {
}
#endif

} // namespace docs
