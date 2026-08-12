#include "app/model_discovery.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string_view>
#include <vector>

namespace dictscribe::app {

namespace {

constexpr std::string_view kRewriteModelFilename = "Qwen3.5-2B-Q8_0.gguf";

std::filesystem::path EnvironmentPath(const char* name) {
    if (const char* value = std::getenv(name); value && value[0] != '\0') {
        return value;
    }
    return {};
}

std::vector<std::filesystem::path> FindGgufFiles(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> result;
    std::error_code error;
    if (root.empty() || !std::filesystem::exists(root, error)) {
        return result;
    }
    std::filesystem::recursive_directory_iterator iterator(
        root,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        if (iterator->is_regular_file(error) && iterator->path().extension() == ".gguf") {
            result.push_back(iterator->path());
        }
        iterator.increment(error);
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::filesystem::path HuggingFaceHubCache() {
    if (const auto configured = EnvironmentPath("HF_HUB_CACHE"); !configured.empty()) {
        return configured;
    }
    if (const auto configured = EnvironmentPath("HF_HOME"); !configured.empty()) {
        return configured / "hub";
    }
    if (const auto configured = EnvironmentPath("XDG_CACHE_HOME"); !configured.empty()) {
        return configured / "huggingface/hub";
    }
    if (const auto home = EnvironmentPath("HOME"); !home.empty()) {
        return home / ".cache/huggingface/hub";
    }
    return {};
}

std::filesystem::path FindNamedFile(
    const std::filesystem::path& root,
    std::string_view filename) {
    std::error_code error;
    if (root.empty() || !std::filesystem::is_directory(root, error)) {
        return {};
    }

    const auto directly_cached = root / filename;
    if (std::filesystem::is_regular_file(directly_cached, error)) {
        return directly_cached;
    }

    std::vector<std::filesystem::path> candidates;
    std::filesystem::recursive_directory_iterator iterator(
        root,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        const auto& path = iterator->path();
        if (path.filename() == filename && iterator->is_regular_file(error)) {
            candidates.push_back(path);
        }
        iterator.increment(error);
    }
    std::sort(candidates.begin(), candidates.end());
    return candidates.empty() ? std::filesystem::path{} : candidates.back();
}

std::filesystem::path FindAsrModel() {
    if (const auto configured = EnvironmentPath("DICTSCRIBE_ASR_MODEL"); !configured.empty()) {
        return configured;
    }
    const auto root = HuggingFaceHubCache() /
        "models--nvidia--nemotron-3.5-asr-streaming-0.6b/snapshots";
    const auto candidates = FindGgufFiles(root);
    return candidates.empty() ? std::filesystem::path{} : candidates.back();
}

std::filesystem::path FindRewriteModel() {
    if (const auto configured = EnvironmentPath("DICTSCRIBE_REWRITE_MODEL"); !configured.empty()) {
        return configured;
    }
    return FindNamedFile(HuggingFaceHubCache(), kRewriteModelFilename);
}

bool IsFile(const std::filesystem::path& path) {
    std::error_code error;
    return !path.empty() && std::filesystem::is_regular_file(path, error);
}

} // namespace

DiscoveryResult DiscoverConfig(int argc, char** argv) {
    const std::filesystem::path project_root = DICTSCRIBE_PROJECT_ROOT;
    DiscoveryResult result;
    result.config.asr_worker = project_root / "build/asr-worker/dictscribe-asr-worker";
    result.config.rewrite_worker = project_root / "build/rewrite-worker/bin/dictscribe-rewrite-worker";
    result.config.asr_model = FindAsrModel();
    result.config.rewrite_model = FindRewriteModel();
    if (const char* language = std::getenv("DICTSCRIBE_LANGUAGE"); language && language[0] != '\0') {
        result.config.language = language;
    }

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        auto require_value = [&](std::filesystem::path& destination) {
            if (index + 1 >= argc) {
                result.error = "Missing value after " + std::string(argument);
                return false;
            }
            destination = argv[++index];
            return true;
        };
        auto require_string = [&](std::string& destination) {
            if (index + 1 >= argc) {
                result.error = "Missing value after " + std::string(argument);
                return false;
            }
            destination = argv[++index];
            return true;
        };
        if (argument == "--asr-model") {
            if (!require_value(result.config.asr_model)) break;
        } else if (argument == "--rewrite-model") {
            if (!require_value(result.config.rewrite_model)) break;
        } else if (argument == "--asr-worker") {
            if (!require_value(result.config.asr_worker)) break;
        } else if (argument == "--rewrite-worker") {
            if (!require_value(result.config.rewrite_worker)) break;
        } else if (argument == "--language") {
            if (!require_string(result.config.language)) break;
        } else if (argument == "--gpu") {
            result.config.use_gpu = true;
        } else if (argument == "--help" || argument == "-h") {
            result.show_help = true;
        } else if (argument == "--version") {
            result.show_version = true;
        } else if (argument == "--smoke-test") {
            result.smoke_test = true;
        } else {
            result.error = "Unknown argument: " + std::string(argument);
            break;
        }
    }

    if (!result.error.empty() || result.show_help || result.show_version) {
        return result;
    }

    std::transform(
        result.config.language.begin(),
        result.config.language.end(),
        result.config.language.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (result.config.language != "auto" && result.config.language != "de" && result.config.language != "en") {
        result.error = "Unsupported language setting: " + result.config.language + " (expected auto, de, or en)";
        return result;
    }

    std::vector<std::string> missing;
    if (!IsFile(result.config.asr_worker)) missing.push_back("ASR worker");
    if (!IsFile(result.config.rewrite_worker)) missing.push_back("rewrite worker");
    if (!IsFile(result.config.asr_model)) missing.push_back("Nemotron ASR model");
    if (!IsFile(result.config.rewrite_model)) {
        missing.push_back(std::string(kRewriteModelFilename) + " rewrite model in the Hugging Face cache");
    }
    if (!missing.empty()) {
        std::ostringstream message;
        message << "Missing required runtime file" << (missing.size() == 1 ? ": " : "s: ");
        for (std::size_t index = 0; index < missing.size(); ++index) {
            if (index > 0) message << ", ";
            message << missing[index];
        }
        message << ". Run ./scripts/build.sh and place the models in the Hugging Face cache"
                   " (HF_HUB_CACHE, HF_HOME/hub, or ~/.cache/huggingface/hub)."
                   " Explicit --asr-model/--rewrite-model overrides remain available for development.";
        result.error = message.str();
    }
    return result;
}

const char* CommandLineHelp() {
    return
        "Usage: dictscribe [options]\n"
        "  --asr-model PATH       Override the Nemotron GGUF path\n"
        "  --rewrite-model PATH   Override the rewrite GGUF path\n"
        "  --asr-worker PATH      Override the ASR worker binary\n"
        "  --rewrite-worker PATH  Override the rewrite worker binary\n"
        "  --language CODE        Initial language: auto, de, or en\n"
        "  --gpu                  Use the CUDA-enabled worker builds\n"
        "  --smoke-test           Load both workers without opening a window\n"
        "  --help                 Show this help\n";
}

} // namespace dictscribe::app
