#pragma once

#include "app/worker_process.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace dictscribe::app {

struct AppConfig {
    std::filesystem::path asr_worker;
    std::filesystem::path rewrite_worker;
    std::filesystem::path asr_model;
    std::filesystem::path rewrite_model;
    std::string language = "auto";
    bool asr_use_gpu = false;
    bool rewrite_use_gpu = false;
};

enum class DictationMode {
    Starting,
    Ready,
    StartingRecording,
    Recording,
    Finalizing,
    Complete,
    Cancelling,
    Error,
};

struct AppSnapshot {
    DictationMode mode = DictationMode::Starting;
    bool asr_ready = false;
    bool rewrite_ready = false;
    bool rewrite_in_progress = false;
    float audio_rms = 0.0F;
    float audio_peak = 0.0F;
    std::string status = "Starting local models...";
    std::string live_text;
    std::string raw_final_text;
    std::string rewritten_text;
    std::string error;
    std::string asr_model_name;
    std::string rewrite_model_name;
    std::string language = "auto";
    bool asr_use_gpu = false;
    bool rewrite_use_gpu = false;
};

class AppController {
public:
    AppController() = default;
    ~AppController();

    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    bool start(const AppConfig& config);
    void set_startup_error(std::string message);
    void toggle_recording();
    void cancel_recording();
    void set_language(std::string language);
    bool set_asr_device(bool use_gpu);
    bool set_rewrite_device(bool use_gpu);
    void tick();
    [[nodiscard]] AppSnapshot snapshot() const;

private:
    bool start_asr_worker(const AppConfig& config, std::string& error);
    bool start_rewrite_worker(const AppConfig& config, std::string& error);
    bool start_recording_locked(bool clear_transcript);
    bool stop_for_language_change_locked();
    void handle_asr_message(const nlohmann::json& message);
    void handle_rewrite_message(const nlohmann::json& message);
    void update_ready_state_locked();
    void update_transcript_locked(std::string text);
    bool dispatch_rewrite_locked();
    void finish_dictation_locked();
    void set_error_locked(std::string message);
    std::string next_id_locked(const char* prefix);

    mutable std::mutex mutex_;
    AppSnapshot state_;
    WorkerProcess asr_;
    WorkerProcess rewrite_;
    AppConfig config_;
    std::uint64_t request_sequence_ = 0;
    bool rewrite_in_flight_ = false;
    bool finalization_waiting_ = false;
    bool language_restart_pending_ = false;
    bool pending_live_cleanup_ = false;
    std::chrono::steady_clock::time_point live_cleanup_pending_since_{};
    std::chrono::steady_clock::time_point live_cleanup_due_{};
    std::string active_rewrite_id_;
    std::string active_rewrite_session_id_;
    std::string session_id_;
    std::string session_prefix_;
    std::string current_session_text_;
};

[[nodiscard]] bool CanToggle(const AppSnapshot& snapshot);
[[nodiscard]] bool CanCancel(const AppSnapshot& snapshot);
[[nodiscard]] bool CanSetLanguage(const AppSnapshot& snapshot);
[[nodiscard]] bool CanSetComputeDevice(const AppSnapshot& snapshot);
[[nodiscard]] const char* PrimaryButtonLabel(const AppSnapshot& snapshot);
[[nodiscard]] const char* LanguageLabel(const AppSnapshot& snapshot);

} // namespace dictscribe::app
