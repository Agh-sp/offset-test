#pragma once
// mem.h - Read-only memory access to another 64-bit Windows process.
// Uses ONLY the Windows API (CreateToolhelp32Snapshot, OpenProcess,
// ReadProcessMemory). No DLL injection, no writes to the target process,
// no external libraries.
#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

struct ModuleInfo {
    std::string name;
    uintptr_t base = 0;
    uint32_t size = 0;
};

class MemReader {
public:
    MemReader();
    ~MemReader();
    MemReader(const MemReader&) = delete;
    MemReader& operator=(const MemReader&) = delete;

    // Open the first running process whose executable name matches `name`
    // (e.g. L"cs2.exe"). Case-insensitive.
    bool openByProcessName(const std::wstring& name);
    bool openById(DWORD pid);
    void close();
    bool isOpen() const { return hProcess != nullptr && hProcess != INVALID_HANDLE_VALUE; }
    DWORD pid() const { return mPid; }

    std::vector<ModuleInfo> enumerateModules() const;
    std::optional<uintptr_t> moduleBase(const std::string& name) const;

    template <typename T>
    std::optional<T> read(uintptr_t addr) const {
        T value{};
        SIZE_T read = 0;
        if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr), &value,
                               sizeof(T), &read) || read != sizeof(T))
            return std::nullopt;
        return value;
    }

    std::optional<uintptr_t> readPtr(uintptr_t addr) const { return read<uintptr_t>(addr); }

private:
    HANDLE hProcess = nullptr;
    DWORD mPid = 0;
};
