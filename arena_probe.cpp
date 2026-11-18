#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kProcessName[] = L"PortalWars2-Win64-Shipping.exe";

DWORD find_process() {
    PROCESSENTRY32W entry{sizeof(entry)};
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    DWORD result = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, kProcessName) == 0) {
                result = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

bool find_main_module(DWORD process_id, uintptr_t& base, size_t& size) {
    MODULEENTRY32W entry{sizeof(entry)};
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    const bool found = Module32FirstW(snapshot, &entry) != FALSE;
    if (found) {
        base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
        size = static_cast<size_t>(entry.modBaseSize);
    }
    CloseHandle(snapshot);
    return found;
}

bool readable_writable_region(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD protection = mbi.Protect & 0xff;
    return protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
           protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

bool save_addresses(const std::wstring& path, const std::vector<uintptr_t>& addresses) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    const uint64_t count = addresses.size();
    output.write(reinterpret_cast<const char*>(&count), sizeof(count));
    output.write(reinterpret_cast<const char*>(addresses.data()), static_cast<std::streamsize>(addresses.size() * sizeof(uintptr_t)));
    return output.good();
}

bool load_addresses(const std::wstring& path, std::vector<uintptr_t>& addresses) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    uint64_t count = 0;
    input.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!input || count > 100'000'000) return false;
    addresses.resize(static_cast<size_t>(count));
    input.read(reinterpret_cast<char*>(addresses.data()), static_cast<std::streamsize>(addresses.size() * sizeof(uintptr_t)));
    return input.good();
}

struct ClockRecord {
    uintptr_t address;
    double value;
};

struct TimerFormRecord {
    uintptr_t address;
    uint32_t form;
};

bool save_timer_forms(const std::wstring& path, const std::vector<TimerFormRecord>& records) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    const uint64_t count = records.size();
    output.write(reinterpret_cast<const char*>(&count), sizeof(count));
    output.write(reinterpret_cast<const char*>(records.data()), static_cast<std::streamsize>(records.size() * sizeof(TimerFormRecord)));
    return output.good();
}

bool load_timer_forms(const std::wstring& path, std::vector<TimerFormRecord>& records) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    uint64_t count = 0;
    input.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!input || count > 100'000'000) return false;
    records.resize(static_cast<size_t>(count));
    input.read(reinterpret_cast<char*>(records.data()), static_cast<std::streamsize>(records.size() * sizeof(TimerFormRecord)));
    return input.good();
}

bool save_clock_records(const std::wstring& path, const std::vector<ClockRecord>& records) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    const uint64_t count = records.size();
    output.write(reinterpret_cast<const char*>(&count), sizeof(count));
    output.write(reinterpret_cast<const char*>(records.data()), static_cast<std::streamsize>(records.size() * sizeof(ClockRecord)));
    return output.good();
}

bool load_clock_records(const std::wstring& path, std::vector<ClockRecord>& records) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    uint64_t count = 0;
    input.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!input || count > 100'000'000) return false;
    records.resize(static_cast<size_t>(count));
    input.read(reinterpret_cast<char*>(records.data()), static_cast<std::streamsize>(records.size() * sizeof(ClockRecord)));
    return input.good();
}

int snapshot_value(HANDLE process, int target, const std::wstring& output_path) {
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    uintptr_t cursor = reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress);
    const uintptr_t maximum = reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress);
    constexpr size_t kChunkSize = 4 * 1024 * 1024;
    std::vector<uint8_t> buffer(kChunkSize);
    std::vector<uintptr_t> matches;

    MEMORY_BASIC_INFORMATION mbi{};
    while (cursor < maximum && VirtualQueryEx(process, reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi))) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = begin + mbi.RegionSize;
        if (readable_writable_region(mbi)) {
            for (uintptr_t chunk = begin; chunk < end;) {
                const size_t requested = static_cast<size_t>(std::min<uintptr_t>(kChunkSize, end - chunk));
                SIZE_T received = 0;
                if (ReadProcessMemory(process, reinterpret_cast<void*>(chunk), buffer.data(), requested, &received)) {
                    for (size_t offset = 0; offset + sizeof(int) <= received; offset += sizeof(int)) {
                        int value = 0;
                        std::memcpy(&value, buffer.data() + offset, sizeof(value));
                        if (value == target) matches.push_back(chunk + offset);
                    }
                }
                chunk += requested;
            }
        }
        if (end <= cursor) break;
        cursor = end;
    }

    if (!save_addresses(output_path, matches)) {
        std::fwprintf(stderr, L"Could not save %ls\n", output_path.c_str());
        return 4;
    }
    std::wprintf(L"Saved %zu addresses containing %d to %ls\n", matches.size(), target, output_path.c_str());
    return 0;
}

int filter_value(HANDLE process, int target, const std::wstring& input_path, const std::wstring& output_path) {
    std::vector<uintptr_t> addresses;
    if (!load_addresses(input_path, addresses)) {
        std::fwprintf(stderr, L"Could not read %ls\n", input_path.c_str());
        return 5;
    }
    std::vector<uintptr_t> matches;
    matches.reserve(addresses.size() / 8 + 1);
    constexpr uintptr_t kChunkSize = 4 * 1024 * 1024;
    std::vector<uint8_t> buffer(kChunkSize);
    for (size_t first = 0; first < addresses.size();) {
        const uintptr_t chunk_base = addresses[first] & ~(kChunkSize - 1);
        size_t last = first + 1;
        while (last < addresses.size() && addresses[last] < chunk_base + kChunkSize) ++last;
        SIZE_T received = 0;
        if (ReadProcessMemory(process, reinterpret_cast<void*>(chunk_base), buffer.data(), kChunkSize, &received)) {
            for (size_t index = first; index < last; ++index) {
                const size_t offset = static_cast<size_t>(addresses[index] - chunk_base);
                if (offset + sizeof(int) > received) continue;
                int value = 0;
                std::memcpy(&value, buffer.data() + offset, sizeof(value));
                if (value == target) matches.push_back(addresses[index]);
            }
        } else {
            for (size_t index = first; index < last; ++index) {
                int value = 0;
                SIZE_T small_received = 0;
                if (ReadProcessMemory(process, reinterpret_cast<void*>(addresses[index]), &value, sizeof(value), &small_received) &&
                    small_received == sizeof(value) && value == target) {
                    matches.push_back(addresses[index]);
                }
            }
        }
        first = last;
    }
    if (!save_addresses(output_path, matches)) return 6;
    std::wprintf(L"Narrowed %zu addresses to %zu containing %d\n", addresses.size(), matches.size(), target);
    for (size_t i = 0; i < std::min<size_t>(matches.size(), 64); ++i) {
        std::wprintf(L"  0x%llX\n", static_cast<unsigned long long>(matches[i]));
    }
    return 0;
}

