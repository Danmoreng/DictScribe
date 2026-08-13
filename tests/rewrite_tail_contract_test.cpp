#include "workers/rewrite/rewrite_tail_contract.hpp"

#include <cassert>
#include <iostream>

int main() {
    using dictscribe::rewrite::RewriteTailInput;
    using dictscribe::rewrite::build_rewrite_tail_model_input;
    using dictscribe::rewrite::parse_replacement_tail_json;
    using dictscribe::rewrite::validate_replacement_tail;

    const auto input = build_rewrite_tail_model_input({
        .language_hint = "de",
        .read_only_context = "Unveränderlich: \"Kontext\"",
        .editable_tail = "Schon da",
        .new_asr_text = "neue Zeile Brot",
    });
    assert(input.find("\\\"Kontext\\\"") != std::string::npos);

    std::string replacement;
    std::string error;
    assert(parse_replacement_tail_json(
        R"({"replacement_tail":"Absatz eins.\n\n- Brot"})",
        true,
        replacement,
        error));
    assert(replacement == "Absatz eins.\n\n- Brot");

    assert(!parse_replacement_tail_json(
        R"({"replacement_tail":"Text","explanation":"no"})",
        true,
        replacement,
        error));
    assert(!parse_replacement_tail_json(
        R"({"replacement_tail":""})", true, replacement, error));
    assert(!parse_replacement_tail_json("not json", true, replacement, error));
    assert(parse_replacement_tail_json(
        R"({"replacement_tail":""})", false, replacement, error));

    const RewriteTailInput technical_input{
        .language_hint = "en",
        .read_only_context = "This context must remain unchanged.",
        .editable_tail = "Set DICTSCRIBE_REWRITE_MODEL",
        .new_asr_text =
            "to C colon slash models slash Qwen3.5-0.8B-Q8_0 dot gguf for 800 MB",
    };
    assert(validate_replacement_tail(
        technical_input,
        "Set DICTSCRIBE_REWRITE_MODEL to C:/models/Qwen3.5-0.8B-Q8_0.gguf for 800 MB.",
        error));
    assert(!validate_replacement_tail(
        technical_input,
        "Set DICTSCRIBE_REWRITE_MODEL to C:/private/Qwen3.5-0.8B-Q8_0.gguf for 800 MB.",
        error));
    assert(!validate_replacement_tail(
        technical_input,
        "Set DICTSCRIBE_REWRITE_MODEL to C:/models/Qwen3.5-0.8B-Q8_0.gguf for 18 MB.",
        error));
    assert(!validate_replacement_tail(
        technical_input,
        "Set DICTSCRIBE_REWRITE_MODEL twice: DICTSCRIBE_REWRITE_MODEL.",
        error));

    const RewriteTailInput list_input{
        .language_hint = "en",
        .read_only_context = "We agreed on the following implementation plan.",
        .editable_tail = "",
        .new_asr_text = "three tasks load the model clean the tail insert the result",
    };
    assert(validate_replacement_tail(
        list_input,
        "  1. Load the model.\n  2. Clean the tail.\n  3. Insert the result.",
        error));
    assert(!validate_replacement_tail(
        list_input,
        "We agreed on the following implementation plan. Load the model.",
        error));
    assert(!validate_replacement_tail(
        list_input,
        "```text\nLoad the model.\n```",
        error));

    std::string invalid_utf8 = "Text";
    invalid_utf8.push_back(static_cast<char>(0xC3));
    assert(!validate_replacement_tail(list_input, invalid_utf8, error));

    std::cout << "Rewrite tail contract tests passed\n";
    return 0;
}
