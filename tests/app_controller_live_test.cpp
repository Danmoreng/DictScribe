#include "app/app_controller.hpp"

#include <cassert>
#include <chrono>
#include <functional>
#include <iostream>
#include <thread>

namespace {

using namespace std::chrono_literals;

dictscribe::app::AppSnapshot wait_until(
    dictscribe::app::AppController& controller,
    const std::function<bool(const dictscribe::app::AppSnapshot&)>& predicate,
    std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        controller.tick();
        const auto state = controller.snapshot();
        if (predicate(state)) return state;
        std::this_thread::sleep_for(20ms);
    } while (std::chrono::steady_clock::now() < deadline);
    const auto state = controller.snapshot();
    std::cerr << "Controller timeout: mode=" << static_cast<int>(state.mode)
              << " status=" << state.status
              << " error=" << state.error
              << " requestStatus=" << state.pipeline_debug.rewrite_request_status
              << " decision=" << state.pipeline_debug.rewrite_decision << '\n';
    assert(false && "controller state timed out");
    return {};
}

dictscribe::app::AppConfig config(dictscribe::app::CleanupMode mode) {
    dictscribe::app::AppConfig result;
    result.asr_worker = DICTSCRIBE_FAKE_ASR_WORKER;
    result.rewrite_worker = DICTSCRIBE_FAKE_REWRITE_WORKER;
    result.asr_model = "unused-asr.gguf";
    result.rewrite_model = "unused-rewrite.gguf";
    result.language = "de-DE";
    result.cleanup_mode = mode;
    return result;
}

} // namespace

