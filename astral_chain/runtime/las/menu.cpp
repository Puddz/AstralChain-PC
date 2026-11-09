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
#include <windows.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_vulkan.h"

extern "C" {
#include "nvn/vulkan/vk.h"

// The runtime's (live_settings.h, app_window.h)
extern volatile long ac_menu_open, ac_menu_close_request, ac_menu_quit_request, ac_menu_modal;
struct AcMenuEvent { int kind, a, b; };
extern AcMenuEvent ac_menu_events[256];
extern volatile long ac_menu_head, ac_menu_tail;
extern volatile long ac_menu_pad_buttons, ac_menu_pad_lx, ac_menu_pad_ly;
extern volatile int ac_title_credit;                     // (live_settings.h)
extern volatile int ac_fps_counter;                      // (live_settings.h)
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
    {"rumble", "Controls", "Rumble strength (%)", "0 turns rumble off.", NUMBER, "100", nullptr, 0, 200, true},
    {"mouse_look", "Controls", "Mouse moves the camera", "Click in the game to give it the mouse; hold Alt to get the cursor back.",
     CHECK, "1", nullptr, 0, 0, true},
    {"mouse_sens", "Controls", "Mouse camera sensitivity (%)", nullptr, NUMBER, "100", nullptr, 10, 500, true},
    {"mouse_invert", "Controls", "Invert mouse up and down", nullptr, CHECK, "0", nullptr, 0, 0, true},
    {"fullscreen", "Display", "Fullscreen", "Also F11 or Alt+Enter. The game starts the way you leave it.", CHECK, "0", nullptr, 0, 0, true},
    {"fps_counter", "Display", "Show the frame rate", "Frames per second and the average frame time, in the top left corner.",
     CHECK, "0", nullptr, 0, 0, true},
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
        else if (k == "mouse_look") v = env("AC_MOUSE_LOOK") == "0" ? "0" : "1";
        else if (k == "mouse_sens") { std::string e = env("AC_MOUSE_SENS"); if (!e.empty()) v = e; }
        else if (k == "mouse_invert") v = env("AC_MOUSE_INVERT") == "1" ? "1" : "0";
        else if (k == "fullscreen") v = ac_fullscreen_state() ? "1" : "0";
        else if (k == "handheld") v = env("AC_HANDHELD") == "1" ? "1" : "0";
        else if (k == "present") { std::string e = env("LARECOMP_VK_PRESENT"); if (!e.empty()) v = e; }
        else if (k == "uncapped") v = env("AC_UNCAPPED") == "1" ? "1" : "0";
        else if (k == "cutscene_fps_30") v = env("AC_CUTSCENE_FPS") == "30" ? "1" : "0";
        else if (k == "ignore_focus") { std::string e = env("AC_IGNORE_FOCUS"); v = !e.empty() && e != "0" ? "1" : "0"; }
        else if (k == "async_submit") v = env("AC_ASYNC_SUBMIT") == "1" ? "1" : "0";
        else if (k == "port_credit") v = env("AC_TITLE_CREDIT") == "0" ? "0" : "1";
        else if (k == "fps_counter") v = env("AC_FPS_COUNTER") == "1" ? "1" : "0";
        else if (k == "title_own_twin") v = env("AC_TITLE_OWN_TWIN") == "1" ? "1" : "0";
        o.value = v;
    }
}

