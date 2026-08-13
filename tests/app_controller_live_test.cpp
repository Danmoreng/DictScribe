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
    dictscribe::app::AppSnapshot starting_snapshot;
    assert(dictscribe::app::CanSetComputeDevice(starting_snapshot));

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

    assert(controller.set_asr_device(true));
    const auto restarting_asr = controller.snapshot();
    assert(restarting_asr.rewrite_ready);
    const auto asr_gpu = wait_until(controller, [](const auto& state) {
        return state.mode == dictscribe::app::DictationMode::Ready;
    });
    assert(asr_gpu.asr_use_gpu);
    assert(!asr_gpu.rewrite_use_gpu);

    assert(controller.set_asr_device(false));
    const auto both_cpu_again = wait_until(controller, [](const auto& state) {
        return state.mode == dictscribe::app::DictationMode::Ready;
    });
    assert(!both_cpu_again.asr_use_gpu);
    assert(!both_cpu_again.rewrite_use_gpu);

    assert(controller.set_rewrite_device(true));
    const auto restarting_rewrite = controller.snapshot();
    assert(restarting_rewrite.asr_ready);
    const auto rewrite_gpu = wait_until(controller, [](const auto& state) {
        return state.mode == dictscribe::app::DictationMode::Ready;
    });
    assert(!rewrite_gpu.asr_use_gpu);
    assert(rewrite_gpu.rewrite_use_gpu);

    assert(controller.set_asr_device(true));
    const auto both_gpu = wait_until(controller, [](const auto& state) {
        return state.mode == dictscribe::app::DictationMode::Ready;
    });
    assert(both_gpu.asr_use_gpu && both_gpu.rewrite_use_gpu);

    controller.toggle_recording();
    const auto cleaning = wait_until(controller, [](const auto& state) {
        return state.mode == dictscribe::app::DictationMode::Recording &&
            state.rewrite_in_progress;
    });
    assert(cleaning.audio_rms > 0.17F && cleaning.audio_peak > 0.71F);

    const auto stop_started = std::chrono::steady_clock::now();
    controller.toggle_recording();
    const auto first_complete = wait_until(controller, [](const auto& state) {
        return state.mode == dictscribe::app::DictationMode::Complete;
    });
    assert(std::chrono::steady_clock::now() - stop_started < 100ms);
    assert(first_complete.raw_final_text == "Ich teste llama_rewriter.cpp weiter final 1");
    assert(first_complete.rewritten_text == first_complete.raw_final_text);
    assert(first_complete.audio_rms == 0.0F && first_complete.audio_peak == 0.0F);
    std::this_thread::sleep_for(200ms);
    assert(controller.snapshot().rewritten_text == first_complete.raw_final_text);

    controller.toggle_recording();
    wait_until(controller, [](const auto& state) {
        return state.mode == dictscribe::app::DictationMode::Recording &&
            state.rewritten_text.find("llama_rewriter.cpp weiter 2") != std::string::npos;
    });
    controller.toggle_recording();
    const auto second_complete = wait_until(controller, [](const auto& state) {
        return state.mode == dictscribe::app::DictationMode::Complete;
    });
    assert(second_complete.rewritten_text == second_complete.raw_final_text);

    controller.set_language("en");
    controller.toggle_recording();
    wait_until(controller, [](const auto& state) {
        return state.mode == dictscribe::app::DictationMode::Recording &&
            state.live_text.find("further 3") != std::string::npos;
    });
    controller.set_language("de");
    const auto switched = wait_until(controller, [](const auto& state) {
        return state.mode == dictscribe::app::DictationMode::Recording &&
            state.language == "de" &&
            state.live_text.find("final 3") != std::string::npos &&
            state.live_text.find("weiter 4") != std::string::npos;
    });
    assert(switched.live_text ==
        "Ich teste llama_rewriter.cpp weiter final 3 Ich äh teste llama_rewriter.cpp weiter 4");
    controller.toggle_recording();
    const auto switched_final = wait_until(controller, [](const auto& state) {
        return state.mode == dictscribe::app::DictationMode::Complete;
    });
    assert(switched_final.raw_final_text ==
        "Ich teste llama_rewriter.cpp weiter final 3 Ich teste llama_rewriter.cpp weiter final 4");
    assert(switched_final.rewritten_text == switched_final.raw_final_text);

    dictscribe::app::AppConfig rewrite_failure_config = config;
    rewrite_failure_config.rewrite_model = "fail-rewrite.gguf";
    dictscribe::app::AppController rewrite_failure_controller;
    assert(rewrite_failure_controller.start(rewrite_failure_config));
    wait_until(rewrite_failure_controller, [](const auto& state) {
        return state.mode == dictscribe::app::DictationMode::Ready;
    });
    rewrite_failure_controller.toggle_recording();
    const auto raw_recording = wait_until(rewrite_failure_controller, [](const auto& state) {
        return state.mode == dictscribe::app::DictationMode::Recording &&
            !state.error.empty() && !state.rewrite_in_progress;
    });
    assert(raw_recording.rewritten_text == raw_recording.live_text);
    rewrite_failure_controller.toggle_recording();
    const auto raw_complete = wait_until(rewrite_failure_controller, [](const auto& state) {
        return state.mode == dictscribe::app::DictationMode::Complete;
    });
    assert(raw_complete.rewritten_text == raw_complete.raw_final_text);

    dictscribe::app::AppConfig rewrite_load_failure_config = config;
    rewrite_load_failure_config.rewrite_model = "fail-load-rewrite.gguf";
    dictscribe::app::AppController rewrite_load_failure_controller;
    assert(rewrite_load_failure_controller.start(rewrite_load_failure_config));
    const auto raw_ready = wait_until(rewrite_load_failure_controller, [](const auto& state) {
        return state.mode == dictscribe::app::DictationMode::Ready;
    });
    assert(raw_ready.asr_ready);
    assert(!raw_ready.rewrite_ready);
    assert(!raw_ready.error.empty());

    dictscribe::app::AppConfig recovery_config = config;
    recovery_config.asr_model = "fail-gpu-asr.gguf";
    dictscribe::app::AppController recovery_controller;
    assert(recovery_controller.start(recovery_config));
    wait_until(recovery_controller, [](const auto& state) {
        return state.mode == dictscribe::app::DictationMode::Ready;
    });
    assert(recovery_controller.set_asr_device(true));
    const auto gpu_failure = wait_until(recovery_controller, [](const auto& state) {
        return state.mode == dictscribe::app::DictationMode::Error;
    });
    assert(dictscribe::app::CanSetComputeDevice(gpu_failure));
    assert(recovery_controller.set_asr_device(false));
    const auto recovered = wait_until(recovery_controller, [](const auto& state) {
        return state.mode == dictscribe::app::DictationMode::Ready;
    });
    assert(!recovered.asr_use_gpu);
    assert(recovered.asr_ready && recovered.rewrite_ready);

    std::cout << "Live cleanup controller tests passed\n";
    return 0;
}
