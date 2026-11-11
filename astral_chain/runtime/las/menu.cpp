// In-game settings menu (Dear ImGui), drawn over the game picture inside the window image's rendering (device.c
// backend_present, after the game image). F1, or Minus+Plus on a controller, opens and closes it; Esc closes it too.
// While it is open the game gets no input (the runtime: live_settings.h, app_window.h, hid_service.h).
//
// The menu runs on the thread that presents (the render thread, or the submission thread with AC_ASYNC_SUBMIT): all
// of its Vulkan work, ImGui's one-time font upload included, is on the thread that does the queue's other work.
// Window input comes from the runtime's event ring, the controllers from three words it updates.
//
// Settings: what the session started with comes from the environment the launcher passed; a change is applied at
// once where the runtime can (ac_setting_apply), else at the next start, and saved to userdata\launcher.ini, the
// launcher's own file (AC_SETTINGS_FILE overrides the path; tests use it).
//
// F2 opens its second page, the HUD layout: with a render resolution wider than 16:9, where each corner's HUD panels
// go (live_settings.h ac_hud_layout, las/nvn/vulkan/exec.c). Kept in the same file as hud_layout; the first time the
// HUD shows that wide in a session, a line says F2 is there, until the page has been opened once (hud_layout_seen).
#include <windows.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>

#include "imgui.h"
#include "imgui_impl_vulkan.h"

extern "C" {
#include "nvn/vulkan/vk.h"

// The runtime's (live_settings.h, app_window.h)
extern volatile long ac_menu_open, ac_menu_close_request, ac_menu_quit_request, ac_menu_modal, ac_menu_page;
extern volatile int ac_hud_layout[4][2];                 // (live_settings.h: the F2 HUD layout)
extern volatile long ac_hud_seen;
struct AcMenuEvent { int kind, a, b; };
extern AcMenuEvent ac_menu_events[256];
extern volatile long ac_menu_head, ac_menu_tail;
extern volatile long ac_menu_pad_buttons, ac_menu_pad_lx, ac_menu_pad_ly;
extern volatile int ac_title_credit;                     // (live_settings.h)
extern volatile int ac_fps_counter;                      // (live_settings.h) 0 off, 1 frame rate, 2 detailed
extern volatile int ac_uncapped;                         // (live_settings.h)
// The renderer's numbers for the detailed frame rate panel (nvn/vulkan/exec.c, device.c)
extern volatile int vk_perf_detail;
void vk_perf_totals(uint64_t *gpu_ns, uint64_t *wait_gpu_ns, uint64_t *wait_shader_ns);
void vk_perf_scene(int *w, int *h);
void vk_perf_vram(uint64_t *usage, uint64_t *budget, uint64_t *size);
void vk_perf_sizes(int *win_w, int *win_h, int *img_w, int *img_h);
const char *vk_perf_gpu_name(void);
int vk_perf_luid(uint32_t *hi, uint32_t *lo);
const char *vk_perf_present_mode(void);
extern volatile unsigned long long ac_title_video_tick;  // (romfs_storage.h: last read of a title-screen video)
void ac_setting_apply(const char *key, const char *value);
int ac_fullscreen_state(void);
}