int inspect_addresses(HANDLE process, const std::wstring& input_path) {
    std::vector<uintptr_t> addresses;
    if (!load_addresses(input_path, addresses)) return 7;
    constexpr size_t kRadius = 0x2000;
    constexpr size_t kSpan = kRadius * 2;
    struct Sample {
        uintptr_t count_address{};
        uintptr_t base{};
        std::vector<uint8_t> before;
        std::vector<uint8_t> after;
        SIZE_T before_size{};
        SIZE_T after_size{};
    };
    std::vector<Sample> samples;
    for (const uintptr_t address : addresses) {
        Sample sample;
        sample.count_address = address;
        sample.base = address > kRadius ? address - kRadius : 0;
        sample.before.resize(kSpan);
        sample.after.resize(kSpan);
        ReadProcessMemory(process, reinterpret_cast<void*>(sample.base), sample.before.data(), kSpan, &sample.before_size);
        samples.push_back(std::move(sample));
    }
    Sleep(400);
    for (Sample& sample : samples) {
        ReadProcessMemory(process, reinterpret_cast<void*>(sample.base), sample.after.data(), kSpan, &sample.after_size);
    }

    for (const Sample& sample : samples) {
        const size_t available = std::min<size_t>(sample.before_size, sample.after_size);
        std::wprintf(L"\nCOUNT 0x%llX\n", static_cast<unsigned long long>(sample.count_address));
        int hits = 0;
        for (size_t offset = 0; offset + sizeof(double) <= available; offset += 4) {
            double a = 0.0, b = 0.0;
            std::memcpy(&a, sample.before.data() + offset, sizeof(a));
            std::memcpy(&b, sample.after.data() + offset, sizeof(b));
            const double change = b - a;
            if (std::isfinite(a) && std::isfinite(b) && a >= 0.0 && a < 86400.0 && change >= 0.25 && change <= 0.65) {
                const intptr_t relative = static_cast<intptr_t>(sample.base + offset - sample.count_address);
                std::wprintf(L"  double %+lld  %.6f -> %.6f (+%.6f)\n", static_cast<long long>(relative), a, b, change);
                ++hits;
            }
            float fa = 0.0f, fb = 0.0f;
            std::memcpy(&fa, sample.before.data() + offset, sizeof(fa));
            std::memcpy(&fb, sample.after.data() + offset, sizeof(fb));
            const float float_change = fb - fa;
            if (std::isfinite(fa) && std::isfinite(fb) && fa >= 0.0f && fa < 86400.0f && float_change >= 0.25f && float_change <= 0.65f) {
                const intptr_t relative = static_cast<intptr_t>(sample.base + offset - sample.count_address);
                std::wprintf(L"  float  %+lld  %.6f -> %.6f (+%.6f)\n", static_cast<long long>(relative), fa, fb, float_change);
                ++hits;
            }
        }
        if (!hits) std::wprintf(L"  no nearby real-time clocks\n");
    }
    return 0;
}

int inspect_roots(HANDLE process, DWORD process_id, const std::wstring& input_path) {
    std::vector<uintptr_t> addresses;
    if (!load_addresses(input_path, addresses)) return 8;
    uintptr_t image_base = 0;
    size_t image_size = 0;
    if (!find_main_module(process_id, image_base, image_size)) return 9;
    constexpr size_t kBack = 0x2000;
    std::vector<uint8_t> buffer(kBack);
    std::wprintf(L"Image 0x%llX - 0x%llX\n", static_cast<unsigned long long>(image_base),
                 static_cast<unsigned long long>(image_base + image_size));
    for (const uintptr_t count_address : addresses) {
        const uintptr_t begin = count_address - kBack;
        SIZE_T received = 0;
        std::wprintf(L"\nCOUNT 0x%llX possible UObject starts:\n", static_cast<unsigned long long>(count_address));
        if (!ReadProcessMemory(process, reinterpret_cast<void*>(begin), buffer.data(), buffer.size(), &received)) continue;
        int hits = 0;
        for (size_t offset = 0; offset + sizeof(uintptr_t) <= received; offset += sizeof(uintptr_t)) {
            uintptr_t pointer = 0;
            std::memcpy(&pointer, buffer.data() + offset, sizeof(pointer));
            if (pointer >= image_base && pointer < image_base + image_size) {
                const uintptr_t possible_base = begin + offset;
                const size_t field_offset = static_cast<size_t>(count_address - possible_base);
                if (field_offset <= 0x1800) {
                    std::wprintf(L"  base 0x%llX  count +0x%zX  vtable RVA 0x%llX\n",
                                 static_cast<unsigned long long>(possible_base), field_offset,
                                 static_cast<unsigned long long>(pointer - image_base));
                    if (++hits >= 40) break;
                }
            }
        }
        if (!hits) std::wprintf(L"  none\n");
    }
    return 0;
}

int find_race_state(HANDLE process, int expected_count) {
    struct Candidate { uintptr_t clock_address; double clock; int count; };
    std::vector<Candidate> candidates;
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    uintptr_t cursor = reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress);
    const uintptr_t maximum = reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress);
    constexpr size_t kChunkSize = 4 * 1024 * 1024;
    constexpr size_t kCountDelta = 56;
    std::vector<uint8_t> buffer(kChunkSize + kCountDelta);
    MEMORY_BASIC_INFORMATION mbi{};
    while (cursor < maximum && VirtualQueryEx(process, reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi))) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = begin + mbi.RegionSize;
        if (readable_writable_region(mbi)) {
            for (uintptr_t chunk = begin; chunk < end;) {
                const size_t requested = static_cast<size_t>(std::min<uintptr_t>(buffer.size(), end - chunk));
                SIZE_T received = 0;
                if (ReadProcessMemory(process, reinterpret_cast<void*>(chunk), buffer.data(), requested, &received)) {
                    for (size_t offset = 0; offset + kCountDelta + sizeof(int) <= received; offset += 8) {
                        double clock = 0.0;
                        int count = 0;
                        std::memcpy(&clock, buffer.data() + offset, sizeof(clock));
                        std::memcpy(&count, buffer.data() + offset + kCountDelta, sizeof(count));
                        // Literal zero fills enormous unused allocations and is
                        // never a running race clock. Wait until the timer has
                        // actually ticked before treating the layout as live.
                        if (!std::isfinite(clock) || clock < 0.01 || clock >= 86400.0) continue;
                        if (count < 0 || count > 64 || (expected_count >= 0 && count != expected_count)) continue;
                        candidates.push_back({chunk + offset, clock, count});
                    }
                }
                const size_t advance = requested > kCountDelta ? requested - kCountDelta : requested;
                if (!advance) break;
                chunk += advance;
            }
        }
        if (end <= cursor) break;
        cursor = end;
    }
    std::wprintf(L"Initial layout candidates: %zu\n", candidates.size());
    Sleep(400);
    int matches = 0;
    for (const Candidate& candidate : candidates) {
        double clock = 0.0;
        int count = -1;
        SIZE_T received = 0;
        if (!ReadProcessMemory(process, reinterpret_cast<void*>(candidate.clock_address), &clock, sizeof(clock), &received) || received != sizeof(clock)) continue;
        if (!ReadProcessMemory(process, reinterpret_cast<void*>(candidate.clock_address + kCountDelta), &count, sizeof(count), &received) || received != sizeof(count)) continue;
        const double change = clock - candidate.clock;
        if (count == candidate.count && change >= 0.25 && change <= 0.65) {
            std::wprintf(L"RACE clock=0x%llX count=0x%llX count=%d time=%.6f delta=%.6f\n",
                         static_cast<unsigned long long>(candidate.clock_address),
                         static_cast<unsigned long long>(candidate.clock_address + kCountDelta),
                         count, clock, change);
            ++matches;
        }
    }
    std::wprintf(L"Live race candidates: %d\n", matches);
    return matches ? 0 : 10;
}

