/* Scripted input for unattended runs: XIV_INPUT_SCRIPT=<file>, a timeline of key, gamepad and mouse
 * actions played into the game as if a player made them. See docs/scripted-input.md.
 *
 * Keys and the mouse are synthesized SDL events, pushed onto SDL's queue: the window pump turns
 * them into what the game reads (window messages, GetKeyboardState, DirectInput's keyboard state
 * and buffer) exactly as it does real ones, so a key held here is held everywhere. The gamepad is
 * input.c's scripted pad 0 (XInput and DirectInput both), present from the start while a script is
 * set. Run from the presenting thread once a frame (d3d9.c present).
 *
 * Each line: <time> <command> [args]. Time is milliseconds since the first frame, or +ms after the
 * previous line (lines appended to the file while the game runs count from when they are read, so
 * `echo "+0 stick l 0 1 3000" >> script` walks forward for 3 s now). '#' starts a comment. */
#include <SDL3/SDL.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>

#include "gwin.h"
#include "input.h"
#include "runtime.h"
#include "user32.h"

enum
{
    A_KEY,    /* a = scancode, b = down */
    A_PAD,    /* a = button bits, b = down */
    A_STICK,  /* a = 0 left / 1 right, x y = -1..1 */
    A_TRIG,   /* a = 0 left / 1 right, x = 0..1 */
    A_MOVE,   /* x y = 0..1 of the window */
    A_BUTTON, /* a = SDL button, b = down */
    A_SHOT,   /* text = path */
    A_LOG,    /* text */
    A_QUIT,
    A_WALKTO, /* x y = target X Z, a = tolerance cm, b = timeout ms, text "via": no slowing down */
    A_FACE,   /* x y = point X Z */
    A_TURN,   /* x = degrees */
    A_POS,    /* log position */
    A_STOP,   /* end a running walkto/face/turn now */
    A_TRACE,  /* a = ms between steering log lines, 0 = off */
    A_WAITFOR,  /* a = 0 in / 1 out / 2 either, b = opcode, x = timeout ms */
    A_TARGET,   /* text = actor id, hex ("" = untarget), a = tries, b = key */
    A_INTERACT, /* b = key */
    A_NETTRACE, /* a = 1 on */
    A_FOLLOW,   /* pts/npts = path (x, z, y), a = tolerance cm, b = timeout ms */
    A_NAVSTATS, /* log and reset the walk statistics */
    A_NAVSET,   /* text = name, x = value */
};

typedef struct Act
{
    uint64_t t; /* ms since the first frame */
    uint32_t order;
    int kind, a, b;
    float x, y;
    char text[256];
    float* pts; /* A_FOLLOW: owned path, x z y per point */
    int npts;
} Act;

/* `path` lines collect points here until a `follow` takes them */
static float* g_pend;
static int g_pend_n, g_pend_cap;

static Act* g_acts;
static size_t g_n, g_cap;
static uint32_t g_order;
static uint64_t g_t0, g_last_t; /* g_last_t: the previous line's time, for +ms */
static FILE* g_file;
static char g_partial[8192];
static size_t g_plen;
static char g_shotdir[512];
static char g_shot_pending[600];
static XPad g_pad;
static float g_mx = 0.5f, g_my = 0.5f;
static int g_line;

static uint64_t now_ms(void) { return (rt_monotonic_ns() - g_t0) / 1000000u; }

static Act* add(uint64_t t, int kind)
{
    if (g_n == g_cap)
    {
        g_cap = g_cap ? g_cap * 2 : 64;
        g_acts = (Act*)realloc(g_acts, g_cap * sizeof *g_acts);
    }
    Act* a = &g_acts[g_n++];
    memset(a, 0, sizeof *a);
    a->t = t, a->kind = kind, a->order = g_order++;
    return a;
}

static const struct
{
    const char* name;
    uint16_t bit;
} PAD_BUTTONS[] = {
    { "up", 0x0001 },   { "down", 0x0002 },  { "left", 0x0004 }, { "right", 0x0008 }, { "start", 0x0010 },
    { "back", 0x0020 }, { "select", 0x0020 }, { "ls", 0x0040 },   { "rs", 0x0080 },    { "lb", 0x0100 },
    { "rb", 0x0200 },   { "a", 0x1000 },     { "b", 0x2000 },    { "x", 0x4000 },     { "y", 0x8000 },
};

static int pad_bit(const char* s)
{
    for (size_t i = 0; i < sizeof PAD_BUTTONS / sizeof PAD_BUTTONS[0]; ++i)
        if (!strcasecmp(s, PAD_BUTTONS[i].name))
            return PAD_BUTTONS[i].bit;
    return 0;
}

/* A key by virtual-key code (0x0D, 13) or SDL key name ("Return", "W", "Up", "Escape") */
static int scancode_of(const char* s)
{
    char* end;
    unsigned long vk = strtoul(s, &end, 0);
    if (*s && !*end && isdigit((unsigned char)*s) && (strlen(s) > 1 || vk > 9))
        return user32_scancode_of_vk((uint32_t)vk);
    SDL_Scancode sc = SDL_GetScancodeFromName(s);
    if (sc == SDL_SCANCODE_UNKNOWN && !strcasecmp(s, "enter"))
        sc = SDL_SCANCODE_RETURN;
    if (sc == SDL_SCANCODE_UNKNOWN && !strcasecmp(s, "esc"))
        sc = SDL_SCANCODE_ESCAPE;
    return (int)sc;
}

