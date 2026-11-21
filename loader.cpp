#include <Windows.h>
#include <TlHelp32.h>
#include <WinHttp.h>
#include <algorithm>
#include <cstddef>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

extern const unsigned char g_bundled_overlay_dll[];
extern const size_t g_bundled_overlay_dll_size;
extern const unsigned char g_bundled_logo_png[];
extern const size_t g_bundled_logo_png_size;

static constexpr wchar_t CURRENT_VERSION[] = L"1.1.3";
static constexpr char CURRENT_VERSION_UTF8[] = "1.1.3";
static constexpr wchar_t RELEASE_API[] = L"https://api.github.com/repos/cody-raves/Splitsgate/releases/latest";

struct GameProcess {
    DWORD pid = 0;
    std::wstring name;
};

struct ReleaseInfo {
    std::wstring version;
    std::wstring asset_url;
    bool is_executable = false;
};

static std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

static std::string wide_to_utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), length, nullptr, nullptr);
    return result;
}

static std::optional<std::vector<unsigned char>> http_get(const std::wstring& url) {
    URL_COMPONENTS parts{sizeof(parts)};
    wchar_t host[256]{};
    wchar_t path[2048]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    parts.dwSchemeLength = 1;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) return std::nullopt;
    HINTERNET session = WinHttpOpen(L"Splitsgate-Updater/1.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return std::nullopt;
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 30000);
    HINTERNET connection = WinHttpConnect(session, std::wstring(host, parts.dwHostNameLength).c_str(), parts.nPort, 0);
    if (!connection) { WinHttpCloseHandle(session); return std::nullopt; }
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connection, L"GET", std::wstring(path, parts.dwUrlPathLength).c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) { WinHttpCloseHandle(connection); WinHttpCloseHandle(session); return std::nullopt; }
    const wchar_t headers[] = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    bool ok = WinHttpSendRequest(request, headers, static_cast<DWORD>(-1), nullptr, 0, 0, 0) &&
              WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (ok) ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX) && status == 200;
    std::vector<unsigned char> result;
    while (ok) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) { ok = false; break; }
        if (!available) break;
        const size_t offset = result.size();
        result.resize(offset + available);
        DWORD received = 0;
        if (!WinHttpReadData(request, result.data() + offset, available, &received)) { ok = false; break; }
        result.resize(offset + received);
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok ? std::optional<std::vector<unsigned char>>(std::move(result)) : std::nullopt;
}

static bool http_post_json(const std::wstring& url, const std::string& body) {
    URL_COMPONENTS parts{sizeof(parts)};
    wchar_t host[256]{};
    wchar_t path[2048]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    parts.dwSchemeLength = 1;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) return false;
    HINTERNET session = WinHttpOpen(L"Splitsgate-CrashReporter/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 15000);
    HINTERNET connection = WinHttpConnect(session, std::wstring(host, parts.dwHostNameLength).c_str(), parts.nPort, 0);
    if (!connection) { WinHttpCloseHandle(session); return false; }
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connection, L"POST", std::wstring(path, parts.dwUrlPathLength).c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) { WinHttpCloseHandle(connection); WinHttpCloseHandle(session); return false; }
    const wchar_t headers[] = L"Content-Type: application/json\r\nAccept: application/json\r\n";
    bool ok = WinHttpSendRequest(request, headers, static_cast<DWORD>(-1),
        const_cast<char*>(body.data()), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) &&
        WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (ok) ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX) && status >= 200 && status < 300;
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}

static std::string json_string(const std::string& json, const std::string& key, size_t start = 0) {
    const std::string marker = "\"" + key + "\"";
    size_t pos = json.find(marker, start);
    if (pos == std::string::npos || (pos = json.find(':', pos + marker.size())) == std::string::npos ||
        (pos = json.find('"', pos + 1)) == std::string::npos) return {};
    std::string value;
    for (++pos; pos < json.size(); ++pos) {
        if (json[pos] == '"') break;
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            const char escaped = json[++pos];
            value.push_back(escaped == '/' ? '/' : escaped);
        } else value.push_back(json[pos]);
    }
    return value;
}