int analyze_race_state(HANDLE process, DWORD process_id, uintptr_t clock_address) {
    constexpr size_t kBack = 0x2000;
    constexpr size_t kSpan = 0x4000;
    std::vector<uint8_t> buffer(kSpan);
    const uintptr_t begin = clock_address - kBack;
    SIZE_T received = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<void*>(begin), buffer.data(), buffer.size(), &received)) return 11;
    double clock = 0.0;
    int count = -1;
    std::memcpy(&clock, buffer.data() + kBack, sizeof(clock));
    std::memcpy(&count, buffer.data() + kBack + 56, sizeof(count));
    std::wprintf(L"clock 0x%llX = %.6f, count +0x38 = %d\n", static_cast<unsigned long long>(clock_address), clock, count);

    std::wprintf(L"Nearby int values equal to course total 31:\n");
    int total_hits = 0;
    for (size_t offset = 0; offset + sizeof(int) <= received; offset += 4) {
        int value = 0;
        std::memcpy(&value, buffer.data() + offset, sizeof(value));
        if (value == 31) {
            const intptr_t relative = static_cast<intptr_t>(begin + offset - clock_address);
            std::wprintf(L"  %+lld (0x%llX)\n", static_cast<long long>(relative),
                         static_cast<unsigned long long>(begin + offset));
            if (++total_hits >= 100) break;
        }
    }
    if (!total_hits) std::wprintf(L"  none\n");

    uintptr_t image_base = 0;
    size_t image_size = 0;
    if (!find_main_module(process_id, image_base, image_size)) return 12;
    std::wprintf(L"Possible object starts before clock:\n");
    int object_hits = 0;
    for (size_t offset = 0; offset <= kBack; offset += 8) {
        uintptr_t pointer = 0;
        std::memcpy(&pointer, buffer.data() + offset, sizeof(pointer));
        if (pointer >= image_base && pointer < image_base + image_size) {
            const uintptr_t possible_base = begin + offset;
            std::wprintf(L"  base 0x%llX clock +0x%llX vtable RVA 0x%llX\n",
                         static_cast<unsigned long long>(possible_base),
                         static_cast<unsigned long long>(clock_address - possible_base),
                         static_cast<unsigned long long>(pointer - image_base));
            if (++object_hits >= 100) break;
        }
    }
    if (!object_hits) std::wprintf(L"  none\n");
    return 0;
}

int snapshot_float(HANDLE process, float target, float tolerance, const std::wstring& output_path) {
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    uintptr_t cursor = reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress);
    const uintptr_t maximum = reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress);
    constexpr size_t kChunkSize = 4 * 1024 * 1024;
    std::vector<uint8_t> buffer(kChunkSize);
    std::vector<uintptr_t> matches;
    MEMORY_BASIC_INFORMATION mbi{};
    while (cursor < maximum && VirtualQueryEx(process, reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi))) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = begin + mbi.RegionSize;
        if (readable_writable_region(mbi)) {
            for (uintptr_t chunk = begin; chunk < end;) {
                const size_t requested = static_cast<size_t>(std::min<uintptr_t>(kChunkSize, end - chunk));
                SIZE_T received = 0;
                if (ReadProcessMemory(process, reinterpret_cast<void*>(chunk), buffer.data(), requested, &received)) {
                    for (size_t offset = 0; offset + sizeof(float) <= received; offset += sizeof(float)) {
                        float value = 0.0f;
                        std::memcpy(&value, buffer.data() + offset, sizeof(value));
                        if (std::isfinite(value) && std::fabs(value - target) <= tolerance) matches.push_back(chunk + offset);
                    }
                }
                chunk += requested;
            }
        }
        if (end <= cursor) break;
        cursor = end;
    }
    if (!save_addresses(output_path, matches)) return 13;
    std::wprintf(L"Saved %zu float candidates near %.4f (+/- %.4f)\n", matches.size(), target, tolerance);
    for (size_t i = 0; i < std::min<size_t>(matches.size(), 80); ++i) {
        float value = 0.0f;
        SIZE_T received = 0;
        ReadProcessMemory(process, reinterpret_cast<void*>(matches[i]), &value, sizeof(value), &received);
        std::wprintf(L"  0x%llX = %.6f\n", static_cast<unsigned long long>(matches[i]), value);
    }
    return 0;
}

int filter_float(HANDLE process, float target, float tolerance, const std::wstring& input_path, const std::wstring& output_path) {
    std::vector<uintptr_t> addresses;
    if (!load_addresses(input_path, addresses)) return 14;
    std::vector<uintptr_t> matches;
    for (const uintptr_t address : addresses) {
        float value = 0.0f;
        SIZE_T received = 0;
        if (ReadProcessMemory(process, reinterpret_cast<void*>(address), &value, sizeof(value), &received) &&
            received == sizeof(value) && std::isfinite(value) && std::fabs(value - target) <= tolerance) {
            matches.push_back(address);
            std::wprintf(L"  0x%llX = %.6f\n", static_cast<unsigned long long>(address), value);
        }
    }
    if (!save_addresses(output_path, matches)) return 15;
    std::wprintf(L"Narrowed %zu float addresses to %zu near %.4f (+/- %.4f)\n", addresses.size(), matches.size(), target, tolerance);
    return 0;
}

int snapshot_double_or_ticks(HANDLE process, double seconds, double tolerance, const std::wstring& output_path, bool ticks) {
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    uintptr_t cursor = reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress);
    const uintptr_t maximum = reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress);
    constexpr size_t kChunkSize = 4 * 1024 * 1024;
    std::vector<uint8_t> buffer(kChunkSize);
    std::vector<uintptr_t> matches;
    const int64_t target_ticks = static_cast<int64_t>(seconds * 10'000'000.0);
    const int64_t tolerance_ticks = static_cast<int64_t>(tolerance * 10'000'000.0);
    MEMORY_BASIC_INFORMATION mbi{};
    while (cursor < maximum && VirtualQueryEx(process, reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi))) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = begin + mbi.RegionSize;
        if (readable_writable_region(mbi)) {
            for (uintptr_t chunk = begin; chunk < end;) {
                const size_t requested = static_cast<size_t>(std::min<uintptr_t>(kChunkSize, end - chunk));
                SIZE_T received = 0;
                if (ReadProcessMemory(process, reinterpret_cast<void*>(chunk), buffer.data(), requested, &received)) {
                    for (size_t offset = 0; offset + 8 <= received; offset += 8) {
                        bool match = false;
                        if (ticks) {
                            int64_t value = 0;
                            std::memcpy(&value, buffer.data() + offset, sizeof(value));
                            match = std::llabs(value - target_ticks) <= tolerance_ticks;
                        } else {
                            double value = 0.0;
                            std::memcpy(&value, buffer.data() + offset, sizeof(value));
                            match = std::isfinite(value) && std::fabs(value - seconds) <= tolerance;
                        }
                        if (match) matches.push_back(chunk + offset);
                    }
                }
                chunk += requested;
            }
        }
        if (end <= cursor) break;
        cursor = end;
    }
    if (!save_addresses(output_path, matches)) return 16;
    std::wprintf(L"Saved %zu %ls candidates near %.4f seconds\n", matches.size(), ticks ? L"timespan" : L"double", seconds);
    for (size_t i = 0; i < std::min<size_t>(matches.size(), 100); ++i) {
        if (ticks) {
            int64_t value = 0; SIZE_T received = 0;
            ReadProcessMemory(process, reinterpret_cast<void*>(matches[i]), &value, sizeof(value), &received);
            std::wprintf(L"  0x%llX = %.6f sec (%lld ticks)\n", static_cast<unsigned long long>(matches[i]), value / 10'000'000.0, static_cast<long long>(value));
        } else {
            double value = 0.0; SIZE_T received = 0;
            ReadProcessMemory(process, reinterpret_cast<void*>(matches[i]), &value, sizeof(value), &received);
            std::wprintf(L"  0x%llX = %.6f\n", static_cast<unsigned long long>(matches[i]), value);
        }
    }
    return 0;
}

