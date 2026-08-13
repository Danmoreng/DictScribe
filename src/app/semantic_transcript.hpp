#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace dictscribe::app {

struct StableAsrSpan {
    std::uint64_t id = 0;
    std::string text;
};

struct RewriteTailSnapshot {
    std::uint64_t session_generation = 0;
    std::uint64_t tail_revision = 0;
    std::uint64_t first_stable_span_id = 0;
    std::uint64_t last_stable_span_id = 0;
    std::string read_only_context;
    std::string editable_tail;
    std::string new_asr_text;
};

class SemanticTranscript {
public:
    void reset(std::uint64_t session_generation);
    void begin_asr_segment();
    void update_asr_hypothesis(std::string hypothesis);
    void finalize_asr_hypothesis(std::string hypothesis);

    [[nodiscard]] std::optional<RewriteTailSnapshot> make_rewrite_snapshot() const;
    [[nodiscard]] bool can_commit(const RewriteTailSnapshot& snapshot) const;
    bool commit(const RewriteTailSnapshot& snapshot, std::string replacement_tail);

    [[nodiscard]] bool has_stable_backlog() const { return !stable_backlog_.empty(); }
    [[nodiscard]] std::string composed_text() const;
    [[nodiscard]] std::string raw_text() const;
    [[nodiscard]] std::uint64_t session_generation() const { return session_generation_; }
    [[nodiscard]] std::uint64_t tail_revision() const { return tail_revision_; }
    [[nodiscard]] const std::deque<StableAsrSpan>& stable_backlog() const {
        return stable_backlog_;
    }

private:
    void promote_confirmed_prefix();
    void promote_through(std::size_t byte_count);
    void freeze_old_editable_output();
    [[nodiscard]] std::string read_only_context() const;

    std::string frozen_output_;
    std::string editable_output_;
    std::deque<StableAsrSpan> stable_backlog_;
    std::string unstable_asr_suffix_;

    std::string raw_finished_segments_;
    std::string current_raw_hypothesis_;
    std::string promoted_segment_text_;
    std::vector<std::string> recent_hypotheses_;
    std::size_t promoted_segment_bytes_ = 0;
    bool cleanup_safe_ = true;

    std::uint64_t session_generation_ = 0;
    std::uint64_t tail_revision_ = 0;
    std::uint64_t next_stable_span_id_ = 1;
};

} // namespace dictscribe::app
