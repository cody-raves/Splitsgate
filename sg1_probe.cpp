#include <Windows.h>
#include <TlHelp32.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace {
constexpr wchar_t kProcessName[] = L"PortalWars-Win64-Shipping.exe";
constexpr uintptr_t kRootRva = 0x57576F0;
constexpr uintptr_t kCountOffset = 0x880;
constexpr uintptr_t kClockOffset = 0x898;

DWORD find_process() {
    PROCESSENTRY32W entry{sizeof(entry)};
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    DWORD pid = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, kProcessName) == 0) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pid;
}

bool main_module(DWORD pid, uintptr_t& base) {
    MODULEENTRY32W entry{sizeof(entry)};
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    const bool ok = Module32FirstW(snapshot, &entry) != FALSE;
    if (ok) base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
    CloseHandle(snapshot);
    return ok;
}

template <typename T>
bool read_value(HANDLE process, uintptr_t address, T& value) {
    SIZE_T received = 0;
    return ReadProcessMemory(process, reinterpret_cast<const void*>(address), &value, sizeof(value), &received) &&
           received == sizeof(value);
}

bool readable_remote(HANDLE process, uintptr_t address, size_t size) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQueryEx(process, reinterpret_cast<const void*>(address), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD protection = mbi.Protect & 0xff;
    const bool readable = protection == PAGE_READONLY || protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
        protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return readable && address + size <= end;
}

struct RaceCandidate {
    uintptr_t address = 0;
    int count = 0;
    double clock = 0.0;
};

std::vector<RaceCandidate> locate_races(HANDLE process, uintptr_t image) {
    uintptr_t manager = 0;
    if (!read_value(process, image + kRootRva, manager) || !manager) return {};
    std::vector<RaceCandidate> candidates;
    uintptr_t address = manager;
    MEMORY_BASIC_INFORMATION mbi{};
    while (VirtualQueryEx(process, reinterpret_cast<const void*>(address), &mbi, sizeof(mbi))) {
        if (reinterpret_cast<uintptr_t>(mbi.AllocationBase) != manager) break;
        const DWORD protection = mbi.Protect & 0xff;
        const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (protection == PAGE_READONLY || protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
             protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY);
        if (readable) {
            const uintptr_t begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const size_t size = mbi.RegionSize;
            std::vector<uint8_t> bytes(size);
            SIZE_T received = 0;
            if (ReadProcessMemory(process, reinterpret_cast<const void*>(begin), bytes.data(), size, &received)) {
                for (size_t offset = 0; offset + sizeof(uintptr_t) <= received; offset += sizeof(uintptr_t)) {
                    uintptr_t pointer = 0;
                    std::memcpy(&pointer, bytes.data() + offset, sizeof(pointer));
                    if (pointer < 0x10000 || !readable_remote(process, pointer + kCountOffset,
                            kClockOffset - kCountOffset + sizeof(double))) continue;
                    int count = -1;
                    double clock = -1.0;
                    if (!read_value(process, pointer + kCountOffset, count) ||
                        !read_value(process, pointer + kClockOffset, clock)) continue;
                    if (count >= 0 && count <= 100 && std::isfinite(clock) && clock >= 0.0 && clock < 86400.0)
                        candidates.push_back({pointer, count, clock});
                }
            }
        }
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= address) break;
        address = next;
    }

    Sleep(350);
    std::vector<RaceCandidate> live;
    std::unordered_set<uintptr_t> seen;
    for (const RaceCandidate& candidate : candidates) {
        if (!seen.insert(candidate.address).second) continue;
        int count = -1;
        double clock = -1.0;
        if (!read_value(process, candidate.address + kCountOffset, count) ||
            !read_value(process, candidate.address + kClockOffset, clock)) continue;
        const double delta = clock - candidate.clock;
        if (count == candidate.count && delta >= 0.15 && delta <= 0.70)
            live.push_back({candidate.address, count, clock});
    }
    return live;
}

void scan_block(HANDLE process, uintptr_t base, size_t span, int target, const wchar_t* label, uintptr_t parent_offset) {
    if (!readable_remote(process, base, span)) return;
    std::vector<uint8_t> bytes(span);
    SIZE_T received = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<const void*>(base), bytes.data(), span, &received)) return;
    for (size_t offset = 0; offset + 16 <= received; offset += 4) {
        int value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        if (value == target) {
            int before = 0, after = 0;
            if (offset >= 4) std::memcpy(&before, bytes.data() + offset - 4, sizeof(before));
            std::memcpy(&after, bytes.data() + offset + 4, sizeof(after));
            std::wprintf(L"%ls parent+0x%llX field+0x%zX = %d (neighbors %d, %d) @ 0x%llX\n",
                label, static_cast<unsigned long long>(parent_offset), offset, value, before, after,
                static_cast<unsigned long long>(base + offset));
        }
        if ((offset % 8) == 0) {
            uintptr_t data = 0;
            int count = 0, capacity = 0;
            std::memcpy(&data, bytes.data() + offset, sizeof(data));
            std::memcpy(&count, bytes.data() + offset + 8, sizeof(count));
            std::memcpy(&capacity, bytes.data() + offset + 12, sizeof(capacity));
            if (count == target && capacity >= count && capacity <= 256 && readable_remote(process, data, sizeof(uintptr_t))) {
                std::wprintf(L"TARRAY %ls parent+0x%llX field+0x%zX data=0x%llX num=%d max=%d\n",
                    label, static_cast<unsigned long long>(parent_offset), offset,
                    static_cast<unsigned long long>(data), count, capacity);
            }
        }
    }
}

void inspect_race(HANDLE process, uintptr_t race, int target) {
    constexpr size_t race_span = 0x2200;
    scan_block(process, race, race_span, target, L"RACE", 0);
    std::vector<uint8_t> race_bytes(race_span);
    SIZE_T received = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<const void*>(race), race_bytes.data(), race_span, &received)) return;
    std::unordered_set<uintptr_t> children;
    for (size_t offset = 0; offset + sizeof(uintptr_t) <= received; offset += sizeof(uintptr_t)) {
        uintptr_t child = 0;
        std::memcpy(&child, race_bytes.data() + offset, sizeof(child));
        if (child < 0x10000 || !children.insert(child).second || !readable_remote(process, child, 0x1200)) continue;
        scan_block(process, child, 0x1200, target, L"CHILD", offset);
    }
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    const int target = argc >= 2 ? _wtoi(argv[1]) : 29;
    const DWORD pid = find_process();
    if (!pid) {
        std::fwprintf(stderr, L"Splitgate 1 is not running.\n");
        return 2;
    }
    uintptr_t image = 0;
    if (!main_module(pid, image)) return 3;
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) return 4;
    const std::vector<RaceCandidate> races = locate_races(process, image);
    std::wprintf(L"Found %zu live race candidate(s). Target total: %d\n", races.size(), target);
    for (const RaceCandidate& race : races) {
        std::wprintf(L"RACE 0x%llX count=%d clock=%.3f\n",
            static_cast<unsigned long long>(race.address), race.count, race.clock);
        inspect_race(process, race.address, target);
    }
    CloseHandle(process);
    return races.empty() ? 5 : 0;
}