int filter_double_or_ticks(HANDLE process, double seconds, double tolerance, const std::wstring& input_path, const std::wstring& output_path, bool ticks) {
    std::vector<uintptr_t> addresses;
    if (!load_addresses(input_path, addresses)) return 17;
    std::vector<uintptr_t> matches;
    const int64_t target_ticks = static_cast<int64_t>(seconds * 10'000'000.0);
    const int64_t tolerance_ticks = static_cast<int64_t>(tolerance * 10'000'000.0);
    for (const uintptr_t address : addresses) {
        SIZE_T received = 0;
        if (ticks) {
            int64_t value = 0;
            if (ReadProcessMemory(process, reinterpret_cast<void*>(address), &value, sizeof(value), &received) &&
                received == sizeof(value) && std::llabs(value - target_ticks) <= tolerance_ticks) {
                matches.push_back(address);
                std::wprintf(L"  0x%llX = %.6f sec (%lld ticks)\n", static_cast<unsigned long long>(address), value / 10'000'000.0, static_cast<long long>(value));
            }
        } else {
            double value = 0.0;
            if (ReadProcessMemory(process, reinterpret_cast<void*>(address), &value, sizeof(value), &received) &&
                received == sizeof(value) && std::isfinite(value) && std::fabs(value - seconds) <= tolerance) {
                matches.push_back(address);
                std::wprintf(L"  0x%llX = %.6f\n", static_cast<unsigned long long>(address), value);
            }
        }
    }
    if (!save_addresses(output_path, matches)) return 18;
    std::wprintf(L"Narrowed %zu %ls addresses to %zu near %.4f seconds\n", addresses.size(), ticks ? L"timespan" : L"double", matches.size(), seconds);
    return 0;
}

int scan_wide_text(HANDLE process, const std::wstring& needle, const std::wstring& output_path) {
    if (needle.empty()) return 19;
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    uintptr_t cursor = reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress);
    const uintptr_t maximum = reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress);
    constexpr size_t kChunkSize = 4 * 1024 * 1024;
    const size_t byte_count = needle.size() * sizeof(wchar_t);
    std::vector<uint8_t> buffer(kChunkSize + byte_count);
    std::vector<uintptr_t> matches;
    MEMORY_BASIC_INFORMATION mbi{};
    while (cursor < maximum && VirtualQueryEx(process, reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi))) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = begin + mbi.RegionSize;
        if (readable_writable_region(mbi)) {
            for (uintptr_t chunk = begin; chunk < end;) {
                const size_t requested = static_cast<size_t>(std::min<uintptr_t>(kChunkSize, end - chunk));
                SIZE_T received = 0;
                if (ReadProcessMemory(process, reinterpret_cast<void*>(chunk), buffer.data(), requested, &received)) {
                    for (size_t offset = 0; offset + byte_count <= received; offset += 2) {
                        if (std::memcmp(buffer.data() + offset, needle.data(), byte_count) == 0) matches.push_back(chunk + offset);
                    }
                }
                chunk += requested;
            }
        }
        if (end <= cursor) break;
        cursor = end;
    }
    if (!save_addresses(output_path, matches)) return 20;
    std::wprintf(L"Saved %zu UTF-16 matches for '%ls'\n", matches.size(), needle.c_str());
    for (uintptr_t address : matches) std::wprintf(L"  0x%llX\n", static_cast<unsigned long long>(address));
    return 0;
}

int scan_map_labels(HANDLE process) {
    const std::wstring prefix = L"MAP: ";
    const size_t prefix_bytes = prefix.size() * sizeof(wchar_t);
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    uintptr_t cursor = reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress);
    const uintptr_t maximum = reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress);
    constexpr size_t kChunkSize = 4 * 1024 * 1024;
    constexpr size_t kMaxLabelChars = 96;
    std::vector<uint8_t> buffer(kChunkSize + kMaxLabelChars * sizeof(wchar_t));
    std::vector<std::pair<uintptr_t, std::wstring>> matches;
    MEMORY_BASIC_INFORMATION mbi{};
    while (cursor < maximum && VirtualQueryEx(process, reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi))) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = begin + mbi.RegionSize;
        if (readable_writable_region(mbi)) {
            for (uintptr_t chunk = begin; chunk < end;) {
                const size_t requested = static_cast<size_t>(std::min<uintptr_t>(kChunkSize, end - chunk));
                SIZE_T received = 0;
                if (ReadProcessMemory(process, reinterpret_cast<void*>(chunk), buffer.data(), requested, &received)) {
                    for (size_t offset = 0; offset + prefix_bytes <= received; offset += sizeof(wchar_t)) {
                        if (std::memcmp(buffer.data() + offset, prefix.data(), prefix_bytes) != 0) continue;
                        std::wstring label;
                        const size_t available_chars = std::min(kMaxLabelChars, (received - offset) / sizeof(wchar_t));
                        const wchar_t* text = reinterpret_cast<const wchar_t*>(buffer.data() + offset);
                        for (size_t i = 0; i < available_chars; ++i) {
                            const wchar_t ch = text[i];
                            if (!ch) break;
                            if (ch < 0x20 || ch > 0x7e) { label.clear(); break; }
                            label.push_back(ch);
                        }
                        if (label.size() > prefix.size() && label.size() < kMaxLabelChars) {
                            matches.emplace_back(chunk + offset, std::move(label));
                        }
                    }
                }
                chunk += requested;
            }
        }
        if (end <= cursor) break;
        cursor = end;
    }
    std::wprintf(L"Found %zu live MAP labels:\n", matches.size());
    for (const auto& match : matches) {
        std::wprintf(L"  0x%llX  %ls\n", static_cast<unsigned long long>(match.first), match.second.c_str());
    }
    return matches.empty() ? 25 : 0;
}

