#include "app/semantic_transcript.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace dictscribe::app {

namespace {

constexpr std::size_t kMinimumHypothesesForPromotion = 3;
constexpr std::size_t kProtectedTrailingTokens = 5;
constexpr std::size_t kReadOnlyContextTokens = 96;
constexpr std::size_t kEditableTailTokens = 192;
constexpr std::size_t kNewAsrTokensPerRequest = 128;
constexpr std::size_t kMaximumReadOnlyBytes = 2 * 1024;
constexpr std::size_t kMaximumEditableBytes = 4 * 1024;
constexpr std::size_t kMaximumNewAsrBytes = 4 * 1024;

struct TokenBoundary {
    std::size_t begin = 0;
    std::size_t end_with_space = 0;
};

std::size_t Utf8SequenceLength(unsigned char lead) {
    if ((lead & 0x80U) == 0) return 1;
    if ((lead & 0xE0U) == 0xC0U) return 2;
    if ((lead & 0xF0U) == 0xE0U) return 3;
    if ((lead & 0xF8U) == 0xF0U) return 4;
    return 1;
}

bool IsUnicodeWhitespace(std::string_view text, std::size_t offset) {
    const unsigned char value = static_cast<unsigned char>(text[offset]);
    if (value < 0x80U) return std::isspace(value) != 0;
    if (offset + 1 < text.size() && value == 0xC2U &&
        static_cast<unsigned char>(text[offset + 1]) == 0xA0U) {
        return true;
    }
    if (offset + 2 < text.size() && value == 0xE3U &&
        static_cast<unsigned char>(text[offset + 1]) == 0x80U &&
        static_cast<unsigned char>(text[offset + 2]) == 0x80U) {
        return true;
    }
    return false;
}

std::vector<TokenBoundary> TokenBoundaries(std::string_view text) {
    std::vector<TokenBoundary> result;
    std::size_t offset = 0;
    while (offset < text.size()) {
        while (offset < text.size() && IsUnicodeWhitespace(text, offset)) {
            offset += std::min(Utf8SequenceLength(static_cast<unsigned char>(text[offset])),
                text.size() - offset);
        }
        if (offset >= text.size()) break;
        const std::size_t begin = offset;
        while (offset < text.size() && !IsUnicodeWhitespace(text, offset)) {
            offset += std::min(Utf8SequenceLength(static_cast<unsigned char>(text[offset])),
                text.size() - offset);
        }
        while (offset < text.size() && IsUnicodeWhitespace(text, offset)) {
            offset += std::min(Utf8SequenceLength(static_cast<unsigned char>(text[offset])),
                text.size() - offset);
        }
        result.push_back({begin, offset});
    }
    return result;
}

std::size_t Utf8SafeCommonPrefix(std::string_view left, std::string_view right) {
    const std::size_t maximum = std::min(left.size(), right.size());
    std::size_t offset = 0;
    while (offset < maximum && left[offset] == right[offset]) ++offset;
    while (offset > 0 && offset < left.size() &&
           (static_cast<unsigned char>(left[offset]) & 0xC0U) == 0x80U) {
        --offset;
    }
    return offset;
}

std::size_t Utf8SafePrefixBytes(std::string_view text, std::size_t maximum) {
    std::size_t result = std::min(text.size(), maximum);
    while (result > 0 && result < text.size() &&
           (static_cast<unsigned char>(text[result]) & 0xC0U) == 0x80U) {
        --result;
    }
    return result;
}

std::size_t Utf8SafeSuffixStart(std::string_view text, std::size_t maximum) {
    if (text.size() <= maximum) return 0;
    std::size_t result = text.size() - maximum;
    while (result < text.size() &&
           (static_cast<unsigned char>(text[result]) & 0xC0U) == 0x80U) {
        ++result;
    }
    return result;
}

std::size_t TokenBoundaryAtOrBefore(std::string_view text, std::size_t byte_count) {
    std::size_t result = 0;
    for (const auto& token : TokenBoundaries(text)) {
        if (token.end_with_space > byte_count) break;
        result = token.end_with_space;
    }
    return result;
}

std::string AppendText(std::string prefix, std::string_view suffix) {
    if (suffix.empty()) return prefix;
    if (prefix.empty()) return std::string(suffix);
    const bool prefix_space = std::isspace(static_cast<unsigned char>(prefix.back())) != 0;
    const bool suffix_space = std::isspace(static_cast<unsigned char>(suffix.front())) != 0;
    if (!prefix_space && !suffix_space) prefix.push_back(' ');
    prefix.append(suffix);
    return prefix;
}

std::string SuffixByTokens(
    std::string_view text,
    std::size_t maximum_tokens,
    std::size_t maximum_bytes) {
    const auto tokens = TokenBoundaries(text);
    std::size_t start = tokens.size() <= maximum_tokens
        ? 0 : tokens[tokens.size() - maximum_tokens].begin;
    start = std::max(start, Utf8SafeSuffixStart(text, maximum_bytes));
    return std::string(text.substr(start));
}

std::size_t PrefixThroughTokens(std::string_view text, std::size_t maximum_tokens) {
    const auto tokens = TokenBoundaries(text);
    if (tokens.empty()) return 0;
    if (tokens.size() <= maximum_tokens) return text.size();
    return tokens[maximum_tokens - 1].end_with_space;
}

std::size_t EditableSuffixStart(std::string_view text, std::size_t maximum_tokens) {
    const auto tokens = TokenBoundaries(text);
    const std::size_t token_start = tokens.size() <= maximum_tokens
        ? 0 : tokens[tokens.size() - maximum_tokens].begin;
    std::size_t start = std::max(
        token_start, Utf8SafeSuffixStart(text, kMaximumEditableBytes));
    if (start == 0) return 0;

    const std::size_t blank_line = text.rfind("\n\n", start);
    if (blank_line != std::string_view::npos && start - blank_line < 256) {
        return blank_line + 2;
    }
    const std::size_t line = text.rfind('\n', start);
    if (line != std::string_view::npos && start - line < 128) return line + 1;
    return start;
}

} // namespace