static std::vector<int> version_parts(std::wstring version) {
    if (!version.empty() && (version.front() == L'v' || version.front() == L'V')) version.erase(version.begin());
    std::vector<int> parts;
    std::wstringstream stream(version);
    std::wstring part;
    while (std::getline(stream, part, L'.')) parts.push_back(_wtoi(part.c_str()));
    while (parts.size() < 3) parts.push_back(0);
    return parts;
}

static bool newer_version(const std::wstring& candidate) {
    return version_parts(candidate) > version_parts(CURRENT_VERSION);
}

static bool ends_with_case_insensitive(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
        _stricmp(value.c_str() + value.size() - suffix.size(), suffix.c_str()) == 0;
}

static std::optional<ReleaseInfo> latest_release(const std::wstring& preferred_asset) {
    auto bytes = http_get(RELEASE_API);
    if (!bytes) return std::nullopt;
    const std::string json(bytes->begin(), bytes->end());
    const std::string tag = json_string(json, "tag_name");
    if (tag.empty()) return std::nullopt;
    std::string executable_url;
    std::string dual_url;
    std::string first_executable_url;
    std::string zip_url;
    size_t position = 0;
    while ((position = json.find("\"browser_download_url\"", position)) != std::string::npos) {
        const std::string url = json_string(json, "browser_download_url", position);
        if (ends_with_case_insensitive(url, ".exe")) {
            if (first_executable_url.empty()) first_executable_url = url;
            const size_t slash = url.find_last_of('/');
            const std::wstring filename = utf8_to_wide(slash == std::string::npos ? url : url.substr(slash + 1));
            if (_wcsicmp(filename.c_str(), preferred_asset.c_str()) == 0) executable_url = url;
            if (_wcsicmp(filename.c_str(), L"Splitsgate.exe") == 0) dual_url = url;
        }
        else if (ends_with_case_insensitive(url, ".zip")) zip_url = url;
        position += 22;
    }
    if (executable_url.empty()) executable_url = !dual_url.empty() ? dual_url : first_executable_url;
    const bool use_executable = !executable_url.empty();
    const std::string& url = use_executable ? executable_url : zip_url;
    if (url.empty()) return std::nullopt;
    return ReleaseInfo{utf8_to_wide(tag), utf8_to_wide(url), use_executable};
}

static bool run_hidden_and_wait(const std::wstring& command) {
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    std::wstring line = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"" + command + L"\"";
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        return false;
    WaitForSingleObject(process.hProcess, 30000);
    DWORD result = 1;
    GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result == 0;
}