int scan_arena_timers(HANDLE process, DWORD process_id) {
    uintptr_t image_base = 0;
    size_t image_size = 0;
    if (!find_main_module(process_id, image_base, image_size)) return 26;
    // Arena Reloaded 3.1-643074: the authoritative race HUD clock lives on
    // this Slate-side object as a double.  The rendered copies are transient;
    // this owner survives normal frame-to-frame buffer churn.
    constexpr uintptr_t kTimerVtableRva = 0xF4E9748;
    constexpr uintptr_t kTimerOffset = 0x128;
    const uintptr_t timer_vtable = image_base + kTimerVtableRva;
    struct Candidate { uintptr_t object; double before; };
    std::vector<Candidate> candidates;
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    uintptr_t cursor = reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress);
    const uintptr_t maximum = reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress);
    constexpr size_t kChunkSize = 4 * 1024 * 1024;
    std::vector<uint8_t> buffer(kChunkSize);
    MEMORY_BASIC_INFORMATION mbi{};
    while (cursor < maximum && VirtualQueryEx(process, reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi))) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = begin + mbi.RegionSize;
        if (readable_writable_region(mbi)) {
            for (uintptr_t chunk = begin; chunk < end;) {
                const size_t requested = static_cast<size_t>(std::min<uintptr_t>(kChunkSize, end - chunk));
                SIZE_T received = 0;
                if (ReadProcessMemory(process, reinterpret_cast<void*>(chunk), buffer.data(), requested, &received)) {
                    for (size_t offset = 0; offset + sizeof(uintptr_t) <= received; offset += sizeof(uintptr_t)) {
                        uintptr_t value = 0;
                        std::memcpy(&value, buffer.data() + offset, sizeof(value));
                        if (value != timer_vtable) continue;
                        double timer = 0.0;
                        SIZE_T timer_received = 0;
                        const uintptr_t object = chunk + offset;
                        if (ReadProcessMemory(process, reinterpret_cast<void*>(object + kTimerOffset), &timer, sizeof(timer), &timer_received) &&
                            timer_received == sizeof(timer) && std::isfinite(timer) && timer >= 0.0 && timer <= 3600.0) {
                            candidates.push_back({object, timer});
                        }
                    }
                }
                chunk += requested;
            }
        }
        if (end <= cursor) break;
        cursor = end;
    }
    Sleep(500);
    int live = 0;
    for (const Candidate& candidate : candidates) {
        double after = 0.0;
        SIZE_T received = 0;
        if (!ReadProcessMemory(process, reinterpret_cast<void*>(candidate.object + kTimerOffset), &after, sizeof(after), &received) || received != sizeof(after)) continue;
        const double delta = after - candidate.before;
        if (std::isfinite(after) && delta >= 0.25 && delta <= 0.9) {
            std::wprintf(L"LIVE TIMER object=0x%llX address=0x%llX value=%.3f delta=%.3f\n",
                         static_cast<unsigned long long>(candidate.object),
                         static_cast<unsigned long long>(candidate.object + kTimerOffset), after, delta);
            ++live;
        }
    }
    std::wprintf(L"Found %d live Arena race timer(s) from %zu matching objects.\n", live, candidates.size());
    return live ? 0 : 27;
}

int peek_float(HANDLE process, uintptr_t address) {
    float value = 0.0f;
    SIZE_T received = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<void*>(address), &value, sizeof(value), &received) || received != sizeof(value)) return 28;
    std::wprintf(L"0x%llX = %.6f\n", static_cast<unsigned long long>(address), value);
    return 0;
}

int peek_ball(HANDLE process, uintptr_t address) {
    int values[3] = {-1, -1, -1};
    SIZE_T received = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<void*>(address), values, sizeof(values), &received) || received != sizeof(values)) return 30;
    std::wprintf(L"0x%llX = [%d, %d, %d]\n", static_cast<unsigned long long>(address), values[0], values[1], values[2]);
    return 0;
}

int scan_ball_structs(HANDLE process, int expected_total, int expected_current, const std::wstring& output_path) {
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    uintptr_t cursor = reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress);
    const uintptr_t maximum = reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress);
    constexpr size_t kChunkSize = 4 * 1024 * 1024;
    std::vector<uint8_t> buffer(kChunkSize + 8);
    std::vector<uintptr_t> matches;
    MEMORY_BASIC_INFORMATION mbi{};
    while (cursor < maximum && VirtualQueryEx(process, reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi))) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = begin + mbi.RegionSize;
        if (readable_writable_region(mbi)) {
            for (uintptr_t chunk = begin; chunk < end;) {
                const size_t requested = static_cast<size_t>(std::min<uintptr_t>(kChunkSize, end - chunk));
                SIZE_T received = 0;
                if (ReadProcessMemory(process, reinterpret_cast<void*>(chunk), buffer.data(), requested, &received)) {
                    for (size_t offset = 0; offset + 3 * sizeof(int) <= received; offset += sizeof(int)) {
                        int current = 0, total = 0, duplicate = 0;
                        std::memcpy(&current, buffer.data() + offset, sizeof(current));
                        std::memcpy(&total, buffer.data() + offset + 4, sizeof(total));
                        std::memcpy(&duplicate, buffer.data() + offset + 8, sizeof(duplicate));
                        if (total == expected_total && current == expected_current && duplicate == current) {
                            matches.push_back(chunk + offset);
                        }
                    }
                }
                chunk += requested;
            }
        }
        if (end <= cursor) break;
        cursor = end;
    }
    if (!save_addresses(output_path, matches)) return 29;
    std::wprintf(L"Saved %zu [%d, %d, %d] ball structures to %ls\n", matches.size(), expected_current, expected_total, expected_current, output_path.c_str());
    for (size_t i = 0; i < std::min<size_t>(matches.size(), 100); ++i) {
        int current = -1;
        SIZE_T received = 0;
        ReadProcessMemory(process, reinterpret_cast<void*>(matches[i]), &current, sizeof(current), &received);
        std::wprintf(L"  0x%llX current=%d\n", static_cast<unsigned long long>(matches[i]), current);
    }
    return 0;
}

bool timer_form_matches(const uint8_t* data, size_t available, uint32_t form, double elapsed) {
    const double remaining = 3600.0 - elapsed;
    switch (form) {
        case 0: case 1: {
            if (available < sizeof(float)) return false;
            float value = 0.0f; std::memcpy(&value, data, sizeof(value));
            const double target = form == 0 ? elapsed : remaining;
            return std::isfinite(value) && std::fabs(static_cast<double>(value) - target) <= 0.06;
        }
        case 2: case 3: {
            if (available < sizeof(double)) return false;
            double value = 0.0; std::memcpy(&value, data, sizeof(value));
            const double target = form == 2 ? elapsed : remaining;
            return std::isfinite(value) && std::fabs(value - target) <= 0.06;
        }
        case 4: case 5: case 6: case 7: {
            if (available < sizeof(int32_t)) return false;
            int32_t value = 0; std::memcpy(&value, data, sizeof(value));
            const double seconds = (form == 4 || form == 6) ? elapsed : remaining;
            const double scale = (form == 4 || form == 5) ? 100.0 : 1000.0;
            return std::llabs(static_cast<long long>(value) - static_cast<long long>(std::llround(seconds * scale))) <= 2;
        }
        case 8: case 9: {
            if (available < sizeof(int64_t)) return false;
            int64_t value = 0; std::memcpy(&value, data, sizeof(value));
            const double seconds = form == 8 ? elapsed : remaining;
            return std::llabs(value - static_cast<int64_t>(std::llround(seconds * 10'000'000.0))) <= 20'000;
        }
        default: return false;
    }
}

const wchar_t* timer_form_name(uint32_t form) {
    static const wchar_t* names[] = {L"float elapsed", L"float remaining", L"double elapsed", L"double remaining",
                                     L"int centiseconds elapsed", L"int centiseconds remaining", L"int milliseconds elapsed",
                                     L"int milliseconds remaining", L"int64 Unreal ticks elapsed", L"int64 Unreal ticks remaining"};
    return form < 10 ? names[form] : L"unknown";
}

