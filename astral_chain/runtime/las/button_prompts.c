/* Keyboard and mouse button prompts. The game draws every button prompt (in text, on the HUD, in menus) from one
 * 256x512 RGBA8 atlas in ui_core (core.wtp, texture 0): white shapes on transparency with the letter cut out, tinted by
 * the game. When that texture is uploaded, each Switch button icon is replaced by the key or mouse button it is bound
 * to (las/input_bindings.h, the launcher's Controls), drawn in the same style inside the same footprint.
 *
 * Hooks of textures.c (texpatch.h): texpatch_identify finds the atlas by its contents, texpatch_apply repaints its
 * level 0 before upload. AC_PROMPTS=switch keeps the original icons. */
#include <windows.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvn/nvn.h"
#include "nvn/tegra.h"
#include "nvn/vulkan/bc7_cache.h"
#include "input_bindings.h"
#include "texpatch.h"

#define ATLAS_HASH 0x4afe0dd389f44dc9ull
#define ATLAS_W 256
#define ATLAS_H 512
#define SS 4  /* supersampling */

enum Kind { K_KEY, K_LSTICK, K_RSTICK, K_DPAD };
enum { D_UP = 1, D_DOWN = 2, D_LEFT = 4, D_RIGHT = 8, D_HORZ = 12, D_VERT = 3, D_ALL = 15, D_ROTATE = 16 };
typedef struct { int x0, y0, x1, y1; int kind, arg; } Slot;  /* bounding box of the original icon (inclusive) */
static const Slot slots[] = {
    {9, 8, 48, 32, K_RSTICK, D_HORZ},     {69, 7, 108, 32, K_LSTICK, D_HORZ},   {129, 7, 163, 32, K_LSTICK, D_UP},
    {185, 7, 217, 32, K_LSTICK, D_LEFT},  {4, 52, 52, 76, K_RSTICK, D_ROTATE},  {69, 51, 103, 76, K_RSTICK, D_VERT},
    {125, 51, 159, 76, K_LSTICK, D_DOWN}, {182, 51, 214, 76, K_LSTICK, D_RIGHT}, {9, 95, 43, 120, K_LSTICK, D_VERT},
    {69, 97, 90, 118, K_KEY, IB_MINUS},   {125, 97, 146, 118, K_KEY, IB_B},     {181, 97, 202, 118, K_KEY, IB_A},
    {224, 93, 243, 120, K_KEY, IB_LSTICK}, {224, 137, 243, 164, K_KEY, IB_RSTICK},
    {9, 141, 30, 162, K_KEY, IB_Y},       {69, 141, 90, 162, K_KEY, IB_X},      {124, 142, 147, 161, K_KEY, IB_R},
    {180, 140, 203, 163, K_KEY, IB_ZL},   {8, 184, 31, 207, K_KEY, IB_ZR},      {69, 185, 90, 206, K_KEY, IB_PLUS},
    {124, 184, 147, 208, K_LSTICK, D_ALL}, {8, 228, 31, 252, K_RSTICK, D_ALL},  {68, 230, 91, 249, K_KEY, IB_L},
    {125, 229, 146, 250, K_DPAD, D_ALL},  {181, 229, 202, 250, K_DPAD, D_UP},
    {9, 273, 30, 294, K_DPAD, D_DOWN},    {69, 273, 90, 294, K_DPAD, D_LEFT},   {125, 273, 146, 294, K_DPAD, D_RIGHT},
    {181, 273, 202, 294, K_DPAD, D_VERT}, {9, 317, 30, 338, K_DPAD, D_HORZ},    {69, 317, 90, 338, K_DPAD, D_UP},
    {125, 317, 146, 338, K_DPAD, D_DOWN}, {181, 317, 202, 338, K_DPAD, D_LEFT}, {9, 361, 30, 382, K_DPAD, D_RIGHT},
    /* the four-circle face button diagrams: top X, bottom B, left Y, right A */
    {63, 354, 96, 388, K_KEY, IB_X},      {119, 355, 152, 389, K_KEY, IB_B},    {174, 355, 208, 388, K_KEY, IB_Y},
    {3, 399, 37, 432, K_KEY, IB_A},
};

static uint8_t map[2][IB_ACTIONS];
static int mouse_look = 1;