void SemanticTranscript::reset(std::uint64_t session_generation) {
    frozen_output_.clear();
    editable_output_.clear();
    stable_backlog_.clear();
    unstable_asr_suffix_.clear();
    raw_finished_segments_.clear();
    current_raw_hypothesis_.clear();
    recent_hypotheses_.clear();
    promoted_segment_bytes_ = 0;
    promoted_segment_text_.clear();
    cleanup_safe_ = true;
    session_generation_ = session_generation;
    tail_revision_ = 0;
    next_stable_span_id_ = 1;
}

void SemanticTranscript::begin_asr_segment() {
    current_raw_hypothesis_.clear();
    recent_hypotheses_.clear();
    unstable_asr_suffix_.clear();
    promoted_segment_bytes_ = 0;
    promoted_segment_text_.clear();
}

void SemanticTranscript::update_asr_hypothesis(std::string hypothesis) {
    current_raw_hypothesis_ = std::move(hypothesis);
    if (!promoted_segment_text_.empty() &&
        !current_raw_hypothesis_.starts_with(promoted_segment_text_)) {
        cleanup_safe_ = false;
        stable_backlog_.clear();
        recent_hypotheses_.clear();
        unstable_asr_suffix_ = current_raw_hypothesis_;
        return;
    }
    recent_hypotheses_.push_back(current_raw_hypothesis_);
    if (recent_hypotheses_.size() > kMinimumHypothesesForPromotion) {
        recent_hypotheses_.erase(recent_hypotheses_.begin());
    }
    promote_confirmed_prefix();
    unstable_asr_suffix_ = current_raw_hypothesis_.substr(promoted_segment_bytes_);
}

void SemanticTranscript::finalize_asr_hypothesis(std::string hypothesis) {
    current_raw_hypothesis_ = std::move(hypothesis);
    if (!promoted_segment_text_.empty() &&
        !current_raw_hypothesis_.starts_with(promoted_segment_text_)) {
        cleanup_safe_ = false;
        stable_backlog_.clear();
    }
    if (cleanup_safe_) promote_through(current_raw_hypothesis_.size());
    unstable_asr_suffix_.clear();
    raw_finished_segments_ = AppendText(
        std::move(raw_finished_segments_), current_raw_hypothesis_);
    current_raw_hypothesis_.clear();
    recent_hypotheses_.clear();
    promoted_segment_bytes_ = 0;
    promoted_segment_text_.clear();
}

void SemanticTranscript::promote_confirmed_prefix() {
    if (recent_hypotheses_.size() < kMinimumHypothesesForPromotion) return;
    std::size_t common = recent_hypotheses_.front().size();
    for (std::size_t index = 1; index < recent_hypotheses_.size(); ++index) {
        common = std::min(common, Utf8SafeCommonPrefix(
            recent_hypotheses_.front(), recent_hypotheses_[index]));
    }
    common = TokenBoundaryAtOrBefore(current_raw_hypothesis_, common);
    const auto tokens = TokenBoundaries(current_raw_hypothesis_.substr(0, common));
    if (tokens.size() <= kProtectedTrailingTokens) return;
    promote_through(tokens[tokens.size() - kProtectedTrailingTokens - 1].end_with_space);
}

