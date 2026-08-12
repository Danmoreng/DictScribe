#include "app/app_controller.hpp"
#include "app/model_discovery.hpp"
#include "ui/dictation_window.hpp"

#include <chrono>
#include <iostream>
#include <thread>

#include <signal.h>

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);

    auto discovery = dictscribe::app::DiscoverConfig(argc, argv);
    if (discovery.show_help) {
        std::cout << dictscribe::app::CommandLineHelp();
        return 0;
    }
    if (discovery.show_version) {
        std::cout << "dictscribe " << DICTSCRIBE_RUNTIME_VERSION << '\n';
        return 0;
    }

    dictscribe::app::AppController controller;
    if (!discovery.error.empty()) {
        std::cerr << discovery.error << '\n';
        if (discovery.smoke_test) {
            return 2;
        }
        controller.set_startup_error(discovery.error);
    } else {
        if (!controller.start(discovery.config) && discovery.smoke_test) {
            return 1;
        }
    }
    if (discovery.smoke_test) {
        for (int attempt = 0; attempt < 600; ++attempt) {
            const auto state = controller.snapshot();
            if (state.asr_ready && state.rewrite_ready) {
                std::cout << "DictScribe UI controller smoke test passed.\n";
                return 0;
            }
            if (state.mode == dictscribe::app::DictationMode::Error) {
                std::cerr << state.error << '\n';
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::cerr << "Timed out while loading the DictScribe workers.\n";
        return 1;
    }
    return dictscribe::ui::RunDictationWindow(controller);
}
