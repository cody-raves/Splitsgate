#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <wincodec.h>
#include <atomic>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "MinHook.h"
#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
using ExecuteCommandListsFn = void(__stdcall*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using RestartRaceFn = void(__fastcall*)(void*);
using RestartVisibilityFn = uint8_t(__fastcall*)(void*);
using DifficultySelectedFn = void(__fastcall*)(void*, void*);
using MapSelectedExecFn = void(__fastcall*)(void*, void*, void*);
using MapSelectedFn = void(__fastcall*)(void*, void*);

static PresentFn g_original_present = nullptr;
static std::atomic_int g_active_hook_calls = 0;
static ExecuteCommandListsFn g_original_execute_command_lists = nullptr;
static std::atomic<ID3D12CommandQueue*> g_arena_command_queue = nullptr;
static ID3D12Device* g_arena_d3d12_device = nullptr;
static ID3D12CommandAllocator* g_arena_copy_allocator = nullptr;
static ID3D12GraphicsCommandList* g_arena_copy_list = nullptr;
static ID3D12Resource* g_arena_readback = nullptr;
static ID3D12Fence* g_arena_copy_fence = nullptr;
static HANDLE g_arena_copy_event = nullptr;
static UINT64 g_arena_copy_fence_value = 0;
static UINT g_arena_readback_width = 0;
static UINT g_arena_readback_height = 0;
static UINT g_arena_readback_pitch = 0;
static DXGI_FORMAT g_arena_readback_format = DXGI_FORMAT_UNKNOWN;
static RestartRaceFn g_original_restart_race = nullptr;
static RestartVisibilityFn g_original_restart_visibility = nullptr;
static DifficultySelectedFn g_original_difficulty_selected = nullptr;
static MapSelectedExecFn g_original_map_selected_exec = nullptr;
static MapSelectedFn g_original_map_selected = nullptr;
static std::atomic_bool g_map_selected_hook_ready = false;
static std::atomic_uintptr_t g_map_selected_hook_target = 0;
static std::atomic_uintptr_t g_restart_widget = 0;
static std::atomic_uintptr_t g_restart_controller = 0;
static std::atomic<float> g_restart_hold_progress = 0.0f;
static std::atomic_bool g_restart_dispatch_pending = false;
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static ID3D11RenderTargetView* g_render_target = nullptr;
static ID3D11ShaderResourceView* g_logo_texture = nullptr;
static HWND g_game_window = nullptr;
static WNDPROC g_original_wndproc = nullptr;
static std::atomic_bool g_restart_key_down = false;
static std::atomic_bool g_settings_open = false;
static std::atomic_bool g_settings_key_down = false;
static std::atomic_bool g_rebind_listening = false;
static std::atomic_uint g_restart_virtual_key = 'R';
static std::atomic_bool g_imgui_ready = false;
static HMODULE g_module = nullptr;
static ImFont* g_font_body = nullptr;
static ImFont* g_font_header = nullptr;
static ImFont* g_font_clock = nullptr;
static std::atomic_uintptr_t g_race_object = 0;
static uintptr_t g_render_race_object = 0;
static std::array<double, 101> g_splits{};
static std::array<std::atomic<double>, 101> g_sampled_splits{};
static int g_last_count = -1;
static double g_previous_clock = -1.0;
static bool g_waiting_for_start_reset = false;
static bool g_prestart_gate = false;
static bool g_prestart_saw_zero_count = false;
static std::chrono::steady_clock::time_point g_prestart_ignore_until{};
static std::atomic_bool g_restart_transition_pending = false;
static bool g_race_running = false;
static bool g_finished = false;
static double g_finish_time = 0.0;
static bool g_finish_banner_seen = false;
static int g_finish_banner_missing_frames = 0;
static ID3D11Texture2D* g_finish_probe = nullptr;
static std::chrono::steady_clock::time_point g_last_finish_probe{};
static int g_total_balls = 0;
static int g_total_candidate = 0;
static int g_total_candidate_hits = 0;
static ID3D11Texture2D* g_banner_probe = nullptr;
static ID3D11Texture2D* g_lobby_probe = nullptr;
static std::chrono::steady_clock::time_point g_last_lobby_probe{};
static std::atomic_bool g_in_race_lobby = false;
static std::chrono::steady_clock::time_point g_last_ball_time{};
static std::string g_map_name = "RACE";
static std::string g_difficulty = "READY";
static std::atomic_bool g_live_profile_seen = false;
static std::chrono::steady_clock::time_point g_last_profile_read{};
static bool g_arena_reloaded = false;
static ID3D11Texture2D* g_arena_hud_probe = nullptr;
static std::chrono::steady_clock::time_point g_last_arena_hud_probe{};
static std::atomic<double> g_arena_clock = 0.0;
static std::atomic_int g_arena_count = 0;
static std::atomic_int g_arena_total = 0;
static std::atomic_bool g_arena_hud_valid = false;
static bool g_arena_pixels_r10 = false;

// PortalWars-Win64-Shipping.exe 2026 race pause-menu handler. Keeping this as
// an RVA makes it independent of ASLR; the hook is also a useful guard against
// trying to restart from the render or sampler thread.
constexpr uintptr_t RESTART_RACE_HANDLER_RVA = 0x137F3E0;
constexpr uintptr_t RESTART_VISIBILITY_HANDLER_RVA = 0x1366990;
constexpr uintptr_t RESTART_WIDGET_VTABLE_RVA = 0x4393540;
constexpr uintptr_t DIFFICULTY_SELECTED_RVA = 0x137BBA0;
constexpr uintptr_t MAP_SELECTED_EXEC_RVA = 0x14681C0;
constexpr UINT RESTART_WINDOW_MESSAGE = WM_APP + 0x247;
constexpr float RESTART_HOLD_SECONDS = 0.85f;
static bool readable_address(uintptr_t address, size_t size);

static const std::array<const char*, 14> LIVE_MAP_NAMES = {
    "ABYSS", "ATLANTIS", "CRAG", "CLUB SILO", "FOREGONE DESTRUCTION", "HELIX", "HIGHWIND",
    "IMPACT", "KARMAN STATION", "LAVAWELL", "OASIS", "OLYMPUS", "PANTHEON", "STADIUM"
};
static const std::array<const char*, 3> LIVE_DIFFICULTIES = {"EASY", "MEDIUM", "HARD"};

static bool region_contains_label(uintptr_t address, size_t bytes, const char* label) {
    if (!readable_address(address, bytes)) return false;
    const size_t length = std::strlen(label);
    const auto* data = reinterpret_cast<const uint8_t*>(address);
    for (size_t i = 0; i + length <= bytes; ++i) {
        bool ascii_match = true;
        for (size_t j = 0; j < length; ++j) {
            if (std::toupper(data[i + j]) != label[j]) { ascii_match = false; break; }
        }
        if (ascii_match) return true;
    }
    for (size_t i = 0; i + length * 2 <= bytes; i += 2) {
        bool wide_match = true;
        for (size_t j = 0; j < length; ++j) {
            if (data[i + j * 2 + 1] != 0 || std::toupper(data[i + j * 2]) != label[j]) { wide_match = false; break; }
        }
        if (wide_match) return true;
    }
    return false;
}

template <size_t N>
static const char* find_live_label(uintptr_t root, const std::array<const char*, N>& labels) {
    if (!root || !readable_address(root, 0x100)) return nullptr;
    std::array<uintptr_t, 96> queue{};
    size_t head = 0, tail = 1;
    queue[0] = root;
    for (int depth = 0; depth < 3 && head < tail; ++depth) {
        const size_t level_end = tail;
        while (head < level_end) {
            const uintptr_t current = queue[head++];
            const size_t span = depth == 0 ? 0x1200 : 0x500;
            if (!readable_address(current, span)) continue;
            for (const char* label : labels) if (region_contains_label(current, span, label)) return label;
            for (size_t offset = 0; offset + sizeof(uintptr_t) <= span && tail < queue.size(); offset += sizeof(uintptr_t)) {
                uintptr_t child = 0;
                std::memcpy(&child, reinterpret_cast<void*>(current + offset), sizeof(child));
                if (child < 0x10000 || !readable_address(child, 0x80)) continue;
                bool seen = false;
                for (size_t k = 0; k < tail; ++k) if (queue[k] == child) { seen = true; break; }
                if (!seen) queue[tail++] = child;
            }
        }
    }
    return nullptr;
}

template <size_t N>
static const char* find_text_field_label(uintptr_t field, const std::array<const char*, N>& labels) {
    if (!field || !readable_address(field, 0x40)) return nullptr;
    for (const char* label : labels) if (region_contains_label(field, 0x40, label)) return label;
    for (size_t offset = 0; offset + sizeof(uintptr_t) <= 0x40; offset += sizeof(uintptr_t)) {
        uintptr_t text_data = 0;
        std::memcpy(&text_data, reinterpret_cast<void*>(field + offset), sizeof(text_data));
        if (!text_data || !readable_address(text_data, 0x200)) continue;
        for (const char* label : labels) if (region_contains_label(text_data, 0x200, label)) return label;
        for (size_t nested_offset = 0; nested_offset + sizeof(uintptr_t) <= 0x80; nested_offset += sizeof(uintptr_t)) {
            uintptr_t text_buffer = 0;
            std::memcpy(&text_buffer, reinterpret_cast<void*>(text_data + nested_offset), sizeof(text_buffer));
            if (!text_buffer || !readable_address(text_buffer, 0x80)) continue;
            for (const char* label : labels) if (region_contains_label(text_buffer, 0x80, label)) return label;
        }
    }
    return nullptr;
}

static uint32_t map_labels_at_pointer_field(uintptr_t object, size_t offset) {
    if (!readable_address(object + offset, sizeof(uintptr_t))) return 0;
    uintptr_t data = 0;
    std::memcpy(&data, reinterpret_cast<void*>(object + offset), sizeof(data));
    if (!data || !readable_address(data, 0x300)) return 0;
    uint32_t matches = 0;
    for (size_t i = 0; i < LIVE_MAP_NAMES.size(); ++i) {
        if (region_contains_label(data, 0x300, LIVE_MAP_NAMES[i])) matches |= (1u << i);
    }
    return matches;
}

static const char* selected_map_label(void* map_object) {
    const uintptr_t object = reinterpret_cast<uintptr_t>(map_object);
    if (!object) return nullptr;
    const uint32_t primary = map_labels_at_pointer_field(object, 0x770);
    const uint32_t secondary = map_labels_at_pointer_field(object, 0x790);
    const uint32_t agreement = primary & secondary;
    auto single_label = [](uint32_t mask) -> const char* {
        if (!mask || (mask & (mask - 1))) return nullptr;
        for (size_t i = 0; i < LIVE_MAP_NAMES.size(); ++i) if (mask == (1u << i)) return LIVE_MAP_NAMES[i];
        return nullptr;
    };
    if (const char* name = single_label(agreement)) return name;
    if (const char* name = single_label(primary)) return name;
    if (const char* name = single_label(secondary)) return name;
    // A few lobby entries use the compact card layout and expose the selected
    // title through the first pair of text fields instead of the standard pair.
    const uint32_t compact_second = map_labels_at_pointer_field(object, 0x28);
    if (const char* name = single_label(compact_second)) return name;
    const uint32_t compact_first = map_labels_at_pointer_field(object, 0x20);
    return single_label(compact_first);
}

static void refresh_profile_from_live_objects(void* primary, void* secondary = nullptr) {
    const uintptr_t roots[] = {reinterpret_cast<uintptr_t>(primary), reinterpret_cast<uintptr_t>(secondary)};
    for (uintptr_t root : roots) {
        if (!root) continue;
        if (const char* map = find_live_label(root, LIVE_MAP_NAMES)) {
            g_map_name = map;
            g_live_profile_seen.store(true, std::memory_order_release);
        }
        if (const char* difficulty = find_live_label(root, LIVE_DIFFICULTIES)) {
            g_difficulty = difficulty;
            g_live_profile_seen.store(true, std::memory_order_release);
        }
    }
}

static void __fastcall hooked_difficulty_selected(void* widget, void* difficulty) {
    g_original_difficulty_selected(widget, difficulty);
    const uintptr_t difficulty_object = reinterpret_cast<uintptr_t>(difficulty);
    // RaceDifficultyData begins at +0x6E8. Its EDifficulty value is the byte
    // at +0x78 inside that structure (None=0, Easy=1, Medium=2, Hard=3).
    // Reading the enum avoids the localized-text graph, which retains cached
    // labels and could make Medium resolve as the previously selected Easy.
    const char* name = nullptr;
    if (difficulty_object && readable_address(difficulty_object + 0x760, 1)) {
        const uint8_t value = *reinterpret_cast<const uint8_t*>(difficulty_object + 0x760);
        if (value >= 1 && value <= LIVE_DIFFICULTIES.size()) name = LIVE_DIFFICULTIES[value - 1];
    }
    if (!name) name = find_text_field_label(difficulty_object + 0x6E8, LIVE_DIFFICULTIES);
    if (name) {
        g_difficulty = name;
        g_live_profile_seen.store(true, std::memory_order_release);
    }
}

static void __fastcall hooked_map_selected(void* widget, void* map) {
    g_original_map_selected(widget, map);
    if (const char* name = selected_map_label(map)) {
        g_map_name = name;
        g_live_profile_seen.store(true, std::memory_order_release);
    }
}

static void __fastcall hooked_map_selected_exec(void* widget, void* frame, void* result) {
    if (widget && readable_address(reinterpret_cast<uintptr_t>(widget), sizeof(uintptr_t))) {
        uintptr_t vtable = 0;
        std::memcpy(&vtable, widget, sizeof(vtable));
        uintptr_t target = 0;
        if (readable_address(vtable + 0x6F8, sizeof(target))) std::memcpy(&target, reinterpret_cast<void*>(vtable + 0x6F8), sizeof(target));
        const uintptr_t previous_target = g_map_selected_hook_target.load(std::memory_order_acquire);
        if (target && target != previous_target) {
            if (previous_target) {
                MH_DisableHook(reinterpret_cast<void*>(previous_target));
                MH_RemoveHook(reinterpret_cast<void*>(previous_target));
            }
            g_map_selected_hook_ready.store(false, std::memory_order_release);
            if (MH_CreateHook(reinterpret_cast<void*>(target), &hooked_map_selected, reinterpret_cast<void**>(&g_original_map_selected)) == MH_OK &&
                MH_EnableHook(reinterpret_cast<void*>(target)) == MH_OK) {
                g_map_selected_hook_target.store(target, std::memory_order_release);
                g_map_selected_hook_ready.store(true, std::memory_order_release);
            } else {
                g_map_selected_hook_target.store(0, std::memory_order_release);
            }
        }
    }
    g_original_map_selected_exec(widget, frame, result);
}

static void capture_restart_controller(void* widget) {
    const uintptr_t object = reinterpret_cast<uintptr_t>(widget);
    if (!object || !readable_address(object, sizeof(uintptr_t))) return;
    uintptr_t vtable = 0;
    std::memcpy(&vtable, reinterpret_cast<void*>(object), sizeof(vtable));
    if (!readable_address(vtable + 0x160, sizeof(uintptr_t))) return;
    uintptr_t getter_address = 0;
    std::memcpy(&getter_address, reinterpret_cast<void*>(vtable + 0x160), sizeof(getter_address));
    if (!getter_address) return;
    using OwnerGetterFn = void*(__fastcall*)(void*);
    void* owner = reinterpret_cast<OwnerGetterFn>(getter_address)(widget);
    const uintptr_t owner_address = reinterpret_cast<uintptr_t>(owner);
    if (!owner_address || !readable_address(owner_address + 0x118, sizeof(uintptr_t))) return;
    uintptr_t controller = 0;
    std::memcpy(&controller, reinterpret_cast<void*>(owner_address + 0x118), sizeof(controller));
    if (controller && readable_address(controller, sizeof(uintptr_t))) {
        g_restart_controller.store(controller, std::memory_order_release);
    }
}

static uintptr_t restart_controller_from_world_context(uintptr_t context) {
    if (!context || !readable_address(context, sizeof(uintptr_t))) return 0;
    uintptr_t vtable = 0;
    std::memcpy(&vtable, reinterpret_cast<void*>(context), sizeof(vtable));
    if (!readable_address(vtable + 0x160, sizeof(uintptr_t))) return 0;
    uintptr_t getter_address = 0;
    std::memcpy(&getter_address, reinterpret_cast<void*>(vtable + 0x160), sizeof(getter_address));
    if (!getter_address) return 0;
    using WorldGetterFn = void*(__fastcall*)(void*);
    void* world = reinterpret_cast<WorldGetterFn>(getter_address)(reinterpret_cast<void*>(context));
    const uintptr_t world_address = reinterpret_cast<uintptr_t>(world);
    if (!world_address || !readable_address(world_address + 0x118, sizeof(uintptr_t))) return 0;
    uintptr_t controller = 0;
    std::memcpy(&controller, reinterpret_cast<void*>(world_address + 0x118), sizeof(controller));
    return controller && readable_address(controller, sizeof(uintptr_t)) ? controller : 0;
}

static void __fastcall hooked_restart_race(void* widget) {
    g_restart_transition_pending.store(true, std::memory_order_release);
    g_restart_widget.store(reinterpret_cast<uintptr_t>(widget), std::memory_order_release);
    capture_restart_controller(widget);
    g_original_restart_race(widget);
}

static uint8_t __fastcall hooked_restart_visibility(void* widget) {
    g_restart_widget.store(reinterpret_cast<uintptr_t>(widget), std::memory_order_release);
    capture_restart_controller(widget);
    return g_original_restart_visibility(widget);
}

static uintptr_t find_current_restart_widget() {
    const uintptr_t image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const uintptr_t wanted_vtable = image + RESTART_WIDGET_VTABLE_RVA;
    uintptr_t best = 0;
    int best_internal_index = -1;
    uintptr_t address = 0x10000;
    MEMORY_BASIC_INFORMATION mbi{};
    while (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi))) {
        const DWORD basic = mbi.Protect & 0xff;
        const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
            (basic == PAGE_READONLY || basic == PAGE_READWRITE || basic == PAGE_WRITECOPY ||
             basic == PAGE_EXECUTE_READ || basic == PAGE_EXECUTE_READWRITE || basic == PAGE_EXECUTE_WRITECOPY);
        if (readable && mbi.RegionSize <= (256ull << 20)) {
            const uintptr_t begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const uintptr_t end = begin + mbi.RegionSize;
            for (uintptr_t candidate = begin; candidate + 0x28 <= end; candidate += sizeof(uintptr_t)) {
                uintptr_t vtable = 0;
                std::memcpy(&vtable, reinterpret_cast<void*>(candidate), sizeof(vtable));
                if (vtable != wanted_vtable) continue;
                uint32_t flags = 0;
                int internal_index = -1;
                uintptr_t object_class = 0;
                std::memcpy(&flags, reinterpret_cast<void*>(candidate + 0x8), sizeof(flags));
                std::memcpy(&internal_index, reinterpret_cast<void*>(candidate + 0xC), sizeof(internal_index));
                std::memcpy(&object_class, reinterpret_cast<void*>(candidate + 0x10), sizeof(object_class));
                constexpr uint32_t destroyed_flags = 0x00008000u | 0x00010000u;
                if ((flags & destroyed_flags) || internal_index < 0 || !readable_address(object_class, sizeof(uintptr_t))) continue;
                if (internal_index > best_internal_index) {
                    best = candidate;
                    best_internal_index = internal_index;
                }
            }
        }
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= address) break;
        address = next;
    }
    return best;
}

