#include "cli/run.hpp"
#include "docs_search.hpp"

int main(int argc, char **argv) {
	const docs::SearchMode all = {"docs", docs::SearchAll};
	return docs::RunCli("docs", all, argc, argv);
}
