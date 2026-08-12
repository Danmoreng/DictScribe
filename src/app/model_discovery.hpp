#pragma once

#include "app/app_controller.hpp"

#include <string>

namespace dictscribe::app {

struct DiscoveryResult {
    AppConfig config;
    std::string error;
    bool show_help = false;
    bool show_version = false;
    bool smoke_test = false;
    bool language_overridden = false;
};

DiscoveryResult DiscoverConfig(int argc, char** argv);
const char* CommandLineHelp();

} // namespace dictscribe::app
