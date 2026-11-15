#include "core/worker.hpp"

#include <system_error>

namespace core {

bool Worker::Start(Body body) {
    if (thread_.joinable()) { return false; }
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    finished_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_ && finished_) {
        try {
            // The thread gets its own copies of the handles, so Stop() may forget them at any time.
            thread_ = std::thread([body, stop = stop_, finished = finished_] {
                body(stop);
                SetEvent(finished);
            });
            return true;
        } catch (const std::system_error&) {
            // fall through: no thread
        }
    }

    if (stop_) { CloseHandle(stop_); }
    if (finished_) { CloseHandle(finished_); }
    stop_ = finished_ = nullptr;
    return false;
}

void Worker::Stop(DWORD timeoutMs) {
    if (!thread_.joinable()) { return; }
    SetEvent(stop_);
    if (WaitForSingleObject(finished_, timeoutMs) == WAIT_OBJECT_0) {
        thread_.join();
        CloseHandle(stop_);
        CloseHandle(finished_);
    } else {
        thread_.detach();  // the handles stay open for it
    }
    stop_ = finished_ = nullptr;
}

}  // namespace core