static void parse_line(char* line, int appended)
{
    g_line++;
    char* hash = strchr(line, '#');
    if (hash)
        *hash = 0;
    char* tok[160];
    int n = 0;
    for (char* p = strtok(line, " \t\r\n"); p && n < 160; p = strtok(NULL, " \t\r\n"))
        tok[n++] = p;
    if (n < 2)
        return;
    uint64_t t;
    if (tok[0][0] == '+')
    {
        uint64_t base = g_last_t;
        if (appended && now_ms() > base)
            base = now_ms();
        t = base + strtoull(tok[0] + 1, NULL, 0);
    }
    else
        t = strtoull(tok[0], NULL, 0);
    g_last_t = t;
    const char* cmd = tok[1];
    /* key K [hold] | down K | up K */
    if (!strcasecmp(cmd, "key") || !strcasecmp(cmd, "down") || !strcasecmp(cmd, "up"))
    {
        int sc = n > 2 ? scancode_of(tok[2]) : 0;
        if (!sc)
        {
            rt_log("[input] line %d: unknown key '%s'\n", g_line, n > 2 ? tok[2] : "");
            return;
        }
        int up = !strcasecmp(cmd, "up");
        Act* a = add(t, A_KEY);
        a->a = sc, a->b = !up;
        if (!strcasecmp(cmd, "key"))
        {
            a = add(t + (n > 3 ? strtoull(tok[3], NULL, 0) : 120), A_KEY);
            a->a = sc, a->b = 0;
        }
    }
    /* pad BUTTON [hold] | paddown BUTTON | padup BUTTON */
    else if (!strcasecmp(cmd, "pad") || !strcasecmp(cmd, "paddown") || !strcasecmp(cmd, "padup"))
    {
        int bit = n > 2 ? pad_bit(tok[2]) : 0;
        if (!bit)
        {
            rt_log("[input] line %d: unknown pad button '%s'\n", g_line, n > 2 ? tok[2] : "");
            return;
        }
        Act* a = add(t, A_PAD);
        a->a = bit, a->b = strcasecmp(cmd, "padup") != 0;
        if (!strcasecmp(cmd, "pad"))
        {
            a = add(t + (n > 3 ? strtoull(tok[3], NULL, 0) : 150), A_PAD);
            a->a = bit, a->b = 0;
        }
    }
    /* stick l|r X Y [hold]: -1..1, Y up; without hold it stays until the next stick command */
    else if (!strcasecmp(cmd, "stick") && n >= 5)
    {
        Act* a = add(t, A_STICK);
        a->a = tolower((unsigned char)tok[2][0]) == 'r';
        a->x = strtof(tok[3], NULL), a->y = strtof(tok[4], NULL);
        if (n > 5)
        {
            int side = a->a;
            a = add(t + strtoull(tok[5], NULL, 0), A_STICK);
            a->a = side;
        }
    }
    /* trigger l|r V [hold]: 0..1 */
    else if (!strcasecmp(cmd, "trigger") && n >= 4)
    {
        Act* a = add(t, A_TRIG);
        a->a = tolower((unsigned char)tok[2][0]) == 'r';
        a->x = strtof(tok[3], NULL);
        if (n > 4)
        {
            int side = a->a;
            a = add(t + strtoull(tok[4], NULL, 0), A_TRIG);
            a->a = side;
        }
    }
    /* move X Y | click X Y [left|right|middle] [hold]: X Y are 0..1 of the window across and down */
    else if ((!strcasecmp(cmd, "move") || !strcasecmp(cmd, "click")) && n >= 4)
    {
        Act* a = add(t, A_MOVE);
        a->x = strtof(tok[2], NULL), a->y = strtof(tok[3], NULL);
        if (!strcasecmp(cmd, "click"))
        {
            int b = n > 4 && !strcasecmp(tok[4], "right") ? SDL_BUTTON_RIGHT
                : n > 4 && !strcasecmp(tok[4], "middle")  ? SDL_BUTTON_MIDDLE
                                                          : SDL_BUTTON_LEFT;
            uint64_t hold = n > 5 ? strtoull(tok[5], NULL, 0) : 100;
            a = add(t + 50, A_BUTTON);
            a->a = b, a->b = 1;
            a = add(t + 50 + hold, A_BUTTON);
            a->a = b, a->b = 0;
        }
    }
    /* walkto X Z [tolerance] [timeout_ms] | face X Z | turn DEG | pos: navigation (nav.* below) */
    else if ((!strcasecmp(cmd, "walkto") || !strcasecmp(cmd, "via")) && n >= 4)
    {
        Act* a = add(t, A_WALKTO);
        int via = !strcasecmp(cmd, "via");
        if (via)
            snprintf(a->text, sizeof a->text, "via");
        a->x = strtof(tok[2], NULL), a->y = strtof(tok[3], NULL);
        a->a = (int)(100.0f * (n > 4 ? strtof(tok[4], NULL) : via ? 1.5f : 1.0f));
        a->b = n > 5 ? (int)strtol(tok[5], NULL, 0) : 60000;
        /* [Y]: also require the height (within 1.2) - a waypoint on a ramp or a floor above */
        snprintf(a->text + 4, sizeof a->text - 4, "%s", n > 6 ? tok[6] : "");
    }
    /* walkpath X1 Z1 X2 Z2 ...: `via` each point but the last, `walkto` the last */
    else if (!strcasecmp(cmd, "walkpath") && n >= 4)
    {
        for (int i = 2; i + 1 < n; i += 2)
        {
            Act* a = add(t, A_WALKTO);
            int last = i + 3 >= n;
            if (!last)
                snprintf(a->text, sizeof a->text, "via");
            a->x = strtof(tok[i], NULL), a->y = strtof(tok[i + 1], NULL);
            a->a = last ? 100 : 150;
            a->b = 30000;
        }
    }
    else if (!strcasecmp(cmd, "face") && n >= 4)
    {
        Act* a = add(t, A_FACE);
        a->x = strtof(tok[2], NULL), a->y = strtof(tok[3], NULL);
    }
    else if (!strcasecmp(cmd, "turn") && n >= 3)
        add(t, A_TURN)->x = strtof(tok[2], NULL);
    else if (!strcasecmp(cmd, "pos"))
        add(t, A_POS);
    else if (!strcasecmp(cmd, "stop"))
        add(t, A_STOP);
    /* waitfor in|out|any OPCODE [timeout_ms]: wait for a game packet (seen since the previous action) */
    else if (!strcasecmp(cmd, "waitfor") && n >= 4)
    {
        Act* a = add(t, A_WAITFOR);
        a->a = !strcasecmp(tok[2], "in") ? 0 : !strcasecmp(tok[2], "out") ? 1 : 2;
        a->b = (int)strtol(tok[3], NULL, 16);
        a->x = n > 4 ? strtof(tok[4], NULL) : 10000.0f;
    }
    /* target ID [tries] [key] | untarget [key] */
    else if (!strcasecmp(cmd, "target") && n >= 3)
    {
        Act* a = add(t, A_TARGET);
        snprintf(a->text, sizeof a->text, "%s", tok[2]);
        a->a = n > 3 ? atoi(tok[3]) : 20;
        a->b = n > 4 ? scancode_of(tok[4]) : SDL_SCANCODE_TAB;
    }
    else if (!strcasecmp(cmd, "untarget"))
    {
        Act* a = add(t, A_TARGET);
        a->a = 1;
        a->b = n > 2 ? scancode_of(tok[2]) : SDL_SCANCODE_ESCAPE;
    }
    /* interact [key]: confirm on the current target, wait for the client's EventStart (0x012D) */
    else if (!strcasecmp(cmd, "interact"))
        add(t, A_INTERACT)->b = n > 2 ? scancode_of(tok[2]) : SDL_SCANCODE_RETURN;
    /* choose N: menu item N (1 = the first): Down N-1 times, then Return */
    else if (!strcasecmp(cmd, "choose") && n >= 3)
    {
        int k = atoi(tok[2]);
        uint64_t at = t;
        for (int i = 1; i < k; ++i, at += 250)
        {
            Act* a = add(at, A_KEY);
            a->a = SDL_SCANCODE_DOWN, a->b = 1;
            a = add(at + 100, A_KEY);
            a->a = SDL_SCANCODE_DOWN, a->b = 0;
        }
        Act* a = add(at, A_KEY);
        a->a = SDL_SCANCODE_RETURN, a->b = 1;
        a = add(at + 120, A_KEY);
        a->a = SDL_SCANCODE_RETURN, a->b = 0;
        g_last_t = at + 120;
    }
    /* path X Z Y [X Z Y ...] | path clear: collect a route; follow [tol] [timeout_ms]: walk it */
    else if (!strcasecmp(cmd, "path") && n >= 3)
    {
        if (!strcasecmp(tok[2], "clear"))
            g_pend_n = 0;
        for (int i = 2; i + 2 < n; i += 3)
        {
            if (g_pend_n == g_pend_cap)
            {
                g_pend_cap = g_pend_cap ? g_pend_cap * 2 : 256;
                g_pend = (float*)realloc(g_pend, (size_t)g_pend_cap * 3 * sizeof(float));
            }
            float* q = g_pend + 3 * g_pend_n++;
            q[0] = strtof(tok[i], NULL), q[1] = strtof(tok[i + 1], NULL), q[2] = strtof(tok[i + 2], NULL);
        }
    }
    else if (!strcasecmp(cmd, "follow"))
    {
        if (g_pend_n < 1)
        {
            rt_log("[input] line %d: follow without a path\n", g_line);
            return;
        }
        Act* a = add(t, A_FOLLOW);
        a->pts = g_pend, a->npts = g_pend_n;
        g_pend = NULL, g_pend_n = g_pend_cap = 0;
        a->a = (int)(100.0f * (n > 2 ? strtof(tok[2], NULL) : 0.8f));
        a->b = n > 3 ? (int)strtol(tok[3], NULL, 0) : 180000;
    }
    else if (!strcasecmp(cmd, "navset") && n >= 4)
    {
        Act* a = add(t, A_NAVSET);
        snprintf(a->text, sizeof a->text, "%s", tok[2]);
        a->x = strtof(tok[3], NULL);
    }
    else if (!strcasecmp(cmd, "navstats"))
        add(t, A_NAVSTATS);
    else if (!strcasecmp(cmd, "nettrace") && n >= 3)
        add(t, A_NETTRACE)->a = atoi(tok[2]);
    else if (!strcasecmp(cmd, "navtrace") && n >= 3)
        add(t, A_TRACE)->a = atoi(tok[2]);
    else if (!strcasecmp(cmd, "shot") && n >= 3)
    {
        Act* a = add(t, A_SHOT);
        snprintf(a->text, sizeof a->text, "%s/%s.ppm", g_shotdir, tok[2]);
    }
    else if (!strcasecmp(cmd, "log"))
    {
        Act* a = add(t, A_LOG);
        for (int i = 2; i < n; ++i)
            snprintf(a->text + strlen(a->text), sizeof a->text - strlen(a->text), "%s%s", i > 2 ? " " : "", tok[i]);
    }
    else if (!strcasecmp(cmd, "quit"))
        add(t, A_QUIT);
    else
        rt_log("[input] line %d: unknown command '%s'\n", g_line, cmd);
}