namespace {

enum Kind { CHECK, CHOICE, NUMBER };
struct Opt {
    const char *key, *group, *label, *help;
    Kind kind;
    const char *def;
    const char *choices;  // "value|Label;..."
    int lo, hi;
    bool live;            // applies at once; otherwise at the next start
    std::string value;
};

// The launcher's options (launcher/Launcher.cs: same keys, defaults and meaning) that make sense while playing.
std::vector<Opt> opts = {
    {"prompts", "Controls", "Button icons in the game",
     "Automatic: the icons follow what you last used (keyboard and mouse, an Xbox, PlayStation or Switch controller).",
     CHOICE, "auto", "auto|Automatic;keyboard|Keyboard and mouse;xbox|Xbox controller;playstation|PlayStation controller;switch|Switch controller", 0, 0, true},
    {"swap_ab", "Controls", "Swap A and B on the controller",
     "On: the bottom button (Xbox A, PlayStation cross) confirms and talks, the right one goes back. The button icons follow.",
     CHECK, "0", nullptr, 0, 0, true},
    {"rumble", "Controls", "Rumble strength (%)", "0 turns rumble off.", NUMBER, "100", nullptr, 0, 200, true},
    {"mouse_look", "Controls", "Mouse moves the camera", "Click in the game to give it the mouse; hold Alt to get the cursor back.",
     CHECK, "1", nullptr, 0, 0, true},
    {"mouse_sens", "Controls", "Mouse camera sensitivity (%)", nullptr, NUMBER, "100", nullptr, 10, 500, true},
    {"mouse_invert", "Controls", "Invert mouse up and down", nullptr, CHECK, "0", nullptr, 0, 0, true},
    {"fullscreen", "Display", "Fullscreen", "Also F11 or Alt+Enter. The game starts the way you leave it.", CHECK, "0", nullptr, 0, 0, true},
    {"resolution", "Display", "Render resolution (experimental)",
     "1600x900 is the Switch's (with its dynamic resolution). The smaller sizes are for slow GPUs; 21:9 and 32:9 for ultrawide screens, 48:9 for three screens side by side.",
     CHOICE, "1600x900", "854x480|854x480 (480p);960x540|960x540 (540p);1280x720|1280x720 (720p);1600x900|1600x900 (default);1920x1080|1920x1080;2560x1440|2560x1440;3840x2160|3840x2160;"
     "2560x1080|2560x1080 (21:9);3440x1440|3440x1440 (21:9);3840x1080|3840x1080 (32:9);5120x1440|5120x1440 (32:9);5760x1080|5760x1080 (48:9);7680x1440|7680x1440 (48:9, drawn at 6788x1272)", 0, 0, false},
    {"upscaler", "Display", "Upscaling (experimental)", "Only when the window is larger than the picture.",
     CHOICE, "off", "off|Smooth (default);fsr1|AMD FSR 1", 0, 0, true},
    {"fsr_sharpness", "Display", "FSR 1 sharpening", nullptr, CHOICE, "0.2", "0|Strong;0.2|Normal (default);0.5|Light;1|Very light", 0, 0, true},
    {"hud_edges", "Display", "Ultrawide: HUD at the screen's edges (experimental)",
     "With a render resolution wider than 16:9, the HUD's side panels (health, minimap, objectives, tutorials) move to "
     "the edges of the screen. Markers over characters stay where they are. HUD layout (F2): how far each corner's "
     "panels move, and up or down.", CHECK, "0", nullptr, 0, 0, true},
    {"fps_counter", "Display", "Show the frame rate",
     "In the top left corner. Frame rate: frames per second and the average frame time. Detailed: also a frame time graph, "
     "the slowest frames, GPU and CPU load, memory, the GPU's temperature and power where its driver gives them, and the "
     "render sizes.",
     CHOICE, "0", "0|Off;1|Frame rate;2|Detailed", 0, 0, true},
    {"port_credit", "Display", "Show \"Port by Puddz\" on the title screen", nullptr, CHECK, "1", nullptr, 0, 0, true},
    {"title_own_twin", "Display", "Title screen shows the twin you play as",
     "The game shows the other twin. From the next time the title screen appears.", CHECK, "0", nullptr, 0, 0, true},
    {"handheld", "Display", "Handheld mode: render at 1280x720", "For GPUs that cannot hold 60 fps.", CHECK, "0", nullptr, 0, 0, false},
    {"present", "Display", "Presentation mode", "Mailbox: no tearing. Vsync: waits for the display. Immediate: tearing, lowest latency.",
     CHOICE, "mailbox", "mailbox|Mailbox (default);fifo|Vsync (FIFO);immediate|Immediate", 0, 0, false},
    {"uncapped", "Game", "Uncapped frame rate (experimental)",
     "Gameplay is not held to 60 fps (cutscenes stay at 60). With Vsync, the display's refresh rate is the limit.",
     CHECK, "0", nullptr, 0, 0, true},
    {"cutscene_fps_30", "Game", "Cutscenes at 30 fps, as on the Switch", "From the next cutscene on.", CHECK, "0", nullptr, 0, 0, true},
    {"ignore_focus", "Game", "Keep running while the game window is minimized", nullptr, CHECK, "0", nullptr, 0, 0, true},
    {"async_submit", "Game", "Prepare graphics on a separate thread (experimental)", nullptr, CHECK, "0", nullptr, 0, 0, false},
};

std::string env(const char *name) {
    char v[256];
    DWORD n = GetEnvironmentVariableA(name, v, sizeof v);
    return n && n < sizeof v ? std::string(v) : std::string();
}

// What this session runs with: the launcher's environment for each option.
void read_session() {
    for (Opt &o : opts) {
        std::string k = o.key, v = o.def;
        if (k == "prompts") { std::string e = env("AC_PROMPTS"); if (!e.empty()) v = e == "0" ? "switch" : e; }
        else if (k == "rumble") { std::string e = env("AC_RUMBLE"); if (!e.empty()) v = e; }
        else if (k == "swap_ab") v = env("AC_SWAP_AB") == "1" ? "1" : "0";
        else if (k == "hud_edges") v = env("AC_HUD_EDGES") == "1" ? "1" : "0";
        else if (k == "mouse_look") v = env("AC_MOUSE_LOOK") == "0" ? "0" : "1";
        else if (k == "mouse_sens") { std::string e = env("AC_MOUSE_SENS"); if (!e.empty()) v = e; }
        else if (k == "mouse_invert") v = env("AC_MOUSE_INVERT") == "1" ? "1" : "0";
        else if (k == "fullscreen") v = ac_fullscreen_state() ? "1" : "0";
        else if (k == "handheld") v = env("AC_HANDHELD") == "1" ? "1" : "0";
        else if (k == "resolution") { std::string e = env("AC_RESOLUTION"); if (!e.empty()) v = e; }
        else if (k == "upscaler") v = env("AC_UPSCALER") == "fsr1" ? "fsr1" : "off";
        else if (k == "fsr_sharpness") { std::string e = env("AC_FSR_SHARPNESS"); if (!e.empty()) v = e; }
        else if (k == "present") { std::string e = env("LARECOMP_VK_PRESENT"); if (!e.empty()) v = e; }
        else if (k == "uncapped") v = env("AC_UNCAPPED") == "1" ? "1" : "0";
        else if (k == "cutscene_fps_30") v = env("AC_CUTSCENE_FPS") == "30" ? "1" : "0";
        else if (k == "ignore_focus") { std::string e = env("AC_IGNORE_FOCUS"); v = !e.empty() && e != "0" ? "1" : "0"; }
        else if (k == "async_submit") v = env("AC_ASYNC_SUBMIT") == "1" ? "1" : "0";
        else if (k == "port_credit") v = env("AC_TITLE_CREDIT") == "0" ? "0" : "1";
        else if (k == "fps_counter") { std::string e = env("AC_FPS_COUNTER"); v = e == "1" || e == "2" ? e : "0"; }
        else if (k == "title_own_twin") v = env("AC_TITLE_OWN_TWIN") == "1" ? "1" : "0";
        o.value = v;
    }
}

// The launcher's file, as a UTF-16 path (so that any folder name works). Empty: none.
std::wstring settings_path() {
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(L"AC_SETTINGS_FILE", buf, 1024);
    if (n && n < 1024) return std::wstring(buf);
    n = GetModuleFileNameW(NULL, buf, 1024);  // build\game\round-00\trace.exe -> userdata\launcher.ini
    if (!n || n >= 1024) return std::wstring();
    std::wstring s(buf);
    for (int i = 0; i < 4; i++) { size_t k = s.find_last_of(L"\\/"); if (k == std::wstring::npos) return std::wstring(); s.resize(k); }
    std::wstring dir = s + L"\\userdata";
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return std::wstring();
    return dir + L"\\launcher.ini";
}

// The file's lines. 1: read; 0: there is no such file; -1: it is there and could not be read (in use, no access).
int read_lines(const std::wstring &path, std::vector<std::string> &lines) {
    FILE *f = NULL;
    for (int tries = 0; tries < 10 && !f; tries++) {  // (the launcher or a scanner has it open for a moment)
        f = _wfopen(path.c_str(), L"rb");
        if (f) break;
        DWORD a = GetFileAttributesW(path.c_str()), e = GetLastError();
        if (a == INVALID_FILE_ATTRIBUTES && (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)) return 0;
        Sleep(20);
    }
    if (!f) return -1;
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n);
    bool bad = ferror(f) != 0;
    fclose(f);
    if (bad) return -1;
    for (size_t at = 0; at < all.size();) {
        size_t end = all.find('\n', at);
        if (end == std::string::npos) end = all.size();
        std::string l = all.substr(at, end - at);
        while (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        at = end + 1;
    }
    return 1;
}

// key=value in the launcher's file: the line replaced, or added; every other line is kept as it is. Written to a
// temporary file, then moved over it. Nothing is written when the file is there and cannot be read (its other
// settings would be lost).
bool save(const char *key, const std::string &value) {
    std::wstring path = settings_path();
    if (path.empty()) return false;
    std::vector<std::string> lines;
    int got = read_lines(path, lines);
    if (got < 0) return false;
    if (lines.empty()) lines.push_back("# Astral Chain PC launcher settings");
    std::string prefix = std::string(key) + "=";
    bool found = false;
    for (std::string &l : lines)
        if (l.compare(0, prefix.size(), prefix) == 0) { l = prefix + value; found = true; }
    if (!found) lines.push_back(prefix + value);
    std::wstring tmp = path + L".tmp";
    FILE *f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    bool ok = true;
    for (const std::string &l : lines) ok = fprintf(f, "%s\r\n", l.c_str()) >= 0 && ok;
    ok = fflush(f) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    for (int tries = 0; tries < 10; tries++) {
        if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
        Sleep(20);
    }
    DeleteFileW(tmp.c_str());
    return false;
}

// A key's value in the launcher's file ("" when it has none).
std::string load(const char *key) {
    std::wstring path = settings_path();
    std::string prefix = std::string(key) + "=", v;
    if (path.empty()) return v;
    std::vector<std::string> lines;
    if (read_lines(path, lines) > 0)
        for (const std::string &l : lines)
            if (l.compare(0, prefix.size(), prefix) == 0) v = l.substr(prefix.size());
    return v;
}

ImGuiContext *ctx;
bool inited, failed, was_open, saved_ok = true;
bool restart_needed;
bool quit_asked;  // the "Quit game?" question is up
ImFont *font;
long pad_last;

ImGuiKey vk_key(int vk) {
    switch (vk) {
    case VK_TAB: return ImGuiKey_Tab; case VK_LEFT: return ImGuiKey_LeftArrow; case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow; case VK_DOWN: return ImGuiKey_DownArrow; case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown; case VK_HOME: return ImGuiKey_Home; case VK_END: return ImGuiKey_End;
    case VK_DELETE: return ImGuiKey_Delete; case VK_BACK: return ImGuiKey_Backspace; case VK_SPACE: return ImGuiKey_Space;
    case VK_RETURN: return ImGuiKey_Enter; case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_SHIFT: case VK_LSHIFT: return ImGuiKey_LeftShift; case VK_RSHIFT: return ImGuiKey_RightShift;
    case VK_CONTROL: case VK_LCONTROL: return ImGuiKey_LeftCtrl; case VK_RCONTROL: return ImGuiKey_RightCtrl;
    }
    if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
    if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
    return ImGuiKey_None;
}

bool init() {
    IMGUI_CHECKVERSION();
    ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    char fonts[MAX_PATH];
    UINT n = GetWindowsDirectoryA(fonts, sizeof fonts);
    std::string segoe = n && n < sizeof fonts ? std::string(fonts) + "\\Fonts\\segoeui.ttf" : std::string();
    if (!segoe.empty() && GetFileAttributesA(segoe.c_str()) != INVALID_FILE_ATTRIBUTES)  // (a missing file asserts in ImGui)
        font = io.Fonts->AddFontFromFileTTF(segoe.c_str(), 34.0f);
    if (!font) font = io.Fonts->AddFontDefault();
    ImGuiStyle &st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 10; st.FrameRounding = 6; st.GrabRounding = 6; st.PopupRounding = 6;
    st.WindowPadding = ImVec2(28, 22); st.FramePadding = ImVec2(12, 6); st.ItemSpacing = ImVec2(14, 10);
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.067f, 0.11f, 0.94f);
    st.Colors[ImGuiCol_FrameBg] = ImVec4(0.094f, 0.118f, 0.18f, 1.0f);
    st.Colors[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.085f, 0.14f, 0.98f);
    st.Colors[ImGuiCol_TitleBg] = st.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.09f, 0.11f, 0.19f, 1.0f);
    st.Colors[ImGuiCol_Border] = ImVec4(0.25f, 0.32f, 0.5f, 0.6f);

    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = g_vk.inst;
    ii.PhysicalDevice = g_vk.phys;
    ii.Device = g_vk.dev;
    ii.QueueFamily = g_vk.qfam;
    ii.Queue = g_vk.queue;
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = 2;
    ii.ImageCount = (g_vk.sc_count > 3 ? g_vk.sc_count : 3) + 1;  // (its vertex buffers rotate per frame: cover frames in flight)
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.PipelineCache = g_vk.pcache;
    ii.UseDynamicRendering = true;
    ii.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_vk.sc_format;
    if (!ImGui_ImplVulkan_Init(&ii)) { printf("[menu] Vulkan backend init failed: no in-game menu\n"); return false; }
    read_session();
    printf("[menu] ready (settings file: %ls)\n", settings_path().empty() ? L"none" : settings_path().c_str());
    return true;
}