std::string settings_path() {
    std::string p = env("AC_SETTINGS_FILE");
    if (!p.empty()) return p;
    char exe[MAX_PATH];  // build\game\round-00\trace.exe -> userdata\launcher.ini
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (!n || n >= sizeof exe) return std::string();
    std::string s(exe);
    for (int i = 0; i < 4; i++) { size_t k = s.find_last_of("\\/"); if (k == std::string::npos) return std::string(); s.resize(k); }
    std::string dir = s + "\\userdata";
    DWORD a = GetFileAttributesA(dir.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return std::string();
    return dir + "\\launcher.ini";
}

// key=value in the launcher's file: the line replaced, or added. Written to a temporary file, then moved over it.
bool save(const char *key, const std::string &value) {
    std::string path = settings_path();
    if (path.empty()) return false;
    std::vector<std::string> lines;
    if (FILE *f = fopen(path.c_str(), "rb")) {
        char buf[4096];
        while (fgets(buf, sizeof buf, f)) {
            std::string l(buf);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            lines.push_back(l);
        }
        fclose(f);
    }
    if (lines.empty()) lines.push_back("# Astral Chain PC launcher settings");
    std::string prefix = std::string(key) + "=";
    bool found = false;
    for (std::string &l : lines)
        if (l.compare(0, prefix.size(), prefix) == 0) { l = prefix + value; found = true; }
    if (!found) lines.push_back(prefix + value);
    std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    for (const std::string &l : lines) fprintf(f, "%s\r\n", l.c_str());
    bool ok = fclose(f) == 0;
    return ok && MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
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
    printf("[menu] ready (settings file: %s)\n", settings_path().empty() ? "none" : settings_path().c_str());
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

void set(Opt &o, const std::string &v) {
    if (o.value == v) return;
    o.value = v;
    if (o.live) ac_setting_apply(o.key, v.c_str());
    else restart_needed = true;
    saved_ok = save(o.key, v) && saved_ok;
}

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
            if (ImGui::SliderInt("##v", &v, o.lo, o.hi, "%d", ImGuiSliderFlags_AlwaysClamp)) set(o, std::to_string(v));
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

// Frame rate counter (off by default; AC_FPS_COUNTER=1 or the menu's option): frames presented per second and their
// average time, counted here (menu_render runs once per presented frame) over half-second windows, in the top left
// corner of the window.
LARGE_INTEGER fps_t0, fps_freq;
int fps_frames;
char fps_text[48];

bool fps_due(void) {
    if (ac_fps_counter < 0) ac_fps_counter = env("AC_FPS_COUNTER") == "1";
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!fps_freq.QuadPart) QueryPerformanceFrequency(&fps_freq);
    if (!ac_fps_counter) { fps_t0.QuadPart = 0; fps_text[0] = 0; return false; }
    if (!fps_t0.QuadPart) { fps_t0 = now; fps_frames = 0; return fps_text[0] != 0; }
    fps_frames++;
    double dt = (double)(now.QuadPart - fps_t0.QuadPart) / (double)fps_freq.QuadPart;
    if (dt >= 0.5) {
        snprintf(fps_text, sizeof fps_text, "%.0f fps  %.1f ms", fps_frames / dt, 1000.0 * dt / fps_frames);
        fps_t0 = now;
        fps_frames = 0;
    }
    return fps_text[0] != 0;
}

void draw_fps(float w, float h) {
    (void)w;
    float size = h * 0.028f;
    if (size < 14) size = 14;
    ImFont *f = font ? font : ImGui::GetFont();
    ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, fps_text);
    float m = size * 0.5f;
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(m * 0.5f, m * 0.5f), ImVec2(m * 1.5f + ts.x, m * 1.5f + ts.y), ImGui::GetColorU32(ImVec4(0, 0, 0, 0.45f)), size * 0.2f);
    dl->AddText(f, size, ImVec2(m, m), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.95f)), fps_text);
}

}  // namespace

extern "C" void menu_render(void *cmd, uint32_t w, uint32_t h) {
    bool open = ac_menu_open != 0;
    ULONGLONG tick = GetTickCount64();
    bool credit = credit_due(tick);
    bool fps = fps_due();
    if (!open) {
        if (was_open || ac_menu_head != ac_menu_tail) {  // (closed: drop what came in, let go of keys)
            InterlockedExchange(&ac_menu_tail, ac_menu_head);
            if (inited) { ImGui::SetCurrentContext(ctx); ImGui::GetIO().ClearInputKeys(); pad_last = 0; }
            was_open = false;
            quit_asked = false;
            InterlockedExchange(&ac_menu_modal, 0);
        }
        if ((!credit && !fps) || failed) return;
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
    static float styled = 1;
    if (styled != scale) { ImGui::GetStyle().ScaleAllSizes(scale / styled); styled = scale; }
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    draw(scale);
    if (fps) draw_fps((float)w, (float)h);
    was_open = true;
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cmd);
}