static void handle_overlay_key(UINT virtual_key, bool released) {
    if (virtual_key == VK_OEM_3) {
        if (!released && !g_settings_key_down.exchange(true, std::memory_order_acq_rel)) {
            g_settings_open.store(!g_settings_open.load(std::memory_order_acquire), std::memory_order_release);
        } else if (released) {
            g_settings_key_down.store(false, std::memory_order_release);
        }
        return;
    }
    if (!released && g_rebind_listening.exchange(false, std::memory_order_acq_rel)) {
        if (virtual_key != VK_ESCAPE && virtual_key != VK_END) g_restart_virtual_key.store(virtual_key, std::memory_order_release);
        return;
    }
    if (virtual_key == g_restart_virtual_key.load(std::memory_order_acquire)) {
        g_restart_key_down.store(!released, std::memory_order_release);
    }
}

static LRESULT CALLBACK overlay_wndproc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INPUT) {
        UINT size = 0;
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) == 0 &&
            size >= sizeof(RAWINPUT)) {
            std::vector<unsigned char> storage(size);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, storage.data(), &size, sizeof(RAWINPUTHEADER)) == size) {
                const RAWINPUT* input = reinterpret_cast<const RAWINPUT*>(storage.data());
                if (input->header.dwType == RIM_TYPEKEYBOARD) {
                    const bool released = (input->data.keyboard.Flags & RI_KEY_BREAK) != 0;
                    handle_overlay_key(input->data.keyboard.VKey, released);
                }
            }
        }
    } else if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
        handle_overlay_key(static_cast<UINT>(wparam), false);
    } else if (message == WM_KEYUP || message == WM_SYSKEYUP) {
        handle_overlay_key(static_cast<UINT>(wparam), true);
    } else if (message == WM_KILLFOCUS) {
        g_restart_key_down.store(false, std::memory_order_release);
    } else if (message == RESTART_WINDOW_MESSAGE) {
        // Resolve from the active race every time. A controller from the last
        // race can remain readable after returning to the lobby, but invoking
        // it only starts working again after the pause menu refreshes it.
        uintptr_t controller = restart_controller_from_world_context(g_race_object.load(std::memory_order_acquire));
        if (controller) g_restart_controller.store(controller, std::memory_order_release);
        uintptr_t widget = g_restart_widget.load(std::memory_order_acquire);
        uintptr_t widget_vtable = 0;
        if (widget && readable_address(widget, sizeof(widget_vtable))) {
            std::memcpy(&widget_vtable, reinterpret_cast<void*>(widget), sizeof(widget_vtable));
        }
        const uintptr_t expected_widget_vtable = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + RESTART_WIDGET_VTABLE_RVA;
        if (widget_vtable != expected_widget_vtable) {
            // Never scan the full UObject address space from this window
            // callback: it runs on the game thread and visibly stalls the
            // simulation. A widget captured by the normal game hooks is safe;
            // otherwise use the controller path below.
            widget = 0;
            g_restart_widget.store(0, std::memory_order_release);
        }
        g_restart_dispatch_pending.store(false, std::memory_order_release);
        // Prefer the game's actual restart widget callback. Besides restarting,
        // it initializes the per-race controller state that previously only
        // became ready after opening the pause menu once.
        if (widget && g_original_restart_race) {
            g_restart_transition_pending.store(true, std::memory_order_release);
            g_original_restart_race(reinterpret_cast<void*>(widget));
        } else if (controller && readable_address(controller, sizeof(uintptr_t))) {
            uintptr_t vtable = 0;
            std::memcpy(&vtable, reinterpret_cast<void*>(controller), sizeof(vtable));
            uintptr_t restart_method = 0;
            if (readable_address(vtable + 0x858, sizeof(uintptr_t))) {
                std::memcpy(&restart_method, reinterpret_cast<void*>(vtable + 0x858), sizeof(restart_method));
            }
            if (restart_method) {
                g_restart_transition_pending.store(true, std::memory_order_release);
                reinterpret_cast<void(__fastcall*)(void*)>(restart_method)(reinterpret_cast<void*>(controller));
            }
        }
        return 0;
    }
    if (g_settings_open.load(std::memory_order_acquire)) ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam);
    return g_original_wndproc ? CallWindowProcW(g_original_wndproc, window, message, wparam, lparam)
                              : DefWindowProcW(window, message, wparam, lparam);
}

static void clear_restart_dispatch() {
    g_restart_dispatch_pending.store(false, std::memory_order_release);
}

static void dispatch_restart_on_game_thread() {
    if (!g_game_window || g_restart_dispatch_pending.exchange(true, std::memory_order_acq_rel)) return;
    if (!PostMessageW(g_game_window, RESTART_WINDOW_MESSAGE, 0, 0)) clear_restart_dispatch();
}

struct PersonalBest {
    int total = 0;
    double finish = 0.0;
    std::array<double, 101> splits{};
};
static std::map<std::string, PersonalBest> g_personal_bests;
static bool g_personal_bests_loaded = false;
static std::array<double, 101> g_reference_splits{};
static bool g_has_reference_pb = false;
static std::string g_run_profile_key;
static bool g_pb_committed_for_finish = false;
static bool g_current_run_is_pb = false;