/* New complete lines of the script (the whole file the first time, what was appended since after) */
static void read_script(int appended)
{
    if (!g_file)
        return;
    clearerr(g_file);
    int c;
    while ((c = fgetc(g_file)) != EOF)
    {
        if (c == '\n')
        {
            g_partial[g_plen] = 0;
            parse_line(g_partial, appended);
            g_plen = 0;
        }
        else if (g_plen + 1 < sizeof g_partial)
            g_partial[g_plen++] = (char)c;
    }
    if (!appended && g_plen) /* the last line of the file as written, without its newline */
    {
        g_partial[g_plen] = 0;
        parse_line(g_partial, appended);
        g_plen = 0;
    }
}

static void push_key(uint32_t win, int sc, int down)
{
    /* the modifiers the script holds, as a real key event carries them (Shift+F10 and the like) */
    static SDL_Keymod held;
    SDL_Keymod m = 0;
    switch (sc)
    {
    case SDL_SCANCODE_LSHIFT: m = SDL_KMOD_LSHIFT; break;
    case SDL_SCANCODE_RSHIFT: m = SDL_KMOD_RSHIFT; break;
    case SDL_SCANCODE_LCTRL: m = SDL_KMOD_LCTRL; break;
    case SDL_SCANCODE_RCTRL: m = SDL_KMOD_RCTRL; break;
    case SDL_SCANCODE_LALT: m = SDL_KMOD_LALT; break;
    case SDL_SCANCODE_RALT: m = SDL_KMOD_RALT; break;
    default: break;
    }
    held = down ? held | m : held & ~m;
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.timestamp = SDL_GetTicksNS();
    e.key.windowID = win;
    e.key.scancode = (SDL_Scancode)sc;
    e.key.key = SDL_GetKeyFromScancode((SDL_Scancode)sc, SDL_KMOD_NONE, false);
    e.key.mod = held;
    e.key.down = down != 0;
    SDL_PushEvent(&e);
}

static void push_mouse(uint32_t win, int ww, int wh, int button, int down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    float x = g_mx * (float)ww, y = g_my * (float)wh;
    if (!button)
    {
        static float px = -1, py = -1;
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.timestamp = SDL_GetTicksNS();
        e.motion.windowID = win;
        e.motion.x = x, e.motion.y = y;
        e.motion.xrel = px < 0 ? 0 : x - px, e.motion.yrel = py < 0 ? 0 : y - py;
        px = x, py = y;
    }
    else
    {
        e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.timestamp = SDL_GetTicksNS();
        e.button.windowID = win;
        e.button.button = (Uint8)button;
        e.button.down = down != 0;
        e.button.clicks = 1;
        e.button.x = x, e.button.y = y;
    }
    user32_force_cursor(1, x, y);
    SDL_PushEvent(&e);
}

static int16_t axis(float v)
{
    v = v < -1 ? -1 : v > 1 ? 1 : v;
    return (int16_t)(v * 32767.0f);
}

static int by_time(const void* p, const void* q)
{
    const Act *a = (const Act*)p, *b = (const Act*)q;
    return a->t != b->t ? (a->t < b->t ? -1 : 1) : (a->order < b->order ? -1 : a->order > b->order);
}


/* --- navigation ---------------------------------------------------------------------------------
 * walkto/face/turn steer the scripted left stick in a closed loop on the local player's position
 * and facing, read from guest memory each frame (docs/scripted-input.md; facts below
 * established with lldb on the live client, 2026-10-05):
 *
 *   [0x01356e10]                 actor manager (the object CharaElement+0x7c points at)
 *   mgr+0x1783c                  my player's actor id (FUN_004d7490)
 *   mgr+0x4ac+0x38 = mgr+0x4e4   std::map<u32 actorId, CharaElement*> (FUN_004d9970 -> FUN_004de190);
 *                                its head node at mgr+0x4e8. Node: left +0, parent +4, right +8,
 *                                key +0xc, value +0x10, isnil byte +0x15.
 *   CharaElement (vtable 0x00fa7c50): +0x88 actor id, +0x9c x, +0xa0 y, +0xa4 z (the position the
 *                                client reports in 0x00CA), +0xac rotation (radians; 0 faces +Z,
 *                                +pi/2 faces +X: direction = (sin r, cos r) in (x, z)).
 *
 * The left stick is camera-relative and mirrored against the rotation: holding the stick at angle
 * s (0 = up, +pi/2 = right) turns the character to r = c - s, where c is the camera's heading.
 * The camera is not read; c is estimated as r + s whenever the facing has settled while the
 * stick is held, so the camera following the player (or being turned) is tracked as it goes. */
#define NAV_MGR_GLOBAL 0x01356e10u
#define NAV_MGR_MYID 0x1783cu
#define NAV_MGR_MAPHEAD 0x4e8u
#define NAV_CHARA_VT 0x00fa7c50u
#define NAV_PI 3.14159265f

enum { NAV_NONE, NAV_WALK, NAV_FACE, NAV_WAIT, NAV_TARGET, NAV_INTERACT, NAV_FOLLOW };
static int g_nav_trace; /* `navtrace MS`: log the steering state every MS (0 = off) */

static struct
{
    int mode;
    float tx, tz, tol, heading; /* heading: NAV_FACE target rotation */
    uint64_t start, deadline, progress_t, detour_until, last_tick;
    int steering;
    float best;
    int detours, detour_side;
    float s;         /* stick angle held now */
    float c;         /* camera heading estimate */
    int have_c;
    float last_r;
    uint64_t last_c_t;
    uint32_t order_limit; /* actions queued before the nav started are shifted by its duration */
    /* waitfor / target / interact */
    int dir, key, tries, untarget, via, has_y;
    float ty;
    uint16_t op;
    uint32_t since, want_id;
    uint64_t pressed;
} g_nav;
static uint32_t g_win;
static int g_nav_held; /* a `via` ended with the stick still held: released unless a walk follows */

static int nav_rd32(uint32_t va, uint32_t* out)
{
#if defined(RT_GUEST_WINDOW)
    if (va < 0x10000u || !gwin_is_committed(va) || !gwin_is_committed(va + 3))
        return 0;
#else
    if (va < 0x10000u)
        return 0;
#endif
    memcpy(out, GUEST_PTR(va), 4);
    return 1;
}

static int nav_rdf(uint32_t va, float* out)
{
    uint32_t v;
    if (!nav_rd32(va, &v))
        return 0;
    memcpy(out, &v, 4);
    return isfinite(*out);
}

/* The CharaElement of an actor id (my player for id 0), or 0 */
static uint32_t element_of(uint32_t id)
{
    uint32_t mgr, my, head, node, v;
    if (!nav_rd32(NAV_MGR_GLOBAL, &mgr) || !nav_rd32(mgr + NAV_MGR_MYID, &my) || !nav_rd32(mgr + NAV_MGR_MAPHEAD, &head) ||
        !nav_rd32(head + 4, &node))
        return 0;
    if (!id)
        id = my;
    for (int i = 0; i < 64 && node && node != head; ++i)
    {
        uint32_t key;
        if (!nav_rd32(node + 0x14, &v) || (v >> 8 & 0xff) || !nav_rd32(node + 0xc, &key)) /* isnil (+0x15) */
            return 0;
        if (key == id)
        {
            uint32_t el, vt, eid;
            if (nav_rd32(node + 0x10, &el) && nav_rd32(el, &vt) && vt == NAV_CHARA_VT && nav_rd32(el + 0x88, &eid) &&
                eid == id)
                return el;
            return 0;
        }
        if (!nav_rd32(node + (id < key ? 0 : 8), &node))
            return 0;
    }
    return 0;
}

static uint32_t my_element(void) { return element_of(0); }

static int my_pos(float* x, float* y, float* z, float* r)
{
    uint32_t el = my_element();
    return el && nav_rdf(el + 0x9c, x) && nav_rdf(el + 0xa0, y) && nav_rdf(el + 0xa4, z) && nav_rdf(el + 0xac, r);
}

/* For xiv_probe.c: an actor's position, rotation and target from its CharaElement (id 0: my
 * player; id 0xFFFFFFFF: *target = my player's actor id). 0 if the client has no element for it. */