void drain(ImGuiIO &io) {
    long tail = ac_menu_tail, head = ac_menu_head;
    MemoryBarrier();
    for (; tail != head; tail++) {
        AcMenuEvent e = ac_menu_events[tail & 255];
        switch (e.kind) {
        case 1: io.AddMousePosEvent((float)e.a, (float)e.b); break;
        case 2: io.AddMouseButtonEvent(e.a, e.b != 0); break;
        case 3: if (e.b) io.AddMouseWheelEvent(-e.a / 120.0f, 0); else io.AddMouseWheelEvent(0, e.a / 120.0f); break;
        case 4: { ImGuiKey k = vk_key(e.a); if (k != ImGuiKey_None) io.AddKeyEvent(k, e.b != 0); break; }
        case 5: io.AddInputCharacterUTF16((ImWchar16)e.a); break;
        }
    }
    InterlockedExchange(&ac_menu_tail, tail);
    // Controllers (Switch button bits, positional): A (right) confirms and B (bottom) goes back, as in the game's menus.
    long b = ac_menu_pad_buttons;
    static const struct { int bit; ImGuiKey key; } map[] = {
        {0, ImGuiKey_GamepadFaceDown}, {1, ImGuiKey_GamepadFaceRight}, {2, ImGuiKey_GamepadFaceUp}, {3, ImGuiKey_GamepadFaceLeft},
        {6, ImGuiKey_GamepadL1}, {7, ImGuiKey_GamepadR1}, {12, ImGuiKey_GamepadDpadLeft}, {13, ImGuiKey_GamepadDpadUp},
        {14, ImGuiKey_GamepadDpadRight}, {15, ImGuiKey_GamepadDpadDown}};
    for (auto &m : map)
        if (((b ^ pad_last) >> m.bit) & 1) io.AddKeyEvent(m.key, (b >> m.bit) & 1);
    pad_last = b;
    float lx = ac_menu_pad_lx / 32767.0f, ly = ac_menu_pad_ly / 32767.0f;  // (already past the dead zone; up is +)
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, lx < -0.3f, lx < 0 ? -lx : 0);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, lx > 0.3f, lx > 0 ? lx : 0);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, ly > 0.3f, ly > 0 ? ly : 0);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, ly < -0.3f, ly < 0 ? -ly : 0);
}

// write: to the launcher's file too (a slider writes when it is let go, not at every step of a drag).
void set(Opt &o, const std::string &v, bool write = true) {
    if (o.value == v) return;
    o.value = v;
    if (o.live) ac_setting_apply(o.key, v.c_str());
    else restart_needed = true;
    if (write) saved_ok = save(o.key, v);
}

bool picture_wide();

