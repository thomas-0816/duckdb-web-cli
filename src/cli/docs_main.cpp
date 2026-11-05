#include "cli/run.hpp"
#include "docs_search.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

void EnableVirtualTerminal(DWORD standard_handle) {
	auto handle = GetStdHandle(standard_handle);
	DWORD mode = 0;
	if (handle != INVALID_HANDLE_VALUE && handle != nullptr && GetConsoleMode(handle, &mode)) {
		SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
	}
}

} // namespace
#endif

int main(int argc, char **argv) {
#if defined(_WIN32)
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);
	EnableVirtualTerminal(STD_OUTPUT_HANDLE);
	EnableVirtualTerminal(STD_ERROR_HANDLE);
#endif
	const docs::SearchMode all = {"search", docs::SearchAll, docs::SearchAllSnippets};
	return docs::RunCli("docs", all, argc, argv);
}
