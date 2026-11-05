#include "cli/run.hpp"
#include "docs_search.hpp"

int main(int argc, char **argv) {
	const docs::SearchMode all = {"search", docs::SearchAll, docs::SearchAllSnippets};
	return docs::RunCli("docs", all, argc, argv);
}
