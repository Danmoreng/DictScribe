#include "workers/rewrite/language_guard.hpp"

#include <cassert>
#include <iostream>

int main() {
    const std::string german =
        "Ich möchte vor allen Dingen testen, wie gut die Transkription ist und wie gut das hier funktioniert.";
    const std::string cleaned_german =
        "Ich möchte vor allem testen, wie gut die Transkription ist und wie gut das hier funktioniert.";
    const std::string translated_english =
        "I would first like to test how good the transcription is and how well it works here.";
    const std::string german_with_technical_english =
        "Ich bearbeite llama_rewriter.cpp und language_guard.cpp, weil der Rewrite technische Begriffe erhalten soll.";

    assert(dictscribe::rewrite::resolve_language_code("de", german) == "de");
    assert(dictscribe::rewrite::resolve_language_code("auto", german) == "de");
    assert(dictscribe::rewrite::output_preserves_language("de", german, cleaned_german));
    assert(!dictscribe::rewrite::output_preserves_language("de", german, translated_english));
    assert(!dictscribe::rewrite::output_preserves_language("auto", german, translated_english));
    assert(dictscribe::rewrite::output_preserves_language("de", "Butter, Milch, Kuchen, Brot.", "Butter, Milch, Kuchen, Brot."));
    assert(dictscribe::rewrite::output_preserves_language(
        "de", german_with_technical_english, german_with_technical_english));

    std::cout << "Rewrite language guard tests passed\n";
    return 0;
}
