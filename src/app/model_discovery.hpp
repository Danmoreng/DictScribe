#pragma once

#include "app/app_controller.hpp"
#include "app/settings.hpp"

#include <string>

namespace dictscribe::app {

struct DiscoveryResult {
    AppConfig config;
    std::string error;
    bool show_help = false;
    bool show_version = false;
    bool smoke_test = false;
    bool language_overridden = false;
    bool asr_device_overridden = false;
    bool rewrite_device_overridden = false;
    bool cleanup_mode_overridden = false;
};

DiscoveryResult DiscoverConfig(int argc, char** argv);
void ApplyStoredSettings(DiscoveryResult& discovery, const AppSettings& settings);
const char* CommandLineHelp();

} // namespace dictscribe::app
