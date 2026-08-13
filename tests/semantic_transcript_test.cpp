#include "app/semantic_transcript.hpp"

#include <cctype>
#include <iostream>
#include <string>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "Check failed at line " << __LINE__ << ": " #condition "\n"; \
            return 1; \
        } \
    } while (false)

int main() {
    using dictscribe::app::SemanticTranscript;
    using dictscribe::app::RewriteCommitResult;

    SemanticTranscript transcript;
    transcript.reset(7);
    transcript.begin_asr_segment();
    transcript.update_asr_hypothesis("one two three four five six seven eight nine");
    transcript.update_asr_hypothesis("one two three four five six seven eight nine ten");
    transcript.update_asr_hypothesis("one two three four five six seven eight nine ten eleven");
    CHECK(transcript.has_stable_backlog());
    CHECK(transcript.composed_text() ==
        "one two three four five six seven eight nine ten eleven");

    const auto first = transcript.make_rewrite_snapshot();
    CHECK(first.has_value());
    CHECK(first->session_generation == 7);
    // The first hypothesis ends directly after "nine", so the following word
    // boundary is not confirmed yet. Conservatively keep five complete trailing
    // tokens plus that unconfirmed boundary outside the stable span.
    CHECK(first->new_asr_text == "one two three ");

    transcript.update_asr_hypothesis(
        "one two three four five six seven eight nine ten revised");
    CHECK(transcript.can_commit(*first));
    CHECK(transcript.commit(*first, "One, two, three, four.") ==
        RewriteCommitResult::Accepted);
    CHECK(transcript.composed_text().find("revised") != std::string::npos);

    transcript.finalize_asr_hypothesis(
        "one two three four five six seven eight nine ten revised final");
    CHECK(transcript.raw_text() ==
        "one two three four five six seven eight nine ten revised final");
    const auto final_tail = transcript.make_rewrite_snapshot();
    CHECK(final_tail.has_value());

    SemanticTranscript replacement_session;
    replacement_session.reset(8);
    replacement_session.begin_asr_segment();
    replacement_session.finalize_asr_hypothesis("short final text");
    CHECK(!replacement_session.can_commit(*final_tail));

    SemanticTranscript revised_stable_prefix;
    revised_stable_prefix.reset(10);
    revised_stable_prefix.begin_asr_segment();
    revised_stable_prefix.update_asr_hypothesis(
        "alpha beta gamma delta epsilon zeta eta theta iota");
    revised_stable_prefix.update_asr_hypothesis(
        "alpha beta gamma delta epsilon zeta eta theta iota kappa");
    revised_stable_prefix.update_asr_hypothesis(
        "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda");
    const auto invalidated = revised_stable_prefix.make_rewrite_snapshot();
    CHECK(invalidated.has_value());
    revised_stable_prefix.update_asr_hypothesis(
        "changed beta gamma delta epsilon zeta eta theta iota kappa lambda");
    CHECK(!revised_stable_prefix.can_commit(*invalidated));
    CHECK(revised_stable_prefix.composed_text().starts_with("changed beta"));

    std::string long_text;
    for (int index = 0; index < 500; ++index) {
        if (!long_text.empty()) long_text.push_back(' ');
        long_text += "token" + std::to_string(index);
    }
    SemanticTranscript bounded;
    bounded.reset(9);
    bounded.begin_asr_segment();
    bounded.finalize_asr_hypothesis(long_text);
    const auto bounded_request = bounded.make_rewrite_snapshot();
    CHECK(bounded_request.has_value());
    std::size_t spaces = 0;
    for (char value : bounded_request->new_asr_text) spaces += value == ' ';
    CHECK(spaces <= 128);
    CHECK(bounded_request->new_asr_text.size() < long_text.size());

    std::string structured_tail;
    for (int index = 0; index < 20; ++index) {
        if (!structured_tail.empty()) structured_tail.push_back(' ');
        structured_tail += "lead" + std::to_string(index);
    }
    structured_tail += "\n\n";
    for (int index = 0; index < 80; ++index) {
        if (index != 0) structured_tail.push_back(' ');
        structured_tail += "tail" + std::to_string(index);
    }

    SemanticTranscript structured;
    structured.reset(11);
    structured.begin_asr_segment();
    structured.finalize_asr_hypothesis(structured_tail);
    const auto structure_request = structured.make_rewrite_snapshot();
    CHECK(structure_request.has_value());
    CHECK(structured.commit(*structure_request, structured_tail) ==
        RewriteCommitResult::Accepted);
    CHECK(structured.composed_text() == structured_tail);

    structured.begin_asr_segment();
    structured.finalize_asr_hypothesis("continued structure");
    const auto continued_structure = structured.make_rewrite_snapshot();
    CHECK(continued_structure.has_value());
    CHECK(continued_structure->read_only_context.ends_with("lead19\n\n"));
    CHECK(continued_structure->editable_tail.starts_with("tail0 "));
    CHECK(continued_structure->editable_tail.find('\n') == std::string::npos);

    SemanticTranscript bounded_context;
    bounded_context.reset(14);
    bounded_context.begin_asr_segment();
    std::string context_source;
    for (int index = 0; index < 120; ++index) {
        if (!context_source.empty()) context_source.push_back(' ');
        context_source += "context" + std::to_string(index);
    }
    bounded_context.finalize_asr_hypothesis(context_source);
    const auto context_source_request = bounded_context.make_rewrite_snapshot();
    CHECK(context_source_request.has_value());
    CHECK(bounded_context.commit(*context_source_request, context_source) ==
        RewriteCommitResult::Accepted);
    bounded_context.begin_asr_segment();
    bounded_context.finalize_asr_hypothesis("new dictated tail");
    const auto bounded_context_request = bounded_context.make_rewrite_snapshot();
    CHECK(bounded_context_request.has_value());
    std::size_t context_words = 0;
    bool in_context_word = false;
    for (const unsigned char value : bounded_context_request->read_only_context) {
        const bool whitespace = std::isspace(value) != 0;
        if (!whitespace && !in_context_word) ++context_words;
        in_context_word = !whitespace;
    }
    CHECK(context_words == 48);

    SemanticTranscript truncated_response;
    truncated_response.reset(12);
    truncated_response.begin_asr_segment();
    truncated_response.finalize_asr_hypothesis(
        "The first accepted passage contains enough words to remain visible.");
    const auto initial_passage = truncated_response.make_rewrite_snapshot();
    CHECK(initial_passage.has_value());
    CHECK(truncated_response.commit(
        *initial_passage,
        "The first accepted passage contains enough words to remain visible.") ==
        RewriteCommitResult::Accepted);
    truncated_response.begin_asr_segment();
    truncated_response.finalize_asr_hypothesis("This is the newest fragment.");
    const auto next_passage = truncated_response.make_rewrite_snapshot();
    CHECK(next_passage.has_value());
    CHECK(truncated_response.commit(*next_passage, "Only the newest fragment.") ==
        RewriteCommitResult::PreservedRaw);
    CHECK(truncated_response.composed_text() ==
        "The first accepted passage contains enough words to remain visible. "
        "This is the newest fragment.");

    SemanticTranscript repeated_context;
    repeated_context.reset(13);
    repeated_context.begin_asr_segment();
    const std::string initial_sentences =
        "Sentence one remains immutable. Sentence two also remains immutable. "
        "Sentence three is still editable. Sentence four is still editable.";
    repeated_context.finalize_asr_hypothesis(initial_sentences);
    const auto initial_context = repeated_context.make_rewrite_snapshot();
    CHECK(initial_context.has_value());
    CHECK(repeated_context.commit(*initial_context, initial_sentences) ==
        RewriteCommitResult::Accepted);
    repeated_context.begin_asr_segment();
    repeated_context.finalize_asr_hypothesis("Sentence five is newly recognized.");
    const auto context_repetition = repeated_context.make_rewrite_snapshot();
    CHECK(context_repetition.has_value());
    CHECK(!context_repetition->read_only_context.empty());
    const std::string repeated_candidate =
        context_repetition->editable_tail + " " +
        context_repetition->read_only_context + " " +
        context_repetition->new_asr_text;
    CHECK(repeated_context.commit(*context_repetition, repeated_candidate) ==
        RewriteCommitResult::PreservedRaw);
    const std::string repetition_result = repeated_context.composed_text();
    const std::size_t first_sentence = repetition_result.find(
        "Sentence one remains immutable.");
    CHECK(first_sentence != std::string::npos);
    CHECK(repetition_result.find(
        "Sentence one remains immutable.", first_sentence + 1) == std::string::npos);

    std::cout << "Semantic transcript tests passed\n";
    return 0;
}