struct RaceTotalEntry { const char* map; const char* difficulty; int total; };
static constexpr RaceTotalEntry k_race_totals[] = {
    {"ABYSS", "EASY", 19}, {"ABYSS", "MEDIUM", 25}, {"ABYSS", "HARD", 22},
    {"ATLANTIS", "EASY", 24}, {"ATLANTIS", "MEDIUM", 24}, {"ATLANTIS", "HARD", 25},
    {"CRAG", "EASY", 24}, {"CRAG", "MEDIUM", 16}, {"CRAG", "HARD", 25},
    {"CLUB SILO", "EASY", 19}, {"CLUB SILO", "MEDIUM", 19}, {"CLUB SILO", "HARD", 25},
    {"FOREGONE DESTRUCTION", "EASY", 18}, {"FOREGONE DESTRUCTION", "MEDIUM", 20}, {"FOREGONE DESTRUCTION", "HARD", 25},
    {"HELIX", "EASY", 15}, {"HELIX", "MEDIUM", 22}, {"HELIX", "HARD", 25},
    {"HIGHWIND", "EASY", 19}, {"HIGHWIND", "MEDIUM", 18}, {"HIGHWIND", "HARD", 25},
    {"IMPACT", "EASY", 20}, {"IMPACT", "MEDIUM", 22}, {"IMPACT", "HARD", 25},
    {"KARMAN STATION", "EASY", 24}, {"KARMAN STATION", "MEDIUM", 21}, {"KARMAN STATION", "HARD", 24},
    {"LAVAWELL", "EASY", 23}, {"LAVAWELL", "MEDIUM", 24}, {"LAVAWELL", "HARD", 25},
    {"OASIS", "EASY", 20}, {"OASIS", "MEDIUM", 20}, {"OASIS", "HARD", 25},
    {"OLYMPUS", "EASY", 19}, {"OLYMPUS", "MEDIUM", 22}, {"OLYMPUS", "HARD", 25},
    {"PANTHEON", "EASY", 23}, {"PANTHEON", "MEDIUM", 23}, {"PANTHEON", "HARD", 24},
    {"STADIUM", "EASY", 19}, {"STADIUM", "MEDIUM", 22}, {"STADIUM", "HARD", 25},
};

static std::string profile_key() {
    return g_map_name + "\t" + g_difficulty;
}

static std::wstring personal_best_path() {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return L"splitgate-personal-bests.tsv";
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    return std::wstring(path) + L"splitgate-personal-bests.tsv";
}

static void load_personal_bests() {
    if (g_personal_bests_loaded) return;
    g_personal_bests_loaded = true;
    std::ifstream input(personal_best_path());
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        std::string map;
        std::string difficulty;
        std::string token;
        if (!std::getline(row, map, '\t') || !std::getline(row, difficulty, '\t') ||
            !std::getline(row, token, '\t')) continue;
        PersonalBest best;
        best.total = std::atoi(token.c_str());
        if (!std::getline(row, token, '\t')) continue;
        best.finish = std::strtod(token.c_str(), nullptr);
        for (int ball = 1; ball <= best.total && ball < static_cast<int>(best.splits.size()); ++ball) {
            if (!std::getline(row, token, '\t')) break;
            best.splits[ball] = std::strtod(token.c_str(), nullptr);
        }
        if (best.total > 0 && best.finish > 0.0) g_personal_bests[map + "\t" + difficulty] = best;
    }
}

static void save_personal_bests() {
    std::ofstream output(personal_best_path(), std::ios::trunc);
    output.precision(12);
    for (const auto& [key, best] : g_personal_bests) {
        const size_t separator = key.find('\t');
        if (separator == std::string::npos) continue;
        output << key.substr(0, separator) << '\t' << key.substr(separator + 1)
               << '\t' << best.total << '\t' << best.finish;
        for (int ball = 1; ball <= best.total && ball < static_cast<int>(best.splits.size()); ++ball) {
            output << '\t' << best.splits[ball];
        }
        output << '\n';
    }
}

static void begin_run_comparison() {
    load_personal_bests();
    g_run_profile_key = profile_key();
    g_reference_splits.fill(0.0);
    g_has_reference_pb = false;
    g_pb_committed_for_finish = false;
    g_current_run_is_pb = false;
    const auto found = g_personal_bests.find(g_run_profile_key);
    if (found != g_personal_bests.end()) {
        g_reference_splits = found->second.splits;
        g_has_reference_pb = true;
    }
}

static void commit_personal_best(int finish_count) {
    if (g_pb_committed_for_finish || finish_count <= 0) return;
    load_personal_bests();
    const std::string key = profile_key();
    if (g_run_profile_key.empty()) begin_run_comparison();
    for (int ball = 1; ball <= finish_count && ball < static_cast<int>(g_splits.size()); ++ball) {
        if (g_splits[ball] <= 0.0) return;
    }
    // Do not latch the finish until every split is present. The final count
    // and the sampler can land on adjacent frames on faster machines.
    g_pb_committed_for_finish = true;
    const auto previous = g_personal_bests.find(key);
    if (previous != g_personal_bests.end() && g_finish_time >= previous->second.finish) return;
    PersonalBest best;
    best.total = finish_count;
    best.finish = g_finish_time;
    best.splits = g_splits;
    g_personal_bests[key] = best;
    g_current_run_is_pb = true;
    save_personal_bests();
}

static int known_total_for_profile() {
    for (const RaceTotalEntry& entry : k_race_totals) {
        if (g_map_name == entry.map && g_difficulty == entry.difficulty) return entry.total;
    }
    return 0;
}

static void refresh_race_profile() {
    if (g_live_profile_seen.load(std::memory_order_acquire)) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - g_last_profile_read < std::chrono::milliseconds(500)) return;
    g_last_profile_read = now;
    wchar_t local_app_data[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH)) return;
    wchar_t path[MAX_PATH]{};
    _snwprintf_s(path, _TRUNCATE, L"%s\\SplitgateSplits\\race_profile.txt", local_app_data);
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"r") != 0 || !file) return;
    char line[160]{};
    if (fgets(line, sizeof(line), file)) {
        if (char* separator = std::strchr(line, '|')) {
            *separator = '\0';
            char* difficulty = separator + 1;
            difficulty[strcspn(difficulty, "\r\n")] = '\0';
            if (*line && *difficulty) {
                g_map_name = line;
                g_difficulty = difficulty;
                std::transform(g_map_name.begin(), g_map_name.end(), g_map_name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                std::transform(g_difficulty.begin(), g_difficulty.end(), g_difficulty.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            }
        }
    }
    fclose(file);
}

static int read_confirmed_hud_total() {
    wchar_t local_app_data[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH)) return 0;
    wchar_t path[MAX_PATH]{};
    _snwprintf_s(path, _TRUNCATE, L"%s\\SplitgateSplits\\hud_total.txt", local_app_data);
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"r") != 0 || !file) return 0;
    int total = 0;
    fwscanf_s(file, L"%d", &total);
    fclose(file);
    return total >= 3 && total <= 40 ? total : 0;
}

constexpr uintptr_t ROOT_OFFSET = 0x57576F0;
constexpr uintptr_t COUNT_OFFSET = 0x880;
constexpr uintptr_t CLOCK_OFFSET = 0x898;
constexpr uintptr_t BALL_ARRAY_OWNER_OFFSET = 0x1470;
constexpr uintptr_t BALL_ARRAY_COUNT_OFFSET = 0x898;

static bool readable_address(uintptr_t address, size_t size) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi))) return false;
    const DWORD basic = mbi.Protect & 0xff;
    const bool readable = basic == PAGE_READONLY || basic == PAGE_READWRITE || basic == PAGE_WRITECOPY || basic == PAGE_EXECUTE_READ || basic == PAGE_EXECUTE_READWRITE || basic == PAGE_EXECUTE_WRITECOPY;
    const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return mbi.State == MEM_COMMIT && readable && !(mbi.Protect & PAGE_GUARD) && address + size <= end;
}

static bool race_sample(uintptr_t race, int& count, double& clock) {
    if (g_arena_reloaded) {
        if (!g_arena_hud_valid.load(std::memory_order_acquire)) return false;
        count = g_arena_count.load(std::memory_order_acquire);
        clock = g_arena_clock.load(std::memory_order_acquire);
        return count >= 0 && count <= 100 && std::isfinite(clock) && clock >= 0.0 && clock < 3600.5;
    }
    if (!readable_address(race + COUNT_OFFSET, CLOCK_OFFSET - COUNT_OFFSET + sizeof(double))) return false;
    std::memcpy(&count, reinterpret_cast<void*>(race + COUNT_OFFSET), sizeof(count));
    std::memcpy(&clock, reinterpret_cast<void*>(race + CLOCK_OFFSET), sizeof(clock));
    return count >= 0 && count <= 100 && std::isfinite(clock) && clock >= 0.0 && clock < 86400.0;
}

static int race_total_balls(uintptr_t race) {
    if (g_arena_reloaded) return g_arena_total.load(std::memory_order_acquire);
    if (!readable_address(race + BALL_ARRAY_OWNER_OFFSET, sizeof(uintptr_t))) return 0;
    uintptr_t owner = 0;
    std::memcpy(&owner, reinterpret_cast<void*>(race + BALL_ARRAY_OWNER_OFFSET), sizeof(owner));
    if (!owner || !readable_address(owner + BALL_ARRAY_COUNT_OFFSET, sizeof(int) * 2)) return 0;
    int total = 0, capacity = 0;
    std::memcpy(&total, reinterpret_cast<void*>(owner + BALL_ARRAY_COUNT_OFFSET), sizeof(total));
    std::memcpy(&capacity, reinterpret_cast<void*>(owner + BALL_ARRAY_COUNT_OFFSET + sizeof(int)), sizeof(capacity));
    return total >= 3 && total <= 40 && capacity >= total && capacity <= 64 ? total : 0;
}

static uintptr_t locate_race() {
    const uintptr_t image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!readable_address(image + ROOT_OFFSET, sizeof(uintptr_t))) return 0;
    uintptr_t manager = 0;
    std::memcpy(&manager, reinterpret_cast<void*>(image + ROOT_OFFSET), sizeof(manager));
    if (!manager) return 0;

    struct Candidate { uintptr_t pointer; int count; double clock; };
    std::vector<Candidate> candidates;
    uintptr_t address = manager;
    MEMORY_BASIC_INFORMATION mbi{};
    while (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi))) {
        if (reinterpret_cast<uintptr_t>(mbi.AllocationBase) != manager) break;
        const DWORD basic = mbi.Protect & 0xff;
        const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
            (basic == PAGE_READONLY || basic == PAGE_READWRITE || basic == PAGE_WRITECOPY || basic == PAGE_EXECUTE_READ || basic == PAGE_EXECUTE_READWRITE || basic == PAGE_EXECUTE_WRITECOPY);
        if (readable) {
            const uintptr_t begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const uintptr_t end = begin + mbi.RegionSize;
            for (uintptr_t slot = begin; slot + sizeof(uintptr_t) <= end; slot += sizeof(uintptr_t)) {
                uintptr_t pointer = 0;
                std::memcpy(&pointer, reinterpret_cast<void*>(slot), sizeof(pointer));
                if (pointer < 0x10000 || pointer > 0x7fffffffffff) continue;
                int count = 0; double clock = 0;
                if (race_sample(pointer, count, clock)) candidates.push_back({pointer, count, clock});
            }
        }
        address = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    const auto started = std::chrono::steady_clock::now();
    Sleep(350);
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    for (const Candidate& candidate : candidates) {
        int count = 0; double clock = 0;
        if (race_sample(candidate.pointer, count, clock) && count == candidate.count && std::abs((clock - candidate.clock) - elapsed) <= 0.20) {
            return candidate.pointer;
        }
    }
    return 0;
}

static bool load_logo_texture() {
    wchar_t module_path[MAX_PATH]{};
    if (!GetModuleFileNameW(g_module, module_path, MAX_PATH)) return false;
    wchar_t* slash = wcsrchr(module_path, L'\\');
    if (!slash) return false;
    wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - module_path), L"assets\\splitgate-new-s-mark-clean.png");

    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool loaded = false;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateDecoderFromFilename(module_path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) &&
        SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        UINT width = 0, height = 0;
        converter->GetSize(&width, &height);
        std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
        if (width && height && SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()))) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data{};
            data.pSysMem = pixels.data();
            data.SysMemPitch = width * 4;
            ID3D11Texture2D* texture = nullptr;
            if (SUCCEEDED(g_device->CreateTexture2D(&desc, &data, &texture))) {
                loaded = SUCCEEDED(g_device->CreateShaderResourceView(texture, nullptr, &g_logo_texture));
                texture->Release();
            }
        }
    }
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    return loaded;
}

