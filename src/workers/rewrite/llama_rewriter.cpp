#include "llama_rewriter.hpp"

#include "language_guard.hpp"

#include <llama.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>
#include <vector>

namespace dictscribe::rewrite {

namespace {

constexpr const char* kSystemPrompt = R"(You clean up dictated text.
Return only a polished transcript in the source language.
Preserve meaning and valid technical terms, identifiers, paths, commands, numbers, and names.
Resolve fillers, repetitions, abandoned starts, spoken corrections, punctuation, capitalization, and grammar.)";

constexpr unsigned kMaximumCpuThreads = 8;
constexpr auto kMaximumGenerationTime = std::chrono::seconds(15);
constexpr int32_t kTopK = 20;
constexpr float kTopP = 0.8F;
constexpr float kTemperature = 0.7F;
constexpr float kPresencePenalty = 1.5F;
constexpr int32_t kPenaltyLastTokens = 64;
constexpr std::string_view kThinkingStart = "<think>";
constexpr std::string_view kThinkingEnd = "</think>";
constexpr std::string_view kNonThinkingAssistantPrefix = "<think>\n\n</think>\n\n";

std::string trim(std::string text) {
    const auto is_space = [](unsigned char value) { return std::isspace(value) != 0; };
    text.erase(text.begin(), std::find_if_not(text.begin(), text.end(), is_space));
    text.erase(std::find_if_not(text.rbegin(), text.rend(), is_space).base(), text.end());
    return text;
}

bool remove_thinking_prefix(std::string& text, std::string& error) {
    text = trim(std::move(text));
    if (!text.starts_with(kThinkingStart)) {
        return true;
    }

    const auto end = text.find(kThinkingEnd);
    if (end == std::string::npos) {
        text.clear();
        error = "rewrite model did not leave thinking mode";
        return false;
    }
    text = trim(text.substr(end + kThinkingEnd.size()));
    return true;
}

} // namespace

LlamaRewriter::LlamaRewriter() {
    llama_backend_init();
}

LlamaRewriter::~LlamaRewriter() {
    if (context_) {
        llama_free(context_);
    }
    if (model_) {
        llama_model_free(model_);
    }
    llama_backend_free();
}

bool LlamaRewriter::load(
    const std::string& model_path,
    int gpu_layers,
    std::uint32_t context_size,
    std::string& error) {
    if (context_) {
        llama_free(context_);
        context_ = nullptr;
    }
    if (model_) {
        llama_model_free(model_);
        model_ = nullptr;
    }

    auto model_params = llama_model_default_params();
    model_params.n_gpu_layers = gpu_layers;
    model_ = llama_model_load_from_file(model_path.c_str(), model_params);
    if (!model_) {
        error = "llama.cpp could not load the GGUF model: " + model_path;
        return false;
    }
    vocabulary_ = llama_model_get_vocab(model_);

    auto context_params = llama_context_default_params();
    context_params.n_ctx = context_size;
    context_params.n_batch = context_size;
    context_params.n_ubatch = std::min<std::uint32_t>(context_size, 512);
    const auto hardware_threads = std::max(1U, std::thread::hardware_concurrency());
    const auto worker_threads = std::min(hardware_threads, kMaximumCpuThreads);
    context_params.n_threads = static_cast<int>(worker_threads);
    context_params.n_threads_batch = static_cast<int>(worker_threads);

    context_ = llama_init_from_model(model_, context_params);
    if (!context_) {
        error = "llama.cpp could not create an inference context";
        llama_model_free(model_);
        model_ = nullptr;
        vocabulary_ = nullptr;
        return false;
    }
    return true;
}

bool LlamaRewriter::rewrite(
    const std::string& transcript,
    const std::string& source_language,
    std::uint32_t maximum_output_tokens,
    std::string& output,
    std::string& error) {
    output.clear();
    error.clear();
    if (!model_ || !context_ || !vocabulary_) {
        error = "rewrite model is not loaded";
        return false;
    }
    if (transcript.empty()) {
        error = "transcript must not be empty";
        return false;
    }

    const std::string resolved_language = resolve_language_code(source_language, transcript);
    const auto deadline = std::chrono::steady_clock::now() + kMaximumGenerationTime;
    const auto prompt = format_prompt(transcript, resolved_language, false, error);
    if (!error.empty()) {
        return false;
    }

    if (!generate(prompt, maximum_output_tokens, deadline, output, error)) {
        return false;
    }
    if (output_preserves_language(resolved_language, transcript, output)) {
        return true;
    }

    error.clear();
    output.clear();
    const auto retry_prompt = format_prompt(transcript, resolved_language, true, error);
    if (!error.empty() || !generate(retry_prompt, maximum_output_tokens, deadline, output, error)) {
        return false;
    }
    if (!output_preserves_language(resolved_language, transcript, output)) {
        output.clear();
        error = "rewrite changed the source language; translated output was rejected";
        return false;
    }
    return true;
}

