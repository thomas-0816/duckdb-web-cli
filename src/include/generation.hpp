#pragma once

#include "docs_search.hpp"

#include <functional>
#include <string>
#include <vector>

namespace docs {

//! Number of documentation content blocks that are sent to the answer model.
constexpr idx_t ANSWER_RESULT_COUNT = 10;

//! Select the Hugging Face repository (optionally with a quantization tag)
//! used by the answer model, for example
//! "openbmb/MiniCPM5-1B-GGUF:Q4_K_M". Call this before PrepareAnswerModel.
//! The default is openbmb/MiniCPM5-1B-GGUF:Q4_K_M.
void SetAnswerModel(const std::string &hf_repo);

//! True when the separate answer model file can be found.
bool AnswerModelAvailable();

//! True once the answer model and its context have been loaded.
bool AnswerModelLoaded();

//! Callback for model preparation progress. The value is between 0 and 1, and
//! `status` describes the current phase (downloading, loading, ...).
using AnswerLoadProgress = std::function<void(float progress, const std::string &status)>;

//! Download the answer model when it is missing, then load it on first use.
//! Calling it again is a no-op. Throws when the model cannot be downloaded or
//! loaded.
void PrepareAnswerModel(const AnswerLoadProgress &progress = nullptr);

//! Receives generated UTF-8 text as it becomes available. `reasoning` is true
//! for the model's thinking text, before the thinking end tag.
//! `tokens_per_second` is the generation speed reported by llama.cpp's timings.
//! Returning false stops generation.
using AnswerTokenCallback =
    std::function<bool(const std::string &text, bool reasoning, double tokens_per_second)>;

//! Reports prompt-processing progress. `done` and `total` count prompt tokens.
using AnswerPromptProgress = std::function<void(std::size_t done, std::size_t total)>;

//! Ask the answer model a question with the given documentation content blocks
//! as context. The answer is streamed through `callback`. `prompt_progress` is
//! called while the prompt is being processed, before the first token.
void GenerateAnswer(const std::string &question, const std::vector<SearchResult> &blocks,
                    const AnswerTokenCallback &callback,
                    const AnswerPromptProgress &prompt_progress = nullptr);

} // namespace docs
