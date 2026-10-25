/* USER32 and WINMM for 64-bit hosts (R3.1), on SDL3 (decided 2026-09-24: SDL3 for window, input
 * and audio). What FFXiMain uses, from the R3.0 boundary trace: one window class and one window
 * (a WS_POPUP "FFXiClass" window sized to the game's resolution), a PeekMessage/Dispatch loop with
 * MsgWaitForMultipleObjects, posted and thread messages, the keyboard state, cursors, a
 * low-level keyboard hook, and its own resources through LoadStringA.
 *
 * The window is an SDL window; SDL's events become the Win32 messages the game's window procedure
 * expects. SDL's video and event calls stay on the thread that created the window (macOS requires
 * the main thread, which is where GameStart runs FFXiMain's window code). Messages posted from
 * other guest threads go through a per-thread queue. */
#include <SDL3/SDL.h>
#include "sdl_main.h" /* window calls on the main thread (FINAL FANTASY XIV makes them on its render thread) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gthread.h"
#include "gwin.h"
#include "input.h"
#include "k32.h"
#include "kobj.h"
#include "navdraw.h"
#include "pe.h"
#include "plat.h"
#include "thunk.h"
#include "user32.h"

#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_SHOWWINDOW 0x0018
#define WM_ACTIVATEAPP 0x001C
#define WM_SETCURSOR 0x0020
#define HTCLIENT 1
#define WM_NCCREATE 0x0081
#define WM_NCDESTROY 0x0082
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCOMMAND 0x0112
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_MOUSEWHEEL 0x020A

/* --- windows and classes ------------------------------------------------------------------------ */
#define MAX_CLASSES 16
#define MAX_WINDOWS 16
#define HWND_BASE 0x00010010u
#define HWND_STRIDE 0x10u

static struct
{
    char name[64];
    uint32_t wndproc;
} g_classes[MAX_CLASSES];

typedef struct Wnd
{
    int used;
    uint32_t hwnd, wndproc, tid, style;
    int x, y, w, h, visible;
    int fullscreen; /* a full-screen D3D device is on it */
    SDL_Window* sdl;
    uint32_t exstyle, userdata; /* GWL_EXSTYLE, GWL_USERDATA (SetWindowLongA) */
    /* comctl32 subclasses (SetWindowSubclass), last added first to see a message */
    struct { uint32_t proc, id, ref; } sub[8];
    int nsub;
} Wnd;
static Wnd g_wnds[MAX_WINDOWS];
static uint32_t g_focus;
static int g_sdl_up;
static float g_ui_aspect; /* --ui-aspect: width / height of the interface's box, 0 off */

static Wnd* wnd(uint32_t hwnd)
{
    if (hwnd < HWND_BASE || (hwnd - HWND_BASE) % HWND_STRIDE)
        return NULL;
    uint32_t i = (hwnd - HWND_BASE) / HWND_STRIDE;
    return i < MAX_WINDOWS && g_wnds[i].used ? &g_wnds[i] : NULL;
}

/* The fraction of the client's width the interface keeps under --ui-aspect: 1 when it is off or the
 * window is no wider than the aspect. */
static float ui_squeeze(const Wnd* w)
{
    if (!(g_ui_aspect > 0) || w->w <= 0 || w->h <= 0)
        return 1.0f;
    float s = g_ui_aspect * (float)w->h / (float)w->w;
    return s < 1.0f ? s : 1.0f;
}

/* ... and of its height, when the window is taller than the aspect (a 16:9 interface on a 16:10
 * screen): 1 when it is off or the window is no taller. */
static float ui_squeeze_y(const Wnd* w)
{
    if (!(g_ui_aspect > 0) || w->w <= 0 || w->h <= 0)
        return 1.0f;
    float s = (float)w->w / (g_ui_aspect * (float)w->h);
    return s < 1.0f ? s : 1.0f;
}

/* The game takes one mouse position for its interface and its 3D world alike. Over the interface
 * (where d3d8.c drew it last frame: user32_ui_hit) the cursor's client x goes to the game as the x
 * the game drew there before its draws were squeezed toward the middle, so its hit tests line up;
 * over the world it goes as it is, so picking and the target arrow do. A press keeps its mode until
 * the buttons are up (dragging a window out over the world); so does a press the addon overlay took,
 * raw, as the overlay is drawn. The game's own cursor is drawn to match (user32_mouse_given). */
int (*user32_ui_hit)(float fx, float fy);
static int g_mouse_raw = 1, g_mouse_held;

/* where the game was told the cursor is, 0..1 of the client, and whether raw: newest at g_given_at.
 * Several, as the game may draw its cursor at a point given before the latest (a frame behind, the
 * mode flipping as the mouse leaves the interface). */
static struct
{
    float fx, fy;
    int raw;
} g_given[USER32_MOUSE_GIVEN];
static int g_given_at, g_given_n;

int user32_mouse_given(int i, float* fx, float* fy)
{
    if (i < 0 || i >= g_given_n)
        return -1;
    int k = (g_given_at - i + USER32_MOUSE_GIVEN) % USER32_MOUSE_GIVEN;
    *fx = g_given[k].fx, *fy = g_given[k].fy;
    return g_given[k].raw;
}

static void mouse_give(const Wnd* w, int x, int y)
{
    float fx = ((float)x + 0.5f) / (float)w->w, fy = ((float)y + 0.5f) / (float)w->h;
    if (g_given_n && g_given[g_given_at].fx == fx && g_given[g_given_at].fy == fy && g_given[g_given_at].raw == g_mouse_raw)
        return;
    g_given_at = (g_given_at + 1) % USER32_MOUSE_GIVEN;
    g_given[g_given_at].fx = fx, g_given[g_given_at].fy = fy, g_given[g_given_at].raw = g_mouse_raw;
    g_given_n += g_given_n < USER32_MOUSE_GIVEN;
}

/* The client point (*x, *y) made the one to give the game: unsqueezed across in a wider window, down
 * in a taller one, over the interface; as it is over the world. */
static void ui_unsqueeze(const Wnd* w, int* x, int* y)
{
    float s = ui_squeeze(w), t = ui_squeeze_y(w);
    if (s >= 1.0f && t >= 1.0f)
    {
        g_mouse_raw = 1;
        mouse_give(w, *x, *y);
        return;
    }
    if (!g_mouse_held)
        g_mouse_raw = !(user32_ui_hit && user32_ui_hit(((float)*x + 0.5f) / (float)w->w, ((float)*y + 0.5f) / (float)w->h));
    if (!g_mouse_raw && s < 1.0f)
    {
        float c = (float)w->w * 0.5f, gx = c + ((float)*x - c) / s;
        *x = gx < 0 ? 0 : gx > (float)(w->w - 1) ? w->w - 1 : (int)(gx + 0.5f);
    }
    if (!g_mouse_raw && t < 1.0f)
    {
        float c = (float)w->h * 0.5f, gy = c + ((float)*y - c) / t;
        *y = gy < 0 ? 0 : gy > (float)(w->h - 1) ? w->h - 1 : (int)(gy + 0.5f);
    }
    mouse_give(w, *x, *y);
}

/* The reverse, for the game putting the cursor somewhere (SetCursorPos: its menus put it on the item
 * they select, the log window on itself): its point is one of the interface the game drew there,
 * which is now squeezed toward the middle. */
static void ui_squeeze_point(const Wnd* w, int* x, int* y)
{
    float s = ui_squeeze(w), t = ui_squeeze_y(w);
    if (s >= 1.0f && t >= 1.0f)
        return;
    float cx = (float)w->w * 0.5f, cy = (float)w->h * 0.5f;
    *x = (int)(cx + ((float)*x - cx) * s + 0.5f);
    *y = (int)(cy + ((float)*y - cy) * t + 0.5f);
    if (!g_mouse_held)
        g_mouse_raw = 0;
}

static Wnd* wnd_of_sdl(SDL_WindowID id)
{
    for (int i = 0; i < MAX_WINDOWS; ++i)
        if (g_wnds[i].used && g_wnds[i].sdl && SDL_GetWindowID(g_wnds[i].sdl) == id)
            return &g_wnds[i];
    return NULL;
}

static SDL_Window* g_adopt;

void user32_adopt_window(void* sdl_window)
{
    g_adopt = sdl_window;
}

static void sdl_up(void)
{
    if (g_sdl_up)
        return;
    /* full screen in place, as on Windows, unless the sign-in screen chose a macOS Space already
     * (it starts SDL first, with the player's choice) */
    if (!SDL_GetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES))
        SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD))
        rt_log("[recomp] SDL_Init: %s\n", SDL_GetError());
    g_sdl_up = 1;
}

static int show_window(Wnd* w, int show);

/* the window procedure, called as the guest would: stdcall (hwnd, msg, wParam, lParam) */
/* the subclass a DefSubclassProc on this thread passes on from: its window and place in the chain */
static _Thread_local uint32_t t_sub_hwnd;
static _Thread_local int t_sub_level;

/* subclass i of w (and from it down the chain) sees a message: below 0 the window procedure itself */
static uint32_t call_subclass(Wnd* w, int i, uint32_t msg, uint32_t wp, uint32_t lp)
{
    if (i >= w->nsub)
        i = w->nsub - 1;
    if (i < 0)
    {
        uint32_t a[4] = { w->hwnd, msg, wp, lp };
        return guest_call(w->wndproc, 4, a);
    }
    uint32_t a[6] = { w->hwnd, msg, wp, lp, w->sub[i].id, w->sub[i].ref };
    uint32_t sh = t_sub_hwnd;
    int sl = t_sub_level;
    t_sub_hwnd = w->hwnd;
    t_sub_level = i;
    uint32_t r = guest_call(w->sub[i].proc, 6, a);
    t_sub_hwnd = sh;
    t_sub_level = sl;
    return r;
}

/* the window procedure, called as the guest would: stdcall (hwnd, msg, wParam, lParam), through
 * its subclasses first */
static uint32_t call_wndproc(Wnd* w, uint32_t msg, uint32_t wp, uint32_t lp)
{
    return call_subclass(w, w->nsub - 1, msg, wp, lp);
}

int user32_set_subclass(uint32_t hwnd, uint32_t proc, uint32_t id, uint32_t ref)
{
    Wnd* w = wnd(hwnd);
    if (!w || !proc)
        return 0;
    for (int i = 0; i < w->nsub; i++)
        if (w->sub[i].proc == proc && w->sub[i].id == id)
        {
            w->sub[i].ref = ref;
            return 1;
        }
    if (w->nsub == (int)(sizeof w->sub / sizeof w->sub[0]))
        return 0;
    w->sub[w->nsub].proc = proc;
    w->sub[w->nsub].id = id;
    w->sub[w->nsub].ref = ref;
    w->nsub++;
    return 1;
}

int user32_remove_subclass(uint32_t hwnd, uint32_t proc, uint32_t id)
{
    Wnd* w = wnd(hwnd);
    if (!w)
        return 0;
    for (int i = 0; i < w->nsub; i++)
        if (w->sub[i].proc == proc && w->sub[i].id == id)
        {
            memmove(&w->sub[i], &w->sub[i + 1], (size_t)(w->nsub - i - 1) * sizeof w->sub[0]);
            w->nsub--;
            if (t_sub_hwnd == hwnd && t_sub_level >= i && t_sub_level > 0)
                t_sub_level--;
            return 1;
        }
    return 0;
}

uint32_t user32_def_subclass(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    Wnd* w = wnd(hwnd);
    if (!w)
        return 0;
    /* outside any subclass of this window (a call from elsewhere) the window procedure answers */
    int i = t_sub_hwnd == hwnd ? t_sub_level - 1 : -1;
    return call_subclass(w, i, msg, wp, lp);
}

/* --- message queues -------------------------------------------------------------------------------- */
typedef struct Msg
{
    uint32_t hwnd, message, wparam, lparam, time;
    int32_t x, y;
} Msg;

#define QUEUE_LEN 1024
typedef struct Queue
{
    uint32_t tid;
    Msg m[QUEUE_LEN];
    unsigned head, count;
} Queue;
static Queue g_queues[16];
static volatile uint32_t g_qlock;

static void qlock(void) { while (plat_atomic_cas32(&g_qlock, 0, 1) != 0) plat_yield(); }
static void qunlock(void) { plat_atomic_cas32(&g_qlock, 1, 0); }

static Queue* queue_of(uint32_t tid)
{
    for (int i = 0; i < 16; ++i)
        if (g_queues[i].tid == tid)
            return &g_queues[i];
    for (int i = 0; i < 16; ++i)
        if (!g_queues[i].tid)
        {
            g_queues[i].tid = tid;
            return &g_queues[i];
        }
    return NULL;
}

static int post(uint32_t tid, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    qlock();
    Queue* q = queue_of(tid);
    if (!q || q->count == QUEUE_LEN)
    {
        qunlock();
        return 0;
    }
    Msg* m = &q->m[(q->head + q->count++) % QUEUE_LEN];
    m->hwnd = hwnd;
    m->message = msg;
    m->wparam = wp;
    m->lparam = lp;
    m->time = (uint32_t)(rt_monotonic_ns() / 1000000u);
    m->x = m->y = 0;
    qunlock();
    k_poke(); /* wakes MsgWaitForMultipleObjects */
    return 1;
}

/* takes the first message for hwnd (0 = any) in [lo, hi] (0,0 = any) */
static int take(uint32_t tid, uint32_t hwnd, uint32_t lo, uint32_t hi, int remove, Msg* out)
{
    qlock();
    Queue* q = queue_of(tid);
    for (unsigned k = 0; q && k < q->count; ++k)
    {
        Msg* m = &q->m[(q->head + k) % QUEUE_LEN];
        if ((hwnd && m->hwnd != hwnd) || ((lo || hi) && (m->message < lo || m->message > hi)))
            continue;
        *out = *m;
        if (remove)
        {
            for (unsigned j = k; j + 1 < q->count; ++j)
                q->m[(q->head + j) % QUEUE_LEN] = q->m[(q->head + j + 1) % QUEUE_LEN];
            q->count--;
        }
        qunlock();
        return 1;
    }
    qunlock();
    return 0;
}

static unsigned queued(uint32_t tid)
{
    qlock();
    Queue* q = queue_of(tid);
    unsigned n = q ? q->count : 0;
    qunlock();
    return n;
}