void draw(float scale) {
    ImGuiIO &io = ImGui::GetIO();
    // (B leaves a list or a slider first: only with nothing open since the last frame does it close the menu)
    bool busy = ImGui::IsAnyItemActive() || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    ImVec2 size(io.DisplaySize.x * 0.62f, io.DisplaySize.y * 0.8f);
    if (size.x < 760 * scale) size.x = io.DisplaySize.x * 0.92f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    if (!was_open) ImGui::SetNextWindowFocus();
    ImGui::Begin("Astral Chain PC: settings", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextDisabled("F1 or Minus + Plus: back to the game. The game does not get your input while this is open.");
    if (picture_wide()) {  // (the HUD layout's button is further down, under Display)
        ImGui::TextDisabled("Wide screen: the HUD's place is in");
        ImGui::SameLine();
        if (ImGui::SmallButton("HUD layout (F2)")) InterlockedExchange(&ac_menu_page, 1);
    }
    const char *group = nullptr;
    float label_w = ImGui::GetContentRegionAvail().x * 0.5f;
    for (Opt &o : opts) {
        if (!group || strcmp(group, o.group)) { group = o.group; ImGui::Spacing(); ImGui::SeparatorText(group); }
        ImGui::PushID(o.key);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(o.label);
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-1);
        if (o.kind == CHECK) {
            bool on = o.value == "1";
            if (ImGui::Checkbox("##v", &on)) set(o, on ? "1" : "0");
        } else if (o.kind == NUMBER) {
            int v = atoi(o.value.c_str());
            if (ImGui::SliderInt("##v", &v, o.lo, o.hi, "%d", ImGuiSliderFlags_AlwaysClamp)) set(o, std::to_string(v), false);
            if (ImGui::IsItemDeactivatedAfterEdit()) saved_ok = save(o.key, o.value);
        } else {
            std::vector<std::pair<std::string, std::string>> ch;
            for (const char *p = o.choices; *p;) {
                const char *bar = strchr(p, '|'), *end = strchr(p, ';');
                if (!end) end = p + strlen(p);
                ch.emplace_back(std::string(p, bar), std::string(bar + 1, end));
                p = *end ? end + 1 : end;
            }
            const char *shown = o.value.c_str();
            for (auto &c : ch) if (c.first == o.value) shown = c.second.c_str();
            if (ImGui::BeginCombo("##v", shown)) {
                for (auto &c : ch) {
                    bool sel = c.first == o.value;
                    if (ImGui::Selectable(c.second.c_str(), sel)) set(o, c.first);
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
        if (!strcmp(o.key, "hud_edges")) {
            ImGui::SetCursorPosX(label_w);
            if (ImGui::Button("HUD layout (F2)...")) InterlockedExchange(&ac_menu_page, 1);
        }
        if (o.help || !o.live) {
            ImGui::PushTextWrapPos(0);
            if (!o.live) ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "Applies at the next start.");
            if (!o.live && o.help) ImGui::SameLine();
            if (o.help) ImGui::TextDisabled("%s", o.help);
            ImGui::PopTextWrapPos();
        }
        ImGui::PopID();
    }
    ImGui::Spacing();
    ImGui::Separator();
    if (restart_needed) ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "Some changes apply when you next start the game.");
    if (!saved_ok) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Could not save to userdata\\launcher.ini: changes last until you close the game.");
    ImGui::TextDisabled("Other settings (texture compression, shaders, keys) are in the launcher.");
    if (ImGui::Button("Back to the game")) InterlockedExchange(&ac_menu_close_request, 1);
    ImGui::SameLine();
    if (ImGui::Button("Quit game")) { quit_asked = true; ImGui::OpenPopup("Quit game?"); }
    // B on a controller with nothing open closes the menu (Esc and F1: app_window.h)
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && !busy) InterlockedExchange(&ac_menu_close_request, 1);
    // Keep playing comes first and has the focus, so a second A press does not quit. (ImGui gives a modal's focus back to
    // the button that opened it, and does not close a modal on B or Esc by itself; F1 can close the menu under it.)
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Quit game?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                               ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("Progress since the game last saved will be lost.");
        ImGui::Spacing();
        bool stay = ImGui::Button("Keep playing");
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Quit game")) { InterlockedExchange(&ac_menu_quit_request, 1); stay = true; }
        if (stay || !quit_asked || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
            quit_asked = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    InterlockedExchange(&ac_menu_modal, quit_asked);
    ImGui::End();
}

// "Port by Puddz" on the title screen (on by default; AC_TITLE_CREDIT=0 or the menu's option turns it off): drawn while the game streams one of
// the title-screen videos (romfs_storage.h), above the version number in the bottom right corner of the picture.
// The picture keeps 16:9 inside the window (device.c), so the place is worked out in that rectangle.
const char *const credit_text = "Port by Puddz";
ULONGLONG credit_since;  // when the title was first seen this time (fade-in)


// ---- the HUD layout (F2)
const char *const corner_names[4] = {"Top left", "Bottom left", "Top right", "Bottom right"};
const int corner_default[4][2] = {{100, 0}, {100, 0}, {100, 0}, {100, 0}};
bool layout_loaded, hint_done, hint_marked;
ULONGLONG hint_since;

std::string layout_text() {
    std::string t;
    for (int c = 0; c < 4; c++)
        t += (c ? ";" : "") + std::to_string(ac_hud_layout[c][0]) + "," + std::to_string(ac_hud_layout[c][1]);
    return t;
}

// "across,down;..." (or "/" between them) for the four corners (across 0-100, down -200..200); a part missing or out of range keeps its default
void parse_layout(const std::string &t) {
    const char *q = t.c_str();
    for (int c = 0; c < 4 && *q; c++) {
        char *e;
        long a = strtol(q, &e, 10);
        if (e == q || *e != ',') break;
        q = e + 1;
        long d = strtol(q, &e, 10);
        if (e == q) break;
        q = *e == ';' || *e == '/' ? e + 1 : e;
        if (a >= 0 && a <= 100 && d >= -200 && d <= 200) { ac_hud_layout[c][0] = (int)a; ac_hud_layout[c][1] = (int)d; }
    }
}

// Before the first frame, from the menu's first call (AC_HUD_LAYOUT first: tests; else the launcher's file).
void load_layout() {
    layout_loaded = true;
    std::string t = env("AC_HUD_LAYOUT");
    if (t.empty()) t = load("hud_layout");
    if (!t.empty()) parse_layout(t);
    hint_done = env("AC_HUD_HINT") == "0" || load("hud_layout_seen") == "1";
}

bool picture_wide() {
    int ww, wh, iw, ih;
    vk_perf_sizes(&ww, &wh, &iw, &ih);
    return iw > 0 && ih > 0 && (long long)iw * 9 * 100 > (long long)ih * 16 * 101;
}

Opt *find_opt(const char *key) {
    for (Opt &o : opts) if (!strcmp(o.key, key)) return &o;
    return nullptr;
}

void draw_hud(float scale) {
    ImGuiIO &io = ImGui::GetIO();
    bool busy = ImGui::IsAnyItemActive() || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    if (!hint_marked) { hint_marked = hint_done = true; saved_ok = save("hud_layout_seen", "1"); }
    float w = 980 * scale;
    if (w > io.DisplaySize.x * 0.92f) w = io.DisplaySize.x * 0.92f;
    // (in the middle: the panels it moves are at the corners, and stay in view)
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    if (!was_open) ImGui::SetNextWindowFocus();
    ImGui::Begin("Astral Chain PC: HUD layout", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("F2 or B: back to the game. F1: the other settings. The game is drawn behind this, so each change shows at once.");
    if (!picture_wide())
        ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "The render resolution is not wider than 16:9, so the HUD fits as it is: this "
                           "applies with a wider one (21:9, 32:9, 48:9: Render resolution, F1).");
    ImGui::PopTextWrapPos();
    Opt *edges = find_opt("hud_edges");
    bool on = edges && edges->value == "1";
    if (ImGui::Checkbox("Move the HUD's side panels toward the screen's edges", &on) && edges) set(*edges, on ? "1" : "0");
    ImGui::Spacing();
    ImGui::BeginDisabled(!on);
    bool done = false;
    float label_w = ImGui::GetContentRegionAvail().x * 0.3f;
    // All four at once: across only (the corners' own heights stay)
    int all = ac_hud_layout[0][0];
    bool same = true;
    for (int c = 1; c < 4; c++) same = same && ac_hud_layout[c][0] == all;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("All corners, across");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##all", &all, 0, 100, same ? "%d%%" : "(differ)", ImGuiSliderFlags_AlwaysClamp))
        for (int c = 0; c < 4; c++) ac_hud_layout[c][0] = all;
    done |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Across: 100%% is the screen's edge, 0%% the HUD's place on a 16:9 screen. Up and down: in 1/720ths of the "
                        "screen's height, minus is up.");
    ImGui::PopTextWrapPos();
    if (ImGui::BeginTable("##corners", 3)) {
        ImGui::TableSetupColumn("##n", ImGuiTableColumnFlags_WidthFixed, label_w - ImGui::GetStyle().CellPadding.x * 2);
        ImGui::TableSetupColumn("Across", ImGuiTableColumnFlags_WidthStretch, 1);
        ImGui::TableSetupColumn("Up and down", ImGuiTableColumnFlags_WidthStretch, 1);
        ImGui::TableHeadersRow();
        for (int c = 0; c < 4; c++) {
            ImGui::PushID(c);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(corner_names[c]);
            ImGui::TableNextColumn();
            int a = ac_hud_layout[c][0], d = ac_hud_layout[c][1];
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("##a", &a, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) ac_hud_layout[c][0] = a;
            done |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("##d", &d, -200, 200, "%d", ImGuiSliderFlags_AlwaysClamp)) ac_hud_layout[c][1] = d;
            done |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Reset")) {
        for (int c = 0; c < 4; c++) { ac_hud_layout[c][0] = corner_default[c][0]; ac_hud_layout[c][1] = corner_default[c][1]; }
        done = true;
    }
    ImGui::EndDisabled();
    if (done) {
        saved_ok = save("hud_layout", layout_text());
        printf("[menu] hud_layout=%s\n", layout_text().c_str());
        fflush(stdout);
    }
    ImGui::Spacing();
    ImGui::Separator();
    if (!saved_ok) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Could not save to userdata\\launcher.ini: changes last until you close the game.");
    if (ImGui::Button("Back to the game")) InterlockedExchange(&ac_menu_close_request, 1);
    ImGui::SameLine();
    if (ImGui::Button("Settings (F1)")) InterlockedExchange(&ac_menu_page, 0);
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && !busy) InterlockedExchange(&ac_menu_close_request, 1);
    ImGui::End();
}

// The hint, once a session (15 s from the first time the HUD is drawn wider than 16:9), until F2 has been opened once.
bool hint_due(ULONGLONG now) {
    if (hint_done || !ac_hud_seen) return false;
    if (!hint_since) hint_since = now;
    if (now - hint_since > 15000) { hint_done = true; return false; }
    return true;
}

void draw_hint(float w, float h, ULONGLONG now) {
    static const char text[] = "Wide screen: F2 (or the menu, Minus + Plus) moves the HUD toward the screen's edges";
    float size = h * 0.026f;
    ImFont *f = font ? font : ImGui::GetFont();
    ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, text);
    ImVec2 pos((w - ts.x) / 2, h * 0.055f);
    float t = (float)(now - hint_since), a = t < 400 ? t / 400 : t > 14000 ? (15000 - t) / 1000 : 1;
    if (a < 0) a = 0;
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    float pad = size * 0.45f;
    dl->AddRectFilled(ImVec2(pos.x - pad * 1.6f, pos.y - pad), ImVec2(pos.x + ts.x + pad * 1.6f, pos.y + ts.y + pad),
                      ImGui::GetColorU32(ImVec4(0.055f, 0.067f, 0.11f, 0.8f * a)), size * 0.3f);
    dl->AddText(f, size, pos, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.95f * a)), text);
}

