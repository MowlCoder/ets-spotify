#pragma once
#include <windows.h>

// COM vtables are shared by every instance of a class, so patching one slot through a
// throwaway object also hooks the game's objects of the same class.
namespace vt {

inline void** SlotOf(void* object, size_t index) {
    return &(*reinterpret_cast<void***>(object))[index];
}

// Atomically replaces *slot with hook. Returns the previous value, or nullptr on failure.
inline void* PatchSlot(void** slot, void* hook) {
    DWORD oldProtect;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) { return nullptr; }
    void* previous = InterlockedExchangePointer(slot, hook);
    VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
    return previous;
}

}  // namespace vt