int autoinput_actor(uint32_t id, float* x, float* y, float* z, float* r, uint32_t* target)
{
    if (id == 0xFFFFFFFFu)
    {
        uint32_t mgr;
        return nav_rd32(NAV_MGR_GLOBAL, &mgr) && nav_rd32(mgr + NAV_MGR_MYID, target);
    }
    uint32_t el = element_of(id);
    if (!el)
        return 0;
    if (x && !(nav_rdf(el + 0x9c, x) && nav_rdf(el + 0xa0, y) && nav_rdf(el + 0xa4, z) && nav_rdf(el + 0xac, r)))
        return 0;
    if (target && !nav_rd32(el + 0x12c, target))
        *target = 0;
    return 1;
}

static float wrap(float a)
{
    while (a > NAV_PI)
        a -= 2 * NAV_PI;
    while (a < -NAV_PI)
        a += 2 * NAV_PI;
    return a;
}

/* `navset maxmag|minmag V`: limits on the stick magnitude while walking (walk at 0.5, say) */
static float g_nav_maxmag = 1.0f, g_nav_minmag = 0.0f;

static void nav_stick(float s, float mag)
{
    if (mag > 0)
        mag = mag > g_nav_maxmag ? g_nav_maxmag : mag < g_nav_minmag ? g_nav_minmag : mag;
    g_nav.s = s;
    g_pad.lx = axis(sinf(s) * mag), g_pad.ly = axis(cosf(s) * mag);
    input_vpad_set(&g_pad);
}

/* Steer toward heading h. The follow camera turns while the player walks, so the stick angle is
 * corrected continuously from the facing error (an integrator, time constant ~0.2 s, rate-limited
 * so a turn in progress does not wind it up) rather than computed once from a camera estimate. */
static void nav_steer(float r, float h, float dt, float mag)
{
    if (!g_nav.steering)
    {
        g_nav.steering = 1;
        g_nav.s = wrap(g_nav.c - h);
    }
    else
    {
        float step = wrap(r - h) * dt * 5.0f, lim = dt * 4.0f;
        g_nav.s = wrap(g_nav.s + (step > lim ? lim : step < -lim ? -lim : step));
    }
    nav_stick(g_nav.s, mag);
}

static void nav_begin(int mode, uint64_t now)
{
    float x, y, z, r;
    g_nav.mode = mode;
    g_nav.steering = 0, g_nav.last_tick = now;
    g_nav.start = g_nav.progress_t = now;
    g_nav.best = 1e30f;
    g_nav.detours = 0, g_nav.detour_until = 0;
    g_nav.order_limit = g_order;
    if (my_pos(&x, &y, &z, &r))
    {
        /* no recent estimate: assume the camera is behind the player (it follows while walking) */
        if (!g_nav.have_c || now - g_nav.last_c_t > 5000)
            g_nav.c = r, g_nav.have_c = 1;
        g_nav.last_r = r;
    }
}


/* --- walk statistics -----------------------------------------------------------------------------
 * A wall contact shows up as speed lost while the stick is held at full tilt and the character is
 * not turning (for the last 500 ms), and the client is not stalling: over each 250 ms window, speed under 60% of the running speed (a running average of
 * such windows, ~5/s) counts as one contact (a run of such windows is one). Logged as `contact at`,
 * summed in `navstats` and in the end line of walkto/follow. */
static struct
{
    float px, pz, vref, minmag, maxerr, dist;
    uint64_t t, start, last_call, maxgap;
    int in_contact, contacts, stucks, have, prev_full;
} g_mv;

static void mv_reset(void)
{
    float v = g_mv.vref;
    memset(&g_mv, 0, sizeof g_mv);
    g_mv.vref = v > 3.0f ? v : 5.0f;
}

static void mv_sample(uint64_t now, float x, float y, float z, float mag, float err)
{
    if (g_mv.vref < 3.0f)
        g_mv.vref = 5.0f;
    if (!g_mv.have)
    {
        g_mv.have = 1, g_mv.px = x, g_mv.pz = z, g_mv.t = now, g_mv.minmag = mag, g_mv.maxerr = err;
        g_mv.last_call = now, g_mv.maxgap = 0;
        if (!g_mv.start)
            g_mv.start = now;
        return;
    }
    if (now - g_mv.last_call > g_mv.maxgap)
        g_mv.maxgap = now - g_mv.last_call;
    g_mv.last_call = now;
    g_mv.minmag = mag < g_mv.minmag ? mag : g_mv.minmag;
    g_mv.maxerr = err > g_mv.maxerr ? err : g_mv.maxerr;
    if (now - g_mv.t < 250)
        return;
    float d = hypotf(x - g_mv.px, z - g_mv.pz), v = d * 1000.0f / (float)(now - g_mv.t);
    g_mv.dist += d;
    /* a frame over 80 ms in the window is the client stalling (a zone merge spawns dozens of
     * actors at once), not a wall */
    int full = g_mv.minmag >= 0.95f && g_mv.maxerr < 0.35f && g_mv.maxgap <= 80;
    if (full && v > 3.0f && v < 7.0f) /* running speed: a slow average of clean full-stick windows */
        g_mv.vref += 0.1f * (v - g_mv.vref);
    /* and the window before was at full stick too: speed lags the stick by a few hundred ms
     * (starting off, after slowing for a corner) */
    int slow = full && g_mv.prev_full && v < 0.6f * g_mv.vref;
    g_mv.prev_full = full;
    if (slow && !g_mv.in_contact)
    {
        g_mv.contacts++;
        rt_log("[input] %llu ms: contact at %.2f,%.2f,%.2f (%.1f/s of %.1f)\n", (unsigned long long)now, x, y, z, v, g_mv.vref);
    }
    g_mv.in_contact = slow;
    g_mv.px = x, g_mv.pz = z, g_mv.t = now, g_mv.minmag = 2, g_mv.maxerr = 0, g_mv.maxgap = 0;
}

/* --- follow: pure pursuit along a dense path ------------------------------------------------------
 * The path is the navmesh polyline (tools/input/route.py: a point every metre, pushed away from
 * walls). Each frame: find the closest point on the path near the current segment, aim at a point
 * further along it (as far as the straight line there stays on the path, see below), and slow
 * down where the path bends within the next few metres, so corners are taken on the path instead
 * of cut. No progress along the path for 0.9 s
 * is a stall (the client's collision has something the navmesh has not): the path is bent round
 * the spot ahead, the walk backs off for 0.4 s and goes on; 6 stalls end it as `stuck`. */
#define FOLLOW_LOOKAHEAD 2.5f
#define FOLLOW_CHORD 0.3f
#define FOLLOW_AVOID 1.1f
static struct
{
    float* p; /* x z y */
    float* cum; /* arc length at each point */
    int n, k;
    float along, best_along, best_dend, tol;
    uint64_t start, deadline, progress_t, backoff_until, hug_until;
    int stalls;
    float prev_r, prev_h, last_err;
    uint64_t settled_since;
    int have_cam;
} g_fol;

/* Steering for follow. The stick is camera-relative and the camera swings round behind the
 * character as it runs, so the stick angle for a heading drifts: holding a fixed sideways stick
 * walks a circle (measured). So the stick angle is closed-loop on the facing error (integral,
 * as nav_steer), plus feed-forward of the heading change: when the aim point swings by dh, the
 * stick swings by -dh in the same frame (the facing the stick asks for is camera - stick), so a
 * corner gets its turn at once instead of after the integrator has caught up and overshot. */
static void fol_steer(uint64_t now, float r, float h, float dt, float mag)
{
    (void)now;
    if (!g_fol.have_cam)
    {
        g_fol.have_cam = 1;
        g_nav.s = wrap(g_nav.c - h);
    }
    else
    {
        float step = wrap(r - h) * dt * 3.0f, lim = dt * 3.0f;
        g_nav.s = wrap(g_nav.s - wrap(h - g_fol.prev_h) + (step > lim ? lim : step < -lim ? -lim : step));
    }
    g_fol.prev_h = h;
    nav_stick(g_nav.s, mag);
}

static void fol_point(float s, float* x, float* z)
{
    int n = g_fol.n;
    if (s <= 0)
    {
        *x = g_fol.p[0], *z = g_fol.p[1];
        return;
    }
    int i0 = g_fol.k > 0 ? g_fol.k - 1 : 0;
    if (s < g_fol.cum[i0])
        i0 = 0;
    for (int i = i0; i + 1 < n; ++i)
        if (g_fol.cum[i + 1] >= s)
        {
            float len = g_fol.cum[i + 1] - g_fol.cum[i], t = len > 0 ? (s - g_fol.cum[i]) / len : 0;
            *x = g_fol.p[3 * i] + t * (g_fol.p[3 * i + 3] - g_fol.p[3 * i]);
            *z = g_fol.p[3 * i + 1] + t * (g_fol.p[3 * i + 4] - g_fol.p[3 * i + 1]);
            return;
        }
    *x = g_fol.p[3 * (n - 1)], *z = g_fol.p[3 * (n - 1) + 1];
}