bool credit_due(ULONGLONG now) {
    if (ac_title_credit < 0) ac_title_credit = env("AC_TITLE_CREDIT") != "0";
    unsigned long long seen = ac_title_video_tick;
    bool due = ac_title_credit && seen && now - seen < 2000;
    if (!due) credit_since = 0;
    else if (!credit_since) credit_since = now;
    return due;
}

void draw_credit(float w, float h, ULONGLONG now) {
    float gw = w, gh = w * 9 / 16;
    if (gh > h) { gh = h; gw = h * 16 / 9; }
    float x0 = (w - gw) / 2, y0 = (h - gh) / 2;
    float size = gh * 0.03f;
    ImFont *f = font ? font : ImGui::GetFont();
    ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, credit_text);
    ImVec2 pos(x0 + gw * 0.966f - ts.x, y0 + gh * 0.91f - ts.y);
    float a = (float)(now - credit_since) / 600.0f;
    if (a > 1) a = 1;
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    float off = size * 0.06f > 1 ? size * 0.06f : 1;
    dl->AddText(f, size, ImVec2(pos.x + off, pos.y + off), ImGui::GetColorU32(ImVec4(0, 0, 0, 0.55f * a)), credit_text);
    dl->AddText(f, size, pos, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.9f * a)), credit_text);
}

// Frame rate counter (off by default; AC_FPS_COUNTER=1 or 2, or the menu's option): frames presented per second and
// their average time, counted here (menu_render runs once per presented frame) over half-second windows, in the top
// left corner of the window. 2 ("Detailed", issue #28) shows a panel instead: the frame time graph and the slowest
// frames, the machine's load and memory, the GPU's sensors where its driver gives them, the renderer's waits and the
// render sizes.
LARGE_INTEGER fps_t0, fps_freq, fps_last;
int fps_frames;
char fps_text[48];

int fps_mode(void) {
    if (ac_fps_counter < 0) { int m = atoi(env("AC_FPS_COUNTER").c_str()); ac_fps_counter = m >= 0 && m <= 2 ? m : 0; }
    return ac_fps_counter >= 0 && ac_fps_counter <= 2 ? ac_fps_counter : 0;
}

// ---- detailed: the frames (render thread)
const int RING = 4096;  // frame times (ms) and when each ended (QPC): the graph, and the lows (over all of them)
float ring_ms[RING];
LONGLONG ring_at[RING];
int ring_n, ring_pos;
struct Summary {  // made twice a second from the frames and the renderer's totals
    float fps, avg_ms, max_ms, low1 = -1, low01 = -1;
    float gpu_ms, wait_gpu_ms, wait_shader_ms;  // per frame
    uint64_t vram_used, vram_budget, vram_size;
    int win_w, win_h, img_w, img_h, scene_w, scene_h;
} sum;
LONGLONG sum_t0;
uint64_t sum_gpu0, sum_wait0, sum_shader0;
int sum_frames;

void frames_reset(void) { fps_last.QuadPart = 0; ring_n = 0; sum_t0 = 0; }

void frames_add(LARGE_INTEGER now) {
    if (fps_last.QuadPart) {
        ring_ms[ring_pos] = (float)(1000.0 * (double)(now.QuadPart - fps_last.QuadPart) / (double)fps_freq.QuadPart);
        ring_at[ring_pos] = now.QuadPart;
        ring_pos = (ring_pos + 1) % RING;
        if (ring_n < RING) ring_n++;
    }
    fps_last = now;
    uint64_t gpu, wg, ws;
    vk_perf_totals(&gpu, &wg, &ws);
    if (!sum_t0) { sum_t0 = now.QuadPart; sum_gpu0 = gpu; sum_wait0 = wg; sum_shader0 = ws; sum_frames = 0; return; }
    sum_frames++;
    double dt = (double)(now.QuadPart - sum_t0) / (double)fps_freq.QuadPart;
    if (dt < 0.5) return;
    // the last second's frames: rate, average, longest; the last 4096 frames (68 s at 60 fps): the 1% and 0.1% lows
    // (the frame time that 1% / 0.1% of the frames take longer than, as a rate)
    static std::vector<float> v;
    v.clear();
    double total = 0;
    int n1 = 0;
    float mx = 0;
    for (int i = 0; i < ring_n; i++) {
        int k = (ring_pos - 1 - i + RING) % RING;
        double age = (double)(now.QuadPart - ring_at[k]) / (double)fps_freq.QuadPart;
        v.push_back(ring_ms[k]);
        if (age <= 1) { total += ring_ms[k]; n1++; if (ring_ms[k] > mx) mx = ring_ms[k]; }
    }
    sum.fps = n1 && total > 0 ? (float)(n1 * 1000.0 / total) : 0;
    sum.avg_ms = n1 ? (float)(total / n1) : 0;
    sum.max_ms = mx;
    sum.low1 = sum.low01 = -1;
    if (v.size() >= 100) {
        std::sort(v.begin(), v.end());
        size_t n = v.size();
        sum.low1 = 1000.0f / v[n - 1 - n / 100];
        if (n >= 1000) sum.low01 = 1000.0f / v[n - 1 - n / 1000];
    }
    sum.gpu_ms = (float)((gpu - sum_gpu0) / 1e6 / sum_frames);
    sum.wait_gpu_ms = (float)((wg - sum_wait0) / 1e6 / sum_frames);
    sum.wait_shader_ms = (float)((ws - sum_shader0) / 1e6 / sum_frames);
    vk_perf_vram(&sum.vram_used, &sum.vram_budget, &sum.vram_size);
    vk_perf_sizes(&sum.win_w, &sum.win_h, &sum.img_w, &sum.img_h);
    vk_perf_scene(&sum.scene_w, &sum.scene_h);
    sum_t0 = now.QuadPart; sum_gpu0 = gpu; sum_wait0 = wg; sum_shader0 = ws; sum_frames = 0;
}

// ---- detailed: the machine. Sampled on a thread of its own twice a second while the panel shows (some of these calls
// take milliseconds). Load and memory from Windows (GetSystemTimes; the "GPU Engine" counters, as Task Manager), the
// GPU's temperature, power and clock from its driver's own library, loaded at run time and not shipped with the port:
// NVML (nvml.dll) on NVIDIA, ADL (atiadlxx.dll) on AMD; without them (Intel), those numbers are not shown. The approach
// is the TotK recomp's overlay's (github.com/valentderah/totk-pc-recomp, runtime/perfmon.c); the ADL declarations are
// AMD's (github.com/GPUOpen-LibrariesAndSDKs/display-library, include/adl_structures.h and adl_defines.h; MIT).
struct Machine {
    float cpu = -1, cpu_game = -1, gpu = -1, gpu_temp = -1, gpu_power = -1, gpu_clock = -1;
    double ram_used = 0, ram_total = 0, ram_game = 0;
} mach;
SRWLOCK mach_lock = SRWLOCK_INIT;
volatile LONG sampler_on;
char cpu_name[64];

bool same_gpu(const char *a, const char *b) { return *a && *b && (!strcmp(a, b) || strstr(a, b) || strstr(b, a)); }

// NVML (NVIDIA)
typedef struct { unsigned gpu, memory; } NvmlUtil;
struct {
    bool tried, ok;
    void *dev;
    int (*temp)(void *, int, unsigned *);
    int (*power)(void *, unsigned *);
    int (*clock)(void *, int, unsigned *);
    int (*util)(void *, NvmlUtil *);
} nv;

