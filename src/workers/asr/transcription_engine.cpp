#include "transcription_engine.hpp"

namespace dictscribe::asr {

TranscriptionEngine::~TranscriptionEngine() {
    cancel();
    if (recognizer_) {
        nemo_speech_asr_destroy(recognizer_);
    }
}

bool TranscriptionEngine::load(
    const std::string& model_path,
    bool use_gpu,
    std::string& error) {
    cancel();
    if (recognizer_) {
        nemo_speech_asr_destroy(recognizer_);
        recognizer_ = nullptr;
    }

    nemo_speech_asr_backend_config backend{};
    backend.size = sizeof(backend);
    backend.gpu = use_gpu ? 0 : -1;

    nemo_speech_asr_model_config model{};
    model.size = sizeof(model);
    model.path = model_path.c_str();

    nemo_speech_asr_streaming_config streaming{};
    streaming.size = sizeof(streaming);
    streaming.chunk_size = 0.16F;
    streaming.ctc_left_padding = 1.92F;
    streaming.ctc_right_padding = 1.92F;
    streaming.rnnt_right_context = -1;

    nemo_speech_asr_recognizer_config config{};
    config.size = sizeof(config);
    config.backend = &backend;
    config.model = &model;
    config.streaming = &streaming;

    const auto status = nemo_speech_asr_create(&config, &recognizer_);
    if (!check(status, "nemo_speech_asr_create", error)) {
        recognizer_ = nullptr;
        return false;
    }
    return true;
}

bool TranscriptionEngine::begin(const std::string& language, std::string& error) {
    cancel();
    if (!recognizer_) {
        error = "NeMo ASR recognizer is not loaded.";
        return false;
    }

    language_ = language;
    options_ = nemo_speech_asr_recognition_options_default();
    options_.language_code = language_ == "auto" ? nullptr : language_.c_str();
    options_.interim_results = true;
    options_.enable_automatic_punctuation = true;

    const auto status = nemo_speech_asr_streaming_recognize(recognizer_, &options_, &stream_);
    if (!check(status, "nemo_speech_asr_streaming_recognize", error)) {
        stream_ = nullptr;
        return false;
    }
    return true;
}

bool TranscriptionEngine::feed(
    const float* samples,
    int count,
    FeedResult& result,
    std::string& error) {
    result = {};
    if (!stream_) {
        error = "NeMo ASR stream is not active.";
        return false;
    }
    const auto status = nemo_speech_asr_stream_push_f32(
        stream_, samples, static_cast<std::size_t>(count), 16000);
    if (!check(status, "nemo_speech_asr_stream_push_f32", error)) {
        return false;
    }
    return collect_results(result, error);
}

bool TranscriptionEngine::finalize(FeedResult& result, std::string& error) {
    result = {};
    if (!stream_) {
        error = "NeMo ASR stream is not active.";
        return false;
    }
    const auto status = nemo_speech_asr_stream_finish(stream_);
    if (!check(status, "nemo_speech_asr_stream_finish", error)) {
        return false;
    }
    const bool collected = collect_results(result, error);
    nemo_speech_asr_stream_close(stream_);
    stream_ = nullptr;
    return collected;
}

void TranscriptionEngine::cancel() {
    if (stream_) {
        nemo_speech_asr_stream_close(stream_);
        stream_ = nullptr;
    }
}

bool TranscriptionEngine::collect_results(FeedResult& result, std::string& error) {
    for (;;) {
        nemo_speech_asr_result* native_result = nullptr;
        const auto status = nemo_speech_asr_stream_next(stream_, &native_result);
        if (!check(status, "nemo_speech_asr_stream_next", error)) {
            return false;
        }
        if (!native_result) {
            break;
        }

        if (nemo_speech_asr_result_alternative_count(native_result) > 0) {
            if (const char* text = nemo_speech_asr_result_transcript(native_result, 0)) {
                result.text = text;
            }
        }
        result.final = nemo_speech_asr_result_is_final(native_result);
        result.audio_processed_seconds = nemo_speech_asr_result_audio_processed(native_result);
        nemo_speech_asr_result_destroy(native_result);
    }
    return true;
}

bool TranscriptionEngine::check(
    nemo_speech_asr_status status,
    const char* operation,
    std::string& error) const {
    if (status == NEMO_SPEECH_ASR_OK) {
        return true;
    }
    const char* detail = nemo_speech_asr_last_error();
    error = std::string(operation) + " failed: " + (detail ? detail : "unknown error");
    return false;
}

} // namespace dictscribe::asr
