#include "app/app_controller.hpp"
#include "app/language_catalog.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <utility>

namespace dictscribe::app {

namespace {

constexpr auto kFinalCleanupTimeout = std::chrono::seconds(35);

std::size_t ApproximateWordCount(std::string_view text) {
    std::size_t count = 0;
    bool in_word = false;
    for (const unsigned char value : text) {
        const bool whitespace = std::isspace(value) != 0;
        if (!whitespace && !in_word) ++count;
        in_word = !whitespace;
    }
    return count;
}

std::string FileName(const std::filesystem::path& path) {
    return path.filename().string();
}

} // namespace

AppController::~AppController() {
    std::string ignored;
    if (asr_.running()) {
        asr_.send({{"v", 1}, {"type", "shutdown"}, {"id", "ui-shutdown-asr"}}, ignored);
    }
    if (rewrite_.running()) {
        rewrite_.send({{"v", 1}, {"type", "shutdown"}, {"id", "ui-shutdown-rewrite"}}, ignored);
    }
    asr_.stop();
    rewrite_.stop();
}

bool AppController::start(const AppConfig& config) {
    {
        std::lock_guard lock(mutex_);
        config_ = config;
        const std::string language = CanonicalLanguageCode(config.language);
        config_.language = language.empty() ? "auto" : language;
        state_ = {};
        state_.asr_model_name = FileName(config.asr_model);
        state_.rewrite_model_name = FileName(config.rewrite_model);
        state_.language = config_.language;
        state_.cleanup_mode = config.cleanup_mode;
        state_.asr_use_gpu = config.asr_use_gpu;
        state_.rewrite_use_gpu = config.rewrite_use_gpu;
        rewrite_unavailable_ = false;
        language_restart_pending_ = false;
        active_rewrite_.reset();
        session_id_.clear();
        dictation_id_.clear();
        transcript_.reset(0);
    }

    std::string error;
    if (!start_asr_worker(config, error)) {
        std::lock_guard lock(mutex_);
        set_error_locked(std::move(error));
        return false;
    }
    if (config.cleanup_mode == CleanupMode::Ai && !start_rewrite_worker(config, error)) {
        std::lock_guard lock(mutex_);
        rewrite_unavailable_ = true;
        state_.rewrite_ready = false;
        state_.error = std::move(error);
    }
    return true;
}

bool AppController::start_asr_worker(const AppConfig& config, std::string& error) {
    std::vector<std::string> arguments = {
        "--stdio", "--model", config.asr_model.string(), "--protocol-version", "1"};
    if (config.asr_use_gpu) arguments.push_back("--gpu");
    return asr_.start(
        config.asr_worker.string(), arguments,
        [this](const nlohmann::json& message) { handle_asr_message(message); }, error);
}

bool AppController::start_rewrite_worker(const AppConfig& config, std::string& error) {
    std::vector<std::string> arguments = {
        "--stdio", "--model", config.rewrite_model.string(), "--protocol-version", "2",
        "--context-size", "4096"};
    if (config.rewrite_use_gpu) {
        arguments.insert(arguments.end(), {"--gpu-layers", "99"});
    }
    return rewrite_.start(
        config.rewrite_worker.string(), arguments,
        [this](const nlohmann::json& message) { handle_rewrite_message(message); }, error);
}

void AppController::set_startup_error(std::string message) {
    std::lock_guard lock(mutex_);
    set_error_locked(std::move(message));
}

void AppController::toggle_recording() {
    std::lock_guard lock(mutex_);
    if (state_.mode == DictationMode::Ready || state_.mode == DictationMode::Complete) {
        start_recording_locked(true);
        return;
    }
    if (state_.mode != DictationMode::Recording) return;

    std::string error;
    state_.mode = DictationMode::Finalizing;
    state_.audio_rms = 0.0F;
    state_.audio_peak = 0.0F;
    state_.status = "Finalizing speech recognition...";
    if (!asr_.send({
            {"v", 1}, {"type", "stop"}, {"id", next_id_locked("stop")},
            {"sessionId", session_id_}}, error)) {
        set_error_locked(std::move(error));
    }
}

bool AppController::start_recording_locked(bool clear_transcript) {
    if (clear_transcript) {
        ++dictation_generation_;
        transcript_.reset(dictation_generation_);
        state_.pipeline_debug = {};
        state_.live_text.clear();
        state_.raw_final_text.clear();
        state_.rewritten_text.clear();
        state_.insertion_confirmation_required = false;
        state_.error.clear();
        dictation_id_ = next_id_locked("dictation");
        language_restart_pending_ = false;
        clear_active_rewrite_locked();
    }
    transcript_.begin_asr_segment();
    session_id_ = next_id_locked("session");
    state_.audio_rms = 0.0F;
    state_.audio_peak = 0.0F;
    state_.mode = DictationMode::StartingRecording;
    state_.status = state_.cleanup_mode == CleanupMode::Ai
        ? "Opening the microphone - cleanup runs after dictation"
        : "Opening the default microphone...";

    std::string error;
    if (!asr_.send({
            {"v", 1}, {"type", "start"}, {"id", next_id_locked("start")},
            {"sessionId", session_id_}, {"language", state_.language}}, error)) {
        language_restart_pending_ = false;
        set_error_locked(std::move(error));
        return false;
    }
    return true;
}

bool AppController::stop_for_language_change_locked() {
    language_restart_pending_ = true;
    if (state_.mode == DictationMode::StartingRecording) {
        state_.status = "Waiting for the microphone before switching language...";
        return true;
    }
    if (state_.mode != DictationMode::Recording) return false;
    state_.mode = DictationMode::Finalizing;
    state_.audio_rms = 0.0F;
    state_.audio_peak = 0.0F;
    state_.status = "Switching speech recognition language...";
    std::string error;
    if (!asr_.send({
            {"v", 1}, {"type", "stop"}, {"id", next_id_locked("language-stop")},
            {"sessionId", session_id_}}, error)) {
        language_restart_pending_ = false;
        set_error_locked(std::move(error));
        return false;
    }
    return true;
}

void AppController::cancel_recording() {
    std::lock_guard lock(mutex_);
    if (state_.mode == DictationMode::Complete &&
        state_.insertion_confirmation_required) {
        ++dictation_generation_;
        transcript_.reset(dictation_generation_);
        clear_active_rewrite_locked();
        state_.mode = DictationMode::Ready;
        state_.status = "Cleanup result discarded - ready for another dictation";
        state_.live_text.clear();
        state_.raw_final_text.clear();
        state_.rewritten_text.clear();
        state_.insertion_confirmation_required = false;
        state_.pipeline_debug = {};
        session_id_.clear();
        dictation_id_.clear();
        return;
    }
    if (state_.mode != DictationMode::Recording &&
        state_.mode != DictationMode::StartingRecording) return;
    state_.mode = DictationMode::Cancelling;
    state_.audio_rms = 0.0F;
    state_.audio_peak = 0.0F;
    state_.status = "Cancelling dictation...";
    std::string error;
    if (!asr_.send({
            {"v", 1}, {"type", "cancel"}, {"id", next_id_locked("cancel")},
            {"sessionId", session_id_}}, error)) {
        set_error_locked(std::move(error));
    }
}

void AppController::set_language(std::string language) {
    std::lock_guard lock(mutex_);
    const std::string canonical = CanonicalLanguageCode(language);
    if (!CanSetLanguage(state_) || canonical.empty() || canonical == state_.language) return;
    state_.language = canonical;
    if (state_.mode == DictationMode::Recording ||
        state_.mode == DictationMode::StartingRecording) {
        stop_for_language_change_locked();
    } else if (state_.mode == DictationMode::Finalizing && language_restart_pending_) {
        state_.status = "Switching speech recognition language...";
    } else if (state_.mode == DictationMode::Ready || state_.mode == DictationMode::Complete) {
        state_.status = "Language changed - ready for dictation";
    }
}

bool AppController::set_cleanup_mode(CleanupMode mode) {
    AppConfig config;
    bool stop_worker = false;
    {
        std::lock_guard lock(mutex_);
        if (!CanSetCleanupMode(state_)) return false;
        if (state_.cleanup_mode == mode) return true;
        config_.cleanup_mode = mode;
        config = config_;
        state_.cleanup_mode = mode;
        state_.error.clear();
        clear_active_rewrite_locked();
        if (mode == CleanupMode::Off) {
            state_.rewrite_ready = false;
            rewrite_unavailable_ = false;
            state_.status = "AI cleanup disabled - ready for raw dictation";
            stop_worker = true;
        } else {
            rewrite_unavailable_ = false;
            state_.status = "Starting AI cleanup model...";
        }
    }

    if (stop_worker) {
        std::string ignored;
        rewrite_.send({{"v", 1}, {"type", "shutdown"}, {"id", "disable-rewrite"}}, ignored);
        rewrite_.stop();
        return true;
    }
    if (mode == CleanupMode::Ai && !rewrite_.running()) {
        // Reap a previously failed lazy worker before reusing WorkerProcess.
        // Its reader thread remains joinable after an unexpected process exit.
        rewrite_.stop();
        std::string error;
        if (!start_rewrite_worker(config, error)) {
            std::lock_guard lock(mutex_);
            rewrite_unavailable_ = true;
            state_.rewrite_ready = false;
            state_.error = std::move(error);
            state_.status = "AI cleanup unavailable - raw dictation remains ready";
        }
    }
    return true;
}

bool AppController::set_asr_device(bool use_gpu) {
    AppConfig config;
    {
        std::lock_guard lock(mutex_);
        if (!CanSetComputeDevice(state_)) return false;
        if (config_.asr_use_gpu == use_gpu) return true;
        config_.asr_use_gpu = use_gpu;
        config = config_;
        state_.asr_use_gpu = use_gpu;
        state_.asr_ready = false;
        state_.mode = DictationMode::Starting;
        state_.error.clear();
        state_.status = use_gpu
            ? "Restarting speech recognition on GPU..."
            : "Restarting speech recognition on CPU...";
    }
    std::string ignored;
    if (asr_.running()) {
        asr_.send({{"v", 1}, {"type", "shutdown"}, {"id", "ui-restart-asr"}}, ignored);
    }
    asr_.stop();
    std::string error;
    if (!start_asr_worker(config, error)) {
        std::lock_guard lock(mutex_);
        set_error_locked(std::move(error));
        return false;
    }
    return true;
}

bool AppController::set_rewrite_device(bool use_gpu) {
    AppConfig config;
    bool restart = false;
    {
        std::lock_guard lock(mutex_);
        if (!CanSetComputeDevice(state_)) return false;
        if (config_.rewrite_use_gpu == use_gpu) return true;
        config_.rewrite_use_gpu = use_gpu;
        config = config_;
        state_.rewrite_use_gpu = use_gpu;
        restart = state_.cleanup_mode == CleanupMode::Ai;
        if (!restart) return true;
        state_.rewrite_ready = false;
        rewrite_unavailable_ = false;
        clear_active_rewrite_locked();
        state_.status = use_gpu
            ? "Restarting AI cleanup on GPU..."
            : "Restarting AI cleanup on CPU...";
    }
    std::string ignored;
    if (rewrite_.running()) {
        rewrite_.send({{"v", 1}, {"type", "shutdown"}, {"id", "ui-restart-rewrite"}}, ignored);
    }
    rewrite_.stop();
    std::string error;
    if (!start_rewrite_worker(config, error)) {
        std::lock_guard lock(mutex_);
        rewrite_unavailable_ = true;
        state_.error = std::move(error);
        state_.status = "AI cleanup unavailable - raw dictation remains ready";
    }
    return true;
}

void AppController::tick() {
    std::lock_guard lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (active_rewrite_ && now - active_rewrite_->started >= kFinalCleanupTimeout) {
        const std::string timed_out_id = active_rewrite_->id;
        state_.pipeline_debug.rewrite_response_request_id = timed_out_id;
        state_.pipeline_debug.rewrite_response_json.clear();
        state_.pipeline_debug.rewrite_request_status = "Timed out";
        clear_active_rewrite_locked();
        state_.pipeline_debug.rewrite_decision =
            "Preserved raw: final cleanup timed out without changing the dictation.";
        complete_with_raw_fallback_locked(
            "Cleanup timed out - review the raw transcript and press Enter to insert",
            "AI cleanup timed out; the complete raw transcript was preserved.");
    }
}

AppSnapshot AppController::snapshot() const {
    std::lock_guard lock(mutex_);
    return state_;
}

void AppController::handle_asr_message(const nlohmann::json& message) {
    std::lock_guard lock(mutex_);
    const std::string type = message.value("type", "");
    if (type == "loading_model") {
        state_.status = "Loading speech recognition model...";
    } else if (type == "ready") {
        state_.asr_ready = true;
        update_ready_state_locked();
    } else if (type == "recording_started") {
        if (message.value("sessionId", "") != session_id_) return;
        state_.mode = DictationMode::Recording;
        state_.status = state_.cleanup_mode == CleanupMode::Ai
            ? (state_.rewrite_ready ? "Listening - cleanup will run after dictation"
                                    : "Listening - cleanup model is still loading")
            : "Listening - raw transcription";
        if (language_restart_pending_) stop_for_language_change_locked();
    } else if (type == "audio_level") {
        if (state_.mode == DictationMode::Recording &&
            message.value("sessionId", "") == session_id_) {
            state_.audio_rms = std::clamp(message.value("rms", 0.0F), 0.0F, 1.0F);
            state_.audio_peak = std::clamp(message.value("peak", 0.0F), 0.0F, 1.0F);
        }
    } else if (type == "transcript_update") {
        if (message.value("sessionId", "") != session_id_) return;
        ++state_.pipeline_debug.asr_event_count;
        state_.pipeline_debug.asr_stage = "Nemotron partial hypothesis";
        state_.pipeline_debug.nemotron_text = message.value("text", "");
        transcript_.update_asr_hypothesis(state_.pipeline_debug.nemotron_text);
        refresh_transcript_locked();
    } else if (type == "recording_finalized") {
        if (message.value("sessionId", "") != session_id_) return;
        state_.audio_rms = 0.0F;
        state_.audio_peak = 0.0F;
        ++state_.pipeline_debug.asr_event_count;
        state_.pipeline_debug.asr_stage = "Nemotron final hypothesis";
        state_.pipeline_debug.nemotron_text = message.value("text", "");
        transcript_.finalize_asr_hypothesis(state_.pipeline_debug.nemotron_text);
        state_.raw_final_text = transcript_.raw_text();
        refresh_transcript_locked();
        if (language_restart_pending_) {
            language_restart_pending_ = false;
            start_recording_locked(false);
            return;
        }
        if (state_.raw_final_text.empty()) {
            clear_active_rewrite_locked();
            state_.mode = DictationMode::Complete;
            state_.status = "No speech recognized - press Enter to try again";
        } else {
            finish_dictation_locked();
        }
    } else if (type == "recording_cancelled") {
        ++dictation_generation_;
        transcript_.reset(dictation_generation_);
        clear_active_rewrite_locked();
        state_.mode = DictationMode::Ready;
        state_.audio_rms = 0.0F;
        state_.audio_peak = 0.0F;
        state_.status = "Cancelled - ready for another dictation";
        state_.live_text.clear();
        state_.raw_final_text.clear();
        state_.rewritten_text.clear();
        state_.insertion_confirmation_required = false;
        state_.pipeline_debug = {};
        language_restart_pending_ = false;
        session_id_.clear();
        dictation_id_.clear();
    } else if (type == "warning") {
        state_.status = message.value("message", "Audio processing warning");
    } else if (type == "error") {
        const std::string text = message.value("message", "Speech worker error");
        if (message.value("recoverable", false) &&
            message.value("code", "") == "MICROPHONE_UNAVAILABLE") {
            state_.mode = DictationMode::Ready;
            state_.audio_rms = 0.0F;
            state_.audio_peak = 0.0F;
            state_.status = "Microphone unavailable - check the default input and try again";
            state_.error = text;
            session_id_.clear();
        } else {
            set_error_locked(text);
        }
    }
}

void AppController::handle_rewrite_message(const nlohmann::json& message) {
    std::lock_guard lock(mutex_);
    if (state_.cleanup_mode != CleanupMode::Ai) return;
    const std::string type = message.value("type", "");
    if (type == "ready") {
        rewrite_unavailable_ = false;
        state_.rewrite_ready = true;
        state_.error.clear();
        if (state_.mode == DictationMode::Recording) {
            state_.status = "Listening - cleanup will run after dictation";
        } else if (state_.mode == DictationMode::Finalizing &&
                   !state_.raw_final_text.empty() && !active_rewrite_) {
            dispatch_final_cleanup_locked();
        } else if (state_.mode == DictationMode::Ready) {
            state_.status = "Ready - AI cleanup loaded";
        }
    } else if (type == "rewrite_completed") {
        if (!active_rewrite_ || message.value("requestId", "") != active_rewrite_->id) return;
        const std::string completed_id = active_rewrite_->id;
        state_.pipeline_debug.rewrite_response_request_id = completed_id;
        state_.pipeline_debug.rewrite_response_json = message.dump(2);
        clear_active_rewrite_locked();
        const std::string cleaned = message.value("text", "");
        if (cleaned.empty()) {
            state_.pipeline_debug.rewrite_decision =
                "Preserved raw: final cleanup returned an empty transcript.";
            complete_with_raw_fallback_locked(
                "Cleanup returned no text - review the raw transcript and press Enter to insert",
                "AI cleanup returned an empty result; the complete raw transcript was preserved.");
            return;
        }
        state_.rewritten_text = cleaned;
        state_.live_text = cleaned;
        state_.pipeline_debug.composed_text = cleaned;
        state_.pipeline_debug.rewrite_request_status = "Completed";
        state_.pipeline_debug.rewrite_decision =
            "Accepted: the complete final transcript was replaced by the cleanup result.";
        state_.error.clear();
        state_.insertion_confirmation_required = true;
        state_.mode = DictationMode::Complete;
        state_.status = "Cleanup complete - review the text and press Enter to insert";
    } else if (type == "error") {
        const std::string failed_id = active_rewrite_
            ? active_rewrite_->id : state_.pipeline_debug.rewrite_request_id;
        state_.pipeline_debug.rewrite_response_request_id = failed_id;
        state_.pipeline_debug.rewrite_response_json = message.dump(2);
        state_.pipeline_debug.rewrite_request_status = "Worker returned an error";
        const bool fatal = !message.value("recoverable", false) ||
            message.value("code", "") == "WORKER_EXITED" ||
            message.value("code", "") == "MODEL_LOAD_FAILED";
        clear_active_rewrite_locked();
        state_.pipeline_debug.rewrite_decision =
            "Preserved raw: final cleanup failed without changing the dictation.";
        if (fatal) {
            rewrite_unavailable_ = true;
            state_.rewrite_ready = false;
        }
        const std::string error = message.value("message", "Rewrite worker error");
        state_.error = error;
        if (state_.mode == DictationMode::Finalizing && !state_.raw_final_text.empty()) {
            complete_with_raw_fallback_locked(
                "Cleanup failed - review the raw transcript and press Enter to insert", error);
        } else if (state_.mode == DictationMode::Ready || state_.mode == DictationMode::Complete) {
            state_.status = "AI cleanup unavailable - raw dictation remains ready";
        }
    }
}

void AppController::update_ready_state_locked() {
    if (state_.asr_ready && state_.mode == DictationMode::Starting) {
        state_.mode = DictationMode::Ready;
        state_.status = state_.cleanup_mode == CleanupMode::Ai
            ? (state_.rewrite_ready ? "Ready - AI cleanup loaded"
                                    : "Ready - AI cleanup is loading; raw dictation is available")
            : "Ready - raw dictation; AI cleanup is off";
    } else if (!state_.asr_ready) {
        state_.status = "Loading speech recognition model...";
    }
}

void AppController::refresh_transcript_locked() {
    state_.live_text = transcript_.raw_text();
    state_.pipeline_debug.composed_text = state_.live_text;
}

bool AppController::dispatch_final_cleanup_locked() {
    if (active_rewrite_ || !state_.rewrite_ready ||
        state_.cleanup_mode != CleanupMode::Ai || state_.raw_final_text.empty()) return false;
    const auto now = std::chrono::steady_clock::now();
    ActiveRewrite active;
    active.id = next_id_locked("rewrite-final");
    active.started = now;
    active_rewrite_ = active;
    state_.rewrite_in_progress = true;
    state_.status = "Cleaning up the complete dictation...";

    const nlohmann::json command = {
            {"v", 1}, {"type", "rewrite"}, {"id", active.id},
            {"requestId", active.id}, {"sessionId", dictation_id_},
            {"language", state_.language},
            {"text", state_.raw_final_text}};
    state_.pipeline_debug.rewrite_request_id = active.id;
    const std::size_t word_count = ApproximateWordCount(state_.raw_final_text);
    std::ostringstream request_status;
    request_status << "Pending one final model response for the complete transcript: "
                   << word_count << " approximate words / "
                   << state_.raw_final_text.size() << " UTF-8 bytes.";
    state_.pipeline_debug.rewrite_request_status = request_status.str();
    state_.pipeline_debug.rewrite_request_json = command.dump(2);

    std::string error;
    if (!rewrite_.send(command, error)) {
        state_.pipeline_debug.rewrite_request_status =
            "Dispatch failed before the rewrite worker accepted the request: " + error;
        clear_active_rewrite_locked();
        if (!rewrite_.running()) {
            rewrite_unavailable_ = true;
            state_.rewrite_ready = false;
        }
        complete_with_raw_fallback_locked(
            "Cleanup could not start - review the raw transcript and press Enter to insert",
            std::move(error));
        return false;
    }
    return true;
}

void AppController::clear_active_rewrite_locked() {
    active_rewrite_.reset();
    state_.rewrite_in_progress = false;
}

void AppController::complete_with_raw_fallback_locked(std::string status, std::string error) {
    state_.rewritten_text = state_.raw_final_text;
    state_.live_text = state_.raw_final_text;
    state_.pipeline_debug.composed_text = state_.raw_final_text;
    state_.insertion_confirmation_required = true;
    state_.mode = DictationMode::Complete;
    state_.status = std::move(status);
    state_.error = std::move(error);
}

void AppController::finish_dictation_locked() {
    clear_active_rewrite_locked();
    if (state_.cleanup_mode == CleanupMode::Ai) {
        state_.insertion_confirmation_required = true;
        if (rewrite_unavailable_) {
            complete_with_raw_fallback_locked(
                "Cleanup unavailable - review the raw transcript and press Enter to insert",
                state_.error.empty() ? "AI cleanup is unavailable." : state_.error);
        } else if (state_.rewrite_ready) {
            dispatch_final_cleanup_locked();
        } else {
            state_.status = "Waiting for the cleanup model to finish loading...";
        }
        return;
    }
    state_.rewritten_text = state_.raw_final_text;
    state_.live_text = state_.raw_final_text;
    state_.insertion_confirmation_required = false;
    state_.status = "Dictation complete using the final raw transcript";
    state_.mode = DictationMode::Complete;
}

void AppController::set_error_locked(std::string message) {
    state_.mode = DictationMode::Error;
    state_.audio_rms = 0.0F;
    state_.audio_peak = 0.0F;
    state_.status = "DictScribe needs attention";
    state_.error = std::move(message);
}

std::string AppController::next_id_locked(const char* prefix) {
    return std::string(prefix) + "-" + std::to_string(++request_sequence_);
}

bool CanToggle(const AppSnapshot& snapshot) {
    return snapshot.mode == DictationMode::Ready ||
        snapshot.mode == DictationMode::Complete ||
        snapshot.mode == DictationMode::Recording;
}

bool CanCancel(const AppSnapshot& snapshot) {
    return snapshot.mode == DictationMode::Recording ||
        snapshot.mode == DictationMode::StartingRecording ||
        (snapshot.mode == DictationMode::Complete &&
         snapshot.insertion_confirmation_required);
}

bool CanSetLanguage(const AppSnapshot& snapshot) {
    return snapshot.mode == DictationMode::Starting || snapshot.mode == DictationMode::Ready ||
        snapshot.mode == DictationMode::StartingRecording ||
        snapshot.mode == DictationMode::Recording || snapshot.mode == DictationMode::Finalizing ||
        snapshot.mode == DictationMode::Complete || snapshot.mode == DictationMode::Error;
}

bool CanSetCleanupMode(const AppSnapshot& snapshot) {
    return snapshot.mode == DictationMode::Starting || snapshot.mode == DictationMode::Ready ||
        snapshot.mode == DictationMode::Complete || snapshot.mode == DictationMode::Error;
}

bool CanSetComputeDevice(const AppSnapshot& snapshot) {
    return CanSetCleanupMode(snapshot);
}

std::string LanguageLabel(const AppSnapshot& snapshot) {
    return "Language: " + std::string(LanguageDisplayName(snapshot.language));
}

const char* PrimaryButtonLabel(const AppSnapshot& snapshot) {
    switch (snapshot.mode) {
    case DictationMode::Recording: return "Stop dictation";
    case DictationMode::StartingRecording: return "Opening microphone...";
    case DictationMode::Finalizing: return "Finalizing transcript...";
    case DictationMode::Complete:
        return snapshot.insertion_confirmation_required
            ? "Insert text" : "Start another dictation";
    case DictationMode::Ready: return "Start dictation";
    case DictationMode::Error: return "Unavailable";
    case DictationMode::Starting: return "Loading speech model...";
    case DictationMode::Cancelling: return "Cancelling...";
    }
    return "Start dictation";
}

} // namespace dictscribe::app