static bool initialize_imgui(IDXGISwapChain* swap_chain) {
    if (FAILED(swap_chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device)))) return false;
    g_device->GetImmediateContext(&g_context);
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swap_chain->GetDesc(&desc))) return false;
    g_game_window = desc.OutputWindow;
    ID3D11Texture2D* back_buffer = nullptr;
    if (FAILED(swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer)))) return false;
    const HRESULT rtv_result = g_device->CreateRenderTargetView(back_buffer, nullptr, &g_render_target);
    back_buffer->Release();
    if (FAILED(rtv_result)) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    g_font_body = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\bahnschrift.ttf", 19.0f);
    g_font_header = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisbi.ttf", 22.0f);
    g_font_clock = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\bahnschrift.ttf", 38.0f);
    if (!g_font_body) g_font_body = io.Fonts->AddFontDefault();
    if (!g_font_header) g_font_header = g_font_body;
    if (!g_font_clock) g_font_clock = g_font_body;
    ImGui::StyleColorsDark();
    load_logo_texture();
    if (!ImGui_ImplWin32_Init(g_game_window)) return false;
    if (!ImGui_ImplDX11_Init(g_device, g_context)) return false;
    g_original_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        g_game_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(overlay_wndproc)));
    if (!g_original_wndproc) return false;
    return true;
}

struct HudGlyph {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    int area = 0;
};

enum class HudInk { dark, yellow, white };

static constexpr uint16_t k_arena_digit_templates[10][16] = {
    {0x7FFC, 0xFFFE, 0xF01F, 0xC007, 0xC007, 0xC007, 0xC007, 0xC007, 0xC007, 0xC007, 0xC007, 0xC007, 0xC007, 0xE00F, 0xFFFE, 0x7FFE},
    {0x07C0, 0x07FF, 0x07FF, 0x07FF, 0x07C0, 0x07C0, 0x07C0, 0x07C0, 0x07C0, 0x07C0, 0x07C0, 0x07C0, 0x07C0, 0xFFFF, 0xFFFF, 0xFFFF},
    {0x7FFE, 0xFFFE, 0xFFFE, 0xE000, 0xC000, 0xC000, 0xE000, 0xF000, 0x7F00, 0x1F00, 0x0FC0, 0x03F0, 0x00FC, 0xFFFE, 0xFFFF, 0xFFFF},
    {0xFFFF, 0xFFFF, 0xFFFE, 0x7C00, 0x3F00, 0x0F80, 0x07C0, 0x7FE0, 0xFFE0, 0xE000, 0xE000, 0xC000, 0xE000, 0xFFFF, 0xFFFF, 0xFFFF},
    {0x0380, 0x03C0, 0x01E0, 0x00E0, 0x0070, 0x0070, 0x1C78, 0x1C3C, 0x1C0E, 0x1C0F, 0xFFFF, 0xFFFF, 0xFFFF, 0x1C00, 0x1C00, 0x1C00},
    {0xFFFF, 0xFFFF, 0xFFFF, 0x0007, 0x0007, 0x0007, 0x3FFF, 0xFFFF, 0xFFFF, 0xC000, 0xC000, 0xC000, 0xC000, 0xFFFF, 0xFFFF, 0xFFFF},
    {0x1C00, 0x1E00, 0x0F80, 0x07C0, 0x03C0, 0x01F0, 0x0078, 0xFFFC, 0xFFFF, 0xE00F, 0xC00F, 0xC00F, 0xC00F, 0xE01F, 0xFFFE, 0xFFFE},
    {0xFFFF, 0xFFFF, 0xFFFF, 0xE000, 0xF000, 0xF800, 0x7800, 0x3C00, 0x1E00, 0x0F00, 0x0F80, 0x0780, 0x03C0, 0x03E0, 0x01E0, 0x01F0},
    {0x7FFE, 0xFFFE, 0xFFFF, 0xF00F, 0xE00F, 0xE00F, 0xF00F, 0xFFFE, 0xFFFF, 0xE00F, 0xE007, 0xE007, 0xE007, 0xF00F, 0xFFFF, 0xFFFF},
    {0x7FFF, 0x7FFF, 0xF807, 0xF003, 0xF003, 0xF003, 0xF007, 0xFFFF, 0x3FFF, 0x1E00, 0x0F80, 0x03C0, 0x03E0, 0x01F0, 0x0078, 0x0038},
};

static bool hud_ink(const unsigned char* pixel, HudInk ink) {
    int c0 = pixel[0], c1 = pixel[1], c2 = pixel[2];
    if (g_arena_pixels_r10) {
        uint32_t packed = 0;
        std::memcpy(&packed, pixel, sizeof(packed));
        c0 = static_cast<int>((packed & 0x3ffu) * 255u / 1023u);
        c1 = static_cast<int>(((packed >> 10) & 0x3ffu) * 255u / 1023u);
        c2 = static_cast<int>(((packed >> 20) & 0x3ffu) * 255u / 1023u);
    }
    const int high = std::max({c0, c1, c2});
    const int low = std::min({c0, c1, c2});
    if (ink == HudInk::dark) return (c0 + c1 + c2) / 3 < 190;
    if (ink == HudInk::white) return low > 155 && high - low < 72;
    return (c0 > 155 && c1 > 115 && c2 < 170) ||
           (c2 > 155 && c1 > 115 && c0 < 170);
}

static std::vector<HudGlyph> find_hud_glyphs(
    const unsigned char* pixels, UINT row_pitch, int width, int height,
    int left, int top, int right, int bottom, HudInk ink) {
    left = std::clamp(left, 0, width);
    right = std::clamp(right, left, width);
    top = std::clamp(top, 0, height);
    bottom = std::clamp(bottom, top, height);
    const int scan_width = right - left;
    const int scan_height = bottom - top;
    std::vector<uint8_t> mask(static_cast<size_t>(scan_width) * scan_height, 0);
    for (int y = 0; y < scan_height; ++y) {
        const auto* row = pixels + (top + y) * row_pitch;
        for (int x = 0; x < scan_width; ++x) {
            mask[static_cast<size_t>(y) * scan_width + x] = hud_ink(row + (left + x) * 4, ink) ? 1 : 0;
        }
    }
    std::vector<HudGlyph> result;
    std::vector<int> stack;
    for (int sy = 0; sy < scan_height; ++sy) {
        for (int sx = 0; sx < scan_width; ++sx) {
            const int seed = sy * scan_width + sx;
            if (!mask[seed]) continue;
            mask[seed] = 0;
            stack.clear();
            stack.push_back(seed);
            HudGlyph glyph{sx, sy, sx + 1, sy + 1, 0};
            while (!stack.empty()) {
                const int item = stack.back();
                stack.pop_back();
                const int x = item % scan_width;
                const int y = item / scan_width;
                ++glyph.area;
                glyph.left = std::min(glyph.left, x);
                glyph.top = std::min(glyph.top, y);
                glyph.right = std::max(glyph.right, x + 1);
                glyph.bottom = std::max(glyph.bottom, y + 1);
                const int nx[] = {x - 1, x + 1, x, x};
                const int ny[] = {y, y, y - 1, y + 1};
                for (int i = 0; i < 4; ++i) {
                    if (nx[i] < 0 || nx[i] >= scan_width || ny[i] < 0 || ny[i] >= scan_height) continue;
                    const int next = ny[i] * scan_width + nx[i];
                    if (!mask[next]) continue;
                    mask[next] = 0;
                    stack.push_back(next);
                }
            }
            glyph.left += left;
            glyph.right += left;
            glyph.top += top;
            glyph.bottom += top;
            const int glyph_width = glyph.right - glyph.left;
            const int glyph_height = glyph.bottom - glyph.top;
            if (glyph_height >= std::max(8, scan_height / 5) &&
                glyph_width >= 4 && glyph_width <= std::max(12, width * 3 / 20) && glyph.area >= 30) {
                result.push_back(glyph);
            }
        }
    }
    std::sort(result.begin(), result.end(), [](const HudGlyph& a, const HudGlyph& b) { return a.left < b.left; });
    return result;
}

static int read_hud_digit(const unsigned char* pixels, UINT row_pitch, const HudGlyph& glyph, HudInk ink) {
    const int source_width = glyph.right - glyph.left;
    const int source_height = glyph.bottom - glyph.top;
    if (source_width <= 0 || source_height <= 0) return -1;
    uint16_t rows[16]{};
    for (int y = 0; y < 16; ++y) {
        const int source_y = glyph.top + std::min(source_height - 1, y * source_height / 16);
        const auto* row = pixels + source_y * row_pitch;
        for (int x = 0; x < 16; ++x) {
            const int source_x = glyph.left + std::min(source_width - 1, x * source_width / 16);
            if (hud_ink(row + source_x * 4, ink)) rows[y] |= static_cast<uint16_t>(1u << x);
        }
    }
    int best_digit = -1;
    int best_score = 10000;
    int ink_pixels = 0;
    for (uint16_t row : rows) ink_pixels += std::popcount(static_cast<unsigned>(row));
    for (int digit = 0; digit < 10; ++digit) {
        int score = 0;
        for (int y = 0; y < 16; ++y) score += std::popcount(static_cast<unsigned>(rows[y] ^ k_arena_digit_templates[digit][y]));
        if (score < best_score) {
            best_score = score;
            best_digit = digit;
        }
    }
    // Arena's 9 has the same falling diagonal as 7, but also contains the
    // closed upper bowl. The supplied screenshots covered every other digit;
    // density cleanly separates these two at any resolution after normalization.
    if (best_digit == 7 && ink_pixels >= 115) best_digit = 9;
    return best_score <= 112 ? best_digit : -1;
}

static bool parse_arena_hud_pixels(const unsigned char* pixels, UINT row_pitch, int width, int height) {
    auto timer_glyphs = find_hud_glyphs(pixels, row_pitch, width, height,
        width / 12, 0, width * 11 / 12, height * 38 / 100, HudInk::dark);
    bool timer_valid = timer_glyphs.size() == 6;
    int timer_digits[6]{};
    if (timer_valid) {
        for (int i = 0; i < 6; ++i) {
            timer_digits[i] = read_hud_digit(pixels, row_pitch, timer_glyphs[i], HudInk::dark);
            if (timer_digits[i] < 0) timer_valid = false;
        }
    }
    if (timer_valid) {
        const int minutes = timer_digits[0] * 10 + timer_digits[1];
        const int seconds = timer_digits[2] * 10 + timer_digits[3];
        const int hundredths = timer_digits[4] * 10 + timer_digits[5];
        if (minutes < 60 && seconds < 60) {
            g_arena_clock.store(minutes * 60.0 + seconds + hundredths / 100.0, std::memory_order_release);
        } else {
            timer_valid = false;
        }
    }
    const int counter_top = height * 52 / 100;
    auto yellow = find_hud_glyphs(pixels, row_pitch, width, height,
        width / 5, counter_top, width / 2, height, HudInk::yellow);
    auto white = find_hud_glyphs(pixels, row_pitch, width, height,
        width * 54 / 100, counter_top, width * 9 / 10, height, HudInk::white);
    int current = 0;
    bool current_valid = !yellow.empty() && yellow.size() <= 2;
    for (const HudGlyph& glyph : yellow) {
        const int digit = read_hud_digit(pixels, row_pitch, glyph, HudInk::yellow);
        if (digit < 0) { current_valid = false; break; }
        current = current * 10 + digit;
    }
    int total = 0;
    bool total_valid = !white.empty() && white.size() <= 2;
    for (const HudGlyph& glyph : white) {
        const int digit = read_hud_digit(pixels, row_pitch, glyph, HudInk::white);
        if (digit < 0) { total_valid = false; break; }
        total = total * 10 + digit;
    }
    if (current_valid && total_valid && current >= 0 && current <= total && total >= 3 && total <= 100) {
        g_arena_count.store(current, std::memory_order_release);
        g_arena_total.store(total, std::memory_order_release);
    }
    g_arena_hud_valid.store(timer_valid, std::memory_order_release);
    return timer_valid;
}