bool LlamaRewriter::generate(
    const std::string& prompt,
    std::uint32_t maximum_output_tokens,
    std::chrono::steady_clock::time_point deadline,
    std::string& output,
    std::string& error) {
    output.clear();

    llama_memory_clear(llama_get_memory(context_), true);
    const int token_count = -llama_tokenize(
        vocabulary_, prompt.c_str(), static_cast<int>(prompt.size()), nullptr, 0, true, true);
    if (token_count <= 0) {
        error = "llama.cpp could not size the tokenized prompt";
        return false;
    }
    if (static_cast<std::uint32_t>(token_count) + maximum_output_tokens > llama_n_ctx(context_)) {
        error = "rewrite request exceeds the configured context window";
        return false;
    }

    std::vector<llama_token> prompt_tokens(static_cast<std::size_t>(token_count));
    if (llama_tokenize(
            vocabulary_,
            prompt.c_str(),
            static_cast<int>(prompt.size()),
            prompt_tokens.data(),
            static_cast<int>(prompt_tokens.size()),
            true,
            true) < 0) {
        error = "llama.cpp could not tokenize the prompt";
        return false;
    }

    auto batch = llama_batch_get_one(prompt_tokens.data(), static_cast<int>(prompt_tokens.size()));
    if (llama_decode(context_, batch) != 0) {
        error = "llama.cpp failed while evaluating the rewrite prompt";
        return false;
    }

    llama_sampler* sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(sampler, llama_sampler_init_penalties(
        llama_vocab_n_tokens(vocabulary_),
        kPenaltyLastTokens,
        1.0F,
        0.0F,
        kPresencePenalty));
    llama_sampler_chain_add(sampler, llama_sampler_init_top_k(kTopK));
    llama_sampler_chain_add(sampler, llama_sampler_init_top_p(kTopP, 1));
    llama_sampler_chain_add(sampler, llama_sampler_init_temp(kTemperature));
    llama_sampler_chain_add(sampler, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
    for (std::uint32_t generated = 0; generated < maximum_output_tokens; ++generated) {
        if (std::chrono::steady_clock::now() >= deadline) {
            llama_sampler_free(sampler);
            output.clear();
            error = "rewrite request exceeded the 15-second live latency limit";
            return false;
        }
        const llama_token token = llama_sampler_sample(sampler, context_, -1);
        if (llama_vocab_is_eog(vocabulary_, token)) {
            break;
        }

        std::vector<char> piece(256);
        int piece_size = llama_token_to_piece(
            vocabulary_, token, piece.data(), static_cast<int>(piece.size()), 0, true);
        if (piece_size < 0) {
            piece.resize(static_cast<std::size_t>(-piece_size));
            piece_size = llama_token_to_piece(
                vocabulary_, token, piece.data(), static_cast<int>(piece.size()), 0, true);
        }
        if (piece_size < 0) {
            llama_sampler_free(sampler);
            error = "llama.cpp could not decode a generated token";
            return false;
        }
        output.append(piece.data(), static_cast<std::size_t>(piece_size));

        auto next_batch = llama_batch_get_one(const_cast<llama_token*>(&token), 1);
        if (llama_decode(context_, next_batch) != 0) {
            llama_sampler_free(sampler);
            error = "llama.cpp failed while generating the rewrite";
            return false;
        }
    }

    llama_sampler_free(sampler);
    if (!remove_thinking_prefix(output, error)) {
        return false;
    }
    if (output.empty()) {
        error = "rewrite model returned an empty response";
        return false;
    }
    return true;
}

std::string LlamaRewriter::format_prompt(
    const std::string& transcript,
    const std::string& source_language,
    bool strict_language_retry,
    std::string& error) const {
    const std::string language_name = language_display_name(source_language);
    std::string user_message =
        "Output language: " + language_name + "\n";
    if (strict_language_retry) {
        user_message += "Create the complete polished transcript in " + language_name + ".\n";
    } else {
        user_message += "Polish the transcript while preserving its content accurately.\n";
    }
    user_message += "Transcript:\n" + transcript;
    const llama_chat_message messages[] = {
        {"system", kSystemPrompt},
        {"user", user_message.c_str()},
    };

    const char* chat_template = llama_model_chat_template(model_, nullptr);
    if (!chat_template) {
        return std::string(kSystemPrompt) + "\n\n" + user_message + "\n\nCLEANED TRANSCRIPT:\n";
    }

    std::vector<char> formatted(
        std::char_traits<char>::length(kSystemPrompt) + user_message.size() + 1024);
    int size = llama_chat_apply_template(
        chat_template, messages, 2, true, formatted.data(), static_cast<int>(formatted.size()));
    if (size > static_cast<int>(formatted.size())) {
        formatted.resize(static_cast<std::size_t>(size));
        size = llama_chat_apply_template(
            chat_template, messages, 2, true, formatted.data(), static_cast<int>(formatted.size()));
    }
    if (size < 0) {
        error = "llama.cpp could not apply the model chat template";
        return {};
    }
    std::string prompt(formatted.data(), static_cast<std::size_t>(size));
    // llama_chat_apply_template is the lightweight compatibility API and does
    // not accept the model template's enable_thinking=false argument. Qwen3.5's
    // own template uses this exact assistant prefix for non-thinking mode.
    prompt += kNonThinkingAssistantPrefix;
    return prompt;
}

} // namespace dictscribe::rewrite
