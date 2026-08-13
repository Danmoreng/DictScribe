#include "app/semantic_transcript.hpp"

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
    CHECK(transcript.commit(*first, "One, two, three, four."));
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

    std::cout << "Semantic transcript tests passed\n";
    return 0;
}