static bool read_arena_hud(IDXGISwapChain* swap_chain) {
    const auto now = std::chrono::steady_clock::now();
    if (now - g_last_arena_hud_probe < std::chrono::milliseconds(15)) {
        return g_arena_hud_valid.load(std::memory_order_acquire);
    }
    g_last_arena_hud_probe = now;
    ID3D11Texture2D* back_buffer = nullptr;
    if (FAILED(swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer)))) return false;
    D3D11_TEXTURE2D_DESC source{};
    back_buffer->GetDesc(&source);
    const UINT crop_left = static_cast<UINT>(source.Width * 0.455f);
    const UINT crop_top = static_cast<UINT>(source.Height * 0.065f);
    const UINT crop_width = static_cast<UINT>(source.Width * 0.090f);
    const UINT crop_height = static_cast<UINT>(source.Height * 0.090f);
    if (crop_width < 140 || crop_height < 75) {
        back_buffer->Release();
        return false;
    }
    D3D11_TEXTURE2D_DESC existing{};
    if (g_arena_hud_probe) g_arena_hud_probe->GetDesc(&existing);
    if (!g_arena_hud_probe || existing.Width != crop_width || existing.Height != crop_height || existing.Format != source.Format) {
        if (g_arena_hud_probe) { g_arena_hud_probe->Release(); g_arena_hud_probe = nullptr; }
        D3D11_TEXTURE2D_DESC desc = source;
        desc.Width = crop_width;
        desc.Height = crop_height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &g_arena_hud_probe))) {
            back_buffer->Release();
            return false;
        }
    }
    D3D11_BOX box{crop_left, crop_top, 0, crop_left + crop_width, crop_top + crop_height, 1};
    g_context->CopySubresourceRegion(g_arena_hud_probe, 0, 0, 0, 0, back_buffer, 0, &box);
    back_buffer->Release();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_context->Map(g_arena_hud_probe, 0, D3D11_MAP_READ, 0, &mapped))) return false;

    const bool valid = parse_arena_hud_pixels(static_cast<const unsigned char*>(mapped.pData), mapped.RowPitch,
        static_cast<int>(crop_width), static_cast<int>(crop_height));
    g_context->Unmap(g_arena_hud_probe, 0);
    return valid;
}

static void __stdcall hooked_execute_command_lists(
    ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    g_active_hook_calls.fetch_add(1, std::memory_order_acq_rel);
    ID3D12CommandQueue* expected = nullptr;
    if (g_arena_reloaded && g_arena_command_queue.compare_exchange_strong(expected, queue, std::memory_order_acq_rel)) {
        queue->AddRef();
    }
    g_original_execute_command_lists(queue, count, lists);
    g_active_hook_calls.fetch_sub(1, std::memory_order_acq_rel);
}

static void release_arena_d3d12_capture() {
    if (g_arena_readback) { g_arena_readback->Release(); g_arena_readback = nullptr; }
    if (g_arena_copy_list) { g_arena_copy_list->Release(); g_arena_copy_list = nullptr; }
    if (g_arena_copy_allocator) { g_arena_copy_allocator->Release(); g_arena_copy_allocator = nullptr; }
    if (g_arena_copy_fence) { g_arena_copy_fence->Release(); g_arena_copy_fence = nullptr; }
    if (g_arena_copy_event) { CloseHandle(g_arena_copy_event); g_arena_copy_event = nullptr; }
    if (g_arena_d3d12_device) { g_arena_d3d12_device->Release(); g_arena_d3d12_device = nullptr; }
    g_arena_readback_width = 0;
    g_arena_readback_height = 0;
    g_arena_readback_pitch = 0;
    g_arena_readback_format = DXGI_FORMAT_UNKNOWN;
}

static bool read_arena_hud_d3d12(IDXGISwapChain* swap_chain) {
    const auto now = std::chrono::steady_clock::now();
    if (now - g_last_arena_hud_probe < std::chrono::milliseconds(15)) {
        return g_arena_hud_valid.load(std::memory_order_acquire);
    }
    g_last_arena_hud_probe = now;
    if (!g_arena_d3d12_device) {
        if (FAILED(swap_chain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&g_arena_d3d12_device)))) return false;
        if (!g_arena_command_queue.load(std::memory_order_acquire)) {
            D3D12_COMMAND_QUEUE_DESC queue_desc{};
            queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            ID3D12CommandQueue* capture_queue = nullptr;
            if (FAILED(g_arena_d3d12_device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&capture_queue)))) return false;
            ID3D12CommandQueue* expected = nullptr;
            if (!g_arena_command_queue.compare_exchange_strong(expected, capture_queue, std::memory_order_acq_rel)) {
                capture_queue->Release();
            }
        }
        if (FAILED(g_arena_d3d12_device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_arena_copy_allocator)))) return false;
        if (FAILED(g_arena_d3d12_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                g_arena_copy_allocator, nullptr, IID_PPV_ARGS(&g_arena_copy_list)))) return false;
        if (FAILED(g_arena_copy_list->Close())) return false;
        if (FAILED(g_arena_d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_arena_copy_fence)))) return false;
        g_arena_copy_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_arena_copy_event) return false;
    }
    ID3D12CommandQueue* queue = g_arena_command_queue.load(std::memory_order_acquire);
    if (!queue) return false;
    IDXGISwapChain3* swap_chain3 = nullptr;
    if (FAILED(swap_chain->QueryInterface(IID_PPV_ARGS(&swap_chain3)))) return false;
    const UINT buffer_index = swap_chain3->GetCurrentBackBufferIndex();
    swap_chain3->Release();
    ID3D12Resource* back_buffer = nullptr;
    if (FAILED(swap_chain->GetBuffer(buffer_index, IID_PPV_ARGS(&back_buffer)))) return false;
    const D3D12_RESOURCE_DESC source = back_buffer->GetDesc();
    const UINT crop_left = static_cast<UINT>(source.Width * 0.455);
    const UINT crop_top = static_cast<UINT>(source.Height * 0.065);
    const UINT crop_width = static_cast<UINT>(source.Width * 0.090);
    const UINT crop_height = static_cast<UINT>(source.Height * 0.090);
    if (crop_width < 140 || crop_height < 75 ||
        (source.Format != DXGI_FORMAT_R8G8B8A8_UNORM && source.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         source.Format != DXGI_FORMAT_B8G8R8A8_UNORM && source.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
         source.Format != DXGI_FORMAT_R10G10B10A2_UNORM)) {
        back_buffer->Release();
        return false;
    }
    const UINT row_pitch = (crop_width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
        ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    if (!g_arena_readback || g_arena_readback_width != crop_width || g_arena_readback_height != crop_height ||
        g_arena_readback_format != source.Format) {
        if (g_arena_readback) { g_arena_readback->Release(); g_arena_readback = nullptr; }
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = static_cast<UINT64>(row_pitch) * crop_height;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_arena_d3d12_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_arena_readback)))) {
            back_buffer->Release();
            return false;
        }
        g_arena_readback_width = crop_width;
        g_arena_readback_height = crop_height;
        g_arena_readback_pitch = row_pitch;
        g_arena_readback_format = source.Format;
    }
    if (FAILED(g_arena_copy_allocator->Reset()) || FAILED(g_arena_copy_list->Reset(g_arena_copy_allocator, nullptr))) {
        back_buffer->Release();
        return false;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = back_buffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_arena_copy_list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = g_arena_readback;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint.Footprint.Format = source.Format;
    destination.PlacedFootprint.Footprint.Width = crop_width;
    destination.PlacedFootprint.Footprint.Height = crop_height;
    destination.PlacedFootprint.Footprint.Depth = 1;
    destination.PlacedFootprint.Footprint.RowPitch = row_pitch;
    D3D12_TEXTURE_COPY_LOCATION source_location{};
    source_location.pResource = back_buffer;
    source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source_location.SubresourceIndex = 0;
    D3D12_BOX source_box{crop_left, crop_top, 0, crop_left + crop_width, crop_top + crop_height, 1};
    g_arena_copy_list->CopyTextureRegion(&destination, 0, 0, 0, &source_location, &source_box);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    g_arena_copy_list->ResourceBarrier(1, &barrier);
    const HRESULT close_result = g_arena_copy_list->Close();
    back_buffer->Release();
    if (FAILED(close_result)) return false;
    ID3D12CommandList* lists[] = {g_arena_copy_list};
    queue->ExecuteCommandLists(1, lists);
    const UINT64 fence_value = ++g_arena_copy_fence_value;
    if (FAILED(queue->Signal(g_arena_copy_fence, fence_value))) return false;
    if (g_arena_copy_fence->GetCompletedValue() < fence_value) {
        if (FAILED(g_arena_copy_fence->SetEventOnCompletion(fence_value, g_arena_copy_event))) return false;
        if (WaitForSingleObject(g_arena_copy_event, 1000) != WAIT_OBJECT_0) return false;
    }
    void* mapped = nullptr;
    D3D12_RANGE read_range{0, static_cast<SIZE_T>(row_pitch) * crop_height};
    if (FAILED(g_arena_readback->Map(0, &read_range, &mapped))) return false;
    g_arena_pixels_r10 = source.Format == DXGI_FORMAT_R10G10B10A2_UNORM;
    const bool valid = parse_arena_hud_pixels(static_cast<const unsigned char*>(mapped), row_pitch,
        static_cast<int>(crop_width), static_cast<int>(crop_height));
    D3D12_RANGE written_range{0, 0};
    g_arena_readback->Unmap(0, &written_range);
    return valid;
}

static int detect_total_balls(IDXGISwapChain* swap_chain) {
    const auto now = std::chrono::steady_clock::now();
    if (now - g_last_finish_probe < std::chrono::milliseconds(250)) return 0;
    g_last_finish_probe = now;

    ID3D11Texture2D* back_buffer = nullptr;
    if (FAILED(swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer)))) return false;
    D3D11_TEXTURE2D_DESC source_desc{};
    back_buffer->GetDesc(&source_desc);
    const UINT crop_left = static_cast<UINT>(source_desc.Width * 0.140f);
    const UINT crop_top = static_cast<UINT>(source_desc.Height * 0.085f);
    const UINT crop_width = static_cast<UINT>(source_desc.Width * 0.240f);
    const UINT crop_height = static_cast<UINT>(source_desc.Height * 0.045f);
    if (crop_width < 200 || crop_height < 20) {
        back_buffer->Release();
        return 0;
    }
    D3D11_TEXTURE2D_DESC existing{};
    if (g_finish_probe) g_finish_probe->GetDesc(&existing);
    if (!g_finish_probe || existing.Width != crop_width || existing.Height != crop_height || existing.Format != source_desc.Format) {
        if (g_finish_probe) { g_finish_probe->Release(); g_finish_probe = nullptr; }
        D3D11_TEXTURE2D_DESC desc = source_desc;
        desc.Width = crop_width;
        desc.Height = crop_height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &g_finish_probe))) {
            back_buffer->Release();
            return 0;
        }
    }
    D3D11_BOX box{crop_left, crop_top, 0, crop_left + crop_width, crop_top + crop_height, 1};
    g_context->CopySubresourceRegion(g_finish_probe, 0, 0, 0, 0, back_buffer, 0, &box);
    back_buffer->Release();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_context->Map(g_finish_probe, 0, D3D11_MAP_READ, 0, &mapped))) return 0;
    int occupied = 0;
    for (int slot = 0; slot < 40; ++slot) {
        const int x0 = static_cast<int>((67.0 + slot * 21.3) / 614.0 * crop_width);
        const int x1 = static_cast<int>((67.0 + slot * 21.3 + 19.0) / 614.0 * crop_width);
        const int y0 = static_cast<int>(20.0 / 65.0 * crop_height);
        const int y1 = static_cast<int>(51.0 / 65.0 * crop_height);
        if (x1 > static_cast<int>(crop_width)) break;
        int colored = 0, pixels = 0;
        for (int y = y0; y < y1; ++y) {
            const unsigned char* row = static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch;
            for (int x = x0; x < x1; ++x) {
                const unsigned char* p = row + x * 4;
                const int c0 = p[0], g = p[1], c2 = p[2];
                const int hi = std::max(c0, std::max(g, c2)), lo = std::min(c0, std::min(g, c2));
                const bool white = hi > 135 && (hi == 0 || (hi - lo) * 255 / hi < 145);
                const bool yellow = (c0 > 135 && g > 105 && c2 < 155 && c0 >= g) ||
                                    (c2 > 135 && g > 105 && c0 < 155 && c2 >= g);
                if (white || yellow) ++colored;
                ++pixels;
            }
        }
        if (pixels == 0 || static_cast<double>(colored) / pixels < 0.16) break;
        ++occupied;
    }
    g_context->Unmap(g_finish_probe, 0);
    // The HUD includes one already-filled starting slot at count 00, so the
    // number of collectible balls is one less than the number of visible slots.
    return occupied >= 4 && occupied <= 40 ? occupied - 1 : 0;
}