void nvml_open(const char *gpu) {
    nv.tried = true;
    HMODULE l = LoadLibraryExW(L"nvml.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!l) l = LoadLibraryW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    if (!l) return;
    auto init = (int (*)(void))GetProcAddress(l, "nvmlInit_v2");
    auto count = (int (*)(unsigned *))GetProcAddress(l, "nvmlDeviceGetCount_v2");
    auto handle = (int (*)(unsigned, void **))GetProcAddress(l, "nvmlDeviceGetHandleByIndex_v2");
    auto name = (int (*)(void *, char *, unsigned))GetProcAddress(l, "nvmlDeviceGetName");
    nv.temp = (int (*)(void *, int, unsigned *))GetProcAddress(l, "nvmlDeviceGetTemperature");
    nv.power = (int (*)(void *, unsigned *))GetProcAddress(l, "nvmlDeviceGetPowerUsage");
    nv.clock = (int (*)(void *, int, unsigned *))GetProcAddress(l, "nvmlDeviceGetClockInfo");
    nv.util = (int (*)(void *, NvmlUtil *))GetProcAddress(l, "nvmlDeviceGetUtilizationRates");
    unsigned n = 0;
    if (!init || !count || !handle || !name || init() != 0 || count(&n) != 0) return;
    for (unsigned i = 0; i < n && !nv.ok; i++) {  // the card the game renders on (if that is another vendor's: none)
        void *d;
        char nm[96] = "";
        if (handle(i, &d) == 0 && name(d, nm, sizeof nm) == 0 && same_gpu(nm, gpu)) { nv.dev = d; nv.ok = true; }
    }
    printf("[perf] NVML: %s\n", nv.ok ? "the game's GPU found" : "no matching GPU");
    fflush(stdout);
}

void nvml_sample(Machine &m) {
    if (!nv.ok) return;
    unsigned v;
    if (nv.temp && nv.temp(nv.dev, 0 /* NVML_TEMPERATURE_GPU */, &v) == 0) m.gpu_temp = (float)v;
    if (nv.power && nv.power(nv.dev, &v) == 0) m.gpu_power = v / 1000.0f;  // (mW)
    if (nv.clock && nv.clock(nv.dev, 0 /* NVML_CLOCK_GRAPHICS */, &v) == 0) m.gpu_clock = (float)v;
    NvmlUtil u;
    if (m.gpu < 0 && nv.util && nv.util(nv.dev, &u) == 0) m.gpu = (float)u.gpu;
}

// ADL (AMD)
#define ADL_MAX_PATH 256
struct AdapterInfo {
    int iSize, iAdapterIndex;
    char strUDID[ADL_MAX_PATH];
    int iBusNumber, iDeviceNumber, iFunctionNumber, iVendorID;
    char strAdapterName[ADL_MAX_PATH], strDisplayName[ADL_MAX_PATH];
    int iPresent, iExist;
    char strDriverPath[ADL_MAX_PATH], strDriverPathExt[ADL_MAX_PATH], strPNPString[ADL_MAX_PATH];
    int iOSDisplayIndex;
};
struct AdlSensor { int supported, value; };
struct AdlPmLog { int size; AdlSensor sensors[256]; };
enum { PMLOG_CLK_GFXCLK = 1, PMLOG_TEMPERATURE_EDGE = 8, PMLOG_INFO_ACTIVITY_GFX = 19, PMLOG_ASIC_POWER = 23,
       PMLOG_TEMPERATURE_HOTSPOT = 27, PMLOG_BOARD_POWER = 73 };
struct {
    bool tried, ok;
    void *ctx;
    int index;
    int (*pmlog)(void *, int, AdlPmLog *);
} adl;

void *__stdcall adl_alloc(int n) { return malloc((size_t)n); }

void adl_open(const char *gpu) {
    adl.tried = true;
    HMODULE l = LoadLibraryExW(L"atiadlxx.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!l) return;
    auto create = (int (*)(void *(__stdcall *)(int), int, void **))GetProcAddress(l, "ADL2_Main_Control_Create");
    auto count = (int (*)(void *, int *))GetProcAddress(l, "ADL2_Adapter_NumberOfAdapters_Get");
    auto info = (int (*)(void *, AdapterInfo *, int))GetProcAddress(l, "ADL2_Adapter_AdapterInfo_Get");
    adl.pmlog = (int (*)(void *, int, AdlPmLog *))GetProcAddress(l, "ADL2_New_QueryPMLogData_Get");
    int n = 0;
    if (!create || !count || !info || !adl.pmlog || create(adl_alloc, 1, &adl.ctx) != 0 || count(adl.ctx, &n) != 0 || n <= 0 || n > 64)
        return;
    std::vector<AdapterInfo> a((size_t)n);
    for (AdapterInfo &x : a) x.iSize = sizeof(AdapterInfo);
    if (info(adl.ctx, a.data(), (int)(sizeof(AdapterInfo) * a.size())) != 0) return;
    for (AdapterInfo &x : a)
        if (x.iVendorID == 0x1002 && same_gpu(x.strAdapterName, gpu)) { adl.index = x.iAdapterIndex; adl.ok = true; break; }
    printf("[perf] ADL: %s\n", adl.ok ? "the game's GPU found" : "no matching GPU");
    fflush(stdout);
}

void adl_sample(Machine &m) {
    if (!adl.ok) return;
    static AdlPmLog log;
    memset(&log, 0, sizeof log);
    log.size = sizeof log;
    if (adl.pmlog(adl.ctx, adl.index, &log) != 0) return;
    const AdlSensor *v = log.sensors;
    if (m.gpu < 0 && v[PMLOG_INFO_ACTIVITY_GFX].supported) m.gpu = (float)v[PMLOG_INFO_ACTIVITY_GFX].value;
    if (v[PMLOG_TEMPERATURE_EDGE].supported) m.gpu_temp = (float)v[PMLOG_TEMPERATURE_EDGE].value;
    else if (v[PMLOG_TEMPERATURE_HOTSPOT].supported) m.gpu_temp = (float)v[PMLOG_TEMPERATURE_HOTSPOT].value;
    if (v[PMLOG_BOARD_POWER].supported) m.gpu_power = (float)v[PMLOG_BOARD_POWER].value;
    else if (v[PMLOG_ASIC_POWER].supported) m.gpu_power = (float)v[PMLOG_ASIC_POWER].value;
    if (v[PMLOG_CLK_GFXCLK].supported) m.gpu_clock = (float)v[PMLOG_CLK_GFXCLK].value;
}

// GPU load from Windows' counters (any vendor): per engine, the sum over all processes; the busiest engine of the
// adapter the game renders on, as Task Manager shows it. The adapter is Vulkan's (its LUID): on a laptop with two GPUs
// the game also has counters on the other one (for its window), which is mostly idle. Without the LUID: the adapter
// where this process is busiest.
struct {
    bool tried, ok;
    PDH_HQUERY q;
    PDH_HCOUNTER c;
    PDH_STATUS(WINAPI *open)(LPCWSTR, DWORD_PTR, PDH_HQUERY *);
    PDH_STATUS(WINAPI *add)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER *);
    PDH_STATUS(WINAPI *collect)(PDH_HQUERY);
    PDH_STATUS(WINAPI *get)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD, PPDH_FMT_COUNTERVALUE_ITEM_W);
} pdh;