static int enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("AC_PROMPTS");
        on = !(e && (!strcmp(e, "switch") || !strcmp(e, "0")));
        ib_load(map);
        e = getenv("AC_MOUSE_LOOK");
        mouse_look = !(e && *e == '0');
    }
    return on;
}

/* ---------------------------------------------------------------------------------------------- coverage canvas */
/* A slot is drawn at SS times its size into a coverage canvas (0 empty, 255 the white shape), then box-filtered into
 * the atlas' alpha. Labels are cut out of the shape, as in the original icons. */
typedef struct { int w, h; uint8_t *c; } Canvas;

static void fill_rrect(Canvas *cv, float x0, float y0, float x1, float y1, float r, uint8_t v) {
    for (int y = (int)floorf(y0); y < (int)ceilf(y1); y++) {
        if (y < 0 || y >= cv->h) continue;
        for (int x = (int)floorf(x0); x < (int)ceilf(x1); x++) {
            if (x < 0 || x >= cv->w) continue;
            float px = x + 0.5f, py = y + 0.5f;
            float dx = px < x0 + r ? x0 + r - px : px > x1 - r ? px - (x1 - r) : 0;
            float dy = py < y0 + r ? y0 + r - py : py > y1 - r ? py - (y1 - r) : 0;
            if (dx * dx + dy * dy <= r * r) cv->c[y * cv->w + x] = v;
        }
    }
}

static void fill_ellipse(Canvas *cv, float cx, float cy, float rx, float ry, uint8_t v) {
    for (int y = (int)(cy - ry); y <= (int)(cy + ry); y++)
        for (int x = (int)(cx - rx); x <= (int)(cx + rx); x++) {
            if (x < 0 || y < 0 || x >= cv->w || y >= cv->h) continue;
            float dx = (x + 0.5f - cx) / rx, dy = (y + 0.5f - cy) / ry;
            if (dx * dx + dy * dy <= 1) cv->c[y * cv->w + x] = v;
        }
}

/* Triangle pointing in direction d (D_UP...), centred at (cx, cy), half size s. */
static void fill_arrow(Canvas *cv, float cx, float cy, float s, int d, uint8_t v) {
    for (int y = (int)(cy - s) - 1; y <= (int)(cy + s) + 1; y++)
        for (int x = (int)(cx - s) - 1; x <= (int)(cx + s) + 1; x++) {
            if (x < 0 || y < 0 || x >= cv->w || y >= cv->h) continue;
            float u = (x + 0.5f - cx) / s, w = (y + 0.5f - cy) / s;  /* (rotate so the arrow points up) */
            float a, b;
            if (d == D_UP) { a = u; b = w; } else if (d == D_DOWN) { a = u; b = -w; }
            else if (d == D_LEFT) { a = w; b = u; } else { a = w; b = -u; }
            if (b >= -0.8f && b <= 0.6f && fabsf(a) <= (b + 0.8f) * 0.75f) cv->c[y * cv->w + x] = v;
        }
}

/* ------------------------------------------------------------------------------------------------- text (GDI) */
/* gdi32 is loaded at run time (the game's link line has no gdi32; app_window.h does the same for StretchDIBits). */
static struct {
    HDC (WINAPI *CreateCompatibleDC)(HDC);
    HBITMAP (WINAPI *CreateDIBSection)(HDC, const BITMAPINFO *, UINT, void **, HANDLE, DWORD);
    HGDIOBJ (WINAPI *SelectObject)(HDC, HGDIOBJ);
    HFONT (WINAPI *CreateFontW)(int, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPCWSTR);
    COLORREF (WINAPI *SetTextColor)(HDC, COLORREF);
    int (WINAPI *SetBkMode)(HDC, int);
    BOOL (WINAPI *GetTextExtentPoint32W)(HDC, LPCWSTR, int, LPSIZE);
    BOOL (WINAPI *TextOutW)(HDC, int, int, LPCWSTR, int);
    BOOL (WINAPI *DeleteObject)(HGDIOBJ);
    BOOL (WINAPI *DeleteDC)(HDC);
    BOOL (WINAPI *GdiFlush)(void);
} gdi;

