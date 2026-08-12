#pragma once

#include <chrono>
#include <cstdint>
#include <string>

struct llama_context;
struct llama_model;
struct llama_vocab;

namespace dictscribe::rewrite {

class LlamaRewriter {
public:
    LlamaRewriter();
    ~LlamaRewriter();

    LlamaRewriter(const LlamaRewriter&) = delete;
    LlamaRewriter& operator=(const LlamaRewriter&) = delete;

    bool load(
        const std::string& model_path,
        int gpu_layers,
        std::uint32_t context_size,
        std::string& error);
    bool rewrite(
        const std::string& transcript,
        const std::string& source_language,
        std::uint32_t maximum_output_tokens,
        std::string& output,
        std::string& error);

private:
    bool generate(
        const std::string& prompt,
        std::uint32_t maximum_output_tokens,
        std::chrono::steady_clock::time_point deadline,
        std::string& output,
        std::string& error);
    std::string format_prompt(
        const std::string& transcript,
        const std::string& source_language,
        bool strict_language_retry,
        std::string& error) const;

    llama_model* model_ = nullptr;
    llama_context* context_ = nullptr;
    const llama_vocab* vocabulary_ = nullptr;
};

} // namespace dictscribe::rewrite
