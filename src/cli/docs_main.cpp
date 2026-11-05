#include "cli/run.hpp"
#include "docs_search.hpp"
#include "platform.hpp"

int main(int argc, char **argv) {
	docs::InitConsole();
	const docs::SearchMode all = {"search", docs::SearchAll, docs::SearchAllSnippets};
	auto result = docs::RunCli("docs", all, argc, argv);
	docs::FinishConsole();
	return result;
}