int main() {
    using dictscribe::app::CleanupMode;
    using dictscribe::app::DictationMode;

    dictscribe::app::AppController raw_controller;
    assert(raw_controller.start(config(CleanupMode::Off)));
    const auto raw_ready = wait_until(raw_controller, [](const auto& state) {
        return state.mode == DictationMode::Ready;
    });
    assert(raw_ready.asr_ready);
    assert(!raw_ready.rewrite_ready);
    assert(raw_ready.cleanup_mode == CleanupMode::Off);

    raw_controller.toggle_recording();
    const auto raw_recording = wait_until(raw_controller, [](const auto& state) {
        return state.mode == DictationMode::Recording &&
            state.live_text.find("llama_rewriter.cpp weiter 1") != std::string::npos;
    });
    assert(!raw_recording.rewrite_in_progress);
    assert(raw_recording.pipeline_debug.asr_event_count >= 1);
    assert(raw_recording.pipeline_debug.asr_stage == "Nemotron partial hypothesis");
    assert(raw_recording.pipeline_debug.nemotron_text.find(
        "llama_rewriter.cpp weiter 1") != std::string::npos);
    assert(raw_recording.pipeline_debug.rewrite_request_json.empty());
    raw_controller.toggle_recording();
    const auto raw_complete = wait_until(raw_controller, [](const auto& state) {
        return state.mode == DictationMode::Complete;
    });
    assert(raw_complete.raw_final_text ==
        "Ich teste die lokale Spracherkennung mit mehreren stabilen Wörtern im Diktat und llama_rewriter.cpp weiter final 1");
    assert(raw_complete.rewritten_text == raw_complete.raw_final_text);

    assert(raw_controller.set_cleanup_mode(CleanupMode::Ai));
    const auto cleanup_ready = wait_until(raw_controller, [](const auto& state) {
        return state.rewrite_ready;
    });
    assert(cleanup_ready.cleanup_mode == CleanupMode::Ai);

    assert(raw_controller.set_asr_device(true));
    wait_until(raw_controller, [](const auto& state) {
        return state.mode == DictationMode::Ready && state.asr_use_gpu;
    });
    assert(raw_controller.set_rewrite_device(true));
    const auto both_gpu = wait_until(raw_controller, [](const auto& state) {
        return state.rewrite_ready && state.rewrite_use_gpu;
    });
    assert(both_gpu.asr_use_gpu);

    raw_controller.toggle_recording();
    const auto cleaning = wait_until(raw_controller, [](const auto& state) {
        return state.mode == DictationMode::Recording && state.rewrite_in_progress;
    });
    assert(cleaning.audio_rms > 0.17F && cleaning.audio_peak > 0.71F);
    assert(cleaning.pipeline_debug.rewrite_request_json.find(
        "\"readOnlyContext\"") != std::string::npos);
    assert(cleaning.pipeline_debug.rewrite_request_json.find(
        "\"editableTail\"") != std::string::npos);
    assert(cleaning.pipeline_debug.rewrite_request_json.find(
        "\"newAsrText\"") != std::string::npos);
    assert(cleaning.pipeline_debug.rewrite_request_status.find(
        "1200 ms quiet debounce") != std::string::npos);
    assert(cleaning.pipeline_debug.rewrite_request_status.find(
        "minimum 8000 ms") != std::string::npos);
    assert(cleaning.pipeline_debug.rewrite_request_status.find(
        "newAsrText contains") != std::string::npos);
    const auto stop_started = std::chrono::steady_clock::now();
    raw_controller.toggle_recording();
    const auto cleaned_complete = wait_until(raw_controller, [](const auto& state) {
        return state.mode == DictationMode::Complete;
    });
    assert(std::chrono::steady_clock::now() - stop_started < 100ms);
    assert(cleaned_complete.raw_final_text.find("weiter final ") != std::string::npos);
    assert(cleaned_complete.rewritten_text.find("weiter final ") != std::string::npos);
    const std::string committed = cleaned_complete.rewritten_text;
    std::this_thread::sleep_for(200ms);
    assert(raw_controller.snapshot().rewritten_text == committed);

    raw_controller.set_language("en-US");
    raw_controller.toggle_recording();
    wait_until(raw_controller, [](const auto& state) {
        return state.mode == DictationMode::Recording &&
            state.live_text.find("further ") != std::string::npos;
    });
    raw_controller.set_language("de-DE");
    const auto switched = wait_until(raw_controller, [](const auto& state) {
        return state.mode == DictationMode::Recording && state.language == "de-DE" &&
            state.live_text.find("further final ") != std::string::npos &&
            state.live_text.find("weiter ") != std::string::npos;
    });
    assert(switched.live_text.find("I am testing") != std::string::npos);
    raw_controller.toggle_recording();
    const auto switched_final = wait_until(raw_controller, [](const auto& state) {
        return state.mode == DictationMode::Complete;
    });
    assert(switched_final.raw_final_text.find("further final ") != std::string::npos);
    assert(switched_final.raw_final_text.find("weiter final ") != std::string::npos);
    assert(switched_final.rewritten_text.find("weiter final ") != std::string::npos);

    assert(raw_controller.set_cleanup_mode(CleanupMode::Off));
    const auto disabled = raw_controller.snapshot();
    assert(disabled.cleanup_mode == CleanupMode::Off);
    assert(!disabled.rewrite_ready);
    assert(raw_controller.set_rewrite_device(false));

    auto rewrite_failure_config = config(CleanupMode::Ai);
    rewrite_failure_config.rewrite_model = "fail-rewrite.gguf";
    dictscribe::app::AppController rewrite_failure_controller;
    assert(rewrite_failure_controller.start(rewrite_failure_config));
    wait_until(rewrite_failure_controller, [](const auto& state) {
        return state.mode == DictationMode::Ready && state.rewrite_ready;
    });
    rewrite_failure_controller.toggle_recording();
    const auto failed_cleanup = wait_until(rewrite_failure_controller, [](const auto& state) {
        return state.mode == DictationMode::Recording && !state.error.empty() &&
            !state.rewrite_in_progress;
    });
    assert(failed_cleanup.live_text.find("llama_rewriter.cpp") != std::string::npos);
    rewrite_failure_controller.toggle_recording();
    const auto failed_complete = wait_until(rewrite_failure_controller, [](const auto& state) {
        return state.mode == DictationMode::Complete;
    });
    assert(failed_complete.rewritten_text.find("weiter final 1") != std::string::npos);

    auto truncating_config = config(CleanupMode::Ai);
    truncating_config.rewrite_model = "truncate-rewrite.gguf";
    dictscribe::app::AppController truncating_controller;
    assert(truncating_controller.start(truncating_config));
    wait_until(truncating_controller, [](const auto& state) {
        return state.mode == DictationMode::Ready && state.rewrite_ready;
    });
    truncating_controller.toggle_recording();
    wait_until(truncating_controller, [](const auto& state) {
        return state.mode == DictationMode::Recording &&
            !state.rewrite_in_progress &&
            !state.pipeline_debug.rewrite_response_request_id.empty() &&
            state.pipeline_debug.rewrite_decision.find("Accepted") != std::string::npos &&
            state.live_text.find("Ich teste die lokale Spracherkennung") != std::string::npos;
    });
    truncating_controller.set_language("en-US");
    const auto preserved_preview = wait_until(truncating_controller, [](const auto& state) {
        return state.mode == DictationMode::Recording &&
            state.error.find("incomplete tail") != std::string::npos &&
            state.live_text.find("Ich teste die lokale Spracherkennung") != std::string::npos &&
            state.live_text.find("I am testing the local speech") != std::string::npos;
    });
    assert(preserved_preview.rewritten_text == preserved_preview.live_text);
    assert(preserved_preview.pipeline_debug.rewrite_response_json.find(
        "\"replacementTail\"") != std::string::npos);
    assert(!preserved_preview.pipeline_debug.rewrite_response_request_id.empty());
    assert(preserved_preview.pipeline_debug.rewrite_decision.find(
        "Preserved raw") != std::string::npos);
    assert(preserved_preview.pipeline_debug.composed_text ==
        preserved_preview.live_text);
    truncating_controller.toggle_recording();
    const auto preserved_complete = wait_until(truncating_controller, [](const auto& state) {
        return state.mode == DictationMode::Complete;
    });
    assert(preserved_complete.rewritten_text.find(
        "Ich teste die lokale Spracherkennung") != std::string::npos);
    assert(preserved_complete.rewritten_text.find(
        "I am testing the local speech") != std::string::npos);

    auto load_failure_config = config(CleanupMode::Ai);
    load_failure_config.rewrite_model = "fail-load-rewrite.gguf";
    dictscribe::app::AppController load_failure_controller;
    assert(load_failure_controller.start(load_failure_config));
    const auto load_failure_ready = wait_until(load_failure_controller, [](const auto& state) {
        return state.mode == DictationMode::Ready;
    });
    assert(load_failure_ready.asr_ready);
    assert(!load_failure_ready.rewrite_ready);
    assert(!load_failure_ready.error.empty());
    assert(load_failure_controller.set_cleanup_mode(CleanupMode::Off));
    assert(load_failure_controller.set_cleanup_mode(CleanupMode::Ai));
    const auto retried_failure = wait_until(load_failure_controller, [](const auto& state) {
        return state.mode == DictationMode::Ready && !state.rewrite_ready &&
            !state.error.empty();
    });
    assert(retried_failure.cleanup_mode == CleanupMode::Ai);

    auto recovery_config = config(CleanupMode::Off);
    recovery_config.asr_model = "fail-gpu-asr.gguf";
    dictscribe::app::AppController recovery_controller;
    assert(recovery_controller.start(recovery_config));
    wait_until(recovery_controller, [](const auto& state) {
        return state.mode == DictationMode::Ready;
    });
    assert(recovery_controller.set_asr_device(true));
    const auto gpu_failure = wait_until(recovery_controller, [](const auto& state) {
        return state.mode == DictationMode::Error;
    });
    assert(dictscribe::app::CanSetComputeDevice(gpu_failure));
    assert(recovery_controller.set_asr_device(false));
    const auto recovered = wait_until(recovery_controller, [](const auto& state) {
        return state.mode == DictationMode::Ready;
    });
    assert(!recovered.asr_use_gpu && recovered.asr_ready);
    assert(!recovered.rewrite_ready);

    std::cout << "Bounded cleanup controller tests passed\n";
    return 0;
}