static bool schedule_self_replacement(const std::filesystem::path& source, const std::filesystem::path& target) {
    const std::wstring command = L"Wait-Process -Id " + std::to_wstring(GetCurrentProcessId()) +
        L"; Copy-Item -LiteralPath '" + source.wstring() + L"' -Destination '" + target.wstring() +
        L"' -Force; Start-Process -FilePath '" + target.wstring() + L"' -WindowStyle Hidden";
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    std::wstring line = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"" + command + L"\"";
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

static bool valid_executable(const std::vector<unsigned char>& bytes) {
    return bytes.size() > 4096 && bytes[0] == 'M' && bytes[1] == 'Z';
}

static bool install_release(const ReleaseInfo& release, const std::filesystem::path& current_exe) {
    auto download = http_get(release.asset_url);
    if (!download || download->empty()) return false;
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        (L"SplitsgateUpdate-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    std::filesystem::path new_exe;
    if (release.is_executable) {
        if (!valid_executable(*download)) return false;
        new_exe = root / L"Splitsgate.exe";
        std::ofstream output(new_exe, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(download->data()), static_cast<std::streamsize>(download->size()));
        output.close();
    } else {
        const std::filesystem::path zip = root / L"release.zip";
        std::ofstream output(zip, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(download->data()), static_cast<std::streamsize>(download->size()));
        output.close();
        const std::filesystem::path expanded = root / L"expanded";
        if (!run_hidden_and_wait(L"Expand-Archive -LiteralPath '" + zip.wstring() +
                L"' -DestinationPath '" + expanded.wstring() + L"' -Force")) return false;
        for (const auto& item : std::filesystem::recursive_directory_iterator(expanded, ec)) {
            if (item.is_regular_file() && _wcsicmp(item.path().extension().c_str(), L".exe") == 0) {
                new_exe = item.path();
                if (_wcsicmp(item.path().filename().c_str(), L"SplitgateOverlayLoader.exe") == 0) break;
            }
        }
    }
    if (new_exe.empty() || !std::filesystem::exists(new_exe)) return false;
    return schedule_self_replacement(new_exe, current_exe);
}

static bool check_for_update(const std::filesystem::path& current_exe) {
    const auto release = latest_release(current_exe.filename().wstring());
    if (!release || !newer_version(release->version)) return false;
    if (install_release(*release, current_exe)) return true;
    // A failed network check must never block the player. Continue loading the
    // bundled overlay and try the update again the next time the launcher runs.
    return false;
}

static std::filesystem::path runtime_directory() {
    wchar_t local_app_data[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH);
    if (length > 0 && length < MAX_PATH)
        return std::filesystem::path(local_app_data) / L"Splitsgate" / L"Runtime";
    return std::filesystem::temp_directory_path() / L"Splitsgate" / L"Runtime";
}

static std::string json_escape(const std::string& value) {
    std::ostringstream escaped;
    for (const unsigned char character : value) {
        switch (character) {
        case '\\': escaped << "\\\\"; break;
        case '"': escaped << "\\\""; break;
        case '\n': escaped << "\\n"; break;
        case '\r': escaped << "\\r"; break;
        case '\t': escaped << "\\t"; break;
        default:
            if (character < 0x20) escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(character);
            else escaped << static_cast<char>(character);
        }
    }
    return escaped.str();
}

static bool automatic_crash_reports_enabled(const std::filesystem::path& runtime) {
    std::ifstream input(runtime / L"splitgate-overlay-settings.ini");
    std::string line;
    while (std::getline(input, line)) {
        if (line == "automatic_crash_reports=1") return true;
        if (line == "automatic_crash_reports=0") return false;
    }
    return false;
}

static std::wstring crash_report_endpoint(const std::filesystem::path& runtime) {
    wchar_t environment[2048]{};
    const DWORD length = GetEnvironmentVariableW(L"SPLITSGATE_CRASH_REPORT_URL", environment, static_cast<DWORD>(std::size(environment)));
    if (length > 0 && length < std::size(environment)) return environment;
    std::wifstream input(runtime / L"crash-report-endpoint.txt");
    std::wstring endpoint;
    std::getline(input, endpoint);
    while (!endpoint.empty() && iswspace(endpoint.back())) endpoint.pop_back();
    return endpoint;
}

static std::string utc_timestamp() {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    char buffer[40]{};
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    return buffer;
}

static std::string sanitized_settings_json(const std::filesystem::path& runtime) {
    static constexpr const char* allowed[] = {
        "arena_hud_scale", "sg1_hud_scale", "arena_text_scale", "sg1_text_scale",
        "arena_panel_x", "arena_panel_y", "sg1_panel_x", "sg1_panel_y",
        "arena_hide_outside_race"
    };
    std::ifstream input(runtime / L"splitgate-overlay-settings.ini");
    std::ostringstream json;
    json << '{';
    bool first = true;
    std::string line;
    while (std::getline(input, line)) {
        const size_t separator = line.find('=');
        if (separator == std::string::npos) continue;
        const std::string key = line.substr(0, separator);
        if (std::find(std::begin(allowed), std::end(allowed), key) == std::end(allowed)) continue;
        const std::string value = line.substr(separator + 1);
        if (!first) json << ',';
        first = false;
        json << '"' << json_escape(key) << "\":\"" << json_escape(value) << '"';
    }
    json << '}';
    return json.str();
}

static bool is_probable_crash_exit(DWORD exit_code) {
    return exit_code != 0 && exit_code != STILL_ACTIVE;
}

static int monitor_game_process(DWORD pid, const std::wstring& game_name) {
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return 1;
    WaitForSingleObject(process, INFINITE);
    DWORD exit_code = 0;
    const bool have_exit_code = GetExitCodeProcess(process, &exit_code) != FALSE;
    CloseHandle(process);
    if (!have_exit_code || !is_probable_crash_exit(exit_code)) return 0;

    const std::filesystem::path runtime = runtime_directory();
    std::error_code ec;
    const std::filesystem::path pending = runtime / L"CrashReports";
    std::filesystem::create_directories(pending, ec);
    std::ostringstream code;
    code << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << exit_code;
    const std::string timestamp = utc_timestamp();
    const std::string game = wide_to_utf8(game_name);
    std::ostringstream report;
    report << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"kind\": \"game_process_exit\",\n"
           << "  \"timestamp_utc\": \"" << timestamp << "\",\n"
           << "  \"splitsgate_version\": \"" << CURRENT_VERSION_UTF8 << "\",\n"
           << "  \"game\": \"" << json_escape(game) << "\",\n"
           << "  \"exit_code\": \"" << code.str() << "\",\n"
           << "  \"settings\": " << sanitized_settings_json(runtime) << "\n"
           << "}\n";
    std::string filename = timestamp;
    std::replace(filename.begin(), filename.end(), ':', '-');
    const std::filesystem::path report_path = pending / utf8_to_wide("crash-" + filename + ".json");
    std::ofstream output(report_path, std::ios::binary | std::ios::trunc);
    output << report.str();
    output.close();

    if (!automatic_crash_reports_enabled(runtime)) return 0;
    const std::wstring endpoint = crash_report_endpoint(runtime);
    if (!endpoint.empty() && http_post_json(endpoint, report.str())) std::filesystem::remove(report_path, ec);
    return 0;
}

static bool start_crash_monitor(const std::filesystem::path& executable, const GameProcess& game) {
    std::wstring command = L"\"" + executable.wstring() + L"\" --monitor " + std::to_wstring(game.pid) +
        L" \"" + game.name + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(), &startup, &process)) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

static bool payload_matches(const std::filesystem::path& path, const unsigned char* payload, size_t size) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || std::filesystem::file_size(path, ec) != size) return false;
    std::ifstream input(path, std::ios::binary);
    std::vector<unsigned char> buffer(64 * 1024);
    size_t offset = 0;
    while (offset < size && input) {
        const size_t wanted = std::min(buffer.size(), size - offset);
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(wanted));
        const size_t received = static_cast<size_t>(input.gcount());
        if (received != wanted || !std::equal(buffer.begin(), buffer.begin() + received, payload + offset)) return false;
        offset += received;
    }
    return offset == size;
}