static bool strict_finish_banner_visible(IDXGISwapChain* swap_chain) {
    ID3D11Texture2D* back_buffer = nullptr;
    if (FAILED(swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer)))) return false;
    D3D11_TEXTURE2D_DESC source{};
    back_buffer->GetDesc(&source);
    constexpr UINT size = 32;
    if (!g_banner_probe) {
        D3D11_TEXTURE2D_DESC desc = source;
        desc.Width = size; desc.Height = size; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.SampleDesc.Count = 1; desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
        if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &g_banner_probe))) {
            back_buffer->Release(); return false;
        }
    }
    int yellow_patches = 0;
    const float xs[] = {0.28f, 0.36f, 0.44f, 0.52f, 0.60f, 0.68f, 0.76f};
    for (float fraction : xs) {
        const UINT left = static_cast<UINT>(source.Width * fraction) - size / 2;
        const UINT top = static_cast<UINT>(source.Height * 0.40f) - size / 2;
        D3D11_BOX box{left, top, 0, left + size, top + size, 1};
        g_context->CopySubresourceRegion(g_banner_probe, 0, 0, 0, 0, back_buffer, 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(g_context->Map(g_banner_probe, 0, D3D11_MAP_READ, 0, &mapped))) continue;
        int yellow = 0;
        for (UINT y = 0; y < size; y += 2) {
            const unsigned char* row = static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch;
            for (UINT x = 0; x < size; x += 2) {
                const unsigned char* p = row + x * 4;
                const int c0 = p[0], g = p[1], c2 = p[2];
                const bool bgra_yellow = c2 > 185 && g > 145 && c0 < 105 && c2 >= g;
                const bool rgba_yellow = c0 > 185 && g > 145 && c2 < 105 && c0 >= g;
                if (bgra_yellow || rgba_yellow) ++yellow;
            }
        }
        g_context->Unmap(g_banner_probe, 0);
        if (yellow >= 35) ++yellow_patches;
    }
    int cyan_top_patches = 0;
    int cyan_bottom_patches = 0;
    const float line_ys[] = {0.263f, 0.563f};
    for (int line = 0; line < 2; ++line) {
        for (float fraction : xs) {
            const UINT left = static_cast<UINT>(source.Width * fraction) - size / 2;
            const UINT top = static_cast<UINT>(source.Height * line_ys[line]) - size / 2;
            D3D11_BOX box{left, top, 0, left + size, top + size, 1};
            g_context->CopySubresourceRegion(g_banner_probe, 0, 0, 0, 0, back_buffer, 0, &box);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(g_context->Map(g_banner_probe, 0, D3D11_MAP_READ, 0, &mapped))) continue;
            int cyan = 0;
            for (UINT y = 0; y < size; y += 2) {
                const unsigned char* row = static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch;
                for (UINT x = 0; x < size; x += 2) {
                    const unsigned char* p = row + x * 4;
                    const int c0 = p[0], g = p[1], c2 = p[2];
                    const bool bgra_cyan = c0 > 185 && g > 165 && c2 < 155;
                    const bool rgba_cyan = c2 > 185 && g > 165 && c0 < 155;
                    if (bgra_cyan || rgba_cyan) ++cyan;
                }
            }
            g_context->Unmap(g_banner_probe, 0);
            if (cyan >= 30) {
                if (line == 0) ++cyan_top_patches;
                else ++cyan_bottom_patches;
            }
        }
    }
    back_buffer->Release();
    return yellow_patches >= 3 && cyan_top_patches >= 4 && cyan_bottom_patches >= 4;
}

static bool race_lobby_visible(IDXGISwapChain* swap_chain) {
    const auto now = std::chrono::steady_clock::now();
    if (now - g_last_lobby_probe < std::chrono::milliseconds(200)) {
        return g_in_race_lobby.load(std::memory_order_acquire);
    }
    g_last_lobby_probe = now;
    ID3D11Texture2D* back_buffer = nullptr;
    if (FAILED(swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer)))) return false;
    D3D11_TEXTURE2D_DESC source{};
    back_buffer->GetDesc(&source);
    constexpr UINT size = 32;
    if (!g_lobby_probe) {
        D3D11_TEXTURE2D_DESC desc = source;
        desc.Width = size; desc.Height = size; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.SampleDesc.Count = 1; desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
        if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &g_lobby_probe))) {
            back_buffer->Release(); return false;
        }
    }
    int dark_bar_patches = 0;
    const float xs[] = {0.08f, 0.20f, 0.32f, 0.44f, 0.56f, 0.68f, 0.80f, 0.92f};
    for (float fraction : xs) {
        const UINT left = static_cast<UINT>(source.Width * fraction) - size / 2;
        const UINT top = static_cast<UINT>(source.Height * 0.965f) - size / 2;
        D3D11_BOX box{left, top, 0, left + size, top + size, 1};
        g_context->CopySubresourceRegion(g_lobby_probe, 0, 0, 0, 0, back_buffer, 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(g_context->Map(g_lobby_probe, 0, D3D11_MAP_READ, 0, &mapped))) continue;
        int dark = 0;
        int samples = 0;
        for (UINT y = 0; y < size; y += 2) {
            const unsigned char* row = static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch;
            for (UINT x = 0; x < size; x += 2) {
                const unsigned char* p = row + x * 4;
                const int high = std::max({static_cast<int>(p[0]), static_cast<int>(p[1]), static_cast<int>(p[2])});
                const int low = std::min({static_cast<int>(p[0]), static_cast<int>(p[1]), static_cast<int>(p[2])});
                // The race lobby footer is a dark navy/gray gradient rather
                // than a single flat color; allow its brighter center while
                // still rejecting colorful gameplay pixels.
                if (high < 96 && high - low < 36) ++dark;
                ++samples;
            }
        }
        g_context->Unmap(g_lobby_probe, 0);
        if (samples > 0 && static_cast<float>(dark) / samples > 0.72f) ++dark_bar_patches;
    }
    back_buffer->Release();
    const bool visible = dark_bar_patches >= 6;
    g_in_race_lobby.store(visible, std::memory_order_release);
    return visible;
}

static void draw_splitgate_mark(ImDrawList* draw, const ImVec2& position, float width) {
    if (g_logo_texture) {
        const float height = width / 1.927f;
        const ImTextureID texture_id = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(g_logo_texture));
        draw->AddImage(ImTextureRef(texture_id), position, ImVec2(position.x + width, position.y + height));
        return;
    }
    const float scale = width / 76.0f;
    const float stroke = 7.0f * scale;
    const ImU32 white = IM_COL32(245, 251, 255, 255);
    draw->PathLineTo(ImVec2(position.x + 72.0f * scale, position.y + 5.0f * scale));
    draw->PathLineTo(ImVec2(position.x + 26.0f * scale, position.y + 5.0f * scale));
    draw->PathBezierCubicCurveTo(ImVec2(position.x + 15.0f * scale, position.y + 5.0f * scale), ImVec2(position.x + 9.0f * scale, position.y + 12.0f * scale), ImVec2(position.x + 9.0f * scale, position.y + 23.0f * scale));
    draw->PathLineTo(ImVec2(position.x + 9.0f * scale, position.y + 31.0f * scale));
    draw->PathStroke(white, 0, stroke);
    draw->PathLineTo(ImVec2(position.x + 4.0f * scale, position.y + 43.0f * scale));
    draw->PathLineTo(ImVec2(position.x + 50.0f * scale, position.y + 43.0f * scale));
    draw->PathBezierCubicCurveTo(ImVec2(position.x + 61.0f * scale, position.y + 43.0f * scale), ImVec2(position.x + 67.0f * scale, position.y + 36.0f * scale), ImVec2(position.x + 67.0f * scale, position.y + 25.0f * scale));
    draw->PathLineTo(ImVec2(position.x + 67.0f * scale, position.y + 17.0f * scale));
    draw->PathStroke(white, 0, stroke);
    draw->AddLine(ImVec2(position.x + 8.0f * scale, position.y + 31.0f * scale), ImVec2(position.x + 45.0f * scale, position.y + 31.0f * scale), white, stroke);
    draw->AddLine(ImVec2(position.x + 30.0f * scale, position.y + 17.0f * scale), ImVec2(position.x + 68.0f * scale, position.y + 17.0f * scale), white, stroke);
}

static void format_race_time(double value, char* output, size_t output_size) {
    // Splitgate displays milliseconds by dropping sub-millisecond precision.
    // Match that presentation instead of letting printf round up by 1 ms.
    value = std::floor(value * 1000.0 + 1e-7) / 1000.0;
    const int minutes = static_cast<int>(value) / 60;
    const double seconds = value - minutes * 60.0;
    std::snprintf(output, output_size, "%d:%06.3f", minutes, seconds);
}

static void format_delta(double value, char* output, size_t output_size) {
    if (std::abs(value) < 0.0005) value = 0.0;
    std::snprintf(output, output_size, "%+.3f", value);
}

static std::string virtual_key_name(UINT virtual_key) {
    UINT scan = MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC);
    if (virtual_key == VK_LEFT || virtual_key == VK_UP || virtual_key == VK_RIGHT || virtual_key == VK_DOWN ||
        virtual_key == VK_PRIOR || virtual_key == VK_NEXT || virtual_key == VK_END || virtual_key == VK_HOME ||
        virtual_key == VK_INSERT || virtual_key == VK_DELETE || virtual_key == VK_DIVIDE || virtual_key == VK_NUMLOCK) {
        scan |= 0x100;
    }
    wchar_t wide[64]{};
    if (!GetKeyNameTextW(static_cast<LONG>(scan << 16), wide, 64)) return "UNKNOWN";
    char narrow[128]{};
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, sizeof(narrow), nullptr, nullptr);
    return narrow;
}

static void reset_run_for_lobby() {
    g_finished = false;
    g_race_running = false;
    g_waiting_for_start_reset = false;
    g_prestart_gate = false;
    g_prestart_saw_zero_count = false;
    g_restart_transition_pending.store(false, std::memory_order_release);
    g_finish_time = 0.0;
    g_previous_clock = -1.0;
    g_last_count = -1;
    g_splits.fill(0.0);
    for (auto& split : g_sampled_splits) split.store(0.0, std::memory_order_release);
    g_reference_splits.fill(0.0);
    g_has_reference_pb = false;
    g_run_profile_key.clear();
    g_pb_committed_for_finish = false;
    g_current_run_is_pb = false;
    g_finish_banner_seen = false;
    g_finish_banner_missing_frames = 0;
    g_restart_hold_progress.store(0.0f, std::memory_order_release);
    g_restart_key_down.store(false, std::memory_order_release);
    g_restart_controller.store(0, std::memory_order_release);
    g_restart_widget.store(0, std::memory_order_release);
    g_race_object.store(0, std::memory_order_release);
    g_render_race_object = 0;
    // A newly-created race lobby initializes these cards without emitting the
    // selection callbacks. These are the game's lobby defaults; subsequent
    // card changes are picked up instantly by the live hooks.
    g_map_name = "STADIUM";
    g_difficulty = "MEDIUM";
    g_live_profile_seen.store(true, std::memory_order_release);
}

static void draw_splitter_settings() {
    const bool open = g_settings_open.load(std::memory_order_acquire);
    ImGui::GetIO().MouseDrawCursor = open;
    if (!open) return;
    ImGui::SetNextWindowPos(ImVec2(54.0f, 80.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(430.0f, 230.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.96f);
    bool keep_open = true;
    ImGui::Begin("SPLITTER SETTINGS  //  ` TO CLOSE", &keep_open,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextColored(ImVec4(0.44f, 0.90f, 1.0f, 1.0f), "RACE CONTROLS");
    ImGui::Separator();
    const std::string restart_name = virtual_key_name(g_restart_virtual_key.load(std::memory_order_acquire));
    ImGui::Text("Hold to restart:  %s", restart_name.c_str());
    ImGui::SameLine(270.0f);
    if (g_rebind_listening.load(std::memory_order_acquire)) {
        ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.17f, 1.0f), "PRESS A KEY...");
    } else if (ImGui::Button("CHANGE BIND", ImVec2(125.0f, 30.0f))) {
        g_rebind_listening.store(true, std::memory_order_release);
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Hold time: %.2f seconds", RESTART_HOLD_SECONDS);
    ImGui::TextDisabled("Release before the bar fills to cancel.");
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.65f, 0.70f, 0.78f, 1.0f), "Press ` at any time to close these settings.");
    ImGui::End();
    if (!keep_open) g_settings_open.store(false, std::memory_order_release);
}

