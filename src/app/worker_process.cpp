#include "app/worker_process.hpp"

#include "dictscribe/protocol/jsonl_protocol.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace {

#ifdef _WIN32

std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            size) <= 0) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
    }
    return result;
}

std::wstring quote_windows_argument(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }

    std::wstring quoted(1, L'"');
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(character);
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::string windows_error(const std::string& prefix, DWORD error_code = GetLastError()) {
    return prefix + ": " + std::system_category().message(static_cast<int>(error_code));
}

#endif

} // namespace

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

#ifdef _WIN32
    std::wstring command_line;
    try {
        command_line = quote_windows_argument(utf8_to_wide(executable));
        for (const auto& argument : arguments) {
            command_line.push_back(L' ');
            command_line += quote_windows_argument(utf8_to_wide(argument));
        }
    } catch (const std::exception& exception) {
        error = std::string("Could not prepare worker command line: ") + exception.what();
        return false;
    }

    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;

    HANDLE child_input = nullptr;
    HANDLE parent_input = nullptr;
    HANDLE parent_output = nullptr;
    HANDLE child_output = nullptr;
    if (!CreatePipe(&child_input, &parent_input, &security_attributes, 0) ||
        !CreatePipe(&parent_output, &child_output, &security_attributes, 0)) {
        const DWORD error_code = GetLastError();
        if (child_input) CloseHandle(child_input);
        if (parent_input) CloseHandle(parent_input);
        if (parent_output) CloseHandle(parent_output);
        if (child_output) CloseHandle(child_output);
        error = windows_error("Could not create worker pipes", error_code);
        return false;
    }
    if (!SetHandleInformation(parent_input, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(parent_output, HANDLE_FLAG_INHERIT, 0)) {
        const DWORD error_code = GetLastError();
        CloseHandle(child_input);
        CloseHandle(parent_input);
        CloseHandle(parent_output);
        CloseHandle(child_output);
        error = windows_error("Could not configure worker pipes", error_code);
        return false;
    }

    HANDLE null_error = CreateFileW(
        L"NUL",
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security_attributes,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (null_error == INVALID_HANDLE_VALUE) {
        const DWORD error_code = GetLastError();
        CloseHandle(child_input);
        CloseHandle(parent_input);
        CloseHandle(parent_output);
        CloseHandle(child_output);
        error = windows_error("Could not open the null device for worker stderr", error_code);
        return false;
    }

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = child_input;
    startup_info.hStdOutput = child_output;
    startup_info.hStdError = null_error;
    PROCESS_INFORMATION process_info{};
    const BOOL created = CreateProcessW(
        nullptr,
        command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup_info,
        &process_info);
    const DWORD error_code = created ? ERROR_SUCCESS : GetLastError();

    CloseHandle(child_input);
    CloseHandle(child_output);
    CloseHandle(null_error);
    if (!created) {
        CloseHandle(parent_input);
        CloseHandle(parent_output);
        error = windows_error("Could not start worker process", error_code);
        return false;
    }

    CloseHandle(process_info.hThread);
    input_handle_ = parent_input;
    process_handle_ = process_info.hProcess;
    running_ = true;
    try {
        reader_ = std::thread(
            &WorkerProcess::read_loop, this, parent_output, std::move(handler));
    } catch (const std::exception& exception) {
        running_ = false;
        CloseHandle(input_handle_);
        input_handle_ = nullptr;
        CloseHandle(parent_output);
        TerminateProcess(process_handle_, 1);
        WaitForSingleObject(process_handle_, INFINITE);
        CloseHandle(process_handle_);
        process_handle_ = nullptr;
        error = std::string("Could not start worker reader: ") + exception.what();
        return false;
    }
    return true;
#else
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
#endif
}

bool WorkerProcess::send(const nlohmann::json& message, std::string& error) {
    std::lock_guard lock(write_mutex_);
#ifdef _WIN32
    if (!running_ || input_handle_ == nullptr) {
        error = "Worker process is not running.";
        return false;
    }

    const std::string encoded = protocol::encode(message);
    std::size_t offset = 0;
    while (offset < encoded.size()) {
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
            encoded.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(input_handle_, encoded.data() + offset, requested, &written, nullptr) ||
            written == 0) {
            error = windows_error("Could not write to worker");
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
#else
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
#endif
}

void WorkerProcess::stop() {
#ifdef _WIN32
    HANDLE process_handle = process_handle_;
    if (process_handle == nullptr && !reader_.joinable()) {
        return;
    }

    running_ = false;
    {
        std::lock_guard lock(write_mutex_);
        if (input_handle_ != nullptr) {
            CloseHandle(input_handle_);
            input_handle_ = nullptr;
        }
    }
    if (process_handle != nullptr && WaitForSingleObject(process_handle, 1000) == WAIT_TIMEOUT) {
        TerminateProcess(process_handle, 1);
        WaitForSingleObject(process_handle, INFINITE);
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    if (process_handle != nullptr) {
        CloseHandle(process_handle);
    }
    process_handle_ = nullptr;
#else
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
#endif
}

#ifdef _WIN32
void WorkerProcess::read_loop(NativeHandle output_handle, MessageHandler handler) {
#else
void WorkerProcess::read_loop(int output_fd, MessageHandler handler) {
#endif
    std::string pending;
    char buffer[4096];
    for (;;) {
#ifdef _WIN32
        DWORD count = 0;
        if (!ReadFile(output_handle, buffer, sizeof(buffer), &count, nullptr) || count == 0) {
            break;
        }
#else
        const ssize_t count = read(output_fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
#endif
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
#ifdef _WIN32
    CloseHandle(output_handle);
#else
    close(output_fd);
#endif
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
