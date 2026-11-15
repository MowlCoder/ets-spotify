#pragma once
#include <utility>

namespace core {

// A process-lifetime global that is never destroyed.
//
// The plugin's globals are used by game threads until the process dies, and static destructors run
// at DLL_PROCESS_DETACH, under the loader lock, when the game may already have torn down D3D or
// COM. So every global with a non-trivial destructor is created once on the heap and deliberately
// leaked:
//
//     std::mutex& g_mutex = Immortal<std::mutex>();
template <typename T, typename... Args>
T& Immortal(Args&&... args) {
    return *new T(std::forward<Args>(args)...);
}

}  // namespace core
