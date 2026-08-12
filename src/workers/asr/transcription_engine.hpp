#pragma once

#include <optional>
#include <string>

#include <nemo_speech/asr.h>

namespace dictscribe::asr {

struct FeedResult {
    std::optional<std::string> text;
    bool final = false;
    float audio_processed_seconds = 0.0F;
};

class TranscriptionEngine {
public:
    ~TranscriptionEngine();

    bool load(const std::string& model_path, bool use_gpu, std::string& error);
    bool begin(const std::string& language, std::string& error);
    bool feed(const float* samples, int count, FeedResult& result, std::string& error);
    bool finalize(FeedResult& result, std::string& error);
    void cancel();

    [[nodiscard]] const char* version() const { return nemo_speech_asr_version(); }

private:
    bool collect_results(FeedResult& result, std::string& error);
    bool check(nemo_speech_asr_status status, const char* operation, std::string& error) const;

    nemo_speech_asr_recognizer* recognizer_ = nullptr;
    nemo_speech_asr_stream* stream_ = nullptr;
    nemo_speech_asr_recognition_options options_{};
    std::string language_;
};

} // namespace dictscribe::asr
