#include "app/app_controller.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace dictscribe::app {

namespace {

constexpr auto kLiveCleanupDebounce = std::chrono::milliseconds(700);
constexpr auto kLiveCleanupMaximumDelay = std::chrono::milliseconds(2000);
constexpr auto kRewriteTimeout = std::chrono::seconds(5);

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
        state_ = {};
        state_.asr_model_name = FileName(config.asr_model);
        state_.rewrite_model_name = FileName(config.rewrite_model);
        state_.language = config.language;
        state_.cleanup_mode = config.cleanup_mode;
        state_.asr_use_gpu = config.asr_use_gpu;
        state_.rewrite_use_gpu = config.rewrite_use_gpu;
        rewrite_unavailable_ = false;
        language_restart_pending_ = false;
        rewrite_pending_ = false;
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
        "--context-size", "2048"};
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
        state_.live_text.clear();
        state_.raw_final_text.clear();
        state_.rewritten_text.clear();
        state_.error.clear();
        dictation_id_ = next_id_locked("dictation");
        language_restart_pending_ = false;
        clear_active_rewrite_locked();
    }
    transcript_.begin_asr_segment();
    session_id_ = next_id_locked("session");
    state_.audio_rms = 0.0F;
    state_.audio_peak = 0.0F;
    if (clear_transcript) {
        rewrite_pending_ = false;
    } else if (state_.cleanup_mode == CleanupMode::Ai && transcript_.has_stable_backlog()) {
        rewrite_pending_ = true;
        rewrite_pending_since_ = std::chrono::steady_clock::now();
        rewrite_due_ = rewrite_pending_since_;
    }
    state_.mode = DictationMode::StartingRecording;
    state_.status = state_.cleanup_mode == CleanupMode::Ai
        ? "Opening the microphone - AI cleanup will start when ready"
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
    if (!CanSetLanguage(state_) ||
        (language != "auto" && language != "de" && language != "en") ||
        language == state_.language) return;
    state_.language = std::move(language);
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
        rewrite_pending_ = false;
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
    if (active_rewrite_ && now - active_rewrite_->started >= kRewriteTimeout) {
        clear_active_rewrite_locked();
        state_.error = "AI cleanup timed out; raw transcription continues.";
        if (state_.mode == DictationMode::Recording) {
            state_.status = "Listening - AI cleanup timed out; raw transcription continues";
        }
        if (transcript_.has_stable_backlog()) {
            rewrite_pending_ = true;
            rewrite_due_ = now;
        }
    }
    if (state_.mode == DictationMode::Recording && state_.cleanup_mode == CleanupMode::Ai &&
        state_.rewrite_ready && !active_rewrite_ && rewrite_pending_ && now >= rewrite_due_) {
        dispatch_rewrite_locked();
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
            ? (state_.rewrite_ready ? "Listening - bounded AI cleanup is ready"
                                    : "Listening - raw text while AI cleanup prepares")
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
        const bool had_stable = transcript_.has_stable_backlog();
        transcript_.update_asr_hypothesis(message.value("text", ""));
        refresh_transcript_locked();
        if (!had_stable && transcript_.has_stable_backlog()) queue_rewrite_locked();
    } else if (type == "recording_finalized") {
        if (message.value("sessionId", "") != session_id_) return;
        state_.audio_rms = 0.0F;
        state_.audio_peak = 0.0F;
        transcript_.finalize_asr_hypothesis(message.value("text", ""));
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
        language_restart_pending_ = false;
        rewrite_pending_ = false;
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
            state_.status = "Listening - bounded AI cleanup is ready";
            if (transcript_.has_stable_backlog()) queue_rewrite_locked();
        } else if (state_.mode == DictationMode::Ready) {
            state_.status = "Ready - AI cleanup loaded";
        }
    } else if (type == "rewrite_tail_completed") {
        if (!active_rewrite_ || message.value("requestId", "") != active_rewrite_->id) return;
        const ActiveRewrite completed = *active_rewrite_;
        clear_active_rewrite_locked();
        const bool identity_matches =
            message.value("sessionId", "") == dictation_id_ &&
            message.value("tailRevision", std::uint64_t(-1)) == completed.transcript.tail_revision &&
            message.value("firstStableSpanId", std::uint64_t(0)) ==
                completed.transcript.first_stable_span_id &&
            message.value("lastStableSpanId", std::uint64_t(0)) ==
                completed.transcript.last_stable_span_id;
        if (identity_matches && transcript_.commit(
                completed.transcript, message.value("replacementTail", ""))) {
            state_.error.clear();
            refresh_transcript_locked();
        }
        if (state_.mode == DictationMode::Recording && transcript_.has_stable_backlog()) {
            rewrite_pending_ = true;
            rewrite_due_ = std::chrono::steady_clock::now();
            state_.status = "Listening - newer speech is waiting for cleanup";
        } else if (state_.mode == DictationMode::Recording) {
            state_.status = "Listening - bounded AI cleanup is up to date";
        }
    } else if (type == "error") {
        const bool fatal = !message.value("recoverable", false) ||
            message.value("code", "") == "WORKER_EXITED" ||
            message.value("code", "") == "MODEL_LOAD_FAILED";
        clear_active_rewrite_locked();
        if (fatal) {
            rewrite_unavailable_ = true;
            state_.rewrite_ready = false;
        }
        state_.error = message.value("message", "Rewrite worker error");
        if (state_.mode == DictationMode::Recording) {
            state_.status = "Listening - AI cleanup failed; raw transcription continues";
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
    state_.live_text = state_.cleanup_mode == CleanupMode::Ai
        ? transcript_.composed_text() : transcript_.raw_text();
    state_.rewritten_text = state_.live_text;
}

void AppController::queue_rewrite_locked() {
    if (state_.cleanup_mode != CleanupMode::Ai || !transcript_.has_stable_backlog()) return;
    const auto now = std::chrono::steady_clock::now();
    if (!rewrite_pending_) rewrite_pending_since_ = now;
    rewrite_pending_ = true;
    rewrite_due_ = std::min(
        now + kLiveCleanupDebounce, rewrite_pending_since_ + kLiveCleanupMaximumDelay);
    if (state_.mode == DictationMode::Recording && state_.rewrite_ready) {
        state_.status = active_rewrite_
            ? "Listening - newer stable speech is coalesced"
            : "Listening - bounded AI cleanup queued";
    }
}

bool AppController::dispatch_rewrite_locked() {
    if (active_rewrite_ || !state_.rewrite_ready ||
        state_.cleanup_mode != CleanupMode::Ai) return false;
    const auto snapshot = transcript_.make_rewrite_snapshot();
    if (!snapshot) {
        rewrite_pending_ = false;
        return false;
    }
    ActiveRewrite active;
    active.id = next_id_locked("rewrite-tail");
    active.transcript = *snapshot;
    active.started = std::chrono::steady_clock::now();
    active_rewrite_ = active;
    state_.rewrite_in_progress = true;
    rewrite_pending_ = false;
    state_.status = "Listening - cleaning a bounded transcript tail...";

    std::string error;
    if (!rewrite_.send({
            {"v", 2}, {"type", "rewrite_tail"}, {"id", active.id},
            {"requestId", active.id}, {"sessionId", dictation_id_},
            {"tailRevision", snapshot->tail_revision},
            {"firstStableSpanId", snapshot->first_stable_span_id},
            {"lastStableSpanId", snapshot->last_stable_span_id},
            {"languageHint", state_.language},
            {"readOnlyContext", snapshot->read_only_context},
            {"editableTail", snapshot->editable_tail},
            {"newAsrText", snapshot->new_asr_text}}, error)) {
        clear_active_rewrite_locked();
        state_.error = std::move(error);
        state_.status = "Listening - AI cleanup unavailable; raw transcription continues";
        if (!rewrite_.running()) {
            rewrite_unavailable_ = true;
            state_.rewrite_ready = false;
        }
        return false;
    }
    return true;
}

void AppController::clear_active_rewrite_locked() {
    active_rewrite_.reset();
    state_.rewrite_in_progress = false;
}

void AppController::finish_dictation_locked() {
    rewrite_pending_ = false;
    clear_active_rewrite_locked();
    state_.rewritten_text = state_.cleanup_mode == CleanupMode::Ai
        ? transcript_.composed_text() : state_.raw_final_text;
    state_.live_text = state_.rewritten_text;
    state_.status = state_.cleanup_mode == CleanupMode::Ai
        ? "Dictation complete using accepted cleanup plus final raw speech"
        : "Dictation complete using the final raw transcript";
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
        snapshot.mode == DictationMode::StartingRecording;
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

const char* LanguageLabel(const AppSnapshot& snapshot) {
    if (snapshot.language == "de") return "Language: Deutsch";
    if (snapshot.language == "en") return "Language: English";
    return "Language: Auto";
}

const char* PrimaryButtonLabel(const AppSnapshot& snapshot) {
    switch (snapshot.mode) {
    case DictationMode::Recording: return "Stop dictation";
    case DictationMode::StartingRecording: return "Opening microphone...";
    case DictationMode::Finalizing: return "Finalizing transcript...";
    case DictationMode::Complete: return "Start another dictation";
    case DictationMode::Ready: return "Start dictation";
    case DictationMode::Error: return "Unavailable";
    case DictationMode::Starting: return "Loading speech model...";
    case DictationMode::Cancelling: return "Cancelling...";
    }
    return "Start dictation";
}

} // namespace dictscribe::app
