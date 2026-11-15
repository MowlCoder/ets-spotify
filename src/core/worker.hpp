#pragma once
#include <windows.h>

#include <thread>

namespace core {

// A background thread with a stop event: the shape shared by every module that runs on its own.
//
//     void Main(HANDLE stop) { while (WaitForSingleObject(stop, 250) == WAIT_TIMEOUT) { ... } }
//     core::Worker& g_worker = core::Immortal<core::Worker>();
//     g_worker.Start(Main);  ...  g_worker.Stop(2000);
//
// The stop signal is a Win32 event so the body can wait on it together with other handles
// (WaitForMultipleObjects). Keep Workers immortal (core/immortal.hpp): destroying a std::thread
// that is still running terminates the process.
class Worker {
public:
    using Body = void (*)(HANDLE stop);

    bool Start(Body body);  // false if already running or the thread could not be started
    // Signals the stop event and waits up to timeoutMs. A thread that is still busy after that
    // (e.g. inside a slow network call) is detached and finishes on its own: the DLL is never
    // unloaded.
    void Stop(DWORD timeoutMs);
    bool Running() const { return thread_.joinable(); }

private:
    std::thread thread_;
    HANDLE stop_ = nullptr;      // set by Stop()
    HANDLE finished_ = nullptr;  // set by the thread when the body returns
};

}  // namespace core