static void draw_proof_of_concept() {
    refresh_race_profile();
    load_personal_bests();
    if (const int known_total = known_total_for_profile(); known_total > 0) g_total_balls = known_total;
    const uintptr_t race = g_race_object.load();
    int count = 0;
    double clock = 0.0;
    const bool have_race = race && race_sample(race, count, clock);
    double frame_aligned_clock = clock;

    if (have_race) {
        const auto state_now = std::chrono::steady_clock::now();
        if (g_restart_transition_pending.exchange(false, std::memory_order_acq_rel)) {
            // The restart callback may run either just before or just after the
            // old race state is cleared. Ignore that ordering window, then wait
            // for the countdown clock to rise and perform its real reset.
            g_splits.fill(0.0);
            for (auto& split : g_sampled_splits) split.store(0.0, std::memory_order_release);
            g_last_count = count;
            g_race_running = false;
            g_prestart_gate = true;
            g_prestart_saw_zero_count = count == 0;
            g_prestart_ignore_until = state_now + std::chrono::milliseconds(350);
            g_waiting_for_start_reset = false;
            g_finished = false;
            g_finish_time = 0.0;
            g_previous_clock = clock;
            g_reference_splits.fill(0.0);
            g_has_reference_pb = false;
            g_run_profile_key.clear();
            g_pb_committed_for_finish = false;
            g_current_run_is_pb = false;
        }
        // A newly located race object can already contain an advancing setup /
        // intro clock. That is not the timed run. Arm the run here and keep the
        // visible clock at zero until this value makes the game's real reset
        // back to zero (or until a ball count proves the run is underway).
        if (race != g_render_race_object) {
            g_render_race_object = race;
            g_splits.fill(0.0);
            for (auto& split : g_sampled_splits) split.store(0.0, std::memory_order_release);
            g_last_count = count;
            g_race_running = g_arena_reloaded || count > 0;
            g_prestart_gate = !g_arena_reloaded && count == 0;
            g_prestart_saw_zero_count = count == 0;
            g_prestart_ignore_until = state_now;
            g_waiting_for_start_reset = count == 0 && clock > 0.75;
            g_finished = false;
            g_finish_time = 0.0;
            g_previous_clock = clock;
            g_reference_splits.fill(0.0);
            g_has_reference_pb = false;
            g_run_profile_key.clear();
            g_pb_committed_for_finish = false;
            g_current_run_is_pb = false;
        }
        // Present runs after Splitgate has drawn its own HUD. At that point the
        // game-owned clock has already advanced to the next simulation frame,
        // which made our live display lead the visible game timer by one frame.
        // The preceding Present sample is the value used by the HUD currently
        // in the backbuffer. Keep the sampler timestamps untouched: those mark
        // the actual count transitions and remain the authoritative splits.
        if (g_previous_clock >= 0.0 && clock >= g_previous_clock &&
            clock - g_previous_clock < 0.10) {
            frame_aligned_clock = g_previous_clock;
        }
        // The sampler runs independently of Present and normally observes the
        // count transition within the same millisecond. Pull those timestamps
        // into the visible list before processing the render-frame sample.
        for (int ball = 1; ball <= count && ball < static_cast<int>(g_splits.size()); ++ball) {
            const double sampled = g_sampled_splits[ball].load(std::memory_order_acquire);
            if (sampled > 0.0) g_splits[ball] = sampled;
        }
        if (g_last_count < 0) {
            g_last_count = count;
            g_race_running = count > 0;
            if (count > 0 && known_total_for_profile() > 0 && g_run_profile_key.empty()) begin_run_comparison();
        }
        if (count < g_last_count) {
            g_splits.fill(0.0);
            for (auto& split : g_sampled_splits) split.store(0.0, std::memory_order_release);
            g_last_count = count;
            g_race_running = false;
            g_waiting_for_start_reset = false;
            if (count == 0) g_prestart_saw_zero_count = true;
            g_finished = false;
            g_finish_time = 0.0;
            g_total_balls = 0;
            g_total_candidate = 0;
            g_total_candidate_hits = 0;
            g_reference_splits.fill(0.0);
            g_has_reference_pb = false;
            g_run_profile_key.clear();
            g_pb_committed_for_finish = false;
            g_current_run_is_pb = false;
        }
        if (count == 0) g_prestart_saw_zero_count = true;
        if (g_prestart_gate && state_now >= g_prestart_ignore_until && count == 0 && clock > 0.75) {
            g_waiting_for_start_reset = true;
        }
        if (g_prestart_gate && count == 0 && g_waiting_for_start_reset && g_previous_clock >= 0.0 &&
            clock < 0.75 && clock + 0.50 < g_previous_clock) {
            g_splits.fill(0.0);
            for (auto& split : g_sampled_splits) split.store(0.0, std::memory_order_release);
            g_last_count = 0;
            g_race_running = true;
            g_prestart_gate = false;
            g_waiting_for_start_reset = false;
            begin_run_comparison();
        }
        if (count > 0 && !g_finished && (!g_prestart_gate || g_prestart_saw_zero_count)) {
            g_prestart_gate = false;
            g_race_running = true;
        }
        if (g_race_running && known_total_for_profile() > 0 && g_run_profile_key.empty()) begin_run_comparison();
        if (g_race_running && count > g_last_count) {
            // Count and clock come from the same game-owned race object.
            // Stamp the transition with that sample; the previous frame is
            // necessarily early and makes meaningful split comparisons drift.
            const double split_clock = clock;
            for (int ball = g_last_count + 1; ball <= count && ball < static_cast<int>(g_splits.size()); ++ball) g_splits[ball] = split_clock;
            g_last_count = count;
            g_last_ball_time = std::chrono::steady_clock::now();
        }
        g_previous_clock = clock;
    }

    const bool expanded = have_race && (g_race_running || g_finished || count > 0);

    // Match Splitgate 2's unobtrusive race restart affordance. The fill gives
    // clear feedback that a tap is harmless and releasing early cancels it.
    if (have_race && (g_race_running || g_finished) && !g_in_race_lobby.load(std::memory_order_acquire)) {
        ImDrawList* restart_draw = ImGui::GetForegroundDrawList();
        const ImGuiIO& restart_io = ImGui::GetIO();
        const ImVec2 prompt_min(44.0f, restart_io.DisplaySize.y - 108.0f);
        const ImVec2 prompt_max(344.0f, restart_io.DisplaySize.y - 62.0f);
        restart_draw->AddRectFilled(prompt_min, prompt_max, IM_COL32(15, 18, 45, 220), 9.0f);
        restart_draw->AddRect(prompt_min, prompt_max, IM_COL32(100, 112, 174, 230), 9.0f, 0, 2.0f);
        restart_draw->AddRectFilled(ImVec2(prompt_min.x + 6.0f, prompt_max.y - 6.0f),
            ImVec2(prompt_min.x + 6.0f + (prompt_max.x - prompt_min.x - 12.0f) *
                std::clamp(g_restart_hold_progress.load(std::memory_order_acquire), 0.0f, 1.0f), prompt_max.y - 3.0f),
            IM_COL32(112, 229, 255, 255), 2.0f);
        restart_draw->AddRect(ImVec2(prompt_min.x + 12.0f, prompt_min.y + 9.0f), ImVec2(prompt_min.x + 38.0f, prompt_min.y + 35.0f),
            IM_COL32(230, 238, 255, 255), 5.0f, 0, 2.0f);
        const std::string restart_key = virtual_key_name(g_restart_virtual_key.load(std::memory_order_acquire));
        const char* restart_key_short = restart_key.empty() ? "?" : restart_key.c_str();
        restart_draw->AddText(g_font_header, 15.0f, ImVec2(prompt_min.x + 17.0f, prompt_min.y + 13.0f), IM_COL32(255, 255, 255, 255), restart_key_short);
        restart_draw->AddText(g_font_header, 17.0f, ImVec2(prompt_min.x + 50.0f, prompt_min.y + 13.0f), IM_COL32(236, 242, 255, 255), "[HOLD] RESTART RACE");
    }
    int visible_rows = 0;
    if (expanded) {
        for (int ball = 1; ball <= count && ball < static_cast<int>(g_splits.size()); ++ball) {
            if (g_splits[ball] > 0.0) ++visible_rows;
        }
    }

    constexpr float panel_width = 400.0f;
    constexpr float row_height = 22.0f;
    const float target_height = expanded ? 159.0f + visible_rows * row_height : 94.0f;
    static float animated_height = 0.0f;
    if (animated_height <= 0.0f) animated_height = target_height;
    animated_height += (target_height - animated_height) * std::min(1.0f, ImGui::GetIO().DeltaTime * 10.0f);

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 origin(io.DisplaySize.x - panel_width - 36.0f, 190.0f);
    const ImVec2 panel_min(origin.x - 18.0f, origin.y - 16.0f);
    const ImVec2 panel_max(origin.x + panel_width, panel_min.y + animated_height);
    const float notch_low_y = std::min(panel_min.y + 116.0f, panel_max.y - 2.0f);
    const float notch_high_y = std::min(panel_min.y + 98.0f, panel_max.y - 18.0f);
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImVec2 panel_shape[] = {
        panel_min, ImVec2(panel_max.x - 24.0f, panel_min.y), ImVec2(panel_max.x, panel_min.y + 24.0f),
        ImVec2(panel_max.x, panel_max.y - 24.0f), ImVec2(panel_max.x - 24.0f, panel_max.y),
        ImVec2(panel_min.x, panel_max.y), ImVec2(panel_min.x, notch_low_y), ImVec2(panel_min.x + 8.0f, notch_high_y)
    };
    draw->AddConvexPolyFilled(panel_shape, 8, IM_COL32(3, 17, 29, 225));
    draw->AddPolyline(panel_shape, 8, IM_COL32(10, 38, 58, 255), ImDrawFlags_Closed, 2.0f);

    char race_title[160]{};
    std::snprintf(race_title, sizeof(race_title), "%s  //  %s", g_map_name.c_str(), g_difficulty.c_str());
    draw->AddText(g_font_header, 18.0f, origin, IM_COL32(112, 229, 255, 255), race_title);
    draw->AddLine(ImVec2(origin.x, origin.y + 29.0f), ImVec2(origin.x + panel_width * 0.70f, origin.y + 29.0f), IM_COL32(244, 213, 44, 255), 3.0f);
    // Center the mark in the full right-hand header pocket rather than
    // pinning it against the clipped outer corner.
    draw_splitgate_mark(draw, ImVec2(origin.x + 294.0f, origin.y - 1.0f), 84.0f);

    if (!expanded) {
        char lobby_balls[64]{};
        if (g_total_balls > 0) std::snprintf(lobby_balls, sizeof(lobby_balls), "%02d BALLS", g_total_balls);
        else std::snprintf(lobby_balls, sizeof(lobby_balls), "-- BALLS");
        draw->AddText(g_font_header, 22.0f, ImVec2(origin.x, origin.y + 47.0f), IM_COL32(244, 213, 44, 255), lobby_balls);
        return;
    }

    char time_text[64]{};
    const double displayed_clock = g_finished ? g_finish_time : (g_race_running ? frame_aligned_clock : 0.0);
    format_race_time(displayed_clock, time_text, sizeof(time_text));
    draw->AddText(g_font_clock, 38.0f, ImVec2(origin.x, origin.y + 43.0f), IM_COL32(255, 255, 255, 255), time_text);

    char ball_text[64]{};
    if (g_total_balls > 0) std::snprintf(ball_text, sizeof(ball_text), "BALL %02d / %02d", count, g_total_balls);
    else std::snprintf(ball_text, sizeof(ball_text), "BALL %02d / --", count);
    const ImVec2 ball_size = g_font_header->CalcTextSizeA(18.0f, FLT_MAX, 0.0f, ball_text);
    draw->AddText(g_font_header, 18.0f, ImVec2(origin.x + panel_width - 18.0f - ball_size.x, origin.y + 58.0f), IM_COL32(244, 213, 44, 255), ball_text);

    draw->AddLine(ImVec2(origin.x, origin.y + 96.0f), ImVec2(origin.x + panel_width - 20.0f, origin.y + 96.0f), IM_COL32(112, 229, 255, 145), 1.0f);
    draw->AddText(g_font_body, 14.0f, ImVec2(origin.x + 4.0f, origin.y + 108.0f), IM_COL32(150, 174, 192, 255), "BALL");
    draw->AddText(g_font_body, 14.0f, ImVec2(origin.x + 74.0f, origin.y + 108.0f), IM_COL32(150, 174, 192, 255), "SPLIT");
    draw->AddText(g_font_body, 14.0f, ImVec2(origin.x + 324.0f, origin.y + 108.0f), IM_COL32(150, 174, 192, 255), "DELTA");

    float y = origin.y + 133.0f;
    const float capture_age = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_last_ball_time).count();
    const float newest_progress = std::clamp(capture_age / 0.30f, 0.0f, 1.0f);
    const float eased = 1.0f - std::pow(1.0f - newest_progress, 3.0f);
    for (int ball = 1; ball <= count && ball < static_cast<int>(g_splits.size()); ++ball) {
        if (g_splits[ball] <= 0.0) continue;
        const bool newest = ball == count;
        const float row_y = y + (newest ? (1.0f - eased) * -8.0f : 0.0f);
        const int alpha = newest ? static_cast<int>(255.0f * eased) : 255;
        const ImU32 row_color = newest ? IM_COL32(244, 213, 44, alpha) : IM_COL32(235, 247, 255, alpha);
        if (newest) {
            draw->AddRectFilled(ImVec2(origin.x, row_y - 2.0f), ImVec2(origin.x + panel_width - 20.0f, row_y + row_height - 2.0f), IM_COL32(244, 213, 44, static_cast<int>(24.0f * eased)));
        }
        char ball_number[16]{};
        char split_time[64]{};
        char delta_text[32]{};
        std::snprintf(ball_number, sizeof(ball_number), "%02d", ball);
        format_race_time(g_splits[ball], split_time, sizeof(split_time));
        draw->AddText(g_font_body, 16.0f, ImVec2(origin.x + 4.0f, row_y), row_color, ball_number);
        draw->AddText(g_font_body, 16.0f, ImVec2(origin.x + 74.0f, row_y), row_color, split_time);
        if (g_has_reference_pb && g_reference_splits[ball] > 0.0) {
            const double delta = g_splits[ball] - g_reference_splits[ball];
            format_delta(delta, delta_text, sizeof(delta_text));
            const ImU32 delta_color = delta <= 0.0
                ? IM_COL32(91, 238, 198, alpha)
                : IM_COL32(255, 111, 111, alpha);
            const ImVec2 delta_size = g_font_body->CalcTextSizeA(16.0f, FLT_MAX, 0.0f, delta_text);
            draw->AddText(g_font_body, 16.0f, ImVec2(origin.x + panel_width - 22.0f - delta_size.x, row_y), delta_color, delta_text);
        } else if (newest) {
            const char* status = g_current_run_is_pb ? "PB" : "LIVE";
            const ImVec2 status_size = g_font_body->CalcTextSizeA(15.0f, FLT_MAX, 0.0f, status);
            draw->AddText(g_font_body, 15.0f, ImVec2(origin.x + panel_width - 22.0f - status_size.x, row_y), row_color, status);
        }
        y += row_height;
    }
}