int scan_timer_forms(HANDLE process, double elapsed, const std::wstring& output_path) {
    SYSTEM_INFO system{}; GetNativeSystemInfo(&system);
    uintptr_t cursor = reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress);
    const uintptr_t maximum = reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress);
    constexpr size_t kChunkSize = 4 * 1024 * 1024;
    std::vector<uint8_t> buffer(kChunkSize);
    std::vector<TimerFormRecord> matches;
    size_t form_counts[10]{};
    MEMORY_BASIC_INFORMATION mbi{};
    while (cursor < maximum && VirtualQueryEx(process, reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi))) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = begin + mbi.RegionSize;
        if (readable_writable_region(mbi)) {
            for (uintptr_t chunk = begin; chunk < end;) {
                const size_t requested = static_cast<size_t>(std::min<uintptr_t>(kChunkSize, end - chunk));
                SIZE_T received = 0;
                if (ReadProcessMemory(process, reinterpret_cast<void*>(chunk), buffer.data(), requested, &received)) {
                    for (size_t offset = 0; offset + 4 <= received; offset += 4) {
                        for (uint32_t form = 0; form < 8; ++form) {
                            if ((form == 2 || form == 3) && (offset % 8 != 0)) continue;
                            if (timer_form_matches(buffer.data() + offset, received - offset, form, elapsed)) {
                                matches.push_back({chunk + offset, form}); ++form_counts[form];
                            }
                        }
                        if (offset % 8 == 0) {
                            for (uint32_t form = 8; form < 10; ++form) {
                                if (timer_form_matches(buffer.data() + offset, received - offset, form, elapsed)) {
                                    matches.push_back({chunk + offset, form}); ++form_counts[form];
                                }
                            }
                        }
                    }
                }
                chunk += requested;
            }
        }
        if (end <= cursor) break;
        cursor = end;
    }
    if (!save_timer_forms(output_path, matches)) return 31;
    std::wprintf(L"Saved %zu timer-form candidates for elapsed %.2f seconds:\n", matches.size(), elapsed);
    for (uint32_t form = 0; form < 10; ++form) std::wprintf(L"  %-30ls %zu\n", timer_form_name(form), form_counts[form]);
    return 0;
}

int filter_timer_forms(HANDLE process, double elapsed, const std::wstring& input_path, const std::wstring& output_path) {
    std::vector<TimerFormRecord> records;
    if (!load_timer_forms(input_path, records)) return 32;
    std::vector<TimerFormRecord> matches;
    size_t form_counts[10]{};
    for (const TimerFormRecord& record : records) {
        uint8_t value[8]{}; SIZE_T received = 0;
        const size_t width = (record.form == 2 || record.form == 3 || record.form >= 8) ? 8 : 4;
        if (ReadProcessMemory(process, reinterpret_cast<void*>(record.address), value, width, &received) && received == width &&
            timer_form_matches(value, received, record.form, elapsed)) {
            matches.push_back(record); if (record.form < 10) ++form_counts[record.form];
        }
    }
    if (!save_timer_forms(output_path, matches)) return 33;
    std::wprintf(L"Narrowed %zu timer-form candidates to %zu for elapsed %.2f seconds:\n", records.size(), matches.size(), elapsed);
    for (uint32_t form = 0; form < 10; ++form) if (form_counts[form]) std::wprintf(L"  %-30ls %zu\n", timer_form_name(form), form_counts[form]);
    for (size_t i = 0; i < std::min<size_t>(matches.size(), 100); ++i)
        std::wprintf(L"  0x%llX  %ls\n", static_cast<unsigned long long>(matches[i].address), timer_form_name(matches[i].form));
    return matches.empty() ? 34 : 0;
}

int scan_running_clocks(HANDLE process, const std::wstring& output_path, bool use_double) {
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    uintptr_t cursor = reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress);
    const uintptr_t maximum = reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress);
    constexpr size_t kChunkSize = 4 * 1024 * 1024;
    std::vector<uint8_t> buffer(kChunkSize);
    struct Candidate { uintptr_t address; double value; };
    std::vector<Candidate> candidates;
    MEMORY_BASIC_INFORMATION mbi{};
    const size_t width = use_double ? sizeof(double) : sizeof(float);
    while (cursor < maximum && VirtualQueryEx(process, reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi))) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = begin + mbi.RegionSize;
        if (readable_writable_region(mbi)) {
            for (uintptr_t chunk = begin; chunk < end;) {
                const size_t requested = static_cast<size_t>(std::min<uintptr_t>(kChunkSize, end - chunk));
                SIZE_T received = 0;
                if (ReadProcessMemory(process, reinterpret_cast<void*>(chunk), buffer.data(), requested, &received)) {
                    for (size_t offset = 0; offset + width <= received; offset += width) {
                        double value = 0.0;
                        if (use_double) std::memcpy(&value, buffer.data() + offset, sizeof(double));
                        else { float f = 0.0f; std::memcpy(&f, buffer.data() + offset, sizeof(float)); value = f; }
                        if (std::isfinite(value) && value >= 60.0 && value <= 3700.0) candidates.push_back({chunk + offset, value});
                    }
                }
                chunk += requested;
            }
        }
        if (end <= cursor) break;
        cursor = end;
    }
    std::wprintf(L"Watching %zu plausible %ls clocks...\n", candidates.size(), use_double ? L"double" : L"float");
    for (int pass = 0; pass < 1; ++pass) {
        Sleep(2000);
        std::vector<Candidate> next;
        for (const Candidate& candidate : candidates) {
            SIZE_T received = 0;
            double value = 0.0;
            bool ok = false;
            if (use_double) {
                ok = ReadProcessMemory(process, reinterpret_cast<void*>(candidate.address), &value, sizeof(double), &received) && received == sizeof(double);
            } else {
                float f = 0.0f;
                ok = ReadProcessMemory(process, reinterpret_cast<void*>(candidate.address), &f, sizeof(float), &received) && received == sizeof(float);
                value = f;
            }
            const double delta = value - candidate.value;
            if (ok && std::isfinite(value) && delta >= 1.5 && delta <= 2.5) next.push_back({candidate.address, value});
        }
        candidates.swap(next);
        std::wprintf(L"Pass %d: %zu clocks rising at real-time speed\n", pass + 1, candidates.size());
    }
    std::vector<uintptr_t> addresses;
    addresses.reserve(candidates.size());
    for (const Candidate& candidate : candidates) {
        addresses.push_back(candidate.address);
        std::wprintf(L"  0x%llX = %.6f\n", static_cast<unsigned long long>(candidate.address), candidate.value);
    }
    if (use_double) {
        std::vector<Candidate> sorted = candidates;
        std::sort(sorted.begin(), sorted.end(), [](const Candidate& a, const Candidate& b) { return a.value < b.value; });
        std::wprintf(L"Synchronized double-clock pairs (within 0.20 seconds):\n");
        size_t pair_count = 0;
        for (size_t i = 0; i < sorted.size(); ++i) {
            for (size_t j = i + 1; j < sorted.size() && sorted[j].value - sorted[i].value <= 0.20; ++j) {
                std::wprintf(L"  0x%llX %.6f  <->  0x%llX %.6f  gap=%.6f\n",
                             static_cast<unsigned long long>(sorted[i].address), sorted[i].value,
                             static_cast<unsigned long long>(sorted[j].address), sorted[j].value,
                             sorted[j].value - sorted[i].value);
                ++pair_count;
            }
        }
        std::wprintf(L"Found %zu synchronized pair(s).\n", pair_count);
    }
    if (!save_addresses(output_path, addresses)) return 21;
    return 0;
}