/* closest point on segments [k0, k1) to (x, z): sets k, returns arc length, *off = distance */
static float fol_project(float x, float y, float z, int k0, int k1, float* off)
{
    float best = 1e30f, bs = 0;
    int bk = g_fol.k;
    for (int i = k0; i < k1 && i + 1 < g_fol.n; ++i)
    {
        const float *a = g_fol.p + 3 * i, *b = a + 3;
        float ex = b[0] - a[0], ez = b[1] - a[1], l2 = ex * ex + ez * ez;
        float t = l2 > 0 ? ((x - a[0]) * ex + (z - a[1]) * ez) / l2 : 0;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        float qx = a[0] + t * ex, qz = a[1] + t * ez, qy = a[2] + t * (b[2] - a[2]);
        float d = hypotf(x - qx, z - qz) + 0.5f * fabsf(y - qy); /* prefer the floor we are on */
        if (d < best)
            best = d, bk = i, bs = g_fol.cum[i] + t * sqrtf(l2);
    }
    g_fol.k = bk;
    *off = best;
    return bs;
}

static void fol_begin(Act* a, uint64_t now, float x, float y, float z)
{
    free(g_fol.p), free(g_fol.cum);
    memset(&g_fol, 0, sizeof g_fol);
    g_fol.p = a->pts, g_fol.n = a->npts;
    a->pts = NULL;
    g_fol.cum = (float*)calloc((size_t)g_fol.n, sizeof(float));
    for (int i = 1; i < g_fol.n; ++i)
        g_fol.cum[i] = g_fol.cum[i - 1] + hypotf(g_fol.p[3 * i] - g_fol.p[3 * i - 3], g_fol.p[3 * i + 1] - g_fol.p[3 * i - 2]);
    g_fol.tol = a->a / 100.0f;
    g_fol.prev_r = g_nav.last_r, g_fol.settled_since = now;
    g_fol.best_dend = 1e30f;
    g_fol.start = g_fol.progress_t = now;
    g_fol.deadline = now + (uint64_t)a->b;
    float off;
    g_fol.along = g_fol.best_along = fol_project(x, y, z, 0, g_fol.n, &off);
    rt_log("[input] %llu ms: follow %d points, %.1f long, from %.2f,%.2f,%.2f (%.2f off the path)\n", (unsigned long long)now,
        g_fol.n, g_fol.cum[g_fol.n - 1], x, y, z, off);
}

static int follow_tick(uint64_t now, float x, float y, float z, float r, float dt)
{
    int n = g_fol.n;
    float total = g_fol.cum[n - 1], off;
    float along = fol_project(x, y, z, g_fol.k > 2 ? g_fol.k - 2 : 0, g_fol.k + 10, &off);
    if (off > 4.0f) /* knocked off the path: find it again */
        along = fol_project(x, y, z, 0, n, &off);
    g_fol.along = along;
    float ex = g_fol.p[3 * (n - 1)] - x, ez = g_fol.p[3 * (n - 1) + 1] - z, dend = hypotf(ex, ez);
    const char* end = dend <= g_fol.tol && total - along < g_fol.tol + 1.0f ? "done" : now > g_fol.deadline ? "timeout" : NULL;
    if (!end && (along > g_fol.best_along + 0.3f || dend < g_fol.best_dend - 0.3f))
        g_fol.best_along = fmaxf(along, g_fol.best_along), g_fol.best_dend = fminf(dend, g_fol.best_dend),
        g_fol.progress_t = now;
    if (g_fol.last_err > 1.0f) /* turning round (at the start, in a hairpin) is not a stall */
        g_fol.progress_t = now;
    if (!end && now - g_fol.progress_t > 900 && now >= g_fol.backoff_until)
    {
        g_mv.stucks++;
        if (++g_fol.stalls > 6)
            end = "stuck";
        else
        {
            /* something the navmesh does not have (a rock edge, a post): it is just ahead, where we
             * are facing. Move the path out of its way (points within FOLLOW_AVOID of it, out to
             * that distance; more each time we stall here), back off, and go on. */
            float ox = x + 0.9f * sinf(r), oz = z + 0.9f * cosf(r);
            float rad = FOLLOW_AVOID + 0.3f * (float)(g_fol.stalls - 1);
            int moved = 0;
            for (int i = 1; i + 1 < n; ++i)
            {
                float* q = g_fol.p + 3 * i;
                float dx = q[0] - ox, dz = q[1] - oz, d = hypotf(dx, dz);
                if (d >= rad || fabsf(q[2] - y) > 2.0f)
                    continue;
                if (d < 0.05f) /* on it: go round the side away from where we came from */
                    dx = cosf(r), dz = -sinf(r), d = 1;
                q[0] = ox + dx / d * rad, q[1] = oz + dz / d * rad;
                moved++;
            }
            for (int i = 1; i < n; ++i)
                g_fol.cum[i] = g_fol.cum[i - 1] + hypotf(g_fol.p[3 * i] - g_fol.p[3 * i - 3], g_fol.p[3 * i + 1] - g_fol.p[3 * i - 2]);
            total = g_fol.cum[n - 1];
            g_fol.best_along = fol_project(x, y, z, 0, n, &off);
            g_fol.backoff_until = now + 400;
            g_fol.progress_t = now + 400;
            rt_log("[input] %llu ms: follow stalled at %.2f,%.2f,%.2f (%.1f of %.1f): obstacle near %.2f,%.2f, %d points moved "
                   "%.1f clear; back off\n",
                (unsigned long long)now, x, y, z, along, total, ox, oz, moved, rad);
        }
    }
    if (end)
    {
        nav_stick(0, 0);
        rt_log("[input] %llu ms: follow %s pos=%.2f,%.2f,%.2f dist=%.2f along=%.1f/%.1f contacts=%d stalls=%d (%llu ms)\n",
            (unsigned long long)now, end, x, y, z, dend, along, total, g_mv.contacts, g_fol.stalls,
            (unsigned long long)(now - g_fol.start));
        return 1;
    }
    if (now < g_fol.backoff_until)
    {
        /* step back toward the path behind us */
        float bx, bz;
        fol_point(along - 2.0f, &bx, &bz);
        float h = hypotf(bx - x, bz - z) > 0.3f ? atan2f(bx - x, bz - z) : wrap(r + NAV_PI);
        fol_steer(now, r, h, dt, 0.6f);
        return 0;
    }
    /* the longest lookahead (up to FOLLOW_LOOKAHEAD) whose straight line from here stays within
     * FOLLOW_CHORD of the path: on a straight it looks far and runs smooth, in a hairpin it shrinks
     * to the next half metre, so the inside of the bend (a railing, a door frame) is not cut */
    float look = now < g_fol.hug_until ? 1.0f : FOLLOW_LOOKAHEAD;
    float lx, lz, fx, fz;
    for (;; look -= 0.25f)
    {
        fol_point(along + look, &lx, &lz);
        if (look <= 0.5f)
            break;
        float cx = lx - x, cz = lz - z, c2 = cx * cx + cz * cz, worst = 0;
        for (float t = 0.25f; t < look; t += 0.25f)
        {
            float qx, qz;
            fol_point(along + t, &qx, &qz);
            float u = c2 > 0 ? ((qx - x) * cx + (qz - z) * cz) / c2 : 0;
            u = u < 0 ? 0 : u > 1 ? 1 : u;
            float dd = hypotf(qx - (x + u * cx), qz - (z + u * cz));
            worst = dd > worst ? dd : worst;
        }
        if (worst <= FOLLOW_CHORD)
            break;
    }
    fol_point(along + look + 2.5f, &fx, &fz);
    float h = atan2f(lx - x, lz - z);
    /* the bend ahead: between where we aim now and where the path goes after it */
    float bend = (hypotf(fx - lx, fz - lz) > 0.2f) ? fabsf(wrap(atan2f(fx - lx, fz - lz) - h)) : 0;
    float err = fabsf(wrap(r - h));
    g_fol.last_err = err;
    float mag = bend > 1.2f ? 0.55f : bend > 0.7f ? 0.75f : 1.0f;
    if (look < 1.25f) /* a tight bend right here */
        mag = fminf(mag, 0.5f);
    if (err > 0.9f)
        mag = mag < 0.45f ? mag : 0.45f;
    float left = total - along;
    if (left < 3.0f)
        mag = fminf(mag, 0.45f + 0.55f * left / 3.0f);
    fol_steer(now, r, h, dt, mag);
    mv_sample(now, x, y, z, mag, err);
    static uint64_t trace_t;
    if (g_nav_trace && now - trace_t >= (uint64_t)g_nav_trace)
    {
        trace_t = now;
        rt_log("[input] %llu ms: fol pos=%.2f,%.2f along=%.1f/%.1f off=%.2f k=%d r=%.3f h=%.3f look=%.2f bend=%.2f mag=%.2f\n",
            (unsigned long long)now, x, z, along, total, off, g_fol.k, r, h, look, bend, mag);
    }
    return 0;
}