void SemanticTranscript::promote_through(std::size_t byte_count) {
    if (!cleanup_safe_) return;
    byte_count = std::min(byte_count, current_raw_hypothesis_.size());
    if (byte_count <= promoted_segment_bytes_) return;
    while (promoted_segment_bytes_ < byte_count) {
        const std::string_view remaining(current_raw_hypothesis_.data() + promoted_segment_bytes_,
            byte_count - promoted_segment_bytes_);
        std::size_t chunk_size = PrefixThroughTokens(remaining, kNewAsrTokensPerRequest);
        if (chunk_size == 0 || chunk_size > remaining.size()) chunk_size = remaining.size();
        chunk_size = std::min(chunk_size, Utf8SafePrefixBytes(remaining, kMaximumNewAsrBytes));
        if (chunk_size == 0) break;
        const std::string chunk(remaining.substr(0, chunk_size));
        stable_backlog_.push_back({
            next_stable_span_id_++, chunk});
        promoted_segment_text_ += chunk;
        promoted_segment_bytes_ += chunk_size;
    }
}

std::optional<RewriteTailSnapshot> SemanticTranscript::make_rewrite_snapshot() const {
    if (!cleanup_safe_ || stable_backlog_.empty()) return std::nullopt;
    RewriteTailSnapshot snapshot;
    snapshot.session_generation = session_generation_;
    snapshot.tail_revision = tail_revision_;
    snapshot.read_only_context = read_only_context();
    snapshot.editable_tail = editable_output_;
    snapshot.first_stable_span_id = stable_backlog_.front().id;

    std::string pending;
    for (const auto& span : stable_backlog_) {
        const std::string candidate = AppendText(pending, span.text);
        if (!pending.empty() &&
            (TokenBoundaries(candidate).size() > kNewAsrTokensPerRequest ||
             candidate.size() > kMaximumNewAsrBytes)) {
            break;
        }
        pending = candidate;
        snapshot.last_stable_span_id = span.id;
    }
    snapshot.new_asr_text = std::move(pending);
    return snapshot;
}

bool SemanticTranscript::can_commit(const RewriteTailSnapshot& snapshot) const {
    if (!cleanup_safe_ || snapshot.session_generation != session_generation_ ||
        snapshot.tail_revision != tail_revision_ ||
        snapshot.editable_tail != editable_output_ || stable_backlog_.empty() ||
        stable_backlog_.front().id != snapshot.first_stable_span_id) {
        return false;
    }
    std::uint64_t expected = snapshot.first_stable_span_id;
    for (const auto& span : stable_backlog_) {
        if (span.id != expected) return false;
        if (span.id == snapshot.last_stable_span_id) return true;
        ++expected;
    }
    return false;
}

bool SemanticTranscript::commit(
    const RewriteTailSnapshot& snapshot,
    std::string replacement_tail) {
    if (!can_commit(snapshot) || replacement_tail.empty()) return false;
    editable_output_ = std::move(replacement_tail);
    while (!stable_backlog_.empty()) {
        const std::uint64_t id = stable_backlog_.front().id;
        stable_backlog_.pop_front();
        if (id == snapshot.last_stable_span_id) break;
    }
    ++tail_revision_;
    freeze_old_editable_output();
    return true;
}

void SemanticTranscript::freeze_old_editable_output() {
    const std::size_t split = EditableSuffixStart(editable_output_, kEditableTailTokens);
    if (split == 0) return;
    frozen_output_ = AppendText(
        std::move(frozen_output_), std::string_view(editable_output_).substr(0, split));
    editable_output_.erase(0, split);
}

std::string SemanticTranscript::read_only_context() const {
    return SuffixByTokens(frozen_output_, kReadOnlyContextTokens, kMaximumReadOnlyBytes);
}

std::string SemanticTranscript::composed_text() const {
    if (!cleanup_safe_) return raw_text();
    std::string result = AppendText({}, frozen_output_);
    result = AppendText(std::move(result), editable_output_);
    for (const auto& span : stable_backlog_) {
        result = AppendText(std::move(result), span.text);
    }
    result = AppendText(std::move(result), unstable_asr_suffix_);
    return result;
}

std::string SemanticTranscript::raw_text() const {
    return AppendText(raw_finished_segments_, current_raw_hypothesis_);
}

} // namespace dictscribe::app
