#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace dictscribe::app {

class WorkerProcess {
public:
    using MessageHandler = std::function<void(const nlohmann::json&)>;

    WorkerProcess() = default;
    ~WorkerProcess();

    WorkerProcess(const WorkerProcess&) = delete;
    WorkerProcess& operator=(const WorkerProcess&) = delete;

    bool start(
        const std::string& executable,
        const std::vector<std::string>& arguments,
        MessageHandler handler,
        std::string& error);
    bool send(const nlohmann::json& message, std::string& error);
    void stop();

    [[nodiscard]] bool running() const { return running_.load(); }

private:
#ifdef _WIN32
    using NativeHandle = void*;
    void read_loop(NativeHandle output_handle, MessageHandler handler);

    NativeHandle input_handle_ = nullptr;
    NativeHandle process_handle_ = nullptr;
#else
    void read_loop(int output_fd, MessageHandler handler);

    int input_fd_ = -1;
    int process_id_ = -1;
#endif
    std::atomic<bool> running_{false};
    std::mutex write_mutex_;
    std::thread reader_;
};

} // namespace dictscribe::app
