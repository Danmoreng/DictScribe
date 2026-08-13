#include "app/semantic_transcript.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <string_view>

namespace dictscribe::app {

namespace {

constexpr std::size_t kMinimumHypothesesForPromotion = 3;
constexpr std::size_t kProtectedTrailingTokens = 5;
constexpr std::size_t kReadOnlyContextTokens = 48;
constexpr std::size_t kEditableTailTokens = 64;
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

std::vector<std::string> NormalizedContentTokens(std::string_view text) {
    std::vector<std::string> result;
    for (const auto& token : TokenBoundaries(text)) {
        std::size_t begin = token.begin;
        std::size_t end = token.end_with_space;
        while (end > begin &&
               std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
            --end;
        }
        while (begin < end && static_cast<unsigned char>(text[begin]) < 0x80U &&
               std::ispunct(static_cast<unsigned char>(text[begin])) != 0) {
            ++begin;
        }
        while (end > begin && static_cast<unsigned char>(text[end - 1]) < 0x80U &&
               std::ispunct(static_cast<unsigned char>(text[end - 1])) != 0) {
            --end;
        }
        if (begin == end) continue;
        std::string normalized(text.substr(begin, end - begin));
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
            [](unsigned char value) {
                return value < 0x80U
                    ? static_cast<char>(std::tolower(value))
                    : static_cast<char>(value);
            });
        result.push_back(std::move(normalized));
    }
    return result;
}

std::size_t RetainedTokenCount(
    const std::vector<std::string>& expected,
    std::string_view replacement_tail) {
    std::map<std::string, std::size_t> available;
    for (const auto& token : NormalizedContentTokens(replacement_tail)) {
        ++available[token];
    }
    std::size_t retained = 0;
    for (const auto& token : expected) {
        auto match = available.find(token);
        if (match == available.end() || match->second == 0) continue;
        --match->second;
        ++retained;
    }

    return retained;
}

bool PreservesRequiredContent(
    std::string_view editable_tail,
    std::string_view new_asr_text,
    std::string_view replacement_tail) {
    const auto editable = NormalizedContentTokens(editable_tail);
    const auto incoming = NormalizedContentTokens(new_asr_text);
    const std::size_t retained_editable = RetainedTokenCount(editable, replacement_tail);
    const std::size_t retained_incoming = RetainedTokenCount(incoming, replacement_tail);

    // An already accepted tail should only receive local edits. The lower
    // threshold for new ASR allows spoken formatting commands and abandoned
    // wording to disappear, while still rejecting a severely truncated result.
    const std::size_t required_editable = (editable.size() * 4 + 4) / 5;
    const std::size_t required_incoming = (incoming.size() + 1) / 2;
    return retained_editable >= required_editable &&
        retained_incoming >= required_incoming;
}

bool RepeatsReadOnlyContent(
    std::string_view read_only_context,
    std::string_view replacement_tail) {
    const auto context = NormalizedContentTokens(read_only_context);
    const auto replacement = NormalizedContentTokens(replacement_tail);
    constexpr std::size_t kRepeatedRun = 6;
    if (context.size() < kRepeatedRun || replacement.size() < kRepeatedRun) return false;
    for (std::size_t context_start = 0;
         context_start + kRepeatedRun <= context.size(); ++context_start) {
        for (std::size_t replacement_start = 0;
             replacement_start + kRepeatedRun <= replacement.size(); ++replacement_start) {
            if (std::equal(
                    context.begin() + static_cast<std::ptrdiff_t>(context_start),
                    context.begin() + static_cast<std::ptrdiff_t>(context_start + kRepeatedRun),
                    replacement.begin() + static_cast<std::ptrdiff_t>(replacement_start))) {
                return true;
            }
        }
    }
    return false;
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
    std::size_t sentence_start = 0;
    std::vector<std::size_t> sentence_starts;
    for (std::size_t index = 0; index + 1 < text.size(); ++index) {
        if (text[index] != '.' && text[index] != '!' && text[index] != '?') continue;
        std::size_t next = index + 1;
        if (std::isspace(static_cast<unsigned char>(text[next])) == 0) continue;
        while (next < text.size() &&
               std::isspace(static_cast<unsigned char>(text[next])) != 0) {
            ++next;
        }
        if (next < text.size()) sentence_starts.push_back(next);
    }
    if (sentence_starts.size() >= 2) {
        sentence_start = sentence_starts[sentence_starts.size() - 2];
    }

    std::size_t start = std::max({
        token_start,
        Utf8SafeSuffixStart(text, kMaximumEditableBytes),
        sentence_start});
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

RewriteCommitResult SemanticTranscript::commit(
    const RewriteTailSnapshot& snapshot,
    std::string replacement_tail) {
    if (!can_commit(snapshot) || replacement_tail.empty()) {
        return RewriteCommitResult::Rejected;
    }
    const bool accepted = PreservesRequiredContent(
        editable_output_, snapshot.new_asr_text, replacement_tail) &&
        !RepeatsReadOnlyContent(snapshot.read_only_context, replacement_tail);
    editable_output_ = accepted
        ? std::move(replacement_tail)
        : AppendText(std::move(editable_output_), snapshot.new_asr_text);
    consume_stable_spans(snapshot);
    ++tail_revision_;
    freeze_old_editable_output();
    return accepted ? RewriteCommitResult::Accepted : RewriteCommitResult::PreservedRaw;
}

bool SemanticTranscript::preserve_raw(const RewriteTailSnapshot& snapshot) {
    if (!can_commit(snapshot)) return false;
    editable_output_ = AppendText(std::move(editable_output_), snapshot.new_asr_text);
    consume_stable_spans(snapshot);
    ++tail_revision_;
    freeze_old_editable_output();
    return true;
}

void SemanticTranscript::consume_stable_spans(const RewriteTailSnapshot& snapshot) {
    while (!stable_backlog_.empty()) {
        const std::uint64_t id = stable_backlog_.front().id;
        stable_backlog_.pop_front();
        if (id == snapshot.last_stable_span_id) break;
    }
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
