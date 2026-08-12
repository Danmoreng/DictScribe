#include "app/worker_process.hpp"

#include "dictscribe/protocol/jsonl_protocol.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>

#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace dictscribe::app {

WorkerProcess::~WorkerProcess() {
    stop();
}

bool WorkerProcess::start(
    const std::string& executable,
    const std::vector<std::string>& arguments,
    MessageHandler handler,
    std::string& error) {
    if (running_) {
        error = "Worker process is already running.";
        return false;
    }

    int input_pipe[2] = {-1, -1};
    int output_pipe[2] = {-1, -1};
    if (pipe2(input_pipe, O_CLOEXEC) != 0 || pipe2(output_pipe, O_CLOEXEC) != 0) {
        error = "Could not create worker pipes: " + std::string(std::strerror(errno));
        if (input_pipe[0] >= 0) close(input_pipe[0]);
        if (input_pipe[1] >= 0) close(input_pipe[1]);
        if (output_pipe[0] >= 0) close(output_pipe[0]);
        if (output_pipe[1] >= 0) close(output_pipe[1]);
        return false;
    }

    std::vector<std::string> owned_arguments;
    owned_arguments.reserve(arguments.size() + 1);
    owned_arguments.push_back(executable);
    owned_arguments.insert(owned_arguments.end(), arguments.begin(), arguments.end());
    std::vector<char*> native_arguments;
    native_arguments.reserve(owned_arguments.size() + 1);
    for (auto& argument : owned_arguments) {
        native_arguments.push_back(argument.data());
    }
    native_arguments.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, input_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, input_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, input_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, output_pipe[1]);
    posix_spawn_file_actions_addopen(
        &actions,
        STDERR_FILENO,
        "/dev/null",
        O_WRONLY,
        0);

    pid_t child = -1;
    const int spawn_status = posix_spawn(
        &child,
        executable.c_str(),
        &actions,
        nullptr,
        native_arguments.data(),
        environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawn_status != 0) {
        error = "Could not start worker process: " + std::string(std::strerror(spawn_status));
        close(input_pipe[0]);
        close(input_pipe[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        return false;
    }

    close(input_pipe[0]);
    close(output_pipe[1]);
    input_fd_ = input_pipe[1];
    process_id_ = static_cast<int>(child);
    running_ = true;
    reader_ = std::thread(&WorkerProcess::read_loop, this, output_pipe[0], std::move(handler));
    return true;
}

bool WorkerProcess::send(const nlohmann::json& message, std::string& error) {
    std::lock_guard lock(write_mutex_);
    if (!running_ || input_fd_ < 0) {
        error = "Worker process is not running.";
        return false;
    }

    const std::string encoded = protocol::encode(message);
    std::size_t offset = 0;
    while (offset < encoded.size()) {
        const ssize_t count = write(input_fd_, encoded.data() + offset, encoded.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            error = "Could not write to worker: " + std::string(std::strerror(errno));
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

void WorkerProcess::stop() {
    const int process_id = process_id_;
    if (process_id < 0 && !reader_.joinable()) {
        return;
    }

    running_ = false;
    {
        std::lock_guard lock(write_mutex_);
        if (input_fd_ >= 0) {
            close(input_fd_);
            input_fd_ = -1;
        }
    }
    bool reaped = false;
    int status = 0;
    if (process_id >= 0) {
        for (int attempt = 0; attempt < 100; ++attempt) {
            const pid_t result = waitpid(static_cast<pid_t>(process_id), &status, WNOHANG);
            if (result == static_cast<pid_t>(process_id) || (result < 0 && errno == ECHILD)) {
                reaped = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!reaped) {
            kill(static_cast<pid_t>(process_id), SIGTERM);
        }
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    if (process_id >= 0 && !reaped) {
        while (waitpid(static_cast<pid_t>(process_id), &status, 0) < 0 && errno == EINTR) {
        }
    }
    process_id_ = -1;
}

void WorkerProcess::read_loop(int output_fd, MessageHandler handler) {
    std::string pending;
    char buffer[4096];
    for (;;) {
        const ssize_t count = read(output_fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        pending.append(buffer, static_cast<std::size_t>(count));
        for (;;) {
            const std::size_t newline = pending.find('\n');
            if (newline == std::string::npos) {
                break;
            }
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (line.empty()) {
                continue;
            }
            try {
                handler(protocol::parse(line));
            } catch (const std::exception& exception) {
                handler({
                    {"v", 1},
                    {"type", "error"},
                    {"code", "MALFORMED_WORKER_MESSAGE"},
                    {"message", std::string("Invalid worker protocol message: ") + exception.what()},
                    {"recoverable", false},
                });
            }
        }
    }
    close(output_fd);
    if (running_.exchange(false)) {
        handler({
            {"v", 1},
            {"type", "error"},
            {"code", "WORKER_EXITED"},
            {"message", "A local inference worker exited unexpectedly."},
            {"recoverable", false},
        });
    }
}

} // namespace dictscribe::app