/* --- SDL events -> Win32 messages ------------------------------------------------------------------- */
static uint8_t g_keys[256]; /* VK -> 0x80 pressed */

static uint32_t vk_of(SDL_Scancode sc)
{
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
        return 'A' + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
        return '1' + (sc - SDL_SCANCODE_1);
    if (sc == SDL_SCANCODE_0)
        return '0';
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12)
        return 0x70 + (sc - SDL_SCANCODE_F1);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9)
        return 0x61 + (sc - SDL_SCANCODE_KP_1);
    switch (sc)
    {
    case SDL_SCANCODE_KP_0: return 0x60;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER: return 0x0D;
    case SDL_SCANCODE_ESCAPE: return 0x1B;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_SPACE: return 0x20;
    case SDL_SCANCODE_LEFT: return 0x25;
    case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_RIGHT: return 0x27;
    case SDL_SCANCODE_DOWN: return 0x28;
    case SDL_SCANCODE_INSERT: return 0x2D;
    case SDL_SCANCODE_DELETE: return 0x2E;
    case SDL_SCANCODE_HOME: return 0x24;
    case SDL_SCANCODE_END: return 0x23;
    case SDL_SCANCODE_PAGEUP: return 0x21;
    case SDL_SCANCODE_PAGEDOWN: return 0x22;
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT: return 0x10;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:
#ifdef __APPLE__
    case SDL_SCANCODE_LGUI: /* Command is the Mac's Control: Cmd+C/V/X/A reach the game as Ctrl+ */
    case SDL_SCANCODE_RGUI:
#endif
        return 0x11;
    case SDL_SCANCODE_LALT:
    case SDL_SCANCODE_RALT: return 0x12;
    case SDL_SCANCODE_CAPSLOCK: return 0x14;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x90;
    case SDL_SCANCODE_SCROLLLOCK: return 0x91;
    case SDL_SCANCODE_PRINTSCREEN: return 0x2C;
    case SDL_SCANCODE_PAUSE: return 0x13;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x6A;
    case SDL_SCANCODE_KP_PLUS: return 0x6B;
    case SDL_SCANCODE_KP_MINUS: return 0x6D;
    case SDL_SCANCODE_KP_PERIOD: return 0x6E;
    case SDL_SCANCODE_KP_DIVIDE: return 0x6F;
    case SDL_SCANCODE_MINUS: return 0xBD;
    case SDL_SCANCODE_EQUALS: return 0xBB;
    case SDL_SCANCODE_LEFTBRACKET: return 0xDB;
    case SDL_SCANCODE_RIGHTBRACKET: return 0xDD;
    case SDL_SCANCODE_BACKSLASH: return 0xDC;
    case SDL_SCANCODE_SEMICOLON: return 0xBA;
    case SDL_SCANCODE_APOSTROPHE: return 0xDE;
    case SDL_SCANCODE_GRAVE: return 0xC0;
    case SDL_SCANCODE_COMMA: return 0xBC;
    case SDL_SCANCODE_PERIOD: return 0xBE;
    case SDL_SCANCODE_SLASH: return 0xBF;
    default: return 0;
    }
}

/* PC set-1 scan codes by virtual key (the keyboard Windows reports in lParam and MapVirtualKey);
 * 0x100 marks the extended keys (E0 prefix). */
static uint16_t scan_of_vk(uint32_t vk)
{
    static const char row1[] = "QWERTYUIOP", row2[] = "ASDFGHJKL", row3[] = "ZXCVBNM";
    for (int i = 0; row1[i]; ++i)
        if (vk == (uint32_t)row1[i])
            return (uint16_t)(0x10 + i);
    for (int i = 0; row2[i]; ++i)
        if (vk == (uint32_t)row2[i])
            return (uint16_t)(0x1E + i);
    for (int i = 0; row3[i]; ++i)
        if (vk == (uint32_t)row3[i])
            return (uint16_t)(0x2C + i);
    if (vk >= '1' && vk <= '9')
        return (uint16_t)(0x02 + vk - '1');
    if (vk >= 0x70 && vk <= 0x79) /* F1-F10 */
        return (uint16_t)(0x3B + vk - 0x70);
    switch (vk)
    {
    case '0': return 0x0B;
    case 0x1B: return 0x01;
    case 0xBD: return 0x0C;
    case 0xBB: return 0x0D;
    case 0x08: return 0x0E;
    case 0x09: return 0x0F;
    case 0xDB: return 0x1A;
    case 0xDD: return 0x1B;
    case 0x0D: return 0x1C;
    case 0x11: return 0x1D;
    case 0xBA: return 0x27;
    case 0xDE: return 0x28;
    case 0xC0: return 0x29;
    case 0x10: return 0x2A;
    case 0xDC: return 0x2B;
    case 0xBC: return 0x33;
    case 0xBE: return 0x34;
    case 0xBF: return 0x35;
    case 0x6A: return 0x37;
    case 0x12: return 0x38;
    case 0x20: return 0x39;
    case 0x14: return 0x3A;
    case 0x90: return 0x45;
    case 0x91: return 0x46;
    case 0x67: return 0x47;
    case 0x68: return 0x48;
    case 0x69: return 0x49;
    case 0x6D: return 0x4A;
    case 0x64: return 0x4B;
    case 0x65: return 0x4C;
    case 0x66: return 0x4D;
    case 0x6B: return 0x4E;
    case 0x61: return 0x4F;
    case 0x62: return 0x50;
    case 0x63: return 0x51;
    case 0x60: return 0x52;
    case 0x6E: return 0x53;
    case 0x7A: return 0x57; /* F11 */
    case 0x7B: return 0x58; /* F12 */
    case 0x24: return 0x147; /* Home */
    case 0x26: return 0x148; /* Up */
    case 0x21: return 0x149; /* Page Up */
    case 0x25: return 0x14B; /* Left */
    case 0x27: return 0x14D; /* Right */
    case 0x23: return 0x14F; /* End */
    case 0x28: return 0x150; /* Down */
    case 0x22: return 0x151; /* Page Down */
    case 0x2D: return 0x152; /* Insert */
    case 0x2E: return 0x153; /* Delete */
    case 0x6F: return 0x135; /* keypad / */
    default: return 0;
    }
}

int (*user32_event_hook)(const void* sdl_event, int window_w, int window_h);
void (*user32_quit_hook)(void);

static uint32_t mouse_keys(SDL_MouseButtonFlags b)
{
    return ((b & SDL_BUTTON_LMASK) ? 1u : 0) | ((b & SDL_BUTTON_RMASK) ? 2u : 0) | ((b & SDL_BUTTON_MMASK) ? 0x10u : 0) |
        (g_keys[0x10] ? 4u : 0) | (g_keys[0x11] ? 8u : 0);
}

/* The mouse buttons (SDL_BUTTON_MASK bits) the overlay took the press of: their release is the
 * overlay's too, and the game's window is not told they are down while they are. */
static SDL_MouseButtonFlags g_overlay_buttons;