/* One frame of a running walkto/face/turn. Returns 1 when it has finished. */
static int nav_tick(uint64_t now)
{
    float x, y, z, r;
    const char* what = g_nav.mode == NAV_WALK ? "walkto" : g_nav.mode == NAV_FOLLOW ? "follow" : "face";
    if (!my_pos(&x, &y, &z, &r))
    {
        if (now - g_nav.start < 3000) /* zoning: the element may come back */
            return 0;
        rt_log("[input] %llu ms: %s lost (no local player element)\n", (unsigned long long)now, what);
        return 1;
    }
    /* the camera heading as of the stick held now (exact once the facing has caught up) */
    float dt = (float)(now - g_nav.last_tick) / 1000.0f;
    dt = dt < 0 ? 0 : dt > 0.1f ? 0.1f : dt;
    g_nav.last_tick = now;
    if (g_pad.lx || g_pad.ly)
    {
        g_nav.c = wrap(r + g_nav.s);
        g_nav.last_c_t = now;
    }
    g_nav.last_r = r;

    if (g_nav.mode == NAV_FOLLOW)
        return follow_tick(now, x, y, z, r, dt);
    if (g_nav.mode == NAV_FACE)
    {
        float err = wrap(r - g_nav.heading);
        if (fabsf(err) < 0.06f || now > g_nav.deadline)
        {
            nav_stick(0, 0);
            rt_log("[input] %llu ms: face %s pos=%.2f,%.2f,%.2f rot=%.3f want=%.3f\n", (unsigned long long)now,
                fabsf(err) < 0.06f ? "done" : "timeout", x, y, z, r, g_nav.heading);
            return 1;
        }
        /* a light push toward the heading: turns the character with little travel */
        nav_steer(r, g_nav.heading, dt, 0.3f);
        return 0;
    }

    float dx = g_nav.tx - x, dz = g_nav.tz - z, d = sqrtf(dx * dx + dz * dz);
    int arrived = d <= g_nav.tol && (!g_nav.has_y || fabsf(y - g_nav.ty) < 1.2f);
    const char* end = arrived ? "done" : now > g_nav.deadline ? "timeout" : NULL;
    if (!end && d < g_nav.best - 0.5f)
        g_nav.best = d, g_nav.progress_t = now;
    if (!end && now - g_nav.progress_t > 2500 && now >= g_nav.detour_until)
    {
        if (g_nav.detours >= 6)
            end = "stuck";
        else
        {
            /* blocked: slide sideways for a moment, alternating sides, then aim again */
            g_nav.detour_side = (g_nav.detours & 1) ? -1 : 1;
            g_nav.detour_until = now + 900 + 300 * (uint64_t)g_nav.detours;
            g_nav.progress_t = now;
            g_nav.detours++;
            g_mv.stucks++;
            rt_log("[input] %llu ms: walkto blocked at %.2f,%.2f,%.2f (%.2f to go): detour %s\n", (unsigned long long)now,
                x, y, z, d, g_nav.detour_side > 0 ? "right" : "left");
        }
    }
    if (end)
    {
        if (g_nav.via && !strcmp(end, "done")) /* keep the stick: the next waypoint takes over */
        {
            g_nav_held = 1;
            rt_log("[input] %llu ms: via done pos=%.2f,%.2f,%.2f\n", (unsigned long long)now, x, y, z);
            return 1;
        }
        nav_stick(0, 0);
        rt_log("[input] %llu ms: %s %s pos=%.2f,%.2f,%.2f rot=%.3f dist=%.2f target=%.2f,%.2f (%llu ms)\n",
            (unsigned long long)now, g_nav.via ? "via" : "walkto", end, x, y, z, r, d, g_nav.tx, g_nav.tz, (unsigned long long)(now - g_nav.start));
        return 1;
    }
    float h = atan2f(dx, dz);
    if (now < g_nav.detour_until)
        h = wrap(h - g_nav.detour_side * NAV_PI * 0.55f); /* r = h - pi/2: to the character's right */
    float err = fabsf(wrap(r - h));
    float mag = d > 4 || g_nav.via ? 1.0f : 0.45f + 0.55f * d / 4;
    if (err > 1.2f) /* still turning: don't run a wide arc */
        mag = mag < 0.5f ? mag : 0.5f;
    nav_steer(r, h, dt, mag);
    if (now >= g_nav.detour_until)
        mv_sample(now, x, y, z, mag, err);
    static uint64_t trace_t;
    if (g_nav_trace && now - trace_t >= (uint64_t)g_nav_trace)
    {
        trace_t = now;
        rt_log("[input] %llu ms: nav pos=%.2f,%.2f r=%.3f h=%.3f c=%.3f s=%.3f mag=%.2f d=%.2f\n", (unsigned long long)now, x,
            z, r, h, g_nav.c, g_nav.s, mag, d);
    }
    return 0;
}


/* --- packet tap ---------------------------------------------------------------------------------
 * ws2.c hands over every byte of the game-server connection (both directions). Packets are split
 * into game messages (BasePacket 0x10 header: +1 compressed, +4 u16 size; subpackets: +0 u16 size,
 * +2 u16 type (3 = game message), +4 source actor, +8 target actor; game message: +0x12 u16
 * opcode, payload from +0x20) and kept in a small ring for `waitfor`, `target` and `interact`.
 * Compressed packets are skipped (Meteor does not compress game traffic). */
typedef struct NetMsg
{
    uint32_t seq;
    int out;
    uint16_t op;
    uint32_t src, plen;
    uint8_t pay[96];
} NetMsg;

#define NET_RING 512
static NetMsg g_net[NET_RING];
static uint32_t g_net_seq;  /* messages seen */
static uint32_t g_act_seq;  /* g_net_seq when the previous action ran */
static int g_net_trace;
static volatile uint32_t g_net_lock;

static struct
{
    uint8_t* buf;
    uint32_t len, cap;
} g_stream[64][2];

void xiv_probe_net(int out, uint16_t op, uint32_t src, const uint8_t* p, uint32_t n); /* xiv_probe.c */
void xiv_discord_net(int out, uint16_t op, uint32_t src, const uint8_t* p, uint32_t n); /* xiv_discord.c */
int xiv_discord_on(void);

static void net_msg(int out, const uint8_t* sub, uint32_t size)
{
    NetMsg* m = &g_net[g_net_seq % NET_RING];
    memset(m, 0, sizeof *m);
    m->out = out;
    memcpy(&m->op, sub + 0x12, 2);
    memcpy(&m->src, sub + 4, 4);
    m->plen = size - 0x20;
    memcpy(m->pay, sub + 0x20, m->plen < sizeof m->pay ? m->plen : sizeof m->pay);
    m->seq = ++g_net_seq;
    xiv_probe_net(out, m->op, m->src, sub + 0x20, size - 0x20);
    xiv_discord_net(out, m->op, m->src, sub + 0x20, size - 0x20);
    if (g_net_trace && m->op != 0x00CA && m->op != 0x00CF && m->op != 0x0001 && m->op != 0x0002)
    {
        char hex[3 * 24 + 1] = "";
        for (uint32_t i = 0; i < m->plen && i < 24; ++i)
            snprintf(hex + 3 * i, 4, "%02x ", m->pay[i]);
        /* Unix ms, the clock the server's packet capture stamps, so the two line up. */
        struct timeval now;
        gettimeofday(&now, NULL);
        const unsigned long long ms = (unsigned long long)now.tv_sec * 1000ULL + (unsigned long long)now.tv_usec / 1000ULL;
        rt_log("[net] t=%llu %s 0x%04X src=0x%08X len=%u %s\n", ms, out ? "out" : "in ", m->op, m->src, m->plen, hex);
    }
}