void pdh_open(void) {
    pdh.tried = true;
    HMODULE l = LoadLibraryExW(L"pdh.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!l) return;
    pdh.open = (decltype(pdh.open))(void *)GetProcAddress(l, "PdhOpenQueryW");
    pdh.add = (decltype(pdh.add))(void *)GetProcAddress(l, "PdhAddEnglishCounterW");
    pdh.collect = (decltype(pdh.collect))(void *)GetProcAddress(l, "PdhCollectQueryData");
    pdh.get = (decltype(pdh.get))(void *)GetProcAddress(l, "PdhGetFormattedCounterArrayW");
    if (!pdh.open || !pdh.add || !pdh.collect || !pdh.get || pdh.open(NULL, 0, &pdh.q) != ERROR_SUCCESS) return;
    if (pdh.add(pdh.q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &pdh.c) != ERROR_SUCCESS) return;
    pdh.collect(pdh.q);
    pdh.ok = true;
}

void pdh_sample(Machine &m) {
    if (!pdh.ok || pdh.collect(pdh.q) != ERROR_SUCCESS) return;
    DWORD bytes = 0, n = 0;
    if (pdh.get(pdh.c, PDH_FMT_DOUBLE, &bytes, &n, NULL) != (PDH_STATUS)PDH_MORE_DATA || !bytes) return;
    std::vector<unsigned char> buf(bytes);
    auto *it = (PPDH_FMT_COUNTERVALUE_ITEM_W)buf.data();
    if (pdh.get(pdh.c, PDH_FMT_DOUBLE, &bytes, &n, it) != ERROR_SUCCESS) return;
    // instance names: pid_<pid>_luid_0x<hi>_0x<lo>_phys_<n>_eng_<n>_engtype_<type>
    auto parse = [](const wchar_t *l, uint64_t *id) {  // l: at "_luid_0x"
        wchar_t *e;
        uint64_t hi = wcstoul(l + 8, &e, 16);
        if (wcsncmp(e, L"_0x", 3)) return false;
        *id = hi << 32 | wcstoul(e + 3, &e, 16);
        return true;
    };
    static int have_vk = -1;
    static uint64_t vk_id;
    if (have_vk < 0) {
        uint32_t hi, lo;
        have_vk = vk_perf_luid(&hi, &lo);
        vk_id = (uint64_t)hi << 32 | lo;
    }
    uint64_t want = 0;
    bool found_want = false;
    if (have_vk) { want = vk_id; found_want = true; }
    else {  // the adapter with the most of this process's load
        wchar_t mine[32];
        swprintf(mine, 32, L"pid_%lu_", GetCurrentProcessId());
        size_t ml = wcslen(mine);
        std::vector<std::pair<uint64_t, double>> mine_by;
        for (DWORD i = 0; i < n; i++) {
            const wchar_t *l = wcsstr(it[i].szName, L"_luid_0x");
            uint64_t id;
            if (!l || wcsncmp(it[i].szName, mine, ml) || !parse(l, &id) || it[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
            bool f = false;
            for (auto &x : mine_by) if (x.first == id) { x.second += it[i].FmtValue.doubleValue; f = true; break; }
            if (!f) mine_by.push_back({id, it[i].FmtValue.doubleValue});
        }
        double most = -1;
        for (auto &x : mine_by) if (x.second > most) { most = x.second; want = x.first; found_want = true; }
    }
    static bool logged;
    if (!logged) {
        logged = true;
        printf("[perf] GPU load: adapter %016llx (%s)\n", (unsigned long long)want, have_vk ? "the Vulkan device's" : found_want ? "busiest for the game" : "none");
        fflush(stdout);
    }
    if (!found_want) return;
    std::vector<std::pair<std::wstring, double>> eng;  // engine (the name from _luid_ on) -> sum over the processes
    for (DWORD i = 0; i < n; i++) {
        const wchar_t *l = wcsstr(it[i].szName, L"_luid_0x");
        uint64_t id;
        if (!l || !parse(l, &id) || id != want || it[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
        bool f = false;
        for (auto &e : eng) if (e.first == l) { e.second += it[i].FmtValue.doubleValue; f = true; break; }
        if (!f) eng.push_back({std::wstring(l), it[i].FmtValue.doubleValue});
    }
    double best = 0;
    for (auto &e : eng) best = std::max(best, e.second);
    m.gpu = (float)std::min(best, 100.0);
}

ULONGLONG u64(FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

DWORD WINAPI sampler(void *) {
    std::string gpu = vk_perf_gpu_name();
    ULONGLONG idle0 = 0, kern0 = 0, user0 = 0, proc0 = 0, wall0 = 0;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    while (fps_mode() == 2) {
        Machine m;
        if (!pdh.tried) pdh_open();
        if (!nv.tried) nvml_open(gpu.c_str());
        if (!nv.ok && !adl.tried) adl_open(gpu.c_str());
        pdh_sample(m);
        nvml_sample(m);
        adl_sample(m);
        FILETIME fi, fk, fu, c, e, pk, pu;
        ULONGLONG wall = GetTickCount64() * 10000ull;  // (100 ns units, as FILETIME)
        if (GetSystemTimes(&fi, &fk, &fu) && GetProcessTimes(GetCurrentProcess(), &c, &e, &pk, &pu)) {
            ULONGLONG idle = u64(fi), kern = u64(fk), user = u64(fu), proc = u64(pk) + u64(pu);
            if (wall0) {
                ULONGLONG all = (kern - kern0) + (user - user0), id = idle - idle0;  // (kernel time includes idle)
                if (all > 0 && all >= id) m.cpu = 100.0f * (float)(all - id) / (float)all;
                if (wall > wall0) m.cpu_game = 100.0f * (float)(proc - proc0) / (float)((wall - wall0) * si.dwNumberOfProcessors);
            }
            idle0 = idle; kern0 = kern; user0 = user; proc0 = proc; wall0 = wall;
        }
        MEMORYSTATUSEX ms;
        ms.dwLength = sizeof ms;
        if (GlobalMemoryStatusEx(&ms)) { m.ram_total = (double)ms.ullTotalPhys; m.ram_used = (double)(ms.ullTotalPhys - ms.ullAvailPhys); }
        PROCESS_MEMORY_COUNTERS pmc;
        pmc.cb = sizeof pmc;
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) m.ram_game = (double)pmc.WorkingSetSize;
        AcquireSRWLockExclusive(&mach_lock);
        mach = m;
        ReleaseSRWLockExclusive(&mach_lock);
        Sleep(500);
    }
    InterlockedExchange(&sampler_on, 0);
    return 0;
}

void read_cpu_name(void) {
    HKEY k;
    DWORD n = sizeof cpu_name - 1;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExA(k, "ProcessorNameString", NULL, NULL, (BYTE *)cpu_name, &n) != ERROR_SUCCESS) cpu_name[0] = 0;
        RegCloseKey(k);
    }
    cpu_name[sizeof cpu_name - 1] = 0;
    char *p = cpu_name;  // (some names start or end with spaces)
    while (*p == ' ') p++;
    memmove(cpu_name, p, strlen(p) + 1);
    for (size_t l = strlen(cpu_name); l && cpu_name[l - 1] == ' ';) cpu_name[--l] = 0;
}

bool fps_due(void) {
    int mode = fps_mode();
    vk_perf_detail = mode == 2;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!fps_freq.QuadPart) QueryPerformanceFrequency(&fps_freq);
    if (!mode) { fps_t0.QuadPart = 0; fps_text[0] = 0; frames_reset(); return false; }
    if (mode == 2) {
        frames_add(now);
        if (!sampler_on && !InterlockedExchange(&sampler_on, 1)) {
            if (!cpu_name[0]) read_cpu_name();
            HANDLE t = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
            if (t) { SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t); } else InterlockedExchange(&sampler_on, 0);
        }
    } else frames_reset();
    if (!fps_t0.QuadPart) { fps_t0 = now; fps_frames = 0; return fps_text[0] != 0 || mode == 2; }
    fps_frames++;
    double dt = (double)(now.QuadPart - fps_t0.QuadPart) / (double)fps_freq.QuadPart;
    if (dt >= 0.5) {
        snprintf(fps_text, sizeof fps_text, "%.0f fps  %.1f ms", fps_frames / dt, 1000.0 * dt / fps_frames);
        fps_t0 = now;
        fps_frames = 0;
    }
    return fps_text[0] != 0 || mode == 2;
}

void draw_fps_simple(float h) {
    float size = h * 0.028f;
    if (size < 14) size = 14;
    ImFont *f = font ? font : ImGui::GetFont();
    ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, fps_text);
    float m = size * 0.5f;
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(m * 0.5f, m * 0.5f), ImVec2(m * 1.5f + ts.x, m * 1.5f + ts.y), ImGui::GetColorU32(ImVec4(0, 0, 0, 0.45f)), size * 0.2f);
    dl->AddText(f, size, ImVec2(m, m), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.95f)), fps_text);
}