/* Pumps SDL on the window's thread and turns its events into queued messages. */
static void pump(void)
{
    if (!g_sdl_up)
        return;
    SDL_Event e;
    static int inputlog = -1;
    if (inputlog < 0)
        inputlog = getenv("XIV_INPUTLOG") != NULL;
    while (SDL_PollEvent(&e))
    {
        if (inputlog)
            rt_log("[input] SDL event %#x window %u\n", e.type, e.window.windowID);
        if (user32_event_hook && e.type != SDL_EVENT_KEY_UP && e.type != SDL_EVENT_MOUSE_BUTTON_UP &&
            e.type != SDL_EVENT_MOUSE_MOTION)
        {
            int ww = 0, wh = 0;
            SDL_Window* sw = SDL_GetWindowFromEvent(&e);
            if (sw)
                SDL_GetWindowSize(sw, &ww, &wh);
            if (user32_event_hook(&e, ww, wh))
            {
                /* the overlay's: neither DirectInput nor the game's window sees it. A press it took
                 * keeps the mouse raw (where the overlay is drawn) until the buttons are up. */
                if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                    g_overlay_buttons |= SDL_BUTTON_MASK(e.button.button), g_mouse_held = 1, g_mouse_raw = 1;
                continue;
            }
        }
        else if (user32_event_hook)
        {
            int ww = 0, wh = 0;
            SDL_Window* sw = SDL_GetWindowFromEvent(&e);
            if (sw)
                SDL_GetWindowSize(sw, &ww, &wh);
            user32_event_hook(&e, ww, wh);
        }
        /* the release of a press the overlay took: the game never saw the press (FFXI acts on the
         * release: targeting, its menus) */
        if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && (g_overlay_buttons & SDL_BUTTON_MASK(e.button.button)))
        {
            g_overlay_buttons &= ~SDL_BUTTON_MASK(e.button.button);
            g_mouse_held = SDL_GetMouseState(NULL, NULL) != 0;
            continue;
        }
        if ((e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) &&
            navdraw_key(e.key.scancode, e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat, (e.key.mod & SDL_KMOD_SHIFT) != 0))
            continue; /* the navmesh overlay's keys (navdraw.h) */
        input_sdl_event(&e); /* DirectInput's view of the same events */
        Wnd* w = NULL;
        switch (e.type)
        {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        {
            w = wnd_of_sdl(e.key.windowID);
            uint32_t vk = vk_of(e.key.scancode);
            if (!w || !vk)
                break;
            int down = e.type == SDL_EVENT_KEY_DOWN;
            uint32_t scan = scan_of_vk(vk);
            if (e.key.scancode == SDL_SCANCODE_RCTRL || e.key.scancode == SDL_SCANCODE_RALT || e.key.scancode == SDL_SCANCODE_KP_ENTER)
                scan |= 0x100;
            if (e.key.scancode == SDL_SCANCODE_RSHIFT)
                scan = 0x36;
            /* lParam: repeat 1, scan code, extended bit 24, previous state 30, transition 31 */
            uint32_t lp = 1u | ((scan & 0xFFu) << 16) | ((scan & 0x100u) << 16) |
                (down ? (e.key.repeat ? 0x40000000u : 0) : 0xC0000000u);
            g_keys[vk] = down ? 0x80 : 0;
            uint32_t msg = vk == 0x12 || (g_keys[0x12] && vk != 0x12) ? (down ? WM_SYSKEYDOWN : WM_SYSKEYUP) : (down ? WM_KEYDOWN : WM_KEYUP);
            post(w->tid, w->hwnd, msg, vk, lp);
            /* what TranslateMessage adds that SDL's text input never carries: the control
             * characters (Backspace, Tab, Enter, Esc, Ctrl+letter), auto-repeat included */
            if (down && msg == WM_KEYDOWN)
            {
                uint32_t c = vk == 0x08 ? (g_keys[0x11] ? 0x7Fu : 0x08u)
                    : vk == 0x0D        ? (g_keys[0x11] ? 0x0Au : 0x0Du)
                    : vk == 0x09 || vk == 0x1B ? vk
                    : g_keys[0x11] && vk >= 'A' && vk <= 'Z' ? vk - 0x40u
                                                              : 0;
                if (c)
                    post(w->tid, w->hwnd, WM_CHAR, c, lp);
            }
            break;
        }
        case SDL_EVENT_TEXT_INPUT:
        {
            w = wnd_of_sdl(e.text.windowID);
            if (!w)
                break;
            /* UTF-8 -> code page 1252 characters, one WM_CHAR each */
            for (const unsigned char* p = (const unsigned char*)e.text.text; *p;)
            {
                unsigned c = *p++;
                if (c >= 0x80)
                {
                    unsigned n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1, v = c & (0x3F >> n);
                    for (unsigned k = 0; k < n && (*p & 0xC0) == 0x80; ++k)
                        v = (v << 6) | (*p++ & 0x3F);
                    c = v <= 0xFF ? v : '?';
                }
                post(w->tid, w->hwnd, WM_CHAR, c, 1);
            }
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
            w = wnd_of_sdl(e.motion.windowID);
            if (w)
            {
                int mx = (int)e.motion.x, my = (int)e.motion.y;
                ui_unsqueeze(w, &mx, &my);
                /* the game's window procedure picks the pointer here: its shape, or none */
                post(w->tid, w->hwnd, WM_SETCURSOR, w->hwnd, ((uint32_t)WM_MOUSEMOVE << 16) | HTCLIENT);
                post(w->tid, w->hwnd, WM_MOUSEMOVE, mouse_keys(e.motion.state & ~g_overlay_buttons),
                    ((uint32_t)(uint16_t)my << 16) | (uint16_t)mx);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        {
            w = wnd_of_sdl(e.button.windowID);
            if (!w)
                break;
            int down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            uint32_t msg = e.button.button == SDL_BUTTON_LEFT    ? (down ? WM_LBUTTONDOWN : WM_LBUTTONUP)
                         : e.button.button == SDL_BUTTON_RIGHT   ? (down ? WM_RBUTTONDOWN : WM_RBUTTONUP)
                         : e.button.button == SDL_BUTTON_MIDDLE ? (down ? WM_MBUTTONDOWN : WM_MBUTTONUP)
                                                                 : 0;
            if (!msg)
                break;
            int bx = (int)e.button.x, by = (int)e.button.y;
            ui_unsqueeze(w, &bx, &by);
            uint32_t lp = ((uint32_t)(uint16_t)by << 16) | (uint16_t)bx;
            g_mouse_held = SDL_GetMouseState(NULL, NULL) != 0;
            post(w->tid, w->hwnd, msg, mouse_keys(SDL_GetMouseState(NULL, NULL) & ~g_overlay_buttons), lp);
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            w = wnd_of_sdl(e.wheel.windowID);
            if (w)
                post(w->tid, w->hwnd, WM_MOUSEWHEEL, ((uint32_t)(uint16_t)(int16_t)(e.wheel.y * 120) << 16),
                    ((uint32_t)(uint16_t)(int)e.wheel.mouse_y << 16) | (uint16_t)(int)e.wheel.mouse_x);
            break;
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            /* Cmd+Q, the Dock's Quit, Ctrl+C, the window's close button: the player leaving. FFXI
             * only leaves from its own menus (its window procedure ignores WM_CLOSE), so the host
             * ends the run here; nothing is lost - the registry is saved as the game changes it. */
            rt_log("[recomp] quit: %s\n", e.type == SDL_EVENT_QUIT ? "the app was told to quit (Cmd+Q, the Dock, the system)"
                                                                 : "the window was closed");
            if (user32_quit_hook)
                user32_quit_hook();
            fflush(NULL);
            _Exit(0);
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        {
            w = wnd_of_sdl(e.window.windowID);
            if (!w)
                break;
            int on = e.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
            /* a scripted run (autoinput.c) plays into the game while another app is in front: the
             * game drops keyboard and pad input when it is not the active window, so it never
             * hears that it lost focus */
            if (!on && getenv("XIV_INPUT_SCRIPT") && *getenv("XIV_INPUT_SCRIPT"))
                break;
            post(w->tid, w->hwnd, WM_ACTIVATEAPP, (uint32_t)on, 0);
            post(w->tid, w->hwnd, WM_ACTIVATE, on ? 1u : 0u, 0);
            post(w->tid, w->hwnd, on ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
            if (on)
                g_focus = w->hwnd;
            else
            {
                memset(g_keys, 0, sizeof g_keys);
                input_release_all();
            }
            break;
        }
        case SDL_EVENT_WINDOW_MOVED:
            w = wnd_of_sdl(e.window.windowID);
            if (w)
            {
                w->x = e.window.data1;
                w->y = e.window.data2;
                post(w->tid, w->hwnd, WM_MOVE, 0, ((uint32_t)(uint16_t)w->y << 16) | (uint16_t)w->x);
            }
            break;
        default: break;
        }
    }
}

/* --- window shims ---------------------------------------------------------------------------------- */
static void sh_RegisterClassA(Guest* g)
{
    uint32_t wc = ARG(0);
    const char* name = (const char*)GUEST_PTR(rd32(wc + 36));
    for (int i = 0; i < MAX_CLASSES; ++i)
        if (!g_classes[i].name[0])
        {
            snprintf(g_classes[i].name, sizeof g_classes[i].name, "%s", name);
            g_classes[i].wndproc = rd32(wc + 4);
            RET(0xC000u + (uint32_t)i, 1);
        }
    RET(0, 1);
}

static void sh_UnregisterClassA(Guest* g)
{
    for (int i = 0; i < MAX_CLASSES; ++i)
        if (!strcmp(g_classes[i].name, ARGS(0)))
            g_classes[i].name[0] = 0;
    RET(1, 2);
}

/* Set by a back end that can cover the whole screen itself (gfx9_metal.m: the menu bar and the Dock
 * hidden, the window over all of the display; SDL's own full screen leaves the menu bar's strip). */
void (*user32_cover_screen)(SDL_Window* w, int on);

/* No window frame here: the client area is the window. */
static void sh_AdjustWindowRectEx(Guest* g) { RET(1, 4); }
static void sh_AdjustWindowRect(Guest* g) { RET(1, 3); }

/* The window's frame from its style, as Windows would draw it (macOS; Windows does its own when the
 * window is made): a WS_POPUP without a caption (FFXI's borderless modes) has none, and one covering
 * the desktop is full screen - the desktop's own mode, no mode change (SDL's fullscreen with no
 * display mode), not a macOS Space. */
static void apply_frame(Wnd* w)
{
#if defined(_WIN32)
    (void)w; /* Windows: the window is made borderless when created, and placed where the game puts it */
    return;
#else
    if (!w->sdl)
        return;
    int popup = (w->style & 0x80000000u) && (w->style & 0x00C00000u) != 0x00C00000u; /* WS_POPUP, no WS_CAPTION */
    uint32_t dw, dh, hz;
    user32_desktop_mode(&dw, &dh, &hz);
    int cover = popup && (uint32_t)w->w >= dw && (uint32_t)w->h >= dh, full = cover || w->fullscreen;
    /* a macOS Space is only for a window with a frame: full screen hides it anyway */
    int spaces = SDL_GetHintBoolean(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, false);
    SDL_SetWindowBordered(w->sdl, !popup || (full && spaces));
    SDL_SetWindowFullscreenMode(w->sdl, NULL);
    bool ok = SDL_SetWindowFullscreen(w->sdl, full);
    if (user32_cover_screen)
        user32_cover_screen(w->sdl, full);
    rt_log("[user32] window %dx%d style %08x: %s%s\n", w->w, w->h, w->style, full ? "full screen" : "windowed",
        ok ? "" : " (SDL refused)");
#endif
}

void user32_set_fullscreen(uint32_t hwnd, int on)
{
    Wnd* w = wnd(hwnd);
    if (!w)
        return;
    w->fullscreen = on != 0;
    apply_frame(w);
}

/* The window in one of the game's modes (0 full screen, 1 a window, 2 a borderless window, 3 borderless
 * over the desktop) at w x h (3: the desktop's size), as the game would have made it: its style, size
 * and frame. The game is told the new size (WM_SIZE). */
void user32_set_window(uint32_t hwnd, int mode, int width, int height)
{
    Wnd* w = wnd(hwnd);
    if (!w)
        return;
    uint32_t dw = 0, dh = 0, hz;
    user32_desktop_mode(&dw, &dh, &hz);
    w->style = (w->style & 0x10000000u) | (mode == 1 ? 0x00CF0000u : 0x80000000u); /* WS_VISIBLE kept */
    w->fullscreen = mode == 0;
    w->w = mode == 3 && dw ? (int)dw : width, w->h = mode == 3 && dh ? (int)dh : height;
    if (w->sdl)
    {
        /* out of full screen first: a full-screen window keeps its size */
        SDL_SetWindowFullscreen(w->sdl, false);
        SDL_SyncWindow(w->sdl);
        SDL_SetWindowSize(w->sdl, w->w, w->h);
        apply_frame(w);
        if (mode == 1 || mode == 2)
            SDL_SetWindowPosition(w->sdl, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        SDL_SyncWindow(w->sdl);
    }
    call_wndproc(w, WM_SIZE, 0, ((uint32_t)(uint16_t)w->h << 16) | (uint16_t)w->w);
}

/* CreateWindowExA(ex, class, title, style, x, y, w, h, parent, menu, instance, param) */
static void sh_CreateWindowExA(Guest* g)
{
    const char* cls = ARGS(1);
    uint32_t proc = 0;
    for (int i = 0; i < MAX_CLASSES; ++i)
        if (g_classes[i].name[0] && !strcmp(g_classes[i].name, cls))
            proc = g_classes[i].wndproc;
    Wnd* w = NULL;
    for (int i = 0; i < MAX_WINDOWS && !w; ++i)
        if (!g_wnds[i].used)
        {
            w = &g_wnds[i];
            memset(w, 0, sizeof *w);
            w->used = 1;
            w->hwnd = HWND_BASE + HWND_STRIDE * (uint32_t)i;
        }
    if (!proc || !w)
    {
        if (w)
            w->used = 0;
        RET(0, 12);
    }
    w->wndproc = proc;
    w->tid = gt_self()->tid;
    w->style = ARG(3);
    w->x = (int32_t)ARG(4) == (int32_t)0x80000000 ? 0 : (int32_t)ARG(4);
    w->y = (int32_t)ARG(5) == (int32_t)0x80000000 ? 0 : (int32_t)ARG(5);
    w->w = (int32_t)ARG(6) == (int32_t)0x80000000 ? 640 : (int32_t)ARG(6);
    w->h = (int32_t)ARG(7) == (int32_t)0x80000000 ? 480 : (int32_t)ARG(7);
    sdl_up();
    /* A WS_POPUP window with no caption (the game's full-screen and borderless windowed modes) has no
     * frame here either, and goes where the game puts it: at its size, borderless full screen. */
    int popup = (w->style & 0x80000000u) && !(w->style & 0x00C00000u);
    (void)popup;
    if (g_adopt && !ARG(8)) /* no parent: the game's main window */
    {
        w->sdl = g_adopt;
        g_adopt = NULL;
        SDL_SetWindowTitle(w->sdl, ARG(2) ? ARGS(2) : "");
        SDL_SetWindowSize(w->sdl, w->w, w->h);
    }
    else
    {
        SDL_WindowFlags flags = SDL_WINDOW_HIDDEN;
#if defined(_WIN32)
        if (popup)
            flags |= SDL_WINDOW_BORDERLESS;
#endif
        w->sdl = SDL_CreateWindow(ARG(2) ? ARGS(2) : "", w->w, w->h, flags);
    }
    apply_frame(w);
    if (!w->sdl)
        rt_log("[recomp] SDL_CreateWindow: %s\n", SDL_GetError());
#if defined(_WIN32)
    else if (popup)
        SDL_SetWindowPosition(w->sdl, w->x, w->y);
#endif

    /* WM_NCCREATE / WM_CREATE with a CREATESTRUCTA, as Windows sends them */
    uint32_t cs = gheap_alloc(48, 1);
    uint32_t v[12] = { ARG(11), ARG(10), ARG(9), ARG(8), (uint32_t)w->h, (uint32_t)w->w, (uint32_t)w->y, (uint32_t)w->x,
                       ARG(3), ARG(2), ARG(1), ARG(0) };
    for (int i = 0; i < 12; ++i)
        wr32(cs + 4 * (uint32_t)i, v[i]);
    uint32_t hwnd = w->hwnd;
    if (!call_wndproc(w, WM_NCCREATE, 0, cs) || call_wndproc(w, WM_CREATE, 0, cs) == 0xFFFFFFFFu)
    {
        gheap_free(cs);
        if (w->sdl)
            SDL_DestroyWindow(w->sdl);
        w->used = 0;
        RET(0, 12);
    }
    gheap_free(cs);
    call_wndproc(w, WM_SIZE, 0, ((uint32_t)(uint16_t)w->h << 16) | (uint16_t)w->w);
    call_wndproc(w, WM_MOVE, 0, ((uint32_t)(uint16_t)w->y << 16) | (uint16_t)w->x);
    if (w->style & 0x10000000u) /* WS_VISIBLE: shown and activated, as Windows does */
        show_window(w, 1);
    RET(hwnd, 12);
}

static void destroy(Wnd* w)
{
    call_wndproc(w, WM_DESTROY, 0, 0);
    call_wndproc(w, WM_NCDESTROY, 0, 0);
    if (w->sdl)
        SDL_DestroyWindow(w->sdl);
    if (g_focus == w->hwnd)
        g_focus = 0;
    w->used = 0;
}

static void sh_DestroyWindow(Guest* g)
{
    Wnd* w = wnd(ARG(0));
    if (w)
        destroy(w);
    RET(w != NULL, 1);
}

static void sh_DefWindowProcA(Guest* g)
{
    uint32_t msg = ARG(1);
    Wnd* w = wnd(ARG(0));
    switch (msg)
    {
    case WM_NCCREATE: RET(1, 4);
    case WM_ERASEBKGND: RET(1, 4);
    case WM_CLOSE:
        if (w)
            destroy(w);
        RET(0, 4);
    case WM_SYSCOMMAND:
        if ((ARG(2) & 0xFFF0u) == 0xF060u && w) /* SC_CLOSE */
            post(w->tid, w->hwnd, WM_CLOSE, 0, 0);
        RET(0, 4);
    default: RET(0, 4);
    }
}

static void sh_ShowWindow(Guest* g)
{
    Wnd* w = wnd(ARG(0));
    if (!w)
        RET(0, 2);
    RET((uint32_t)show_window(w, ARG(1) != 0), 2); /* SW_HIDE = 0 */
}

/* shows and activates (or hides) a window, with the messages Windows sends; returns whether it was visible */
static int show_window(Wnd* w, int show)
{
    int was = w->visible;
    if (show && !was)
    {
        if (w->sdl)
        {
            SDL_ShowWindow(w->sdl);
            SDL_RaiseWindow(w->sdl);
            SDL_StartTextInput(w->sdl);
        }
        call_wndproc(w, WM_SHOWWINDOW, 1, 0);
        call_wndproc(w, WM_ACTIVATEAPP, 1, 0);
        call_wndproc(w, WM_ACTIVATE, 1, 0);
        call_wndproc(w, WM_SETFOCUS, 0, 0);
        g_focus = w->hwnd;
    }
    else if (!show && was && w->sdl)
        SDL_HideWindow(w->sdl);
    w->visible = show;
    return was;
}

static void sh_UpdateWindow(Guest* g) { RET(1, 1); }

static void write_rect(uint32_t p, int l, int t, int r, int b)
{
    wr32(p, (uint32_t)l);
    wr32(p + 4, (uint32_t)t);
    wr32(p + 8, (uint32_t)r);
    wr32(p + 12, (uint32_t)b);
}

static void sh_GetWindowRect(Guest* g)
{
    Wnd* w = wnd(ARG(0));
    if (!w)
        RET(0, 2);
    write_rect(ARG(1), w->x, w->y, w->x + w->w, w->y + w->h);
    RET(1, 2);
}

static void sh_GetClientRect(Guest* g)
{
    Wnd* w = wnd(ARG(0));
    if (!w)
        RET(0, 2);
    write_rect(ARG(1), 0, 0, w->w, w->h);
    RET(1, 2);
}

static void sh_SetRect(Guest* g) { write_rect(ARG(0), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4)); RET(1, 5); }

static void sh_ClientToScreen(Guest* g)
{
    Wnd* w = wnd(ARG(0));
    if (w)
    {
        wr32(ARG(1), rd32(ARG(1)) + (uint32_t)w->x);
        wr32(ARG(1) + 4, rd32(ARG(1) + 4) + (uint32_t)w->y);
    }
    RET(w != NULL, 2);
}

static void sh_ScreenToClient(Guest* g)
{
    Wnd* w = wnd(ARG(0));
    if (w)
    {
        wr32(ARG(1), rd32(ARG(1)) - (uint32_t)w->x);
        wr32(ARG(1) + 4, rd32(ARG(1) + 4) - (uint32_t)w->y);
    }
    RET(w != NULL, 2);
}

/* The cursor's desktop position, kept by the main thread (user32_main_tick): asking SDL from a
 * guest thread is a round trip to the main thread with the guest lock held, and the game asks
 * every frame on its logic thread. */
static volatile float g_cursor_x, g_cursor_y;
static volatile int g_cursor_known;

static volatile int g_cursor_forced; /* autoinput.c: a scripted cursor, in window points */
static volatile float g_cursor_fx, g_cursor_fy;

static void cursor_apply(void);

void user32_main_tick(void)
{
    if (!g_sdl_up)
        return;
    cursor_apply();
    float x = 0, y = 0;
    SDL_GetGlobalMouseState(&x, &y); /* on the main thread: no marshalling */
    if (g_cursor_forced)
    {
        Wnd* w = wnd(g_focus);
        for (int i = 0; i < MAX_WINDOWS && !w; ++i)
            if (g_wnds[i].used && g_wnds[i].sdl)
                w = &g_wnds[i];
        if (w)
            x = (float)w->x + g_cursor_fx, y = (float)w->y + g_cursor_fy;
    }
    g_cursor_x = x, g_cursor_y = y;
    g_cursor_known = 1;
}

static void sh_GetCursorPos(Guest* g)
{
    float x = 0, y = 0;
    if (g_cursor_known)
        x = g_cursor_x, y = g_cursor_y;
    else if (g_sdl_up)
        SDL_GetGlobalMouseState(&x, &y);
    Wnd* w = wnd(g_focus);
    if (w && x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h)
    {
        int cx = (int)x - w->x, cy = (int)y - w->y;
        ui_unsqueeze(w, &cx, &cy);
        x = (float)(w->x + cx), y = (float)(w->y + cy);
    }
    wr32(ARG(0), (uint32_t)(int32_t)x);
    wr32(ARG(0) + 4, (uint32_t)(int32_t)y);
    RET(1, 1);
}

static void sh_GetSystemMetrics(Guest* g)
{
    int32_t cx = 1920, cy = 1080;
    sdl_up();
    const SDL_DisplayMode* dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    if (dm)
        cx = dm->w, cy = dm->h;
    switch (ARG(0))
    {
    case 0:  /* SM_CXSCREEN */
    case 78: /* SM_CXVIRTUALSCREEN */
    case 16: /* SM_CXFULLSCREEN */
        RET((uint32_t)cx, 1);
    case 1:
    case 79:
    case 17:
        RET((uint32_t)cy, 1);
    case 4: RET(23, 1);     /* SM_CYCAPTION */
    case 5:
    case 6: RET(1, 1);      /* SM_CXBORDER, SM_CYBORDER */
    case 7:
    case 8: RET(3, 1);      /* SM_CXFIXEDFRAME */
    case 32:
    case 33: RET(4, 1);     /* SM_CXFRAME */
    case 11:
    case 12: RET(32, 1);    /* SM_CXICON */
    case 13:
    case 14: RET(32, 1);    /* SM_CXCURSOR */
    case 15: RET(20, 1);    /* SM_CYMENU */
    case 43: RET(3, 1);     /* SM_CMOUSEBUTTONS */
    case 80: RET(1, 1);     /* SM_CMONITORS */
    default: RET(0, 1);
    }
}

static void sh_GetSystemMenu(Guest* g) { RET(0, 2); }
static void sh_InsertMenuItemA(Guest* g) { RET(0, 4); }

/* LoadStringA(instance, id, buf, n): RT_STRING block (id >> 4) + 1, entry id & 15 */
static void sh_LoadStringA(Guest* g)
{
    uint32_t id = ARG(1), buf = ARG(2);
    int32_t n = (int32_t)ARG(3);
    uint32_t size = 0, block = pe_resource(ARG(0), 6, (id >> 4) + 1, &size);
    if (!block || n <= 0)
        RET(0, 4);
    uint32_t p = block;
    for (uint32_t i = 0; i < (id & 15); ++i)
        p += 2 + 2 * rd16(p);
    uint32_t len = rd16(p), o = 0;
    for (; o < len && o + 1 < (uint32_t)n; ++o)
    {
        uint16_t c = rd16(p + 2 + 2 * o);
        wr8(buf + o, c > 0xFF ? '?' : (uint8_t)c);
    }
    wr8(buf + o, 0);
    RET(o, 4);
}

/* Icons are opaque handles; the game only hands them back. Cursors from a module's resources
 * (the game's pointer shapes, ffxivgame.exe RT_GROUP_CURSOR 130..137, which it picks by what is
 * under the mouse) become SDL colour cursors; system ones (IDC_ARROW) are the macOS arrow. The
 * game thread only records the cursor it set; the main thread puts it up (user32_main_tick). */
#define RT_CURSOR 1
#define RT_GROUP_CURSOR 12
#define HCURSOR_ARROW 0x00030001u
#define HCURSOR_FILE 0x00030002u
#define HCURSOR_BASE 0x00030100u
#define MAX_CURSORS 16

static struct
{
    uint32_t module, id;
    int w, h, hx, hy;
    uint8_t* rgba;
    SDL_Cursor* sdl;
} g_cursors[MAX_CURSORS];
static int g_ncursors;
static volatile uint32_t g_cursor = HCURSOR_ARROW;
static volatile int g_cursor_count;
static uint32_t g_cursor_up = ~0u; /* main thread: what SDL shows */
static int g_cursor_up_vis = -1;

/* An RT_CURSOR (a hotspot, then a DIB: BITMAPINFOHEADER, palette, image, AND mask, bottom-up)
 * as RGBA. Pixels the mask would invert on Windows are left transparent. */
static int cursor_decode(uint32_t p, uint32_t size, int* w, int* h, int* hx, int* hy, uint8_t** rgba)
{
    if (size < 4 + 40)
        return 0;
    *hx = rd16(p), *hy = rd16(p + 2);
    uint32_t bi = p + 4, hdr = rd32(bi), bpp = rd16(bi + 14), comp = rd32(bi + 16), used = rd32(bi + 32);
    int cw = (int)rd32(bi + 4), ch = (int)rd32(bi + 8) / 2;
    if (comp || cw <= 0 || ch <= 0 || cw > 256 || ch > 256 || (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32))
        return 0;
    uint32_t pal = bi + hdr, ncol = bpp <= 8 ? (used ? used : 1u << bpp) : 0;
    uint32_t xs = ((cw * bpp + 31) / 32) * 4, as = ((cw + 31) / 32) * 4;
    uint32_t xor = pal + 4 * ncol, and = xor + xs * ch;
    if (and + as * ch > p + size)
        return 0;
    uint8_t* out = malloc((size_t)cw * ch * 4);
    int alpha32 = 0;
    if (bpp == 32)
        for (int i = 0; i < cw * ch && !alpha32; ++i)
            alpha32 = rd8(xor + 4 * i + 3) != 0;
    for (int y = 0; y < ch; ++y)
    {
        uint32_t row = xor + xs * (ch - 1 - y), mrow = and + as * (ch - 1 - y);
        for (int x = 0; x < cw; ++x)
        {
            uint8_t* o = out + 4 * (y * cw + x);
            uint32_t c; /* BGRx */
            if (bpp == 32)
                c = rd32(row + 4 * x);
            else if (bpp == 24)
                c = rd8(row + 3 * x) | rd8(row + 3 * x + 1) << 8 | rd8(row + 3 * x + 2) << 16;
            else
            {
                uint32_t bit = x * bpp, v = (rd8(row + bit / 8) >> (8 - bpp - bit % 8)) & ((1u << bpp) - 1);
                c = v < ncol ? rd32(pal + 4 * v) : 0;
            }
            int masked = (rd8(mrow + x / 8) >> (7 - x % 8)) & 1;
            o[0] = (uint8_t)(c >> 16), o[1] = (uint8_t)(c >> 8), o[2] = (uint8_t)c;
            o[3] = alpha32 ? (uint8_t)(c >> 24) : masked ? 0 : 255;
        }
    }
    *w = cw, *h = ch, *rgba = out;
    return 1;
}

/* LoadCursorA(module, id) for a module's own cursor: its largest, deepest image */
static uint32_t cursor_load(uint32_t module, uint32_t id)
{
    for (int i = 0; i < g_ncursors; ++i)
        if (g_cursors[i].module == module && g_cursors[i].id == id)
            return HCURSOR_BASE + (uint32_t)i;
    uint32_t gsize = 0, grp = pe_resource(module, RT_GROUP_CURSOR, id, &gsize);
    if (!grp || gsize < 6 || g_ncursors == MAX_CURSORS)
        return 0;
    uint32_t n = rd16(grp + 4), best = 0, best_key = 0;
    for (uint32_t i = 0; i < n && 6 + 14 * (i + 1) <= gsize; ++i)
    {
        uint32_t e = grp + 6 + 14 * i, key = (uint32_t)rd16(e) << 16 | rd16(e + 6);
        if (!best || key > best_key)
            best = e, best_key = key;
    }
    uint32_t size = 0, cur = best ? pe_resource(module, RT_CURSOR, rd16(best + 12), &size) : 0;
    int i = g_ncursors;
    if (!cur || !cursor_decode(cur, size, &g_cursors[i].w, &g_cursors[i].h, &g_cursors[i].hx, &g_cursors[i].hy, &g_cursors[i].rgba))
        return 0;
    g_cursors[i].module = module, g_cursors[i].id = id;
    if (getenv("XIV_UI_LOG"))
        rt_log("[cursor] %u: %dx%d hotspot %d,%d\n", id, g_cursors[i].w, g_cursors[i].h, g_cursors[i].hx, g_cursors[i].hy);
    __atomic_store_n(&g_ncursors, i + 1, __ATOMIC_RELEASE);
    return HCURSOR_BASE + (uint32_t)i;
}

/* main thread: the SDL cursor of a handle, made the first time; NULL for the system arrow. A
 * second image at twice the size keeps the pixels sharp on Retina screens. */
static SDL_Cursor* cursor_sdl(uint32_t h)
{
    int i = (int)(h - HCURSOR_BASE);
    if (h < HCURSOR_BASE || i >= __atomic_load_n(&g_ncursors, __ATOMIC_ACQUIRE))
        return NULL;
    if (g_cursors[i].sdl)
        return g_cursors[i].sdl;
    int w = g_cursors[i].w, ht = g_cursors[i].h;
    SDL_Surface* s = SDL_CreateSurfaceFrom(w, ht, SDL_PIXELFORMAT_RGBA32, g_cursors[i].rgba, w * 4);
    SDL_Surface* s2 = SDL_CreateSurface(w * 2, ht * 2, SDL_PIXELFORMAT_RGBA32);
    if (!s)
        return NULL;
    if (s2)
    {
        for (int y = 0; y < ht * 2; ++y)
            for (int x = 0; x < w * 2; ++x)
                memcpy((uint8_t*)s2->pixels + y * s2->pitch + 4 * x, g_cursors[i].rgba + 4 * ((y / 2) * w + x / 2), 4);
        SDL_AddSurfaceAlternateImage(s, s2);
        SDL_DestroySurface(s2);
    }
    g_cursors[i].sdl = SDL_CreateColorCursor(s, g_cursors[i].hx, g_cursors[i].hy);
    if (!g_cursors[i].sdl)
        rt_log("[cursor] SDL_CreateColorCursor: %s\n", SDL_GetError());
    SDL_DestroySurface(s);
    return g_cursors[i].sdl;
}

/* main thread: show the cursor the game last set, or none (SetCursor(NULL), ShowCursor below 0) */
static void cursor_apply(void)
{
    uint32_t h = g_cursor;
    int vis = h && g_cursor_count >= 0;
    if (h && h != g_cursor_up)
    {
        SDL_Cursor* c = cursor_sdl(h);
        SDL_SetCursor(c ? c : SDL_GetDefaultCursor());
        g_cursor_up = h;
    }
    if (vis != g_cursor_up_vis)
    {
        if (vis)
            SDL_ShowCursor();
        else
            SDL_HideCursor();
        g_cursor_up_vis = vis;
    }
}

static void sh_LoadIconA(Guest* g) { RET(0x00020001u, 2); }

static void sh_LoadCursorA(Guest* g)
{
    uint32_t h = ARG(0) && ARG(1) < 0x10000u ? cursor_load(ARG(0), ARG(1)) : 0;
    RET(h ? h : HCURSOR_ARROW, 2);
}

static void sh_LoadCursorFromFileA(Guest* g) { RET(HCURSOR_FILE, 1); }
static void sh_SetCursor(Guest* g) { uint32_t old = g_cursor; g_cursor = ARG(0); RET(old, 1); }

static void sh_ShowCursor(Guest* g)
{
    int n = __atomic_add_fetch(&g_cursor_count, ARG(0) ? 1 : -1, __ATOMIC_RELAXED);
    RET((uint32_t)n, 1);
}

/* The low-level keyboard hook (it keeps the Windows key from leaving the game): never called here. */
static void sh_SetWindowsHookExA(Guest* g) { RET(0x00040001u, 4); }
static void sh_UnhookWindowsHookEx(Guest* g) { RET(1, 1); }
static void sh_CallNextHookEx(Guest* g) { RET(0, 4); }

static void sh_GetKeyboardState(Guest* g)
{
    memcpy(ARGP(0), g_keys, 256);
    RET(1, 1);
}

static void sh_GetKeyState(Guest* g) { RET(g_keys[ARG(0) & 0xFF] ? 0xFFFF8000u : 0, 1); }
static void sh_GetAsyncKeyState(Guest* g) { RET(g_keys[ARG(0) & 0xFF] ? 0x8000u : 0, 1); }
static void sh_GetKeyboardLayout(Guest* g) { RET(0x04090409u, 1); } /* US English */

static void sh_GetForegroundWindow(Guest* g) { RET(g_focus, 0); }
static void sh_GetFocus(Guest* g) { RET(g_focus, 0); }
static void sh_SetFocus(Guest* g) { uint32_t old = g_focus; g_focus = ARG(0); RET(old, 1); }
static void sh_SetForegroundWindow(Guest* g) { RET(1, 1); }
static void sh_IsWindow(Guest* g) { RET(wnd(ARG(0)) != NULL, 1); }
static void sh_IsIconic(Guest* g) { RET(0, 1); }
static void sh_IsWindowVisible(Guest* g) { Wnd* w = wnd(ARG(0)); RET(w && w->visible, 1); }

/* --- messages ----------------------------------------------------------------------------------------- */
static void write_msg(uint32_t p, const Msg* m)
{
    wr32(p, m->hwnd);
    wr32(p + 4, m->message);
    wr32(p + 8, m->wparam);
    wr32(p + 12, m->lparam);
    wr32(p + 16, m->time);
    wr32(p + 20, (uint32_t)m->x);
    wr32(p + 24, (uint32_t)m->y);
}

/* PeekMessageA(msg, hwnd, min, max, remove) */
static void sh_PeekMessageA(Guest* g)
{
    pump();
    Msg m;
    if (!take(gt_self()->tid, ARG(1), ARG(2), ARG(3), (ARG(4) & 1) != 0, &m))
        RET(0, 5);
    write_msg(ARG(0), &m);
    RET(1, 5);
}

/* GetMessageA(msg, hwnd, min, max): waits; 0 for WM_QUIT */
static void sh_GetMessageA(Guest* g)
{
    Msg m;
    uint32_t tid = gt_self()->tid;
    for (;;)
    {
        pump();
        if (take(tid, ARG(1), ARG(2), ARG(3), 1, &m))
            break;
        gt_unlock();
        plat_sleep_ms(5);
        gt_lock();
    }
    write_msg(ARG(0), &m);
    RET(m.message != WM_QUIT, 4);
}

int user32_post(uint32_t tid, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) { return post(tid, hwnd, msg, wp, lp); }

int user32_take(uint32_t tid, uint32_t hwnd, uint32_t lo, uint32_t hi, int remove, uint32_t msg_out)
{
    Msg m;
    if (!take(tid, hwnd, lo, hi, remove, &m))
        return 0;
    write_msg(msg_out, &m);
    return 1;
}

void user32_pump(void) { pump(); }

/* WM_CHAR comes from SDL's text input, and the control characters from pump() */
static void sh_TranslateMessage(Guest* g) { RET(0, 1); }

static void sh_DispatchMessageA(Guest* g)
{
    uint32_t p = ARG(0);
    Wnd* w = wnd(rd32(p));
    if (getenv("XIV_INPUTLOG") && rd32(p + 4) >= 0x100 && rd32(p + 4) < 0x300 && rd32(p + 4) != 0x200)
        rt_log("[input] dispatch hwnd %08x msg %#x wp %x lp %x (window %s, focus %08x)\n", rd32(p), rd32(p + 4), rd32(p + 8),
            rd32(p + 12), w ? "known" : "unknown", g_focus);
    RET(w ? call_wndproc(w, rd32(p + 4), rd32(p + 8), rd32(p + 12)) : 0, 1);
}

static void sh_SendMessageA(Guest* g)
{
    Wnd* w = wnd(ARG(0));
    RET(w ? call_wndproc(w, ARG(1), ARG(2), ARG(3)) : 0, 4);
}

static void sh_PostMessageA(Guest* g)
{
    Wnd* w = wnd(ARG(0));
    RET(w ? post(w->tid, w->hwnd, ARG(1), ARG(2), ARG(3)) : post(gt_self()->tid, 0, ARG(1), ARG(2), ARG(3)), 4);
}

static void sh_PostThreadMessageA(Guest* g) { RET(post(ARG(0), 0, ARG(1), ARG(2), ARG(3)), 4); }
static void sh_PostQuitMessage(Guest* g) { post(gt_self()->tid, 0, WM_QUIT, ARG(0), 0); RET(0, 1); }

static void sh_GetQueueStatus(Guest* g)
{
    pump();
    unsigned n = queued(gt_self()->tid);
    RET(n ? (ARG(0) << 16) | ARG(0) : 0, 1);
}

/* MsgWaitForMultipleObjects(n, handles, all, ms, wake): the handles, or a message in the queue */
static void sh_MsgWaitForMultipleObjects(Guest* g)
{
    uint32_t n = ARG(0), all = ARG(2), ms = ARG(3), hs[64];
    for (uint32_t i = 0; i < n && i < 64; ++i)
        hs[i] = rd32(ARG(1) + 4 * i);
    uint32_t tid = gt_self()->tid;
    uint64_t deadline = ms == 0xFFFFFFFFu ? ~0ull : rt_monotonic_ns() / 1000000u + ms;
    for (;;)
    {
        pump();
        if (queued(tid))
            RET(n, 5); /* WAIT_OBJECT_0 + n: input */
        uint64_t now = rt_monotonic_ns() / 1000000u;
        uint32_t slice = now >= deadline ? 0 : (uint32_t)(deadline - now < 10 ? deadline - now : 10);
        uint32_t r = n ? k_wait(hs, n, all != 0, slice) : K_WAIT_TIMEOUT;
        if (!n && slice)
        {
            gt_unlock();
            plat_sleep_ms(slice);
            gt_lock();
        }
        if (r != K_WAIT_TIMEOUT)
            RET(r, 5);
        if (now >= deadline)
            RET(K_WAIT_TIMEOUT, 5);
    }
}

/* --- WINMM timers ----------------------------------------------------------------------------------- */
static void sh_timeGetTime(Guest* g) { RET((uint32_t)(rt_monotonic_ns() / 1000000u), 0); }
static void sh_timeBeginPeriod(Guest* g) { RET(0, 1); }
static void sh_timeEndPeriod(Guest* g) { RET(0, 1); }

static void sh_timeGetDevCaps(Guest* g)
{
    wr32(ARG(0), 1);          /* wPeriodMin */
    wr32(ARG(0) + 4, 1000000); /* wPeriodMax */
    RET(0, 2);
}

/* timeSetEvent(uDelay, uResolution, lpTimeProc, dwUser, fuEvent): a host thread per timer, calling
 * the guest's TimeProc(uTimerID, uMsg, dwUser, dw1, dw2) - or setting an event, for
 * TIME_CALLBACK_EVENT_SET - once (TIME_ONESHOT) or every uDelay ms (TIME_PERIODIC) */
typedef struct Timer
{
    volatile uint32_t live;
    uint32_t id, delay, proc, user, flags;
} Timer;

static Timer g_timers[16];

static void timer_thread(void* arg)
{
    Timer* t = (Timer*)arg;
    uint64_t next = rt_monotonic_ns() / 1000000u + t->delay;
    while (t->live)
    {
        uint64_t now = rt_monotonic_ns() / 1000000u;
        if (now < next)
        {
            plat_sleep_ms((uint32_t)(next - now < 5 ? next - now : 5));
            continue;
        }
        if (!t->live)
            break;
        if (t->flags & 0x10) /* TIME_CALLBACK_EVENT_SET */
            k_event_set(t->proc, 1);
        else if (t->flags & 0x20) /* TIME_CALLBACK_EVENT_PULSE */
            k_event_set(t->proc, 1);
        else
        {
            uint32_t a[5] = { t->id, 0, t->user, 0, 0 };
            guest_call(t->proc, 5, a);
        }
        if (!(t->flags & 1)) /* TIME_ONESHOT */
        {
            t->live = 0;
            break;
        }
        next += t->delay ? t->delay : 1;
    }
    gt_exit_self();
}

static void sh_timeSetEvent(Guest* g)
{
    for (uint32_t i = 0; i < 16; ++i)
        if (!g_timers[i].live)
        {
            Timer* t = &g_timers[i];
            t->id = i + 1, t->delay = ARG(0), t->proc = ARG(2), t->user = ARG(3), t->flags = ARG(4);
            t->live = 1;
            if (!plat_thread_start(timer_thread, t))
            {
                t->live = 0;
                RET(0, 5);
            }
            RET(t->id, 5);
        }
    RET(0, 5);
}

static void sh_timeKillEvent(Guest* g)
{
    uint32_t id = ARG(0);
    if (!id || id > 16 || !g_timers[id - 1].live)
        RET(97, 1); /* MMSYSERR_INVALPARAM */
    g_timers[id - 1].live = 0;
    RET(0, 1);
}

/* --- the rest of USER32 ---------------------------------------------------------------------------------- */
static void sh_GetWindowThreadProcessId(Guest* g)
{
    Wnd* w = wnd(ARG(0));
    if (ARG(1))
        wr32(ARG(1), w ? 0x1000u : 0); /* the process id the TEB reports */
    RET(w ? w->tid : 0, 2);
}

/* keybd_event(bVk, bScan, dwFlags, dwExtraInfo): synthesised keys go through the queue */
static void sh_keybd_event(Guest* g)
{
    Wnd* w = wnd(g_focus);
    uint32_t vk = ARG(0) & 0xFF, up = ARG(2) & 2; /* KEYEVENTF_KEYUP */
    if (w && vk)
    {
        g_keys[vk] = up ? 0 : 0x80;
        post(w->tid, w->hwnd, up ? WM_KEYUP : WM_KEYDOWN, vk, 1u | (scan_of_vk(vk) & 0xFFu) << 16 | (up ? 0xC0000000u : 0));
    }
    RET(0, 4);
}

/* MapVirtualKeyA(uCode, uMapType): 0 VK->scan, 1 scan->VK, 2 VK->character, 3 scan->VK (left/right) */
static void sh_MapVirtualKeyA(Guest* g)
{
    uint32_t c = ARG(0);
    switch (ARG(1))
    {
    case 0: RET(scan_of_vk(c) & 0xFF, 2);
    case 1:
    case 3:
        for (uint32_t vk = 1; vk < 256; ++vk)
            if ((scan_of_vk(vk) & 0xFF) == c && !(scan_of_vk(vk) & 0x100))
                RET(vk, 2);
        RET(0, 2);
    case 2:
    {
        static const struct { uint8_t vk, ch; } oem[] = { { 0xBA, ';' }, { 0xBB, '=' }, { 0xBC, ',' }, { 0xBD, '-' },
            { 0xBE, '.' }, { 0xBF, '/' }, { 0xC0, '`' }, { 0xDB, '[' }, { 0xDC, '\\' }, { 0xDD, ']' }, { 0xDE, '\'' },
            { 0x20, ' ' }, { 0x0D, '\r' }, { 0x08, '\b' }, { 0x09, '\t' }, { 0x1B, 0x1B } };
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            RET(c, 2);
        for (size_t i = 0; i < sizeof oem / sizeof oem[0]; ++i)
            if (oem[i].vk == c)
                RET(oem[i].ch, 2);
        RET(0, 2);
    }
    default: RET(0, 2);
    }
}

/* FindWindowA(lpClassName, lpWindowName): only this process's windows exist */
static void sh_FindWindowA(Guest* g)
{
    const char* cls = ARG(0) && ARG(0) > 0xFFFF ? ARGS(0) : NULL;
    for (int i = 0; i < MAX_WINDOWS; ++i)
    {
        Wnd* w = &g_wnds[i];
        if (!w->used || !cls)
            continue;
        for (int c = 0; c < MAX_CLASSES; ++c)
            if (g_classes[c].wndproc == w->wndproc && !strcmp(g_classes[c].name, cls))
                RET(w->hwnd, 2);
    }
    RET(0, 2);
}

/* wsprintf's formatting: %[-#0][width][.prec][h|l]{c d i u x X s S p %}, no floating point, at
 * most 1024 characters. `args` walks the guest's argument list. */
static uint32_t guest_format(uint32_t out, uint32_t fmt, uint32_t args)
{
    char buf[1100];
    uint32_t o = 0;
    for (uint32_t f = fmt; rd8(f) && o < 1024;)
    {
        char c = (char)rd8(f++);
        if (c != '%')
        {
            buf[o++] = c;
            continue;
        }
        char spec[16] = "%";
        int s = 1, wide = 0;
        while (strchr("-#0", (char)rd8(f)) && rd8(f) && s < 4)
            spec[s++] = (char)rd8(f++);
        while (rd8(f) >= '0' && rd8(f) <= '9' && s < 8)
            spec[s++] = (char)rd8(f++);
        if (rd8(f) == '.')
            for (spec[s++] = (char)rd8(f++); rd8(f) >= '0' && rd8(f) <= '9' && s < 12;)
                spec[s++] = (char)rd8(f++);
        if (rd8(f) == 'l' || rd8(f) == 'h')
            wide = rd8(f++) == 'l' ? 1 : -1;
        char t = (char)rd8(f++);
        char piece[1100];
        piece[0] = 0;
        switch (t)
        {
        case '%': strcpy(piece, "%"); break;
        case 'c':
        case 'C':
            spec[s++] = 'c', spec[s] = 0;
            snprintf(piece, sizeof piece, spec, (int)(rd32(args) & 0xFF)), args += 4;
            break;
        case 'd':
        case 'i':
        case 'u':
        case 'x':
        case 'X':
            spec[s++] = t, spec[s] = 0;
            if (t == 'd' || t == 'i')
                snprintf(piece, sizeof piece, spec, (int)(int32_t)rd32(args));
            else
                snprintf(piece, sizeof piece, spec, (unsigned)rd32(args));
            args += 4;
            break;
        case 'p':
            snprintf(piece, sizeof piece, "%08X", rd32(args)), args += 4;
            break;
        case 's':
        case 'S':
        {
            char str[1025];
            uint32_t p = rd32(args), n = 0;
            args += 4;
            int w16 = t == 'S' ? wide >= 0 : wide == 1;
            if (!p)
                strcpy(str, "(null)");
            else
            {
                for (; n < 1024; ++n)
                {
                    uint32_t ch = w16 ? rd16(p + 2 * n) : rd8(p + n);
                    if (!ch)
                        break;
                    str[n] = (char)(ch > 0xFF ? '?' : ch);
                }
                str[n] = 0;
            }
            spec[s++] = 's', spec[s] = 0;
            snprintf(piece, sizeof piece, spec, str);
            break;
        }
        default:
            piece[0] = t, piece[1] = 0;
            break;
        }
        for (const char* p = piece; *p && o < 1024; ++p)
            buf[o++] = *p;
    }
    for (uint32_t i = 0; i < o; ++i)
        wr8(out + i, (uint8_t)buf[i]);
    wr8(out + o, 0);
    return o;
}

static void sh_wsprintfA(Guest* g) { RETC(guest_format(ARG(0), ARG(1), g->esp + 12)); } /* cdecl, varargs */
static void sh_wvsprintfA(Guest* g) { RET(guest_format(ARG(0), ARG(1), ARG(2)), 3); }

static uint32_t g_next_message = 0xC100;
static struct
{
    char name[64];
    uint32_t id;
} g_messages[32];

static void sh_RegisterWindowMessageA(Guest* g)
{
    for (int i = 0; i < 32; ++i)
        if (g_messages[i].id && !strcmp(g_messages[i].name, ARGS(0)))
            RET(g_messages[i].id, 1);
    for (int i = 0; i < 32; ++i)
        if (!g_messages[i].id)
        {
            snprintf(g_messages[i].name, sizeof g_messages[i].name, "%s", ARGS(0));
            g_messages[i].id = g_next_message++;
            RET(g_messages[i].id, 1);
        }
    RET(0, 1);
}

static void sh_RemoveMenu(Guest* g) { RET(1, 3); }
static void sh_AttachThreadInput(Guest* g) { RET(1, 3); }

static void sh_SetCursorPos(Guest* g)
{
    float x = (float)(int32_t)ARG(0), y = (float)(int32_t)ARG(1);
    Wnd* w = wnd(g_focus);
    if (w && x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h)
    {
        int cx = (int)x - w->x, cy = (int)y - w->y;
        ui_squeeze_point(w, &cx, &cy);
        x = (float)(w->x + cx), y = (float)(w->y + cy);
    }
    if (g_sdl_up)
        SDL_WarpMouseGlobal(x, y);
    RET(1, 2);
}

/* the clipboard: text only (CF_TEXT, code page 1252, and CF_UNICODETEXT, UTF-16), the host's
 * clipboard as UTF-8 */
static void sh_OpenClipboard(Guest* g) { RET(1, 1); }
static void sh_EmptyClipboard(Guest* g) { RET(1, 0); }
static void sh_CloseClipboard(Guest* g) { RET(1, 0); }

static void sh_SetClipboardData(Guest* g)
{
    if ((ARG(0) == 1 || ARG(0) == 13) && ARG(1)) /* CF_TEXT, CF_UNICODETEXT; the handle is GlobalAlloc's pointer */
    {
        char utf8[8192];
        size_t o = 0;
        int wide = ARG(0) == 13;
        for (uint32_t p = ARG(1); o + 4 < sizeof utf8; p += wide ? 2 : 1)
        {
            uint32_t c = wide ? rd16(p) : rd8(p);
            if (!c)
                break;
            if (wide && c >= 0xD800 && c < 0xDC00 && (rd16(p + 2) & 0xFC00) == 0xDC00)
                c = 0x10000 + ((c - 0xD800) << 10) + (rd16(p + 2) - 0xDC00), p += 2;
            if (c < 0x80)
                utf8[o++] = (char)c;
            else if (c < 0x800)
                utf8[o++] = (char)(0xC0 | c >> 6), utf8[o++] = (char)(0x80 | (c & 0x3F));
            else if (c < 0x10000)
                utf8[o++] = (char)(0xE0 | c >> 12), utf8[o++] = (char)(0x80 | (c >> 6 & 0x3F)), utf8[o++] = (char)(0x80 | (c & 0x3F));
            else
                utf8[o++] = (char)(0xF0 | c >> 18), utf8[o++] = (char)(0x80 | (c >> 12 & 0x3F)),
                utf8[o++] = (char)(0x80 | (c >> 6 & 0x3F)), utf8[o++] = (char)(0x80 | (c & 0x3F));
        }
        utf8[o] = 0;
        sdl_up();
        SDL_SetClipboardText(utf8);
    }
    RET(ARG(1), 2);
}

/* SystemParametersInfoA(uiAction, uiParam, pvParam, fWinIni) */
static void sh_SystemParametersInfoA(Guest* g)
{
    uint32_t p = ARG(2);
    switch (ARG(0))
    {
    case 0x0030: /* SPI_GETWORKAREA */
    {
        uint32_t w, h, hz;
        user32_desktop_mode(&w, &h, &hz);
        write_rect(p, 0, 0, (int)w, (int)h);
        break;
    }
    case 0x0010: /* SPI_GETSCREENSAVEACTIVE */
    case 0x0044: /* SPI_GETSCREENREADER */
        if (p)
            wr32(p, 0);
        break;
    case 0x0032: /* SPI_GETFILTERKEYS, SPI_GETTOGGLEKEYS, SPI_GETSTICKYKEYS: {cbSize, dwFlags, ...}: off */
    case 0x0034:
    case 0x003A:
        if (p)
            wr32(p + 4, 0);
        break;
    default: break; /* the SET actions: accepted */
    }
    RET(1, 4);
}

/* WINDOWPLACEMENT {length, flags, showCmd, ptMinPosition, ptMaxPosition, rcNormalPosition} */
static void sh_GetWindowPlacement(Guest* g)
{
    Wnd* w = wnd(ARG(0));
    if (!w)
        RET(0, 2);
    uint32_t p = ARG(1);
    wr32(p + 4, 0);
    wr32(p + 8, w->visible ? 1 : 0); /* SW_SHOWNORMAL / SW_HIDE */
    for (uint32_t i = 12; i < 28; i += 4)
        wr32(p + i, 0xFFFFFFFFu);
    write_rect(p + 28, w->x, w->y, w->x + w->w, w->y + w->h);
    RET(1, 2);
}

/* MoveWindow(hWnd, X, Y, nWidth, nHeight, bRepaint) */
static void sh_MoveWindow(Guest* g)
{
    Wnd* w = wnd(ARG(0));
    if (!w)
        RET(0, 6);
    w->x = (int32_t)ARG(1), w->y = (int32_t)ARG(2), w->w = (int32_t)ARG(3), w->h = (int32_t)ARG(4);
    if (w->sdl)
    {
        SDL_SetWindowPosition(w->sdl, w->x, w->y);
        SDL_SetWindowSize(w->sdl, w->w, w->h);
        apply_frame(w);
    }
    call_wndproc(w, WM_MOVE, 0, ((uint32_t)(uint16_t)w->y << 16) | (uint16_t)w->x);
    call_wndproc(w, WM_SIZE, 0, ((uint32_t)(uint16_t)w->h << 16) | (uint16_t)w->w);
    RET(1, 6);
}

/* --- GDI: the device contexts, DIB sections and fonts the game draws text with -------------------------------
 * The objects are real (a DIB's bits are guest memory the game reads back); DrawText measures but
 * does not rasterise yet - text through GDI is for the IME's windows, which a US client never
 * opens. */
enum
{
    GDI_FREE,
    GDI_DC,
    GDI_DIB,
    GDI_FONT,
};

typedef struct GdiObj
{
    int kind;
    uint32_t bits, width, height, bpp, stride; /* DIB */
    uint32_t font, bitmap, bk_mode, bk_color, text_color; /* DC: selected objects and state */
    int32_t font_height;
} GdiObj;

#define GDI_BASE 0x00060010u
#define GDI_MAX 64
static GdiObj g_gdi[GDI_MAX];

static uint32_t gdi_new(int kind)
{
    for (uint32_t i = 0; i < GDI_MAX; ++i)
        if (g_gdi[i].kind == GDI_FREE)
        {
            memset(&g_gdi[i], 0, sizeof g_gdi[i]);
            g_gdi[i].kind = kind;
            return GDI_BASE + 4 * i;
        }
    return 0;
}

static GdiObj* gdi(uint32_t h, int kind)
{
    uint32_t i = (h - GDI_BASE) / 4;
    return h >= GDI_BASE && !((h - GDI_BASE) % 4) && i < GDI_MAX && g_gdi[i].kind == kind ? &g_gdi[i] : NULL;
}

static void sh_CreateCompatibleDC(Guest* g)
{
    uint32_t h = gdi_new(GDI_DC);
    GdiObj* d = gdi(h, GDI_DC);
    if (d)
        d->bk_mode = 2, d->bk_color = 0xFFFFFF; /* OPAQUE, white */
    RET(h, 1);
}

static void sh_DeleteDC(Guest* g)
{
    GdiObj* d = gdi(ARG(0), GDI_DC);
    if (d)
        d->kind = GDI_FREE;
    RET(d != NULL, 1);
}

/* CreateDIBSection(hdc, pbmi, usage, ppvBits, hSection, offset): bottom-up unless the height is negative */
static void sh_CreateDIBSection(Guest* g)
{
    uint32_t bi = ARG(1);
    int32_t w = (int32_t)rd32(bi + 4), h = (int32_t)rd32(bi + 8);
    uint32_t bpp = rd16(bi + 14);
    if (w <= 0 || !h || (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32))
        RET(0, 6);
    uint32_t hb = gdi_new(GDI_DIB);
    GdiObj* b = gdi(hb, GDI_DIB);
    if (!b)
        RET(0, 6);
    b->width = (uint32_t)w, b->height = (uint32_t)(h < 0 ? -h : h), b->bpp = bpp;
    b->stride = (((uint32_t)w * bpp + 31) / 32) * 4;
    b->bits = gheap_alloc(b->stride * b->height, 1);
    if (ARG(3))
        wr32(ARG(3), b->bits);
    RET(hb, 6);
}

/* CreateFontIndirectA(LOGFONTA*) */
static void sh_CreateFontIndirectA(Guest* g)
{
    uint32_t h = gdi_new(GDI_FONT);
    GdiObj* f = gdi(h, GDI_FONT);
    if (f)
        f->font_height = (int32_t)rd32(ARG(0));
    RET(h, 1);
}

static void sh_SelectObject(Guest* g)
{
    GdiObj* d = gdi(ARG(0), GDI_DC);
    uint32_t h = ARG(1), old = 0;
    if (!d)
        RET(0, 2);
    if (gdi(h, GDI_FONT))
        old = d->font, d->font = h;
    else if (gdi(h, GDI_DIB))
        old = d->bitmap, d->bitmap = h;
    else
        old = 0x00060001u; /* a stock object stood in for */
    RET(old ? old : 0x00060001u, 2);
}

static void sh_DeleteObject(Guest* g)
{
    GdiObj* b = gdi(ARG(0), GDI_DIB);
    GdiObj* f = gdi(ARG(0), GDI_FONT);
    if (b)
    {
        gheap_free(b->bits);
        b->kind = GDI_FREE;
    }
    if (f)
        f->kind = GDI_FREE;
    RET(1, 1);
}

/* GetObjectA(h, c, pv): BITMAP {bmType, bmWidth, bmHeight, bmWidthBytes, bmPlanes, bmBitsPixel, bmBits} */
static void sh_GetObjectA(Guest* g)
{
    GdiObj* b = gdi(ARG(0), GDI_DIB);
    if (b)
    {
        if (!ARG(2))
            RET(24, 3);
        uint32_t p = ARG(2);
        wr32(p, 0);
        wr32(p + 4, b->width);
        wr32(p + 8, b->height);
        wr32(p + 12, b->stride);
        wr16(p + 16, 1);
        wr16(p + 18, (uint16_t)b->bpp);
        wr32(p + 20, b->bits);
        RET(24, 3);
    }
    GdiObj* f = gdi(ARG(0), GDI_FONT);
    if (f)
    {
        if (ARG(2))
        {
            memset(GUEST_PTR(ARG(2)), 0, ARG(1) < 60 ? ARG(1) : 60);
            wr32(ARG(2), (uint32_t)f->font_height);
        }
        RET(60, 3);
    }
    RET(0, 3);
}

static void sh_SetBkMode(Guest* g)
{
    GdiObj* d = gdi(ARG(0), GDI_DC);
    uint32_t old = d ? d->bk_mode : 0;
    if (d)
        d->bk_mode = ARG(1);
    RET(old, 2);
}

static void sh_SetBkColor(Guest* g)
{
    GdiObj* d = gdi(ARG(0), GDI_DC);
    uint32_t old = d ? d->bk_color : 0xFFFFFFFFu;
    if (d)
        d->bk_color = ARG(1);
    RET(old, 2);
}

static void sh_SetTextColor(Guest* g)
{
    GdiObj* d = gdi(ARG(0), GDI_DC);
    uint32_t old = d ? d->text_color : 0xFFFFFFFFu;
    if (d)
        d->text_color = ARG(1);
    RET(old, 2);
}

/* DrawTextA/W(hdc, lpchText, cchText, lprc, format): measured with a fixed cell (half the font's
 * height wide), DT_CALCRECT (0x400) honoured; nothing is rasterised yet */
static void draw_text(Guest* g, int wide)
{
    GdiObj* d = gdi(ARG(0), GDI_DC);
    GdiObj* f = d ? gdi(d->font, GDI_FONT) : NULL;
    int32_t ch = f && f->font_height ? (f->font_height < 0 ? -f->font_height : f->font_height) : 16;
    int32_t n = (int32_t)ARG(2), lines = 1, col = 0, width = 0;
    uint32_t s = ARG(1);
    for (int32_t i = 0; n < 0 ? (wide ? rd16(s + 2 * (uint32_t)i) : rd8(s + (uint32_t)i)) != 0 : i < n; ++i)
    {
        uint32_t c = wide ? rd16(s + 2 * (uint32_t)i) : rd8(s + (uint32_t)i);
        if (c == '\n')
            lines++, col = 0;
        else if (++col > width)
            width = col;
    }
    static int warned;
    if (!warned++)
        rt_log("[recomp] DrawText: text is measured, not drawn (GDI text is not rasterised yet)\n");
    if (ARG(4) & 0x400)
    {
        uint32_t r = ARG(3);
        wr32(r + 8, rd32(r) + (uint32_t)(width * ch / 2));
        wr32(r + 12, rd32(r + 4) + (uint32_t)(lines * ch));
    }
    RET((uint32_t)(lines * ch), 5);
}

static void sh_DrawTextA(Guest* g) { draw_text(g, 0); }
static void sh_DrawTextW(Guest* g) { draw_text(g, 1); }

/* --- IMM32: the US client with no IME: a context that is never open ----------------------------------------- */
static void sh_ImmGetContext(Guest* g) { RET(0x00070001u, 1); }
static void sh_ImmReleaseContext(Guest* g) { RET(1, 2); }
static void sh_ImmGetOpenStatus(Guest* g) { RET(0, 1); }
static void sh_ImmSetOpenStatus(Guest* g) { RET(1, 2); }
static void sh_ImmNotifyIME(Guest* g) { RET(1, 4); }
static void sh_ImmSetCandidateWindow(Guest* g) { RET(1, 2); }
static void sh_ImmSetCompositionWindow(Guest* g) { RET(1, 2); }
static void sh_ImmGetCandidateListA(Guest* g) { RET(0, 4); }
static void sh_ImmGetCandidateListCountA(Guest* g) { if (ARG(1)) wr32(ARG(1), 0); RET(0, 2); }
static void sh_ImmGetCompositionStringA(Guest* g) { RET(0, 4); }
static void sh_ImmSetCompositionStringA(Guest* g) { RET(0, 6); }
static void sh_ImmSetConversionStatus(Guest* g) { RET(1, 3); }
static void sh_ImmGetDescriptionA(Guest* g) { RET(0, 3); }
static void sh_ImmGetVirtualKey(Guest* g) { RET(0, 1); }

static void sh_ImmGetConversionStatus(Guest* g)
{
    if (ARG(1))
        wr32(ARG(1), 0);
    if (ARG(2))
        wr32(ARG(2), 0);
    RET(1, 3);
}

/* --- the window calls FINAL FANTASY XIV makes beyond FFXI's -------------------------------------- */

static void forward_a(Guest* g, Shim s) { s(g); }

/* UTF-16 to a temporary guest string in code page 1252 (characters past 0xFF become '?'). */
static uint32_t narrow_w(uint32_t w)
{
    if (!w || w < 0x10000u) /* an atom, not a string */
        return w;
    uint32_t n = 0;
    while (rd16(w + 2 * n))
        n++;
    uint32_t a = gheap_alloc(n + 1, 0);
    for (uint32_t i = 0; i < n; ++i)
    {
        uint16_t c = rd16(w + 2 * i);
        wr8(a + i, c > 0xFF ? '?' : (uint8_t)c);
    }
    wr8(a + n, 0);
    return a;
}

/* RegisterClassExA(WNDCLASSEXA*): lpfnWndProc at +8, lpszClassName at +40 */
static void register_class(Guest* g, uint32_t name)
{
    uint32_t wc = ARG(0);
    for (int i = 0; i < MAX_CLASSES; ++i)
        if (!g_classes[i].name[0])
        {
            snprintf(g_classes[i].name, sizeof g_classes[i].name, "%s", (const char*)GUEST_PTR(name));
            g_classes[i].wndproc = rd32(wc + 8);
            RET(0xC000u + (uint32_t)i, 1);
        }
    RET(0, 1);
}
static void sh_RegisterClassExA(Guest* g) { register_class(g, rd32(ARG(0) + 40)); }
static void sh_RegisterClassExW(Guest* g)
{
    uint32_t name = narrow_w(rd32(ARG(0) + 40));
    register_class(g, name);
    if (name >= 0x10000u)
        gheap_free(name);
}

static void sh_DefWindowProcW(Guest* g) { forward_a(g, sh_DefWindowProcA); }
static void sh_DispatchMessageW(Guest* g) { forward_a(g, sh_DispatchMessageA); }
static void sh_PeekMessageW(Guest* g) { forward_a(g, sh_PeekMessageA); }

/* GetWindowLongA(hwnd, index) */
static void sh_GetWindowLongA(Guest* g)
{
    Wnd* w = wnd(ARG(0));
    if (!w)
        RET(0, 2);
    switch ((int32_t)ARG(1))
    {
    case -4: RET(w->wndproc, 2);                                  /* GWL_WNDPROC */
    case -16: RET(w->style | (w->visible ? 0x10000000u : 0), 2); /* GWL_STYLE */
    case -20: RET(w->exstyle, 2);                                 /* GWL_EXSTYLE */
    case -21: RET(w->userdata, 2);                                /* GWL_USERDATA */
    }
    RET(0, 2);
}

/* SetWindowLongA(hwnd, index, value): returns the previous value */
static void sh_SetWindowLongA(Guest* g)
{
    Wnd* w = wnd(ARG(0));
    uint32_t v = ARG(2), old = 0;
    if (!w)
        RET(0, 3);
    switch ((int32_t)ARG(1))
    {
    case -4: old = w->wndproc; w->wndproc = v; break; /* GWL_WNDPROC */
    case -16: old = w->style; w->style = v; apply_frame(w); break; /* GWL_STYLE */
    case -20: old = w->exstyle; w->exstyle = v; break; /* GWL_EXSTYLE */
    case -21: old = w->userdata; w->userdata = v; break; /* GWL_USERDATA */
    default: break;
    }
    RET(old, 3);
}

/* SetWindowPos(hwnd, after, x, y, cx, cy, flags) */
static void sh_SetWindowPos(Guest* g)
{
    Wnd* w = wnd(ARG(0));
    uint32_t flags = ARG(6);
    if (!w)
        RET(0, 7);
    if (!(flags & 0x0002u)) /* SWP_NOMOVE */
    {
        w->x = (int32_t)ARG(2), w->y = (int32_t)ARG(3);
        if (w->sdl)
            SDL_SetWindowPosition(w->sdl, w->x, w->y);
        call_wndproc(w, WM_MOVE, 0, ((uint32_t)(uint16_t)w->y << 16) | (uint16_t)w->x);
    }
    if (!(flags & 0x0001u) && (int32_t)ARG(4) > 0 && (int32_t)ARG(5) > 0) /* SWP_NOSIZE */
    {
        w->w = (int32_t)ARG(4), w->h = (int32_t)ARG(5);
        if (w->sdl)
            SDL_SetWindowSize(w->sdl, w->w, w->h);
        apply_frame(w);
        call_wndproc(w, WM_SIZE, 0, ((uint32_t)(uint16_t)w->h << 16) | (uint16_t)w->w);
    }
    if (flags & 0x0040u) /* SWP_SHOWWINDOW */
        show_window(w, 1);
    else if (flags & 0x0080u) /* SWP_HIDEWINDOW */
        show_window(w, 0);
    RET(1, 7);
}

static void sh_SetWindowTextW(Guest* g)
{
    Wnd* w = wnd(ARG(0));
    uint32_t t = narrow_w(ARG(1));
    if (w && w->sdl && t)
        SDL_SetWindowTitle(w->sdl, (const char*)GUEST_PTR(t));
    if (t >= 0x10000u)
        gheap_free(t);
    RET(w != NULL, 2);
}

static void sh_GetDC(Guest* g) { RET(gdi_new(GDI_DC), 1); }
static void sh_ReleaseDC(Guest* g)
{
    GdiObj* d = gdi(ARG(1), GDI_DC);
    if (d)
        d->kind = GDI_FREE;
    RET(d != NULL, 2);
}

static uint32_t g_capture;
static void sh_SetCapture(Guest* g) { uint32_t old = g_capture; g_capture = ARG(0); RET(old, 1); }
static void sh_ReleaseCapture(Guest* g) { g_capture = 0; RET(1, 0); }

/* ToAscii(vk, scan, keystate, out, flags): letters, digits and space, shifted from the state */
static void sh_ToAscii(Guest* g)
{
    uint32_t vk = ARG(0) & 0xFF, state = ARG(2), out = ARG(3);
    int shift = state && (rd8(state + 0x10) & 0x80);
    uint8_t c = 0;
    if (vk >= 'A' && vk <= 'Z')
        c = (uint8_t)(shift ? vk : vk + 32);
    else if ((vk >= '0' && vk <= '9') || vk == ' ')
        c = (uint8_t)vk;
    if (!c)
        RET(0, 5);
    wr16(out, c);
    RET(1, 5);
}

static void sh_EnumWindows(Guest* g) { RET(1, 2); }
static void sh_GetProcessWindowStation(Guest* g) { RET(0x000A0001u, 0); }
static void sh_GetUserObjectInformationW(Guest* g) { gt_set_error(50); RET(0, 5); }
static void sh_GetDesktopWindow(Guest* g) { RET(0x00010010u, 0); }
static void sh_GetActiveWindow(Guest* g) { RET(g_focus, 0); }
static void sh_SetActiveWindow(Guest* g) { uint32_t old = g_focus; g_focus = ARG(0); RET(old, 1); }
/* GetClipboardData(format): the host's clipboard text in a block of the guest heap the clipboard
 * owns (the next call frees it), as CF_UNICODETEXT or CF_TEXT */
static void sh_GetClipboardData(Guest* g)
{
    static uint32_t block;
    uint32_t fmt = ARG(0);
    if (fmt != 1 && fmt != 13)
        RET(0, 1);
    sdl_up();
    char* text = SDL_HasClipboardText() ? SDL_GetClipboardText() : NULL;
    if (!text)
        RET(0, 1);
    size_t n = strlen(text);
    if (block)
        gheap_free(block);
    block = gheap_alloc((uint32_t)(n + 1) * 2u + 2u, 0);
    uint32_t o = block;
    for (const unsigned char* p = (const unsigned char*)text; *p;)
    {
        uint32_t c = *p++;
        if (c >= 0xC0)
        {
            unsigned k = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
            c &= 0x3F >> k;
            for (unsigned i = 0; i < k && (*p & 0xC0) == 0x80; ++i)
                c = (c << 6) | (*p++ & 0x3F);
        }
        else if (c >= 0x80)
            c = '?';
        if (c == '\r' && *p == '\n')
            continue; /* the host's line ends are the game's \n */
        if (fmt == 1)
            wr8(o++, (uint8_t)(c <= 0xFF ? c : '?'));
        else if (c >= 0x10000)
            wr16(o, (uint16_t)(0xD800 + ((c - 0x10000) >> 10))), wr16(o + 2, (uint16_t)(0xDC00 + (c & 0x3FF))), o += 4;
        else
            wr16(o, (uint16_t)c), o += 2;
    }
    if (fmt == 1)
        wr8(o, 0);
    else
        wr16(o, 0);
    SDL_free(text);
    RET(block, 1);
}
static void sh_SetDlgItemTextW(Guest* g) { RET(1, 3); }
static void sh_GetMenu(Guest* g) { RET(0, 1); }
static void sh_CreateMenu(Guest* g) { RET(0x00090001u, 0); }
static void sh_ClipCursor(Guest* g) { RET(1, 1); }

/* SetTimer(hwnd, id, ms, proc) / KillTimer(hwnd, id): the game's timers are not driven yet (logged) */
static void sh_SetTimer(Guest* g)
{
    rt_log("[recomp] SetTimer(%08x, %u, %u ms, %08x): not delivered\n", ARG(0), ARG(1), ARG(2), ARG(3));
    RET(ARG(1) ? ARG(1) : 1, 4);
}
static void sh_KillTimer(Guest* g) { RET(1, 2); }

static void message_box(Guest* g, uint32_t text, uint32_t caption)
{
    rt_log("[recomp] MessageBox \"%s\": %s\n", caption ? (const char*)GUEST_PTR(caption) : "",
        text ? (const char*)GUEST_PTR(text) : "");
    RET(1, 4); /* IDOK */
}
static void sh_MessageBoxA(Guest* g) { message_box(g, ARG(1), ARG(2)); }
static void sh_MessageBoxW(Guest* g)
{
    uint32_t t = narrow_w(ARG(1)), c = narrow_w(ARG(2));
    rt_log("[recomp] MessageBox \"%s\": %s\n", c ? (const char*)GUEST_PTR(c) : "", t ? (const char*)GUEST_PTR(t) : "");
    if (t >= 0x10000u) gheap_free(t);
    if (c >= 0x10000u) gheap_free(c);
    RET(1, 4);
}

/* GDI: stock objects, pens and the debug drawing calls draw nothing; text metrics are the font's
 * height, at half that per character. */
static void sh_GetStockObject(Guest* g) { RET(0x00080000u | (ARG(0) & 0xFFu), 1); }
static void sh_CreatePen(Guest* g) { RET(0x00080100u, 3); }
static void sh_MoveToEx(Guest* g) { if (ARG(3)) { wr32(ARG(3), 0); wr32(ARG(3) + 4, 0); } RET(1, 4); }
static void sh_LineTo(Guest* g) { RET(1, 3); }
static void sh_Ellipse(Guest* g) { RET(1, 5); }
static void sh_TextOutA(Guest* g) { RET(1, 5); }

/* CreateFontW(height, width, escapement, orientation, weight, italic, underline, strikeout,
 * charset, outprecision, clipprecision, quality, pitch, face) */
static void sh_CreateFontW(Guest* g)
{
    uint32_t h = gdi_new(GDI_FONT);
    GdiObj* f = gdi(h, GDI_FONT);
    if (f)
        f->font_height = (int32_t)ARG(0);
    RET(h, 14);
}

static int32_t dc_font_height(uint32_t dc)
{
    GdiObj* d = gdi(dc, GDI_DC);
    GdiObj* f = d ? gdi(d->font, GDI_FONT) : NULL;
    int32_t h = f ? f->font_height : 16;
    return h < 0 ? -h : h ? h : 16;
}

/* GetTextMetricsA(dc, TEXTMETRICA*) */
static void sh_GetTextMetricsA(Guest* g)
{
    int32_t h = dc_font_height(ARG(0));
    uint32_t tm = ARG(1);
    memset(GUEST_PTR(tm), 0, 56);
    wr32(tm + 0, (uint32_t)h);                 /* tmHeight */
    wr32(tm + 4, (uint32_t)(h * 4 / 5));       /* tmAscent */
    wr32(tm + 8, (uint32_t)(h - h * 4 / 5));   /* tmDescent */
    wr32(tm + 20, (uint32_t)(h / 2));          /* tmAveCharWidth */
    wr32(tm + 24, (uint32_t)h);                /* tmMaxCharWidth */
    wr32(tm + 28, 400);                        /* tmWeight */
    RET(1, 2);
}

/* GetTextExtentPoint32A(dc, text, count, SIZE*) */
static void sh_GetTextExtentPoint32A(Guest* g)
{
    int32_t h = dc_font_height(ARG(0));
    wr32(ARG(3), (uint32_t)((int32_t)ARG(2) * h / 2));
    wr32(ARG(3) + 4, (uint32_t)h);
    RET(1, 4);
}

/* GetGlyphOutlineW: no outlines from this GDI (GDI_ERROR) */
static void sh_GetGlyphOutlineW(Guest* g) { RET(0xFFFFFFFFu, 7); }

static const ShimDef USER32_XIV[] = {
    { "user32.dll", "RegisterClassExA", sh_RegisterClassExA },
    { "user32.dll", "RegisterClassExW", sh_RegisterClassExW },
    { "user32.dll", "DefWindowProcW", sh_DefWindowProcW },
    { "user32.dll", "DispatchMessageW", sh_DispatchMessageW },
    { "user32.dll", "PeekMessageW", sh_PeekMessageW },
    { "user32.dll", "SetWindowLongA", sh_SetWindowLongA },
    { "user32.dll", "GetWindowLongA", sh_GetWindowLongA },
    { "user32.dll", "SetWindowPos", sh_SetWindowPos },
    { "user32.dll", "SetWindowTextW", sh_SetWindowTextW },
    { "user32.dll", "GetDC", sh_GetDC },
    { "user32.dll", "ReleaseDC", sh_ReleaseDC },
    { "user32.dll", "SetCapture", sh_SetCapture },
    { "user32.dll", "ReleaseCapture", sh_ReleaseCapture },
    { "user32.dll", "ToAscii", sh_ToAscii },
    { "user32.dll", "EnumWindows", sh_EnumWindows },
    { "user32.dll", "GetProcessWindowStation", sh_GetProcessWindowStation },
    { "user32.dll", "GetUserObjectInformationW", sh_GetUserObjectInformationW },
    { "user32.dll", "GetDesktopWindow", sh_GetDesktopWindow },
    { "user32.dll", "GetActiveWindow", sh_GetActiveWindow },
    { "user32.dll", "SetActiveWindow", sh_SetActiveWindow },
    { "user32.dll", "GetClipboardData", sh_GetClipboardData },
    { "user32.dll", "SetDlgItemTextW", sh_SetDlgItemTextW },
    { "user32.dll", "GetMenu", sh_GetMenu },
    { "user32.dll", "CreateMenu", sh_CreateMenu },
    { "user32.dll", "ClipCursor", sh_ClipCursor },
    { "user32.dll", "SetTimer", sh_SetTimer },
    { "user32.dll", "KillTimer", sh_KillTimer },
    { "user32.dll", "MessageBoxA", sh_MessageBoxA },
    { "user32.dll", "MessageBoxW", sh_MessageBoxW },
    { "gdi32.dll", "GetStockObject", sh_GetStockObject },
    { "gdi32.dll", "CreatePen", sh_CreatePen },
    { "gdi32.dll", "MoveToEx", sh_MoveToEx },
    { "gdi32.dll", "LineTo", sh_LineTo },
    { "gdi32.dll", "Ellipse", sh_Ellipse },
    { "gdi32.dll", "TextOutA", sh_TextOutA },
    { "gdi32.dll", "CreateFontW", sh_CreateFontW },
    { "gdi32.dll", "GetTextMetricsA", sh_GetTextMetricsA },
    { "gdi32.dll", "GetTextExtentPoint32A", sh_GetTextExtentPoint32A },
    { "gdi32.dll", "GetGlyphOutlineW", sh_GetGlyphOutlineW },
    { NULL, NULL, NULL },
};

static const ShimDef USER32[] = {
    { "user32.dll", "RegisterClassA", sh_RegisterClassA },
    { "user32.dll", "UnregisterClassA", sh_UnregisterClassA },
    { "user32.dll", "AdjustWindowRectEx", sh_AdjustWindowRectEx },
    { "user32.dll", "AdjustWindowRect", sh_AdjustWindowRect },
    { "user32.dll", "CreateWindowExA", sh_CreateWindowExA },
    { "user32.dll", "DestroyWindow", sh_DestroyWindow },
    { "user32.dll", "DefWindowProcA", sh_DefWindowProcA },
    { "user32.dll", "ShowWindow", sh_ShowWindow },
    { "user32.dll", "UpdateWindow", sh_UpdateWindow },
    { "user32.dll", "GetWindowRect", sh_GetWindowRect },
    { "user32.dll", "GetClientRect", sh_GetClientRect },
    { "user32.dll", "SetRect", sh_SetRect },
    { "user32.dll", "ClientToScreen", sh_ClientToScreen },
    { "user32.dll", "ScreenToClient", sh_ScreenToClient },
    { "user32.dll", "GetCursorPos", sh_GetCursorPos },
    { "user32.dll", "GetSystemMetrics", sh_GetSystemMetrics },
    { "user32.dll", "GetSystemMenu", sh_GetSystemMenu },
    { "user32.dll", "InsertMenuItemA", sh_InsertMenuItemA },
    { "user32.dll", "LoadStringA", sh_LoadStringA },
    { "user32.dll", "LoadIconA", sh_LoadIconA },
    { "user32.dll", "LoadCursorA", sh_LoadCursorA },
    { "user32.dll", "LoadCursorFromFileA", sh_LoadCursorFromFileA },
    { "user32.dll", "SetCursor", sh_SetCursor },
    { "user32.dll", "ShowCursor", sh_ShowCursor },
    { "user32.dll", "SetWindowsHookExA", sh_SetWindowsHookExA },
    { "user32.dll", "UnhookWindowsHookEx", sh_UnhookWindowsHookEx },
    { "user32.dll", "CallNextHookEx", sh_CallNextHookEx },
    { "user32.dll", "GetKeyboardState", sh_GetKeyboardState },
    { "user32.dll", "GetKeyState", sh_GetKeyState },
    { "user32.dll", "GetAsyncKeyState", sh_GetAsyncKeyState },
    { "user32.dll", "GetKeyboardLayout", sh_GetKeyboardLayout },
    { "user32.dll", "GetForegroundWindow", sh_GetForegroundWindow },
    { "user32.dll", "GetFocus", sh_GetFocus },
    { "user32.dll", "SetFocus", sh_SetFocus },
    { "user32.dll", "SetForegroundWindow", sh_SetForegroundWindow },
    { "user32.dll", "IsWindow", sh_IsWindow },
    { "user32.dll", "IsIconic", sh_IsIconic },
    { "user32.dll", "IsWindowVisible", sh_IsWindowVisible },
    { "user32.dll", "PeekMessageA", sh_PeekMessageA },
    { "user32.dll", "GetMessageA", sh_GetMessageA },
    { "user32.dll", "TranslateMessage", sh_TranslateMessage },
    { "user32.dll", "DispatchMessageA", sh_DispatchMessageA },
    { "user32.dll", "SendMessageA", sh_SendMessageA },
    { "user32.dll", "PostMessageA", sh_PostMessageA },
    { "user32.dll", "PostThreadMessageA", sh_PostThreadMessageA },
    { "user32.dll", "PostQuitMessage", sh_PostQuitMessage },
    { "user32.dll", "GetQueueStatus", sh_GetQueueStatus },
    { "user32.dll", "MsgWaitForMultipleObjects", sh_MsgWaitForMultipleObjects },
    { "winmm.dll", "timeGetTime", sh_timeGetTime },
    { "winmm.dll", "timeBeginPeriod", sh_timeBeginPeriod },
    { "winmm.dll", "timeEndPeriod", sh_timeEndPeriod },
    { "winmm.dll", "timeGetDevCaps", sh_timeGetDevCaps },
    { "winmm.dll", "timeSetEvent", sh_timeSetEvent },
    { "winmm.dll", "timeKillEvent", sh_timeKillEvent },
    { "user32.dll", "GetWindowThreadProcessId", sh_GetWindowThreadProcessId },
    { "user32.dll", "keybd_event", sh_keybd_event },
    { "user32.dll", "MapVirtualKeyA", sh_MapVirtualKeyA },
    { "user32.dll", "FindWindowA", sh_FindWindowA },
    { "user32.dll", "wsprintfA", sh_wsprintfA },
    { "user32.dll", "wvsprintfA", sh_wvsprintfA },
    { "user32.dll", "RegisterWindowMessageA", sh_RegisterWindowMessageA },
    { "user32.dll", "RemoveMenu", sh_RemoveMenu },
    { "user32.dll", "AttachThreadInput", sh_AttachThreadInput },
    { "user32.dll", "SetCursorPos", sh_SetCursorPos },
    { "user32.dll", "OpenClipboard", sh_OpenClipboard },
    { "user32.dll", "EmptyClipboard", sh_EmptyClipboard },
    { "user32.dll", "CloseClipboard", sh_CloseClipboard },
    { "user32.dll", "SetClipboardData", sh_SetClipboardData },
    { "user32.dll", "SystemParametersInfoA", sh_SystemParametersInfoA },
    { "user32.dll", "GetWindowPlacement", sh_GetWindowPlacement },
    { "user32.dll", "MoveWindow", sh_MoveWindow },
    { "user32.dll", "DrawTextA", sh_DrawTextA },
    { "user32.dll", "DrawTextW", sh_DrawTextW },
    { "gdi32.dll", "CreateCompatibleDC", sh_CreateCompatibleDC },
    { "gdi32.dll", "DeleteDC", sh_DeleteDC },
    { "gdi32.dll", "CreateDIBSection", sh_CreateDIBSection },
    { "gdi32.dll", "CreateFontIndirectA", sh_CreateFontIndirectA },
    { "gdi32.dll", "SelectObject", sh_SelectObject },
    { "gdi32.dll", "DeleteObject", sh_DeleteObject },
    { "gdi32.dll", "GetObjectA", sh_GetObjectA },
    { "gdi32.dll", "SetBkMode", sh_SetBkMode },
    { "gdi32.dll", "SetBkColor", sh_SetBkColor },
    { "gdi32.dll", "SetTextColor", sh_SetTextColor },
    { "imm32.dll", "ImmGetContext", sh_ImmGetContext },
    { "imm32.dll", "ImmReleaseContext", sh_ImmReleaseContext },
    { "imm32.dll", "ImmGetOpenStatus", sh_ImmGetOpenStatus },
    { "imm32.dll", "ImmSetOpenStatus", sh_ImmSetOpenStatus },
    { "imm32.dll", "ImmNotifyIME", sh_ImmNotifyIME },
    { "imm32.dll", "ImmSetCandidateWindow", sh_ImmSetCandidateWindow },
    { "imm32.dll", "ImmSetCompositionWindow", sh_ImmSetCompositionWindow },
    { "imm32.dll", "ImmGetCandidateListA", sh_ImmGetCandidateListA },
    { "imm32.dll", "ImmGetCandidateListCountA", sh_ImmGetCandidateListCountA },
    { "imm32.dll", "ImmGetCompositionStringA", sh_ImmGetCompositionStringA },
    { "imm32.dll", "ImmSetCompositionStringA", sh_ImmSetCompositionStringA },
    { "imm32.dll", "ImmGetConversionStatus", sh_ImmGetConversionStatus },
    { "imm32.dll", "ImmSetConversionStatus", sh_ImmSetConversionStatus },
    { "imm32.dll", "ImmGetDescriptionA", sh_ImmGetDescriptionA },
    { "imm32.dll", "ImmGetVirtualKey", sh_ImmGetVirtualKey },
    { NULL, NULL, NULL },
};

void user32_init(void)
{
    thunk_register(USER32);
    thunk_register(USER32_XIV);
}

/* A key press and release to the focused window, as the keyboard would make them (tests: d3d9.c
 * XIV_AUTOKEYS). */
void user32_press_key(uint32_t vk)
{
    Wnd* w = wnd(g_focus);
    if (!w)
        for (int i = 0; i < MAX_WINDOWS && !w; ++i)
            if (g_wnds[i].used)
                w = &g_wnds[i];
    if (!w)
        return;
    if (!vk)
    {
        /* 0: the window activated, as when the player clicks it */
        g_focus = w->hwnd;
        post(w->tid, w->hwnd, WM_ACTIVATEAPP, 1, 0);
        post(w->tid, w->hwnd, WM_ACTIVATE, 1, 0);
        post(w->tid, w->hwnd, WM_SETFOCUS, 0, 0);
        return;
    }
    uint32_t scan = scan_of_vk(vk);
    post(w->tid, w->hwnd, WM_KEYDOWN, vk, 1u | ((scan & 0xFFu) << 16));
    post(w->tid, w->hwnd, WM_KEYUP, vk, 1u | ((scan & 0xFFu) << 16) | 0xC0000000u);
}

/* For autoinput.c: the game's window (the focused one, else the first with an SDL window), its
 * SDL window ID and size in points; 0 if there is none yet. */
uint32_t user32_game_window(int* w_out, int* h_out)
{
    Wnd* w = wnd(g_focus);
    if (!w || !w->sdl)
        w = NULL;
    for (int i = 0; i < MAX_WINDOWS && !w; ++i)
        if (g_wnds[i].used && g_wnds[i].sdl)
            w = &g_wnds[i];
    if (!w)
        return 0;
    int ww = 0, wh = 0;
    SDL_GetWindowSize((SDL_Window*)w->sdl, &ww, &wh);
    if (w_out)
        *w_out = ww;
    if (h_out)
        *h_out = wh;
    return (uint32_t)SDL_GetWindowID((SDL_Window*)w->sdl);
}

/* The SDL scancode that the pump turns into this virtual key (0 if none). */
int user32_scancode_of_vk(uint32_t vk)
{
    for (int sc = 1; sc < SDL_SCANCODE_COUNT; ++sc)
        if (vk_of((SDL_Scancode)sc) == vk)
            return sc;
    return 0;
}

void user32_force_cursor(int on, float x, float y)
{
    g_cursor_fx = x, g_cursor_fy = y;
    g_cursor_forced = on;
}

void* user32_sdl_window(uint32_t hwnd)
{
    Wnd* w = wnd(hwnd);
    return w ? w->sdl : NULL;
}

void user32_set_ui_aspect(float aspect) { g_ui_aspect = aspect; }

float user32_ui_aspect(void) { return g_ui_aspect; }

float user32_ui_squeeze(uint32_t hwnd)
{
    Wnd* w = wnd(hwnd);
    return w ? ui_squeeze(w) : 1.0f;
}

float user32_ui_squeeze_y(uint32_t hwnd)
{
    Wnd* w = wnd(hwnd);
    return w ? ui_squeeze_y(w) : 1.0f;
}

void user32_client_size(uint32_t hwnd, uint32_t* cw, uint32_t* ch)
{
    Wnd* w = wnd(hwnd);
    if (w)
        *cw = (uint32_t)w->w, *ch = (uint32_t)w->h;
}

void user32_desktop_mode(uint32_t* w, uint32_t* h, uint32_t* hz)
{
    *w = 1920, *h = 1080, *hz = 60;
    sdl_up();
    const SDL_DisplayMode* dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    if (dm)
    {
        *w = (uint32_t)dm->w, *h = (uint32_t)dm->h;
        if (dm->refresh_rate > 0)
            *hz = (uint32_t)(dm->refresh_rate + 0.5f);
    }
}
