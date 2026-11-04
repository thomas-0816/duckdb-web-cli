#include "cli/clipboard.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace docs {
namespace {

std::string Base64(const std::string &input) {
	static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string output;
	output.reserve((input.size() + 2) / 3 * 4);
	std::size_t i = 0;
	while (i + 2 < input.size()) {
		unsigned int value = (static_cast<unsigned char>(input[i]) << 16) |
		                     (static_cast<unsigned char>(input[i + 1]) << 8) |
		                     static_cast<unsigned char>(input[i + 2]);
		output.push_back(alphabet[(value >> 18) & 63]);
		output.push_back(alphabet[(value >> 12) & 63]);
		output.push_back(alphabet[(value >> 6) & 63]);
		output.push_back(alphabet[value & 63]);
		i += 3;
	}
	if (i < input.size()) {
		unsigned int value = static_cast<unsigned char>(input[i]) << 16;
		if (i + 1 < input.size()) {
			value |= static_cast<unsigned char>(input[i + 1]) << 8;
		}
		output.push_back(alphabet[(value >> 18) & 63]);
		output.push_back(alphabet[(value >> 12) & 63]);
		output.push_back(i + 1 < input.size() ? alphabet[(value >> 6) & 63] : '=');
		output.push_back('=');
	}
	return output;
}

#if !defined(_WIN32)
//! Temporarily ignore SIGPIPE while writing to a helper process that may exit
//! before reading all of its input.
class SigpipeGuard {
public:
	SigpipeGuard() {
		struct sigaction action = {};
		action.sa_handler = SIG_IGN;
		sigemptyset(&action.sa_mask);
		active_ = sigaction(SIGPIPE, &action, &previous_) == 0;
	}
	~SigpipeGuard() {
		if (active_) {
			sigaction(SIGPIPE, &previous_, nullptr);
		}
	}
	SigpipeGuard(const SigpipeGuard &) = delete;
	SigpipeGuard &operator=(const SigpipeGuard &) = delete;

private:
	struct sigaction previous_ = {};
	bool active_ = false;
};

//! Run `command` and write `text` to its standard input. Returns true when the
//! command ran and exited successfully.
bool WriteToCommand(const std::vector<std::string> &command, const std::string &text) {
	if (command.empty()) {
		return false;
	}
	std::vector<char *> argv;
	argv.reserve(command.size() + 1);
	for (auto &argument : command) {
		argv.push_back(const_cast<char *>(argument.c_str()));
	}
	argv.push_back(nullptr);

	int pipe_fds[2];
	if (pipe(pipe_fds) != 0) {
		return false;
	}
	pid_t pid = fork();
	if (pid < 0) {
		close(pipe_fds[0]);
		close(pipe_fds[1]);
		return false;
	}
	if (pid == 0) {
		dup2(pipe_fds[0], STDIN_FILENO);
		close(pipe_fds[0]);
		close(pipe_fds[1]);
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > STDERR_FILENO) {
				close(devnull);
			}
		}
		execvp(argv[0], argv.data());
		_exit(127);
	}
	close(pipe_fds[0]);
	{
		SigpipeGuard guard;
		std::size_t written = 0;
		while (written < text.size()) {
			auto chunk = write(pipe_fds[1], text.data() + written, text.size() - written);
			if (chunk <= 0) {
				break;
			}
			written += static_cast<std::size_t>(chunk);
		}
	}
	close(pipe_fds[1]);
	int status = 0;
	if (waitpid(pid, &status, 0) != pid) {
		return false;
	}
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

//! Write the text to an X11 clipboard tool. Both CLIPBOARD and PRIMARY are set
//! so middle-click paste works as well. The tools detach themselves and keep
//! owning the selection after this process exits.
bool WriteToX11(const std::string &text) {
	if (!std::getenv("DISPLAY")) {
		return false;
	}
	if (WriteToCommand({"xclip", "-selection", "clipboard"}, text)) {
		WriteToCommand({"xclip", "-selection", "primary"}, text);
		return true;
	}
	if (WriteToCommand({"xsel", "--clipboard", "--input"}, text)) {
		WriteToCommand({"xsel", "--primary", "--input"}, text);
		return true;
	}
	return false;
}
#endif

#if defined(_WIN32)
bool CopyToWindowsClipboard(const std::string &text) {
	int wide_length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
	if (wide_length <= 0) {
		return false;
	}
	std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
	if (MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), wide_length) !=
	    wide_length) {
		return false;
	}
	HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t));
	if (!memory) {
		return false;
	}
	auto *destination = static_cast<wchar_t *>(GlobalLock(memory));
	if (!destination) {
		GlobalFree(memory);
		return false;
	}
	std::memcpy(destination, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
	GlobalUnlock(memory);

	if (!OpenClipboard(nullptr)) {
		GlobalFree(memory);
		return false;
	}
	EmptyClipboard();
	if (!SetClipboardData(CF_UNICODETEXT, memory)) {
		CloseClipboard();
		GlobalFree(memory);
		return false;
	}
	CloseClipboard();
	return true;
}
#endif

} // namespace

void SetClipboard(const std::string &text) {
	if (text.empty()) {
		return;
	}
	std::string copy = text;
	if (copy.back() != '\n') {
		copy.push_back('\n');
	}
	auto encoded = Base64(copy);
	// OSC 52 works in kitty, WezTerm, foot and similar terminals. Konsole
	// ignores it, so each platform also uses its native clipboard below.
	std::fputs("\x1b]52;c;", stdout);
	std::fputs(encoded.c_str(), stdout);
	std::fputc('\a', stdout);
	std::fputs("\x1b]52;p;", stdout);
	std::fputs(encoded.c_str(), stdout);
	std::fputc('\a', stdout);
	std::fflush(stdout);

#if defined(_WIN32)
	CopyToWindowsClipboard(copy);
#elif defined(__APPLE__)
	WriteToCommand({"pbcopy"}, copy);
#else
	bool copied = false;
	if (std::getenv("WAYLAND_DISPLAY")) {
		copied = WriteToCommand({"wl-copy"}, copy);
	}
	if (!copied) {
		WriteToX11(copy);
	}
#endif
}

} // namespace docs
