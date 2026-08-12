#include "workers/rewrite/dictation_normalizer.hpp"

#include <cassert>
#include <iostream>
#include <string>

int main() {
    const auto corrected = dictscribe::rewrite::normalize_spoken_dictation(
        "Die Überarbeitung ist noch nicht nein noch nicht zuverlässig. "
        "Am Donnerstag Quatsch am Freitag teste ich weiter.");
    assert(corrected.find("nein") == std::string::npos);
    assert(corrected.find("Quatsch") == std::string::npos);
    assert(corrected.find("noch nicht zuverlässig") != std::string::npos);
    assert(corrected.find("am Freitag") != std::string::npos);
    assert(corrected.find("Donnerstag") == std::string::npos);

    const auto formatted = dictscribe::rewrite::normalize_spoken_dictation(
        "Einkauf neuer Absatz Überschrift Einkaufsliste Doppelpunkt neue Zeile "
        "Erstens Butter neue Zeile Zweitens Brot.");
    assert(formatted.find("Einkauf\n\nEinkaufsliste:\n1. Butter\n2. Brot") != std::string::npos);
    assert(formatted.find("neue Zeile") == std::string::npos);

    const auto technical = dictscribe::rewrite::normalize_spoken_dictation(
        "Llama Unterstrich Rewriter Punkt Cpp liegt unter Slash Home Slash Sebastian Slash "
        "Qwen Bindestrich Q acht Unterstrich null Punkt GGUF.");
    assert(technical.find("Llama_Rewriter.Cpp") != std::string::npos);
    assert(technical.find("/Home/Sebastian/Qwen-Q acht_null.GGUF") != std::string::npos);

    const auto replaced_path = dictscribe::rewrite::normalize_spoken_dictation(
        "Der Pfad lautet Slash Home Slash alt Slash. Nein, genauer gesagt "
        "Slash Home Slash neu Slash Datei Punkt txt.");
    assert(replaced_path == "Der Pfad lautet /Home/neu/Datei.txt.");

    const auto redundant = dictscribe::rewrite::normalize_spoken_dictation(
        "Danach also anschließend vergleichen wir. Erstens Butter, neue Zeile Zweitens Brot.");
    assert(redundant == "anschließend vergleichen wir. 1. Butter\n2. Brot.");

    std::cout << "Dictation normalizer tests passed\n";
    return 0;
}