static bool gdi_load(void) {
    static int state;
    if (state) return state > 0;
    state = -1;
    HMODULE m = LoadLibraryExW(L"gdi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return false;
#define GDI_FN(name) if (!(*(FARPROC *)&gdi.name = GetProcAddress(m, #name))) return false
    GDI_FN(CreateCompatibleDC); GDI_FN(CreateDIBSection); GDI_FN(SelectObject); GDI_FN(CreateFontW); GDI_FN(SetTextColor);
    GDI_FN(SetBkMode);
    GDI_FN(GetTextExtentPoint32W); GDI_FN(TextOutW); GDI_FN(DeleteObject); GDI_FN(DeleteDC); GDI_FN(GdiFlush);
#undef GDI_FN
    state = 1;
    return true;
}

/* Cuts text out of the shape (coverage 0 where the glyphs are), centred in the box, as large as fits. */
static void cut_text(Canvas *cv, const wchar_t *text, float bx0, float by0, float bx1, float by1) {
    if (!gdi_load()) return;
    int n = (int)wcslen(text), bw = (int)(bx1 - bx0), bh = (int)(by1 - by0);
    if (n == 0 || bw < 4 || bh < 4) return;
    HDC dc = gdi.CreateCompatibleDC(NULL);
    if (!dc) return;
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = cv->w; bi.bmiHeader.biHeight = -cv->h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    uint32_t *px = NULL;
    HBITMAP bmp = gdi.CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    if (!bmp || !px) { gdi.DeleteDC(dc); return; }
    memset(px, 0, (size_t)cv->w * cv->h * 4);
    HGDIOBJ old_bmp = gdi.SelectObject(dc, bmp);
    /* largest font height whose text fits the box */
    int height = bh, fitted = 0;
    SIZE size = {0};
    HFONT font = NULL;
    for (; height >= 6; height = height * 9 / 10) {
        font = gdi.CreateFontW(-height, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                               NONANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (!font) break;
        HGDIOBJ old = gdi.SelectObject(dc, font);
        gdi.GetTextExtentPoint32W(dc, text, n, &size);
        gdi.SelectObject(dc, old);
        if (size.cx <= bw && size.cy <= bh * 1.45f) { fitted = 1; break; }
        gdi.DeleteObject(font);
        font = NULL;
    }
    if (fitted) {
        HGDIOBJ old = gdi.SelectObject(dc, font);
        gdi.SetBkMode(dc, TRANSPARENT);
        gdi.SetTextColor(dc, RGB(255, 255, 255));
        /* (Segoe UI's cell has room above capitals: centre on the cell, nudged up a little) */
        int tx = (int)(bx0 + (bw - size.cx) / 2.0f), ty = (int)(by0 + (bh - size.cy) / 2.0f + size.cy * 0.02f);
        gdi.TextOutW(dc, tx, ty, text, n);
        gdi.GdiFlush();
        gdi.SelectObject(dc, old);
        for (int i = 0; i < cv->w * cv->h; i++) if (px[i] & 0xFFFFFF) cv->c[i] = 0;
    }
    if (font) gdi.DeleteObject(font);
    gdi.SelectObject(dc, old_bmp);
    gdi.DeleteObject(bmp);
    gdi.DeleteDC(dc);
}

/* ------------------------------------------------------------------------------------------------------- glyphs */
static const wchar_t *key_label(unsigned vk, wchar_t buf[8]) {
    switch (vk) {
    case 0x08: return L"Bksp"; case 0x09: return L"Tab"; case 0x0D: return L"Enter"; case 0x10: return L"Shift";
    case 0x11: return L"Ctrl"; case 0x12: return L"Alt"; case 0x13: return L"Pause"; case 0x14: return L"Caps";
    case 0x1B: return L"Esc"; case 0x20: return L"Space"; case 0x21: return L"PgUp"; case 0x22: return L"PgDn";
    case 0x23: return L"End"; case 0x24: return L"Home"; case 0x2D: return L"Ins"; case 0x2E: return L"Del";
    case 0xA0: case 0xA1: return L"Shift"; case 0xA2: case 0xA3: return L"Ctrl"; case 0xA4: case 0xA5: return L"Alt";
    case 0x6A: return L"*"; case 0x6B: return L"+"; case 0x6D: return L"-"; case 0x6E: return L"."; case 0x6F: return L"/";
    }
    if (vk >= 0x70 && vk <= 0x87) { swprintf(buf, 8, L"F%u", vk - 0x6F); return buf; }
    if (vk >= 0x60 && vk <= 0x69) { swprintf(buf, 8, L"N%u", vk - 0x60); return buf; }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) { buf[0] = (wchar_t)vk; buf[1] = 0; return buf; }
    UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0xFFFF;  /* (punctuation, on this keyboard layout) */
    if (ch >= 0x21) { buf[0] = (wchar_t)ch; buf[1] = 0; return buf; }
    swprintf(buf, 8, L"%u", vk);
    return buf;
}

/* A mouse: rounded body, buttons split at the top; `which` is filled solid, the rest of the body outlined. dirs adds
 * movement arrows beside it (camera prompts): left and right on either side, up and down stacked on its right, all four
 * as a small cross on its right. */
static void draw_mouse(Canvas *cv, float x0, float y0, float x1, float y1, unsigned which, int dirs) {
    float w = x1 - x0, h = y1 - y0;
    /* width of the mouse and its arrows, in mouse heights */
    float aw = dirs == D_HORZ ? 0.66f + 2 * 0.42f : dirs == D_VERT ? 0.66f + 0.5f : dirs ? 0.66f + 0.95f : 0.66f;
    float mh = fminf(h * 0.96f, w / aw), mw = mh * 0.66f, arrow = mh * 0.17f;
    float left = x0 + (w - mh * aw) / 2 + (dirs == D_HORZ ? mh * 0.42f : 0);  /* the mouse's left edge */
    float cx = left + mw / 2, cy = y0 + h / 2;
    float mx0 = cx - mw / 2, my0 = cy - mh / 2, mx1 = cx + mw / 2, my1 = cy + mh / 2;
    float line = fmaxf(mw * 0.13f, SS * 1.3f), split = my0 + mh * 0.42f;
    fill_rrect(cv, mx0, my0, mx1, my1, mw * 0.48f, 255);
    fill_rrect(cv, mx0 + line, my0 + line, mx1 - line, my1 - line, mw * 0.48f - line, 0);  /* (outline) */
    fill_rrect(cv, mx0, split - line / 2, mx1, split + line / 2, 0, 255);                    /* button line */
    fill_rrect(cv, cx - line / 2, my0, cx + line / 2, split, 0, 255);                        /* between buttons */
    if (which == IB_VK_LBUTTON || which == IB_VK_RBUTTON) {
        float hx0 = which == IB_VK_LBUTTON ? mx0 : cx, hx1 = which == IB_VK_LBUTTON ? cx : mx1;
        for (int y = (int)my0; y < (int)split; y++)
            for (int x = (int)hx0; x < (int)hx1; x++) {
                if (x < 0 || y < 0 || x >= cv->w || y >= cv->h) continue;
                float dx = (x + 0.5f - cx) / (mw / 2), dy = (y + 0.5f - (my0 + mw * 0.48f)) / (mw * 0.48f);
                if (y + 0.5f > my0 + mw * 0.48f || dx * dx + dy * dy <= 1.0f) cv->c[y * cv->w + x] = 255;
            }
    } else if (which == IB_VK_MBUTTON || which == IB_VK_WHEEL_UP || which == IB_VK_WHEEL_DOWN) {
        fill_rrect(cv, cx - mw * 0.16f, my0 + mh * 0.10f, cx + mw * 0.16f, split - line, mw * 0.16f, 255);  /* wheel */
        if (which != IB_VK_MBUTTON) fill_arrow(cv, cx, which == IB_VK_WHEEL_UP ? my0 + mh * 0.66f : my0 + mh * 0.72f,
                                               mw * 0.24f, which == IB_VK_WHEEL_UP ? D_UP : D_DOWN, 255);
    } else if (which == IB_VK_XBUTTON1 || which == IB_VK_XBUTTON2) {  /* side buttons: a tab on the left, 4 / 5 */
        float ty = which == IB_VK_XBUTTON1 ? my0 + mh * 0.64f : my0 + mh * 0.47f;
        fill_rrect(cv, mx0 - line * 1.2f, ty, mx0 + line * 2.6f, ty + mh * 0.15f, line * 0.6f, 255);
    }
    if (dirs == D_HORZ) {
        fill_arrow(cv, mx0 - mh * 0.22f, cy, arrow, D_LEFT, 255);
        fill_arrow(cv, mx1 + mh * 0.22f, cy, arrow, D_RIGHT, 255);
    } else if (dirs == D_VERT) {
        fill_arrow(cv, mx1 + mh * 0.27f, cy - mh * 0.2f, arrow, D_UP, 255);
        fill_arrow(cv, mx1 + mh * 0.27f, cy + mh * 0.2f, arrow, D_DOWN, 255);
    } else if (dirs) {
        float ax = mx1 + mh * 0.5f, d = mh * 0.27f, a = arrow * 0.9f;
        fill_arrow(cv, ax, cy - d, a, D_UP, 255);
        fill_arrow(cv, ax, cy + d, a, D_DOWN, 255);
        fill_arrow(cv, ax - d, cy, a, D_LEFT, 255);
        fill_arrow(cv, ax + d, cy, a, D_RIGHT, 255);
    }
}

/* One key cap filling the box: the arrow keys get an arrow, others their name. */
static void draw_keycap(Canvas *cv, float x0, float y0, float x1, float y1, unsigned vk) {
    if (!vk) return;  /* (unbound: left empty) */
    if (ib_is_mouse(vk)) { draw_mouse(cv, x0, y0, x1, y1, vk, 0); return; }
    float w = x1 - x0, h = y1 - y0, r = fminf(w, h) * 0.22f;
    fill_rrect(cv, x0, y0, x1, y1, r, 255);
    int arrow = vk == 0x26 ? D_UP : vk == 0x28 ? D_DOWN : vk == 0x25 ? D_LEFT : vk == 0x27 ? D_RIGHT : 0;
    if (arrow) { fill_arrow(cv, x0 + w / 2, y0 + h / 2, fminf(w, h) * 0.3f, arrow, 0); return; }
    wchar_t buf[8];
    float pad = fminf(w, h) * 0.14f;
    cut_text(cv, key_label(vk, buf), x0 + pad, y0 + pad, x1 - pad, y1 - pad);
}

/* The binding shown for an action: the main one, else the alternative (as the README and the launcher say; a mouse
 * alternative preferred over a keyboard main one made L and R show the mouse side buttons instead of Q and E). */
static unsigned shown(int action) {
    return map[0][action] ? map[0][action] : map[1][action];
}

/* Several key caps side by side (two directions), or a four-key cluster (all directions). */
static void draw_directions(Canvas *cv, float x0, float y0, float x1, float y1, const int act[4] /* up down left right */,
                            int dirs) {
    float w = x1 - x0, h = y1 - y0;
    int single = dirs == D_UP ? 0 : dirs == D_DOWN ? 1 : dirs == D_LEFT ? 2 : dirs == D_RIGHT ? 3 : -1;
    if (single >= 0) {  /* a square cap in the middle */
        float s = fminf(w, h), cx = x0 + w / 2, cy = y0 + h / 2;
        draw_keycap(cv, cx - s / 2, cy - s / 2, cx + s / 2, cy + s / 2, shown(act[single]));
        return;
    }
    if (dirs == D_HORZ || dirs == D_VERT) {  /* two caps side by side, a small gap */
        int a = dirs == D_HORZ ? 2 : 0;
        float g = SS * 1.5f, s = fminf((w - g) / 2, h), cx = x0 + w / 2, cy = y0 + h / 2;
        draw_keycap(cv, cx - g / 2 - s, cy - s / 2, cx - g / 2, cy + s / 2, shown(act[a]));
        draw_keycap(cv, cx + g / 2, cy - s / 2, cx + g / 2 + s, cy + s / 2, shown(act[a + 1]));
        return;
    }
    /* four caps: up above, left / down / right below (labels only when they are big enough to read) */
    float g = SS * 1.0f, s = fminf((w - 2 * g) / 3, (h - g) / 2), cx = x0 + w / 2, cy = y0 + h / 2;
    float top = cy - g / 2 - s, bottom = cy + g / 2;
    bool labels = s >= SS * 9;
    float xs[3] = {cx - s * 1.5f - g, cx - s / 2, cx + s / 2 + g};
    struct { int a; float x, y; } caps[4] = {{0, xs[1], top}, {2, xs[0], bottom}, {1, xs[1], bottom}, {3, xs[2], bottom}};
    for (int i = 0; i < 4; i++) {
        if (labels) draw_keycap(cv, caps[i].x, caps[i].y, caps[i].x + s, caps[i].y + s, shown(act[caps[i].a]));
        else fill_rrect(cv, caps[i].x, caps[i].y, caps[i].x + s, caps[i].y + s, s * 0.22f, 255);
    }
}

static void draw_slot(Canvas *cv, const Slot *sl) {
    float x0 = 0, y0 = 0, x1 = (float)cv->w, y1 = (float)cv->h;
    static const int lstick[4] = {IB_LUP, IB_LDOWN, IB_LLEFT, IB_LRIGHT}, rstick[4] = {IB_RUP, IB_RDOWN, IB_RLEFT, IB_RRIGHT},
                     dpad[4] = {IB_DUP, IB_DDOWN, IB_DLEFT, IB_DRIGHT};
    switch (sl->kind) {
    case K_KEY: {
        unsigned vk = shown(sl->arg);
        if (!vk) return;
        float s = fminf(x1, y1), w = x1;
        /* (a key name longer than a letter gets a wider cap where the icon was wider) */
        if (ib_is_mouse(vk) || (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) w = s;
        float cx = x1 / 2, cy = y1 / 2;
        draw_keycap(cv, cx - w / 2, cy - s / 2, cx + w / 2, cy + s / 2, vk);
        return;
    }
    case K_RSTICK:
        if (mouse_look) { draw_mouse(cv, x0, y0, x1, y1, 0, sl->arg == D_ALL ? 0 : sl->arg); return; }
        if (sl->arg == D_ROTATE) { draw_directions(cv, x0, y0, x1, y1, rstick, D_ALL); return; }
        draw_directions(cv, x0, y0, x1, y1, rstick, sl->arg);
        return;
    case K_LSTICK: draw_directions(cv, x0, y0, x1, y1, lstick, sl->arg); return;
    case K_DPAD: draw_directions(cv, x0, y0, x1, y1, dpad, sl->arg); return;
    }
}

/* ---------------------------------------------------------------------------------------------------- the hooks */
int texpatch_identify(const void *texture) {
    const NvnTexture *t = texture;
    if (t->format != 0x25 || t->layout.width != ATLAS_W || t->layout.height != ATLAS_H || !t->addr || !enabled()) return -1;
    uint64_t total = tegra_total_size(&t->layout);
    if (total != (uint64_t)ATLAS_W * ATLAS_H * 4) return -1;
    /* (a texture of the same size that is not the atlas is asked about at every check: hash it again only when a
     *  sample of its bytes changed) */
    static struct { const void *t; uint64_t sample; } seen[16];
    const uint8_t *mem = (const uint8_t *)(uintptr_t)t->addr;
    uint64_t sample = 0;
    for (uint64_t i = 0; i < total; i += total / 16) { uint64_t v; memcpy(&v, mem + i, 8); sample = (sample ^ v) * 0x100000001B3ull; }
    unsigned k = (unsigned)(((uintptr_t)t >> 4) % 16);
    if (seen[k].t == t && seen[k].sample == sample) return -1;
    if (bc7_hash_full(mem, total) != ATLAS_HASH) { seen[k].t = t; seen[k].sample = sample; return -1; }
    printf("[prompts] button icon atlas found: keyboard and mouse prompts\n");
    return 0;
}

uint32_t texpatch_generation(void) { return 1; }

void texpatch_apply(int id, uint32_t level, uint8_t *data, uint32_t w, uint32_t h, uint32_t row) {
    if (id != 0 || level != 0 || w != ATLAS_W || h != ATLAS_H) return;
    for (unsigned i = 0; i < sizeof slots / sizeof slots[0]; i++) {
        const Slot *sl = &slots[i];
        int sw = sl->x1 - sl->x0 + 1, sh = sl->y1 - sl->y0 + 1;
        Canvas cv = {sw * SS, sh * SS, calloc((size_t)sw * sh, SS * SS)};
        if (!cv.c) return;
        draw_slot(&cv, sl);
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++) {
                unsigned sum = 0;
                for (int j = 0; j < SS; j++)
                    for (int i2 = 0; i2 < SS; i2++) sum += cv.c[(y * SS + j) * cv.w + x * SS + i2];
                uint8_t *p = data + (size_t)(sl->y0 + y) * row + (size_t)(sl->x0 + x) * 4;
                p[0] = p[1] = p[2] = 255;
                p[3] = (uint8_t)(sum / (SS * SS));
            }
        free(cv.c);
    }
}