static bool write_payload(const std::filesystem::path& path, const unsigned char* payload, size_t size) {
    if (payload_matches(path, payload, size)) return true;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path temporary = path.wstring() + L".new";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(payload), static_cast<std::streamsize>(size));
    output.close();
    if (!output) return false;
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

static void migrate_user_file(const std::filesystem::path& old_directory,
        const std::filesystem::path& runtime, const wchar_t* name) {
    const auto source = old_directory / name;
    const auto target = runtime / name;
    std::error_code ec;
    if (!std::filesystem::exists(target, ec) && std::filesystem::exists(source, ec))
        std::filesystem::copy_file(source, target, std::filesystem::copy_options::none, ec);
}

enum class TargetMode { Dual, Splitgate1, Splitgate2 };

static TargetMode target_mode_for(const std::filesystem::path& executable) {
    std::wstring name = executable.stem().wstring();
    std::transform(name.begin(), name.end(), name.begin(), towlower);
    if (name.find(L"splitgate1") != std::wstring::npos) return TargetMode::Splitgate1;
    if (name.find(L"splitgate2") != std::wstring::npos) return TargetMode::Splitgate2;
    return TargetMode::Dual;
}

static std::vector<GameProcess> find_splitgate_games(TargetMode mode) {
    std::vector<GameProcess> games;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return games;
    PROCESSENTRY32W entry{sizeof(entry)};
    if (Process32FirstW(snapshot, &entry)) {
        do {
            const bool splitgate1 = _wcsicmp(entry.szExeFile, L"PortalWars-Win64-Shipping.exe") == 0;
            const bool splitgate2 = _wcsicmp(entry.szExeFile, L"PortalWars2-Win64-Shipping.exe") == 0;
            if ((splitgate1 && mode != TargetMode::Splitgate2) || (splitgate2 && mode != TargetMode::Splitgate1))
                games.push_back({entry.th32ProcessID, splitgate1 ? L"Splitgate 1" : L"Splitgate 2"});
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return games;
}

static bool inject_overlay(DWORD pid, const std::wstring& path) {
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!process) return false;
    const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(process, remote, path.c_str(), bytes, nullptr)) {
        if (remote) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }
    auto load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, load_library, remote, 0, nullptr);
    if (!thread) {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }
    WaitForSingleObject(thread, 10000);
    DWORD result = 0;
    GetExitCodeThread(thread, &result);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    return result != 0;
}

