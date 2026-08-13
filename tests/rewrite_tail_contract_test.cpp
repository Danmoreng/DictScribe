#include "workers/rewrite/rewrite_tail_contract.hpp"

#include <cassert>
#include <iostream>

int main() {
    using dictscribe::rewrite::RewriteTailInput;
    using dictscribe::rewrite::build_rewrite_tail_model_input;
    using dictscribe::rewrite::parse_replacement_tail_json;

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

    std::cout << "Rewrite tail contract tests passed\n";
    return 0;
}
