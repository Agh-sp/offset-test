#include "mem.h"
#include <iostream>
#include <algorithm>

MemReader::MemReader() = default;
MemReader::~MemReader() { close(); }

void MemReader::close() {
    if (isOpen()) {
        CloseHandle(hProcess);
        hProcess = nullptr;
    }
    mPid = 0;
}

bool MemReader::openById(DWORD pid) {
    close();
    // PROCESS_VM_READ is all we need (read-only). PROCESS_QUERY_INFORMATION
    // lets us query module info. We deliberately do NOT request
    // PROCESS_VM_WRITE / PROCESS_VM_OPERATION (no writes to the game).
    hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                           FALSE, pid);
    if (!hProcess) {
        std::cerr << "[mem] OpenProcess failed for PID " << pid
                  << " (error " << GetLastError() << ").\n"
                  << "[mem] If cs2.exe runs elevated, run this terminal as Administrator.\n";
        return false;
    }
    mPid = pid;
    return true;
}

bool MemReader::openByProcessName(const std::wstring& name) {
    close();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        std::cerr << "[mem] CreateToolhelp32Snapshot failed (error "
                  << GetLastError() << ").\n";
        return false;
    }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring exe(pe.szExeFile);
            std::transform(exe.begin(), exe.end(), exe.begin(),
                           [](wchar_t c) { return std::tolower(c); });
            std::wstring target(name);
            std::transform(target.begin(), target.end(), target.begin(),
                           [](wchar_t c) { return std::tolower(c); });
            if (exe == target) {
                found = openById(pe.th32ProcessID);
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (!found) {
        std::string narrow(name.begin(), name.end());
        std::cerr << "[mem] Process '" << narrow << "' not found. Is cs2.exe running?\n";
    }
    return found;
}

std::vector<ModuleInfo> MemReader::enumerateModules() const {
    std::vector<ModuleInfo> out;
    if (!isOpen()) return out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, mPid);
    if (snap == INVALID_HANDLE_VALUE) return out;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            ModuleInfo info;
            info.name = std::string(me.szModule, me.szModule + wcslen(me.szModule));
            info.base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
            info.size = static_cast<uint32_t>(me.modBaseSize);
            out.push_back(info);
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return out;
}

std::optional<uintptr_t> MemReader::moduleBase(const std::string& name) const {
    for (const auto& m : enumerateModules()) {
        std::string a = m.name, b = name;
        std::transform(a.begin(), a.end(), a.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        std::transform(b.begin(), b.end(), b.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (a == b) return m.base;
    }
    return std::nullopt;
}