int wmain(int argc, wchar_t** argv) {
    wchar_t executable_buffer[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable_buffer, MAX_PATH);
    const std::filesystem::path executable = executable_buffer;
    if (argc >= 4 && _wcsicmp(argv[1], L"--monitor") == 0) {
        return monitor_game_process(static_cast<DWORD>(_wcstoui64(argv[2], nullptr, 10)), argv[3]);
    }
    if (check_for_update(executable)) return 0;

    const std::filesystem::path runtime = runtime_directory();
    std::error_code ec;
    std::filesystem::create_directories(runtime / L"assets", ec);
    migrate_user_file(executable.parent_path(), runtime, L"splitgate-overlay-settings.ini");
    migrate_user_file(executable.parent_path(), runtime, L"splitgate-personal-bests.tsv");
    const std::filesystem::path dll = runtime / L"SplitgateSplitsOverlay.dll";
    if (!write_payload(dll, g_bundled_overlay_dll, g_bundled_overlay_dll_size) ||
        !write_payload(runtime / L"assets" / L"splitgate-new-s-mark-clean.png", g_bundled_logo_png, g_bundled_logo_png_size)) {
        MessageBoxW(nullptr, L"The bundled overlay could not be prepared. Unload the current overlay and try again.",
            L"Splitsgate", MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
        return 2;
    }
    if (argc > 1 && _wcsicmp(argv[1], L"--prepare-only") == 0) return 0;

    const TargetMode mode = target_mode_for(executable);
    const std::vector<GameProcess> games = find_splitgate_games(mode);
    if (games.empty()) {
        const wchar_t* game_name = mode == TargetMode::Splitgate1 ? L"Splitgate 1" :
            (mode == TargetMode::Splitgate2 ? L"Splitgate 2" : L"Splitgate 1 or Splitgate 2");
        const std::wstring message = L"Start " + std::wstring(game_name) + L", then open this launcher again.";
        MessageBoxW(nullptr, message.c_str(), L"Splitsgate",
            MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
        return 1;
    }
    const std::wstring path = dll.wstring();
    bool success = true;
    for (const GameProcess& game : games) {
        const bool injected = inject_overlay(game.pid, path);
        if (injected) start_crash_monitor(executable, game);
        success = injected && success;
    }
    if (!success) {
        MessageBoxW(nullptr, L"The overlay could not be loaded. Unload any existing copy and try again.",
            L"Splitsgate", MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
        return 3;
    }
    return 0;
}