static HRESULT __stdcall hooked_present(IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags) {
    struct PresentCallGuard {
        PresentCallGuard() { g_active_hook_calls.fetch_add(1, std::memory_order_acq_rel); }
        ~PresentCallGuard() { g_active_hook_calls.fetch_sub(1, std::memory_order_acq_rel); }
    } guard;
    if (g_arena_reloaded) {
        read_arena_hud_d3d12(swap_chain);
        return g_original_present(swap_chain, sync_interval, flags);
    }
    if (!g_imgui_ready.load()) {
        g_imgui_ready.store(initialize_imgui(swap_chain));
    }
    if (g_imgui_ready.load()) {
        const bool in_lobby = race_lobby_visible(swap_chain);
        int finish_count = 0; double finish_clock = 0.0;
        const uintptr_t finish_race = g_race_object.load();
        const bool have_finish_sample = finish_race && race_sample(finish_race, finish_count, finish_clock);
        const int memory_total = have_finish_sample ? race_total_balls(finish_race) : 0;
        if (memory_total > 0) g_total_balls = memory_total;
        if (!g_finished && g_race_running && have_finish_sample && g_total_balls > 0 &&
            finish_count >= g_total_balls) {
            // The finish sample is observed before the regular draw/update pass.
            // Capture the final ball here so stopping the race cannot suppress
            // the last row in the split list.
            if (finish_count > 0 && finish_count < static_cast<int>(g_splits.size()) &&
                g_splits[finish_count] <= 0.0) {
                const double sampled = g_sampled_splits[finish_count].load(std::memory_order_acquire);
                g_splits[finish_count] = sampled > 0.0 ? sampled : finish_clock;
                g_last_count = finish_count;
                g_last_ball_time = std::chrono::steady_clock::now();
            }
            g_finished = true;
            g_race_running = false;
            g_finish_banner_seen = false;
            g_finish_banner_missing_frames = 0;
            g_finish_time = (finish_count < static_cast<int>(g_splits.size()) && g_splits[finish_count] > 0.0)
                ? g_splits[finish_count]
                : (g_previous_clock > 0.0 ? g_previous_clock : finish_clock);
            commit_personal_best(finish_count);
        }
        if (g_finished) {
            // Retry until the independently sampled final split is available.
            // commit_personal_best remains a no-op once the result is stored.
            commit_personal_best(g_last_count);
            const bool banner_visible = strict_finish_banner_visible(swap_chain);
            if (banner_visible) {
                g_finish_banner_seen = true;
                g_finish_banner_missing_frames = 0;
            } else if (g_finish_banner_seen && ++g_finish_banner_missing_frames >= 3) {
                reset_run_for_lobby();
            }
        }
        // Commit the final native race sample before clearing the retained
        // object. Some PCs reach the lobby in the same render frame as the
        // last ball and previously lost the PB before it could be written.
        if (in_lobby && (g_race_running || g_finished || g_last_count >= 0 || g_race_object.load())) {
            reset_run_for_lobby();
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        draw_proof_of_concept();
        draw_splitter_settings();
        ImGui::Render();
        g_context->OMSetRenderTargets(1, &g_render_target, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    return g_original_present(swap_chain, sync_interval, flags);
}

static LRESULT CALLBACK dummy_wndproc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

static void* find_present_address() {
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, dummy_wndproc, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"SplitgateOverlayDummy", nullptr};
    RegisterClassExW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swap_chain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_0};
    HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, requested, 1, D3D11_SDK_VERSION, &desc, &swap_chain, &device, &level, &context);
    void* present = nullptr;
    if (SUCCEEDED(result)) {
        void** vtable = *reinterpret_cast<void***>(swap_chain);
        present = vtable[8];
    }
    if (context) context->Release();
    if (device) device->Release();
    if (swap_chain) swap_chain->Release();
    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return present;
}

static void* find_execute_command_lists_address() {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    void* execute = nullptr;
    if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)))) {
            void** vtable = *reinterpret_cast<void***>(queue);
            execute = vtable[10];
        }
    }
    if (queue) queue->Release();
    if (device) device->Release();
    return execute;
}

static DWORD WINAPI overlay_thread(void*) {
    while (!GetModuleHandleW(L"d3d11.dll")) Sleep(100);
    void* present = find_present_address();
    if (!present || MH_Initialize() != MH_OK) return 1;
    if (MH_CreateHook(present, &hooked_present, reinterpret_cast<void**>(&g_original_present)) != MH_OK) return 2;
    if (g_arena_reloaded) {
        void* execute_command_lists = find_execute_command_lists_address();
        if (!execute_command_lists || MH_CreateHook(execute_command_lists, &hooked_execute_command_lists,
                reinterpret_cast<void**>(&g_original_execute_command_lists)) != MH_OK) return 8;
    }
    if (!g_arena_reloaded) {
        const uintptr_t image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        void* restart_handler = reinterpret_cast<void*>(image + RESTART_RACE_HANDLER_RVA);
        if (MH_CreateHook(restart_handler, &hooked_restart_race, reinterpret_cast<void**>(&g_original_restart_race)) != MH_OK) return 3;
        void* visibility_handler = reinterpret_cast<void*>(image + RESTART_VISIBILITY_HANDLER_RVA);
        if (MH_CreateHook(visibility_handler, &hooked_restart_visibility, reinterpret_cast<void**>(&g_original_restart_visibility)) != MH_OK) return 4;
        void* difficulty_handler = reinterpret_cast<void*>(image + DIFFICULTY_SELECTED_RVA);
        if (MH_CreateHook(difficulty_handler, &hooked_difficulty_selected, reinterpret_cast<void**>(&g_original_difficulty_selected)) != MH_OK) return 5;
        void* map_selected_exec = reinterpret_cast<void*>(image + MAP_SELECTED_EXEC_RVA);
        if (MH_CreateHook(map_selected_exec, &hooked_map_selected_exec, reinterpret_cast<void**>(&g_original_map_selected_exec)) != MH_OK) return 6;
    } else {
        g_race_object.store(1, std::memory_order_release);
        g_map_name = "ARENA RELOADED";
        g_difficulty = "RACE";
        g_live_profile_seen.store(true, std::memory_order_release);
    }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) return 7;

    int sampled_count = -1;
    bool restart_latched = false;
    bool restart_was_down = false;
    auto restart_hold_started = std::chrono::steady_clock::now();
    auto next_locator_check = std::chrono::steady_clock::now();
    while (!(GetAsyncKeyState(VK_END) & 1)) {
        uintptr_t race = g_race_object.load();
        int count = 0; double clock = 0;
        const bool valid = race && race_sample(race, count, clock);
        if (valid) {
            if (sampled_count < 0) sampled_count = count;
            if (count < sampled_count) {
                for (auto& split : g_sampled_splits) split.store(0.0, std::memory_order_release);
                sampled_count = count;
            } else if (count > sampled_count) {
                for (int ball = sampled_count + 1; ball <= count && ball < static_cast<int>(g_sampled_splits.size()); ++ball) {
                    g_sampled_splits[ball].store(clock, std::memory_order_release);
                }
                sampled_count = count;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        const bool restart_down = !g_arena_reloaded && g_restart_key_down.load(std::memory_order_acquire);
        if (restart_down) {
            if (!restart_was_down) restart_hold_started = now;
            restart_was_down = true;
            const float held = std::chrono::duration<float>(now - restart_hold_started).count();
            const float progress = std::clamp(held / RESTART_HOLD_SECONDS, 0.0f, 1.0f);
            g_restart_hold_progress.store(progress, std::memory_order_release);
            if (progress >= 1.0f && !restart_latched) {
                restart_latched = true;
                if (valid && !g_in_race_lobby.load(std::memory_order_acquire)) dispatch_restart_on_game_thread();
            }
        } else {
            g_restart_hold_progress.store(0.0f, std::memory_order_release);
            restart_latched = false;
            restart_was_down = false;
        }
        // Lobby objects can retain the just-exited race with a perfectly valid
        // frozen clock. Do not let the background locator re-adopt that stale
        // object after the render thread has cleared it for the lobby.
        if (!g_arena_reloaded && (!valid || g_finished) &&
            !g_in_race_lobby.load(std::memory_order_acquire) &&
            now >= next_locator_check) {
            const uintptr_t located = locate_race();
            if (located && located != race) {
                g_race_object.store(located);
                g_restart_controller.store(0, std::memory_order_release);
                g_restart_widget.store(0, std::memory_order_release);
                sampled_count = -1;
            }
            next_locator_check = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        } else if (g_in_race_lobby.load(std::memory_order_acquire)) {
            sampled_count = -1;
        }
        // This is deliberately independent of the render rate. Splitgate
        // already requests a 1 ms system timer period while running.
        Sleep(1);
    }
    MH_DisableHook(MH_ALL_HOOKS);
    // A Present callback can already be executing on the render thread when
    // END requests a hot unload. Wait for it to leave this DLL before freeing
    // the D3D resources or the module itself.
    for (int i = 0; i < 2000 && g_active_hook_calls.load(std::memory_order_acquire) > 0; ++i) Sleep(1);
    MH_Uninitialize();
    if (g_game_window && g_original_wndproc) {
        SetWindowLongPtrW(g_game_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original_wndproc));
        g_original_wndproc = nullptr;
    }
    if (g_logo_texture) { g_logo_texture->Release(); g_logo_texture = nullptr; }
    if (g_lobby_probe) { g_lobby_probe->Release(); g_lobby_probe = nullptr; }
    if (g_arena_hud_probe) { g_arena_hud_probe->Release(); g_arena_hud_probe = nullptr; }
    release_arena_d3d12_capture();
    if (ID3D12CommandQueue* queue = g_arena_command_queue.exchange(nullptr, std::memory_order_acq_rel)) queue->Release();
    Sleep(100);
    FreeLibraryAndExitThread(g_module, 0);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        wchar_t executable[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, executable, MAX_PATH)) {
            const wchar_t* name = wcsrchr(executable, L'\\');
            name = name ? name + 1 : executable;
            g_arena_reloaded = _wcsicmp(name, L"PortalWars2-Win64-Shipping.exe") == 0;
        }
        DisableThreadLibraryCalls(module);
        if (HANDLE thread = CreateThread(nullptr, 0, overlay_thread, nullptr, 0, nullptr)) CloseHandle(thread);
    }
    return TRUE;
}