int refine_running_clocks(HANDLE process, const std::wstring& input_path, const std::wstring& output_path, int passes, bool use_double) {
    std::vector<uintptr_t> addresses;
    if (!load_addresses(input_path, addresses)) return 35;
    struct Candidate { uintptr_t address; double value; };
    std::vector<Candidate> candidates;
    candidates.reserve(addresses.size());
    for (uintptr_t address : addresses) {
        double value = 0.0; SIZE_T received = 0; bool ok = false;
        if (use_double) {
            ok = ReadProcessMemory(process, reinterpret_cast<void*>(address), &value, sizeof(value), &received) && received == sizeof(value);
        } else {
            float f = 0.0f;
            ok = ReadProcessMemory(process, reinterpret_cast<void*>(address), &f, sizeof(f), &received) && received == sizeof(f);
            value = f;
        }
        if (ok && std::isfinite(value))
            candidates.push_back({address, value});
    }
    for (int pass = 0; pass < passes; ++pass) {
        Sleep(2000);
        std::vector<Candidate> next;
        for (const Candidate& candidate : candidates) {
            double value = 0.0; SIZE_T received = 0; bool ok = false;
            if (use_double) {
                ok = ReadProcessMemory(process, reinterpret_cast<void*>(candidate.address), &value, sizeof(value), &received) && received == sizeof(value);
            } else {
                float f = 0.0f;
                ok = ReadProcessMemory(process, reinterpret_cast<void*>(candidate.address), &f, sizeof(f), &received) && received == sizeof(f);
                value = f;
            }
            if (!ok || !std::isfinite(value)) continue;
            const double delta = value - candidate.value;
            if (delta >= 1.5 && delta <= 2.5) next.push_back({candidate.address, value});
        }
        candidates.swap(next);
        std::wprintf(L"Refinement pass %d: %zu real-time %ls clocks remain\n", pass + 1, candidates.size(), use_double ? L"double" : L"float");
    }
    addresses.clear(); addresses.reserve(candidates.size());
    for (const Candidate& candidate : candidates) {
        addresses.push_back(candidate.address);
        std::wprintf(L"  0x%llX = %.6f\n", static_cast<unsigned long long>(candidate.address), candidate.value);
    }
    return save_addresses(output_path, addresses) ? 0 : 36;
}

int snapshot_clock_values(HANDLE process, const std::wstring& output_path, bool use_double) {
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    uintptr_t cursor = reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress);
    const uintptr_t maximum = reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress);
    constexpr size_t kChunkSize = 4 * 1024 * 1024;
    std::vector<uint8_t> buffer(kChunkSize);
    std::vector<ClockRecord> records;
    MEMORY_BASIC_INFORMATION mbi{};
    const size_t width = use_double ? sizeof(double) : sizeof(float);
    while (cursor < maximum && VirtualQueryEx(process, reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi))) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = begin + mbi.RegionSize;
        if (readable_writable_region(mbi)) {
            for (uintptr_t chunk = begin; chunk < end;) {
                const size_t requested = static_cast<size_t>(std::min<uintptr_t>(kChunkSize, end - chunk));
                SIZE_T received = 0;
                if (ReadProcessMemory(process, reinterpret_cast<void*>(chunk), buffer.data(), requested, &received)) {
                    for (size_t offset = 0; offset + width <= received; offset += width) {
                        double value = 0.0;
                        if (use_double) std::memcpy(&value, buffer.data() + offset, sizeof(double));
                        else { float f = 0.0f; std::memcpy(&f, buffer.data() + offset, sizeof(float)); value = f; }
                        if (std::isfinite(value) && value >= 60.0 && value <= 10'000.0) records.push_back({chunk + offset, value});
                    }
                }
                chunk += requested;
            }
        }
        if (end <= cursor) break;
        cursor = end;
    }
    if (!save_clock_records(output_path, records)) return 22;
    std::wprintf(L"Saved %zu plausible %ls clock values\n", records.size(), use_double ? L"double" : L"float");
    return 0;
}