// The detailed panel: a window of its own in the top left corner (no input, no focus), its text sized for the window.
void draw_fps_detailed(float h) {
    float size = h * 0.02f;
    if (size < 13) size = 13;
    ImGuiIO &io = ImGui::GetIO();
    ImFont *f = font ? font : ImGui::GetFont();
    float pad = size * 0.5f;
    ImGui::SetNextWindowPos(ImVec2(pad, pad), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(size * 0.7f, size * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(size * 0.5f, size * 0.2f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, size * 0.3f);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(size * 0.4f, size * 0.05f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##perf", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::SetWindowFontScale(size / (f->FontSize * io.FontGlobalScale));
    Machine m;
    AcquireSRWLockShared(&mach_lock);
    m = mach;
    ReleaseSRWLockShared(&mach_lock);
    const ImVec4 green(0.55f, 0.85f, 0.35f, 1), blue(0.4f, 0.7f, 1, 1), pink(0.95f, 0.5f, 0.75f, 1), gold(0.95f, 0.8f, 0.4f, 1),
        lav(0.7f, 0.72f, 1, 1), white(1, 1, 1, 0.95f), grey(1, 1, 1, 0.55f);
    char a[48], b[48], c[48];
    auto num = [](char *o, const char *fmt, float v) { if (v < 0) snprintf(o, 48, "-"); else snprintf(o, 48, fmt, v); };
    auto row4 = [](const ImVec4 &col, const char *label, const char *x, const char *y, const char *z) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextColored(col, "%s", label);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(x);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(y);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(z);
    };
    if (ImGui::BeginTable("##m", 4, ImGuiTableFlags_SizingFixedFit)) {
        num(a, "%.0f%%", m.gpu); num(b, "%.0f \xC2\xB0" "C", m.gpu_temp); num(c, "%.0f W", m.gpu_power);
        row4(green, "GPU", a, m.gpu_temp < 0 ? "" : b, m.gpu_power < 0 ? "" : c);
        snprintf(a, 48, "%.1f GB", sum.vram_used / 1073741824.0);
        snprintf(b, 48, "/ %.1f GB", sum.vram_size / 1073741824.0);
        num(c, "%.0f MHz", m.gpu_clock);
        row4(green, "VRAM", a, b, m.gpu_clock < 0 ? "" : c);
        num(a, "%.0f%%", m.cpu); num(c, "game %.0f%%", m.cpu_game);
        row4(blue, "CPU", a, "", c);
        snprintf(a, 48, "%.1f GB", m.ram_used / 1073741824.0);
        snprintf(b, 48, "/ %.1f GB", m.ram_total / 1073741824.0);
        snprintf(c, 48, "game %.1f GB", m.ram_game / 1073741824.0);
        row4(pink, "RAM", a, b, c);
        snprintf(a, 48, "%.0f", sum.fps); num(b, "1%% low %.0f", sum.low1); num(c, "0.1%% low %.0f", sum.low01);
        row4(white, "FPS", a, b, c);
        snprintf(a, 48, "%.1f ms", sum.avg_ms); snprintf(b, 48, "max %.1f ms", sum.max_ms);
        row4(white, "Frame", a, b, "");
        ImGui::EndTable();
    }
    {   // the frame times of the last 4 s, 0-50 ms
        static float g[RING];
        int n = 0;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        for (int i = ring_n - 1; i >= 0; i--) {
            int k = (ring_pos - 1 - i + RING) % RING;
            if ((double)(now.QuadPart - ring_at[k]) / (double)fps_freq.QuadPart <= 4) g[n++] = ring_ms[k];
        }
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.25f, 0.45f, 0.6f));
        ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(1, 1, 1, 0.85f));
        ImGui::PlotLines("##ft", g, n, 0, nullptr, 0, 50, ImVec2(size * 20, size * 2.6f));
        ImGui::PopStyleColor(2);
    }
    if (ImGui::BeginTable("##r", 2, ImGuiTableFlags_SizingFixedFit)) {
        auto row = [](const ImVec4 &col, const char *label, const char *v) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextColored(col, "%s", label);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(v);
        };
        snprintf(a, 48, "%.2f ms", sum.gpu_ms);
        row(gold, "GPU busy per frame", a);
        snprintf(a, 48, "%.2f ms", sum.wait_gpu_ms);
        row(gold, "Waiting for the GPU", a);
        snprintf(a, 48, "%.2f ms", sum.wait_shader_ms);
        row(gold, "Waiting for shaders", a);
        snprintf(a, 48, "%dx%d", sum.win_w, sum.win_h);
        row(lav, "Window", a);
        snprintf(a, 48, "%dx%d", sum.img_w, sum.img_h);
        row(lav, "Game image", a);
        if (sum.scene_w > 0 && sum.img_w > 0) snprintf(a, 48, "%dx%d, %.0f%%", sum.scene_w, sum.scene_h, 100.0 * sum.scene_w / sum.img_w);
        else snprintf(a, 48, "-");
        row(lav, "3D scene", a);
        int unc = ac_uncapped;
        if (unc < 0) unc = env("AC_UNCAPPED") == "1";
        row(lav, "Frame limit", unc ? "uncapped (cutscenes 60)" : "60 FPS");
        row(lav, "Presentation", vk_perf_present_mode());
        ImGui::EndTable();
    }
    ImGui::TextColored(grey, "%s", vk_perf_gpu_name());
    if (cpu_name[0]) ImGui::TextColored(grey, "%s", cpu_name);
    ImGui::End();
    ImGui::PopStyleVar(5);
}

void draw_fps(float w, float h) {
    (void)w;
    if (fps_mode() == 2) draw_fps_detailed(h);
    else draw_fps_simple(h);
}

}  // namespace

extern "C" void menu_render(void *cmd, uint32_t w, uint32_t h) {
    if (!layout_loaded) load_layout();
    bool open = ac_menu_open != 0;
    ULONGLONG tick = GetTickCount64();
    bool credit = credit_due(tick);
    bool fps = fps_due();
    bool hint = !open && hint_due(tick);
    if (!open) {
        if (was_open || ac_menu_head != ac_menu_tail) {  // (closed: drop what came in, let go of keys)
            InterlockedExchange(&ac_menu_tail, ac_menu_head);
            if (inited) { ImGui::SetCurrentContext(ctx); ImGui::GetIO().ClearInputKeys(); pad_last = 0; }
            was_open = false;
            quit_asked = false;
            InterlockedExchange(&ac_menu_modal, 0);
        }
        if ((!credit && !fps && !hint) || failed) return;
        if (!inited) {
            if (!init()) { failed = true; return; }
            inited = true;
        }
        ImGui::SetCurrentContext(ctx);
        ImGuiIO &io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)w, (float)h);
        io.DeltaTime = 1.0f / 60;
        ImGui_ImplVulkan_NewFrame();
        ImGui::NewFrame();
        if (credit) draw_credit((float)w, (float)h, tick);
        if (fps) draw_fps((float)w, (float)h);
        if (hint) draw_hint((float)w, (float)h, tick);
        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cmd);
        return;
    }
    if (failed) return;
    if (!inited) {
        if (!init()) { failed = true; return; }
        inited = true;
    }
    if (!was_open)  // (fullscreen may have changed with F11)
        for (Opt &o : opts) if (o.key == std::string("fullscreen")) o.value = ac_fullscreen_state() ? "1" : "0";
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    static ULONGLONG last;
    ULONGLONG now = GetTickCount64();
    io.DeltaTime = last && now > last ? (float)(now - last) / 1000.0f : 1.0f / 60;
    if (io.DeltaTime > 0.25f) io.DeltaTime = 0.25f;
    last = now;
    drain(io);
    float scale = (float)h / 1080.0f;
    if (scale < 0.5f) scale = 0.5f;
    io.FontGlobalScale = scale;
    // (each time from the unscaled style: ScaleAllSizes rounds down, and scaling the scaled sizes again loses a pixel)
    static float styled = 1;
    static ImGuiStyle base_style = ImGui::GetStyle();
    if (styled != scale) { ImGui::GetStyle() = base_style; ImGui::GetStyle().ScaleAllSizes(scale); styled = scale; }
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    static long page_shown = -1;
    long page = ac_menu_page;
    if (page != page_shown) { was_open = false; page_shown = page; }  // (the other page: its window gets the focus)
    if (page == 1) { quit_asked = false; InterlockedExchange(&ac_menu_modal, 0); draw_hud(scale); }
    else draw(scale);
    if (fps) draw_fps((float)w, (float)h);
    was_open = true;
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cmd);
}