void autoinput_net(int stream, int out, const uint8_t* p, uint32_t n)
{
    /* also for the Discord presence, which reads the player's name, class and level here */
    if (stream < 0 || stream >= 64 || !(getenv("XIV_INPUT_SCRIPT") || xiv_discord_on()))
        return;
    while (__atomic_exchange_n(&g_net_lock, 1, __ATOMIC_ACQUIRE))
        ;
    __typeof__(g_stream[0][0])* st = &g_stream[stream][out != 0];
    if (st->len + n > st->cap)
    {
        st->cap = (st->len + n) * 2;
        st->buf = (uint8_t*)realloc(st->buf, st->cap);
    }
    memcpy(st->buf + st->len, p, n);
    st->len += n;
    uint32_t off = 0;
    while (st->len - off >= 0x10)
    {
        const uint8_t* b = st->buf + off;
        uint16_t size;
        memcpy(&size, b + 4, 2);
        if (size < 0x10)
        {
            off = st->len; /* lost sync: drop what we have */
            break;
        }
        if (st->len - off < size)
            break;
        if (!b[1])
            for (uint32_t q = 0x10; q + 0x10 <= size;)
            {
                uint16_t ss, type;
                memcpy(&ss, b + q, 2);
                memcpy(&type, b + q + 2, 2);
                if (ss < 0x10 || q + ss > size)
                    break;
                if (type == 3 && ss >= 0x20)
                    net_msg(out, b + q, ss);
                q += ss;
            }
        off += size;
    }
    memmove(st->buf, st->buf + off, st->len - off);
    st->len -= off;
    __atomic_store_n(&g_net_lock, 0, __ATOMIC_RELEASE);
}

/* The newest message after `since` matching dir (0 in, 1 out, 2 either) and opcode */
static const NetMsg* net_find(uint32_t since, int dir, uint16_t op)
{
    for (uint32_t s = g_net_seq; s > since && g_net_seq - s < NET_RING; --s)
    {
        const NetMsg* m = &g_net[(s - 1) % NET_RING];
        if (m->seq == s && m->op == op && (dir == 2 || m->out == dir))
            return m;
    }
    return NULL;
}