int filter_clock_delta(HANDLE process, double expected_delta, double tolerance, const std::wstring& input_path, const std::wstring& output_path, bool use_double) {
    std::vector<ClockRecord> records;
    if (!load_clock_records(input_path, records)) return 23;
    std::vector<ClockRecord> matches;
    matches.reserve(1024);
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    uintptr_t cursor = reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress);
    const uintptr_t maximum = reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress);
    constexpr size_t kChunkSize = 4 * 1024 * 1024;
    std::vector<uint8_t> buffer(kChunkSize);
    size_t index = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    const size_t width = use_double ? sizeof(double) : sizeof(float);
    while (cursor < maximum && index < records.size() && VirtualQueryEx(process, reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi))) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = begin + mbi.RegionSize;
        while (index < records.size() && records[index].address < begin) ++index;
        if (readable_writable_region(mbi)) {
            for (uintptr_t chunk = begin; chunk < end && index < records.size();) {
                const size_t requested = static_cast<size_t>(std::min<uintptr_t>(kChunkSize, end - chunk));
                const uintptr_t chunk_end = chunk + requested;
                SIZE_T received = 0;
                if (ReadProcessMemory(process, reinterpret_cast<void*>(chunk), buffer.data(), requested, &received)) {
                    const uintptr_t read_end = chunk + received;
                    while (index < records.size() && records[index].address < read_end) {
                        const ClockRecord& old = records[index];
                        if (old.address >= chunk && old.address + width <= read_end) {
                            const size_t offset = static_cast<size_t>(old.address - chunk);
                            double value = 0.0;
                            if (use_double) std::memcpy(&value, buffer.data() + offset, sizeof(double));
                            else { float f = 0.0f; std::memcpy(&f, buffer.data() + offset, sizeof(float)); value = f; }
                            const double delta = value - old.value;
                            if (std::isfinite(value) && std::fabs(delta - expected_delta) <= tolerance) {
                                matches.push_back({old.address, value});
                                if (matches.size() <= 200) std::wprintf(L"  0x%llX: %.6f -> %.6f (delta %.6f)\n", static_cast<unsigned long long>(old.address), old.value, value, delta);
                            }
                        }
                        ++index;
                    }
                } else {
                    while (index < records.size() && records[index].address < chunk_end) ++index;
                }
                chunk = chunk_end;
            }
        }
        if (end <= cursor) break;
        cursor = end;
    }
    if (!save_clock_records(output_path, matches)) return 24;
    std::wprintf(L"Narrowed %zu %ls clocks to %zu with delta %.4f (+/- %.4f)\n", records.size(), use_double ? L"double" : L"float", matches.size(), expected_delta, tolerance);
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const bool inspect = argc >= 3 && _wcsicmp(argv[1], L"inspect") == 0;
    const bool roots = argc >= 3 && _wcsicmp(argv[1], L"roots") == 0;
    const bool find_race = argc >= 3 && _wcsicmp(argv[1], L"findrace") == 0;
    const bool analyze = argc >= 3 && _wcsicmp(argv[1], L"analyze") == 0;
    const bool scan_float = argc >= 5 && _wcsicmp(argv[1], L"scanfloat") == 0;
    const bool filter_float_mode = argc >= 6 && _wcsicmp(argv[1], L"filterfloat") == 0;
    const bool scan_double = argc >= 5 && _wcsicmp(argv[1], L"scandouble") == 0;
    const bool scan_ticks = argc >= 5 && _wcsicmp(argv[1], L"scanticks") == 0;
    const bool filter_double = argc >= 6 && _wcsicmp(argv[1], L"filterdouble") == 0;
    const bool filter_ticks = argc >= 6 && _wcsicmp(argv[1], L"filterticks") == 0;
    const bool scan_text = argc >= 4 && _wcsicmp(argv[1], L"scantext") == 0;
    const bool scan_maps = argc >= 2 && _wcsicmp(argv[1], L"scanmaps") == 0;
    const bool scan_timer = argc >= 2 && _wcsicmp(argv[1], L"scantimer") == 0;
    const bool peek_float_mode = argc >= 3 && _wcsicmp(argv[1], L"peekfloat") == 0;
    const bool peek_ball_mode = argc >= 3 && _wcsicmp(argv[1], L"peekball") == 0;
    const bool scan_balls = argc >= 5 && _wcsicmp(argv[1], L"scanballs") == 0;
    const bool scan_forms = argc >= 4 && _wcsicmp(argv[1], L"scanforms") == 0;
    const bool filter_forms = argc >= 5 && _wcsicmp(argv[1], L"filterforms") == 0;
    const bool scan_running_float = argc >= 3 && _wcsicmp(argv[1], L"scanrunningfloat") == 0;
    const bool scan_running_double = argc >= 3 && _wcsicmp(argv[1], L"scanrunningdouble") == 0;
    const bool refine_running_float = argc >= 5 && _wcsicmp(argv[1], L"refinerunningfloat") == 0;
    const bool refine_running_double = argc >= 5 && _wcsicmp(argv[1], L"refinerunningdouble") == 0;
    const bool snapshot_clocks_float = argc >= 3 && _wcsicmp(argv[1], L"snapshotclocksfloat") == 0;
    const bool snapshot_clocks_double = argc >= 3 && _wcsicmp(argv[1], L"snapshotclocksdouble") == 0;
    const bool delta_clocks_float = argc >= 6 && _wcsicmp(argv[1], L"deltaclocksfloat") == 0;
    const bool delta_clocks_double = argc >= 6 && _wcsicmp(argv[1], L"deltaclocksdouble") == 0;
    const bool special_clock_mode = snapshot_clocks_float || snapshot_clocks_double || delta_clocks_float || delta_clocks_double;
    if ((!inspect && !roots && !find_race && !analyze && !scan_float && !filter_float_mode && !scan_double && !scan_ticks && !filter_double && !filter_ticks && !scan_text && !scan_maps && !scan_timer && !peek_float_mode && !peek_ball_mode && !scan_balls && !scan_forms && !filter_forms && !scan_running_float && !scan_running_double && !refine_running_float && !refine_running_double && !special_clock_mode && argc < 4) || (!inspect && !roots && !find_race && !analyze && !scan_float && !filter_float_mode && !scan_double && !scan_ticks && !filter_double && !filter_ticks && !scan_text && !scan_maps && !scan_timer && !peek_float_mode && !peek_ball_mode && !scan_balls && !scan_forms && !filter_forms && !scan_running_float && !scan_running_double && !refine_running_float && !refine_running_double && !special_clock_mode && _wcsicmp(argv[1], L"snapshot") != 0 && _wcsicmp(argv[1], L"filter") != 0)) {
        std::fwprintf(stderr, L"Usage:\n  ArenaRaceProbe snapshot <value> <output.bin>\n  ArenaRaceProbe filter <value> <input.bin> <output.bin>\n  ArenaRaceProbe inspect <input.bin>\n  ArenaRaceProbe roots <input.bin>\n  ArenaRaceProbe findrace <current-count>\n  ArenaRaceProbe analyze <clock-address-hex>\n  ArenaRaceProbe scanfloat <seconds> <tolerance> <output.bin>\n  ArenaRaceProbe filterfloat <seconds> <tolerance> <input.bin> <output.bin>\n  ArenaRaceProbe scandouble <seconds> <tolerance> <output.bin>\n  ArenaRaceProbe scanticks <seconds> <tolerance> <output.bin>\n  ArenaRaceProbe filterdouble <seconds> <tolerance> <input.bin> <output.bin>\n  ArenaRaceProbe filterticks <seconds> <tolerance> <input.bin> <output.bin>\n");
        return 1;
    }
    const DWORD process_id = find_process();
    if (!process_id) {
        std::fwprintf(stderr, L"Arena Reloaded is not running.\n");
        return 2;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, process_id);
    if (!process) {
        std::fwprintf(stderr, L"Could not open process %lu (error %lu).\n", process_id, GetLastError());
        return 3;
    }
    const int target = (inspect || roots || analyze || scan_float || filter_float_mode || scan_double || scan_ticks || filter_double || filter_ticks || scan_text || scan_maps || scan_timer || peek_float_mode || peek_ball_mode || scan_forms || filter_forms || scan_running_float || scan_running_double || refine_running_float || refine_running_double || special_clock_mode) ? 0 : _wtoi(argv[2]);
    int result = 0;
    if (inspect) {
        result = inspect_addresses(process, argv[2]);
    } else if (roots) {
        result = inspect_roots(process, process_id, argv[2]);
    } else if (find_race) {
        result = find_race_state(process, target);
    } else if (analyze) {
        const uintptr_t address = static_cast<uintptr_t>(_wcstoui64(argv[2], nullptr, 16));
        result = analyze_race_state(process, process_id, address);
    } else if (scan_float) {
        result = snapshot_float(process, static_cast<float>(_wtof(argv[2])), static_cast<float>(_wtof(argv[3])), argv[4]);
    } else if (filter_float_mode) {
        result = filter_float(process, static_cast<float>(_wtof(argv[2])), static_cast<float>(_wtof(argv[3])), argv[4], argv[5]);
    } else if (scan_double || scan_ticks) {
        result = snapshot_double_or_ticks(process, _wtof(argv[2]), _wtof(argv[3]), argv[4], scan_ticks);
    } else if (filter_double || filter_ticks) {
        result = filter_double_or_ticks(process, _wtof(argv[2]), _wtof(argv[3]), argv[4], argv[5], filter_ticks);
    } else if (scan_text) {
        result = scan_wide_text(process, argv[2], argv[3]);
    } else if (scan_maps) {
        result = scan_map_labels(process);
    } else if (scan_timer) {
        result = scan_arena_timers(process, process_id);
    } else if (peek_float_mode) {
        result = peek_float(process, static_cast<uintptr_t>(_wcstoui64(argv[2], nullptr, 16)));
    } else if (peek_ball_mode) {
        result = peek_ball(process, static_cast<uintptr_t>(_wcstoui64(argv[2], nullptr, 16)));
    } else if (scan_balls) {
        result = scan_ball_structs(process, _wtoi(argv[2]), _wtoi(argv[3]), argv[4]);
    } else if (scan_forms) {
        result = scan_timer_forms(process, _wtof(argv[2]), argv[3]);
    } else if (filter_forms) {
        result = filter_timer_forms(process, _wtof(argv[2]), argv[3], argv[4]);
    } else if (scan_running_float || scan_running_double) {
        result = scan_running_clocks(process, argv[2], scan_running_double);
    } else if (refine_running_float || refine_running_double) {
        result = refine_running_clocks(process, argv[2], argv[3], _wtoi(argv[4]), refine_running_double);
    } else if (snapshot_clocks_float || snapshot_clocks_double) {
        result = snapshot_clock_values(process, argv[2], snapshot_clocks_double);
    } else if (delta_clocks_float || delta_clocks_double) {
        result = filter_clock_delta(process, _wtof(argv[2]), _wtof(argv[3]), argv[4], argv[5], delta_clocks_double);
    } else if (_wcsicmp(argv[1], L"snapshot") == 0) {
        result = snapshot_value(process, target, argv[3]);
    } else {
        if (argc < 5) result = 1;
        else result = filter_value(process, target, argv[3], argv[4]);
    }
    CloseHandle(process);
    return result;
}
