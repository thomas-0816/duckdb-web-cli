#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace docs {

//! One chunk of Markdown that is embedded into the semantic index. A page can
//! produce several chunks when its body is longer than the model's input
//! window. Every chunk keeps the page it belongs to, so several matching
//! chunks still produce a single result page.
struct SemanticDocument {
	std::string text;
	uint8_t type = 0; // 0 = documentation, 1 = blog
	uint32_t page_id = 0;
};

struct SemanticHit {
	uint8_t type = 0;
	uint32_t page_id = 0;
	//! Index into the semantic document vector, identifying the matched chunk.
	std::size_t document_id = 0;
	float score = 0;
};

//! The embedding model used for semantic search. The GGUF data is compiled
//! into the binary, so no model file is needed at run time.
struct SemanticModel {
	std::string id;
};

const SemanticModel &GetSemanticModel();

//! True when the embedded model is available (always true for this build).
bool SemanticModelAvailable();

//! Optional callback invoked while the semantic index is being built. `done`
//! and `total` count the chunks that have been embedded so far.
using SemanticProgressCallback = void (*)(std::size_t done, std::size_t total, void *user);

//! Register a progress callback. Pass nullptr to restore the default terminal
//! progress indicator.
void SetSemanticProgressCallback(SemanticProgressCallback callback, void *user);

//! Load or build the semantic index without searching. Used to prepare the
//! index at startup so the first query does not have to wait silently.
void EnsureSemanticIndex(const std::vector<SemanticDocument> &documents, const std::string &fingerprint);

//! Search the semantic index. The index is built on first use and cached next
//! to the other generated files. Throws when the model or index cannot be
//! loaded.
std::vector<SemanticHit> SearchSemantic(const std::vector<SemanticDocument> &documents,
                                        const std::string &fingerprint, const std::string &query,
                                        std::size_t limit);

} // namespace docs