static uint32_t rd_u32(const uint8_t* p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* One frame of waitfor / target / interact; 1 when finished */
static int busy_tick(uint64_t now)
{
    if (g_nav.mode == NAV_WALK || g_nav.mode == NAV_FACE || g_nav.mode == NAV_FOLLOW)
        return nav_tick(now);
    static uint64_t down_since;
    static int down_key;
    if (down_key && now - down_since >= 100)
    {
        push_key(g_win, down_key, 0);
        down_key = 0;
    }
#define PRESS(k) (push_key(g_win, (k), 1), down_key = (k), down_since = now, g_nav.pressed = now, g_nav.since = g_net_seq)
    if (g_nav.mode == NAV_WAIT)
    {
        const NetMsg* m = net_find(g_nav.since, g_nav.dir, g_nav.op);
        if (m || now > g_nav.deadline)
        {
            rt_log("[input] %llu ms: waitfor %s 0x%04X %s%s (%llu ms)\n", (unsigned long long)now,
                g_nav.dir == 0 ? "in" : g_nav.dir == 1 ? "out" : "any", g_nav.op, m ? "seen" : "timeout",
                m ? (m->out ? " (sent)" : " (received)") : "", (unsigned long long)(now - g_nav.start));
            return 1;
        }
        return 0;
    }
    if (g_nav.mode == NAV_INTERACT)
    {
        if (!g_nav.pressed)
            PRESS(g_nav.key);
        const NetMsg* m = net_find(g_nav.since, 1, 0x012D);
        if (m || now > g_nav.deadline)
        {
            char ev[40] = "";
            if (m)
                for (uint32_t i = 0x10; i + 3 < m->plen && i + 3 < sizeof m->pay; ++i)
                    if (isalpha(m->pay[i]) && isalnum(m->pay[i + 1]) && isalnum(m->pay[i + 2]))
                    {
                        snprintf(ev, sizeof ev, "%.*s", (int)(sizeof m->pay - i), (const char*)m->pay + i);
                        break;
                    }
            rt_log("[input] %llu ms: interact %s%s%s target=0x%08X\n", (unsigned long long)now, m ? "started " : "no event",
                ev, "", m ? rd_u32(m->pay + 4) : 0);
            return 1;
        }
        return 0;
    }
    /* NAV_TARGET: press the key, look at the SetTarget (0x00CD) the client sends, repeat */
    if (!g_nav.pressed || (now - g_nav.pressed > 800 && g_nav.tries > 0))
    {
        if (!g_nav.pressed && !g_nav.untarget)
        {
            uint32_t el = my_element(), cur = 0;
            if (el && nav_rd32(el + 0x12c, &cur) && cur == g_nav.want_id && g_nav.want_id)
            {
                rt_log("[input] %llu ms: target done id=0x%08X (already)\n", (unsigned long long)now, cur);
                return 1;
            }
        }
        g_nav.tries--;
        PRESS(g_nav.key);
        return 0;
    }
    const NetMsg* m = net_find(g_nav.since, 1, 0x00CD);
    if (m && m->plen >= 4)
    {
        uint32_t id = rd_u32(m->pay);
        int ok = g_nav.untarget ? id == 0xC0000000u : id == g_nav.want_id;
        g_nav.since = m->seq; /* this one is handled */
        if (ok)
        {
            rt_log("[input] %llu ms: %s done id=0x%08X\n", (unsigned long long)now, g_nav.untarget ? "untarget" : "target", id);
            return 1;
        }
        rt_log("[input] %llu ms: target: got 0x%08X, next\n", (unsigned long long)now, id);
        if (g_nav.tries > 0)
            g_nav.pressed = now - 801; /* press again now */
    }
    if (now - g_nav.pressed > 800 && g_nav.tries <= 0)
    {
        rt_log("[input] %llu ms: %s not found\n", (unsigned long long)now, g_nav.untarget ? "untarget" : "target");
        return 1;
    }
    return 0;
#undef PRESS
}

const char* autoinput_frame(void)
{
    static int init;
    if (!init)
    {
        init = 1;
        const char* path = getenv("XIV_INPUT_SCRIPT");
        if (!path || !*path)
            return NULL;
        g_t0 = rt_monotonic_ns();
        g_file = fopen(path, "r");
        if (!g_file)
        {
            rt_log("[input] cannot open XIV_INPUT_SCRIPT %s\n", path);
            return NULL;
        }
        const char* dir = getenv("XIV_INPUT_SHOTS");
        if (dir && *dir)
            snprintf(g_shotdir, sizeof g_shotdir, "%s", dir);
        else
        {
            snprintf(g_shotdir, sizeof g_shotdir, "%s", path);
            char* slash = strrchr(g_shotdir, '/');
            if (slash)
                *slash = 0;
            else
                snprintf(g_shotdir, sizeof g_shotdir, ".");
        }
        read_script(0);
        rt_log("[input] script %s: %zu actions\n", path, g_n);
    }
    if (!g_file)
        return NULL;
    static uint64_t last_read;
    uint64_t now = now_ms();
    if (now - last_read >= 100) /* lines appended since */
    {
        last_read = now;
        read_script(1);
    }
    if (!g_n && !g_nav.mode && !g_nav_held)
        return NULL;
    qsort(g_acts, g_n, sizeof *g_acts, by_time);
    int ww = 0, wh = 0;
    uint32_t win = user32_game_window(&ww, &wh);
    if (!win)
        return NULL;
    g_win = win;
    size_t done = 0;
    int pad_changed = 0;
    const char* shot = NULL;
    if (g_nav.mode)
    {
        /* the timeline waits for the blocking command; only `stop` and `quit` get through */
        int stop = 0;
        for (size_t i = 0; i < g_n && g_acts[i].t <= now; ++i)
            if (g_acts[i].kind == A_QUIT)
            {
                rt_log("[input] %llu ms: quit (during a blocking command)\n", (unsigned long long)now);
                fflush(NULL);
                _Exit(0);
            }
            else if (g_acts[i].kind == A_STOP)
            {
                stop = 1;
                g_acts[i].kind = A_LOG, snprintf(g_acts[i].text, sizeof g_acts[i].text, "stop");
            }
        if (stop)
        {
            if (g_nav.mode == NAV_WALK || g_nav.mode == NAV_FACE || g_nav.mode == NAV_FOLLOW)
                nav_stick(0, 0);
            rt_log("[input] %llu ms: stopped\n", (unsigned long long)now);
        }
        if (!stop && !busy_tick(now))
            return NULL;
        uint64_t paused = now - g_nav.start;
        for (size_t i = 0; i < g_n; ++i)
            if (g_acts[i].order < g_nav.order_limit)
                g_acts[i].t += paused;
        g_last_t += paused;
        g_nav.mode = NAV_NONE;
        qsort(g_acts, g_n, sizeof *g_acts, by_time);
    }
    for (; done < g_n && g_acts[done].t <= now && !g_nav.mode; ++done)
    {
        Act* a = &g_acts[done];
        /* waitfor matches packets since the last action that could have caused one */
        if (a->kind != A_WAITFOR && a->kind != A_SHOT && a->kind != A_LOG && a->kind != A_POS &&
            a->kind != A_TRACE && a->kind != A_NETTRACE && a->kind != A_MOVE)
            g_act_seq = g_net_seq;
        switch (a->kind)
        {
        case A_FOLLOW:
        {
            float x, y, z, r;
            if (!my_pos(&x, &y, &z, &r))
            {
                rt_log("[input] %llu ms: follow: no local player element (not in world?)\n", (unsigned long long)now);
                free(a->pts), a->pts = NULL;
                break;
            }
            nav_begin(NAV_FOLLOW, now);
            fol_begin(a, now, x, y, z);
            break;
        }
        case A_NAVSET:
            if (!strcasecmp(a->text, "maxmag"))
                g_nav_maxmag = a->x;
            else if (!strcasecmp(a->text, "minmag"))
                g_nav_minmag = a->x;
            else
                rt_log("[input] navset: unknown %s\n", a->text);
            break;
        case A_NAVSTATS:
            rt_log("[input] %llu ms: navstats contacts=%d stalls=%d dist=%.1f time=%llu ms\n", (unsigned long long)now,
                g_mv.contacts, g_mv.stucks, g_mv.dist, (unsigned long long)(g_mv.start ? now - g_mv.start : 0));
            mv_reset();
            break;
        case A_WALKTO:
        case A_FACE:
        case A_TURN:
        {
            nav_begin(a->kind == A_WALKTO ? NAV_WALK : NAV_FACE, now);
            float x, y, z, r;
            if (!my_pos(&x, &y, &z, &r))
            {
                rt_log("[input] %llu ms: %s: no local player element (not in world?)\n", (unsigned long long)now,
                    a->kind == A_WALKTO ? "walkto" : "face");
                g_nav.mode = NAV_NONE;
                break;
            }
            g_nav.tx = a->x, g_nav.tz = a->y;
            if (a->kind == A_WALKTO)
            {
                g_nav.tol = a->a / 100.0f;
                g_nav.via = a->text[0] == 'v';
                g_nav.has_y = a->text[4] != 0;
                g_nav.ty = g_nav.has_y ? strtof(a->text + 4, NULL) : 0;
                g_nav.deadline = now + (uint64_t)a->b;
                rt_log("[input] %llu ms: %s %.2f,%.2f (tol %.2f) from %.2f,%.2f,%.2f\n", (unsigned long long)now,
                    g_nav.via ? "via" : "walkto", a->x, a->y, g_nav.tol, x, y, z);
            }
            else
            {
                g_nav.heading = a->kind == A_TURN ? wrap(r + a->x * NAV_PI / 180.0f) : atan2f(a->x - x, a->y - z);
                g_nav.deadline = now + 3000;
                rt_log("[input] %llu ms: face rot %.3f -> %.3f\n", (unsigned long long)now, r, g_nav.heading);
            }
            /* the walk starts on the next frame; later actions wait for it (shifted when it ends) */
            break;
        }
        case A_POS:
        {
            float x, y, z, r;
            if (my_pos(&x, &y, &z, &r))
                rt_log("[input] %llu ms: pos=%.2f,%.2f,%.2f rot=%.3f el=0x%08x\n", (unsigned long long)now, x, y, z, r,
                    my_element());
            else
                rt_log("[input] %llu ms: pos: no local player element\n", (unsigned long long)now);
            break;
        }
        case A_WAITFOR:
            g_nav.mode = NAV_WAIT, g_nav.start = now, g_nav.order_limit = g_order;
            g_nav.dir = a->a, g_nav.op = (uint16_t)a->b, g_nav.since = g_act_seq;
            g_nav.deadline = now + (uint64_t)a->x;
            break;
        case A_TARGET:
        case A_INTERACT:
        {
            g_nav.mode = a->kind == A_TARGET ? NAV_TARGET : NAV_INTERACT;
            g_nav.start = now, g_nav.order_limit = g_order, g_nav.pressed = 0;
            g_nav.key = a->b, g_nav.tries = a->a, g_nav.deadline = now + 3000;
            g_nav.untarget = a->kind == A_TARGET && !a->text[0];
            g_nav.want_id = 0;
            if (a->kind == A_TARGET && a->text[0])
            {
                char* end;
                unsigned long id = strtoul(a->text, &end, 16);
                if (*end || !id)
                {
                    /* NPC names are not in the client's CharaElement (sheet text, held on the Lua
                     * side): look the id up with meteor_actors */
                    rt_log("[input] %llu ms: target %s: give the actor id in hex\n", (unsigned long long)now, a->text);
                    g_nav.mode = NAV_NONE;
                    break;
                }
                g_nav.want_id = (uint32_t)id;
            }
            rt_log("[input] %llu ms: %s %s\n", (unsigned long long)now,
                a->kind == A_INTERACT ? "interact" : g_nav.untarget ? "untarget" : "target", a->text);
            break;
        }
        case A_NETTRACE: g_net_trace = a->a; break;
        case A_STOP: break;
        case A_TRACE: g_nav_trace = a->a; break;
        case A_KEY: push_key(win, a->a, a->b); break;
        case A_PAD:
            g_pad.buttons = a->b ? (uint16_t)(g_pad.buttons | a->a) : (uint16_t)(g_pad.buttons & ~a->a);
            pad_changed = 1;
            break;
        case A_STICK:
            if (a->a)
                g_pad.rx = axis(a->x), g_pad.ry = axis(a->y);
            else
                g_pad.lx = axis(a->x), g_pad.ly = axis(a->y);
            pad_changed = 1;
            break;
        case A_TRIG:
        {
            float v = a->x < 0 ? 0 : a->x > 1 ? 1 : a->x;
            if (a->a)
                g_pad.rt = (uint8_t)(v * 255.0f);
            else
                g_pad.lt = (uint8_t)(v * 255.0f);
            pad_changed = 1;
            break;
        }
        case A_MOVE:
            g_mx = a->x, g_my = a->y;
            push_mouse(win, ww, wh, 0, 0);
            break;
        case A_BUTTON: push_mouse(win, ww, wh, a->a, a->b); break;
        case A_SHOT:
            snprintf(g_shot_pending, sizeof g_shot_pending, "%s", a->text);
            shot = g_shot_pending;
            break;
        case A_LOG: break;
        case A_QUIT:
            rt_log("[input] %llu ms: quit\n", (unsigned long long)now);
            fflush(NULL);
            _Exit(0);
        }
        if (a->kind >= A_WALKTO)
            ;
        else if (a->kind == A_LOG)
            rt_log("[input] %llu ms: %s\n", (unsigned long long)now, a->text);
        else if (a->kind != A_MOVE && a->kind != A_SHOT)
            rt_log("[input] %llu ms: %s %d %d %.2f %.2f\n", (unsigned long long)now,
                (const char*[]){ "key", "pad", "stick", "trigger", "move", "button", "shot", "log", "quit" }[a->kind], a->a,
                a->b, a->x, a->y);
    }
    if (g_nav_held && g_nav.mode != NAV_WALK)
    {
        g_nav_held = 0;
        if (!g_nav.mode)
            nav_stick(0, 0);
    }
    if (pad_changed)
        input_vpad_set(&g_pad);
    if (done)
    {
        memmove(g_acts, g_acts + done, (g_n - done) * sizeof *g_acts);
        g_n -= done;
    }
    return shot;
}
