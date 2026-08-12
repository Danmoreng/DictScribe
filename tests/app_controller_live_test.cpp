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
    std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        controller.tick();
        const auto state = controller.snapshot();
        if (predicate(state)) {
            return state;
        }
        std::this_thread::sleep_for(20ms);
    } while (std::chrono::steady_clock::now() < deadline);
    assert(false && "controller state timed out");
    return {};
}

} // namespace

int main() {
    dictscribe::app::AppConfig config;
    config.asr_worker = DICTSCRIBE_FAKE_ASR_WORKER;
    config.rewrite_worker = DICTSCRIBE_FAKE_REWRITE_WORKER;
    config.asr_model = "unused-asr.gguf";
    config.rewrite_model = "unused-rewrite.gguf";
    config.language = "de";

    dictscribe::app::AppController controller;
    assert(controller.start(config));
    wait_until(controller, [](const auto& state) {
        return state.mode == dictscribe::app::DictationMode::Ready;
    });

    controller.toggle_recording();
    const auto live = wait_until(controller, [](const auto& state) {
        return state.mode == dictscribe::app::DictationMode::Recording &&
            state.rewritten_text.find("llama_rewriter.cpp weiter 1") != std::string::npos;
    });
    assert(live.rewritten_text.starts_with("de:"));
    assert(!live.rewrite_in_progress);
    assert(live.audio_rms > 0.17F && live.audio_peak > 0.71F);

    controller.toggle_recording();
    const auto with_final = wait_until(controller, [](const auto& state) {
        return state.mode == dictscribe::app::DictationMode::Complete;
    });
    assert(with_final.rewritten_text == "de:Ich teste llama_rewriter.cpp weiter final 1");
    assert(with_final.audio_rms == 0.0F && with_final.audio_peak == 0.0F);

    controller.set_final_cleanup_enabled(false);
    controller.toggle_recording();
    wait_until(controller, [](const auto& state) {
        return state.mode == dictscribe::app::DictationMode::Recording &&
            state.rewritten_text.find("llama_rewriter.cpp weiter 2") != std::string::npos;
    });
    controller.toggle_recording();
    const auto live_only = wait_until(controller, [](const auto& state) {
        return state.mode == dictscribe::app::DictationMode::Complete;
    });
    assert(live_only.rewritten_text == "de:Ich äh teste llama_rewriter.cpp weiter 2");
    assert(!live_only.final_cleanup_enabled);

    std::cout << "Live cleanup controller tests passed\n";
    return 0;
}
