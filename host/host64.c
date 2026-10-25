/* The game host for 64-bit machines (R3: Windows x64 now, arm64 macOS next).
 *
 * What the retail launcher and COM do for FFXI on Windows, with no x86 anywhere: the retail
 * FFXiMain.dll and FFXi.dll are mapped into the guest window (FFXiMain at its preferred
 * 0x10000000, FFXi.dll - which prefers the same base - relocated), both CRTs start, FFXi.dll's
 * FFXiEntry is created through its class factory, and IFFXiEntry::GameStart runs the game with
 * our own gamecore (runtime/portable/gamecore.c) in place of the retail core library.
 *
 * usage: host64 --game <FINAL FANTASY XI folder> [--reg <file.reg>]... [--reg-overlay <file.reg>]
 *               [--reg-final <file.reg>]...   loaded after the overlay: a launcher's settings
 *               [--data-dir <folder>]   where host64 writes its own files (default: beside it)
 *               [--server <name or a.b.c.d>]
 *               [--user <name> [--pass <password>] [--otp <code>] [--login-token <t>]
 *                [--authport 54231] [--dataport 54230] [--viewport 54001]]
 *               [--dats <folder>]...   DAT overlays, as XIPivot: the first folder given wins
 *               [--textures <folder>]...   texture packs: high-resolution replacements for the
 *                                      game's textures (tools/make_texpack.py); default <data dir>/textures
 *               [--user-dir <folder>]  the game's USER folder (settings, macros) there instead of
 *                                      in the install, for installs that cannot be written (UWP)
 *
 * --server is where the game's servers are: the lobby and every other host under the game's
 * domain resolve to it instead of through DNS. Default 127.0.0.1 (this machine); --lobby is an
 * older name for it.
 *
 * --fps-divisor: FFXI's frames are 60 / divisor per second; 1 (60 fps) here, 2 (30) as shipped.
 *
 * --aspect auto|off|<w:h>: the 3D scene's aspect ratio. auto (the default) follows the window's
 * shape, so a widescreen window shows more to the sides instead of a 4:3 view stretched across it.
 *
 * --ui-aspect <w:h>: the interface keeps this shape (16:9, say) centered in a wider or taller window, instead
 * of being stretched across it; the mouse is mapped to match. Off by default; an app bundle's
 * FFXIUIAspect key is the default.
 *
 * --nameplates fix|off: the names over characters' heads keep the shape they have in a 4:3 window
 * (fix, the default) or widen with the window as the game draws them (off).
 * --nameplate-scale <s>|<sx>x<sy>: their size, 1 as the game draws them (1.25; 1x1.2 for taller).
 * An app bundle's FFXINameplates and FFXINameplateScale keys are the defaults for both.
 *
 * --draw-distance <k>|<world>x<characters>: how far out the world and characters are drawn, as a
 * factor of the game's own distances (1 as shipped; 3, or 3x1.5). An app bundle's FFXIDrawDistance
 * key is the default.
 *
 * Signing in (host/lsb_login.c): --user signs in on the LandSandBoat server's auth port first. The password comes from --pass, else FFXI_PASSWORD, else the
 *     sign-in screen; --otp is the two-factor code, if the account has one. --login-token is a
 *     launch token from the server's own launcher, in place of the password and code.
 *
 * Without --user, the sign-in screen (host/signin.c) comes first, in the game's own UI art, with the
 * server in its Settings; it remembers them in <data dir>/signin.cfg
 * (--data-dir, else the user's app data), the password in the keychain, and writes display
 * defaults to <data dir>/settings.reg, loaded when no --reg-final is given. Its window becomes the
 * game's. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gthread.h"
#include "gwin.h"
#include "k32.h"
#include "pe.h"
#include "gamecore.h"
#include "gamecore_config.h"
#include "lsb_login.h"
#include "signin.h"
#include "appdefaults.h"
#include "modern.h"
#include "discord.h"
#include "addons/addons.h"
#if defined(_WIN32)
#include "sampler.h"
#endif
#include "thunk.h"
#include "user32.h"
#include "d3d8.h"
#include "gfx.h"
#include "dsound.h"
#include "dinput.h"
#include "ws2.h"
#include "plat.h"
#include "vfs.h"
#include "build.h" /* FFXI_VERSION */

extern const RtModule rt_module_ffxi; /* recomp.py --module ffxi */

#define FFXI_BASE 0x0F000000u /* where FFXi.dll goes: free, below FFXiMain */
#define DEFAULT_GAME_SERVER 0x7F000001u /* 127.0.0.1: a server on this machine */

/* FFXiEntry {989D790D-6236-11D4-80E9-00105A81E890}, IFFXiEntry {989D790C-...} */
static const uint8_t CLSID_FFXiEntry[16] = { 0x0D, 0x79, 0x9D, 0x98, 0x36, 0x62, 0xD4, 0x11,
                                             0x80, 0xE9, 0x00, 0x10, 0x5A, 0x81, 0xE8, 0x90 };
static const uint8_t IID_IFFXiEntry[16] = { 0x0C, 0x79, 0x9D, 0x98, 0x36, 0x62, 0xD4, 0x11,
                                            0x80, 0xE9, 0x00, 0x10, 0x5A, 0x81, 0xE8, 0x90 };
/* GameMain {1027DC46-750D-4B1F-8834-1D25B8BEBAB8} (FFXiMain), FxFileManager
 * {0DF0E951-D03C-4A94-90EF-40AE60668F5F} (FFXi.dll): the classes FFXi.dll creates */
static const uint8_t CLSID_GameMain[16] = { 0x46, 0xDC, 0x27, 0x10, 0x0D, 0x75, 0x1F, 0x4B,
                                            0x88, 0x34, 0x1D, 0x25, 0xB8, 0xBE, 0xBA, 0xB8 };
static const uint8_t CLSID_FxFileManager[16] = { 0x51, 0xE9, 0xF0, 0x0D, 0x3C, 0xD0, 0x94, 0x4A,
                                                 0x90, 0xEF, 0x40, 0xAE, 0x60, 0x66, 0x8F, 0x5F };
static const uint8_t IID_IClassFactory[16] = { 0x01, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };

static uint32_t guest_bytes(const void* p, uint32_t n)
{
    uint32_t a = gheap_alloc(n, 1);
    memcpy(GUEST_PTR(a), p, n);
    return a;
}

static uint32_t com_call(uint32_t obj, unsigned slot, unsigned nargs, const uint32_t* args)
{
    uint32_t all[8] = { obj };
    for (unsigned i = 0; i < nargs && i < 7; ++i)
        all[i + 1] = args[i];
    return guest_call(rd32(rd32(obj) + 4u * slot), nargs + 1, all);
}

/* --- the frame rate ---------------------------------------------------------------------------------
 * FFXiMain paces its frames by a divisor of 60: 2 (30 fps) as shipped, 1 for 60. It is a field
 * of an object FFXiMain creates at startup; Ashita's fps addon finds it the same way: the code
 * `sub esp, 0x100; cmp eax, ecx; je +0x21; mov ecx, [global]` names the global that points at
 * the object, and the divisor is at +0x30. The game may reset it (zoning), so every frame puts
 * it back. */
static uint32_t g_fps_divisor = 1, g_fps_global;

static void find_fps_global(void)
{
    static const uint8_t PAT[] = { 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00, 0x3B, 0xC1, 0x74, 0x21, 0x8B, 0x0D };
    uint32_t start = rt_image_base + rt_image_text_rva, end = start + rt_image_text_size;
    for (uint32_t a = start; a + sizeof PAT + 4 <= end; ++a)
        if (!memcmp(GUEST_PTR(a), PAT, sizeof PAT))
        {
            g_fps_global = rd32(a + sizeof PAT);
            rt_log("[recomp] frame-rate divisor: global %08x (code at %08x), set to %u (%u fps)\n", g_fps_global, a,
                g_fps_divisor, 60 / g_fps_divisor);
            return;
        }
    rt_log("[recomp] frame-rate divisor: not found; the game keeps its own frame rate\n");
    g_fps_global = 0xFFFFFFFFu;
}

/* --- the aspect ratio ---------------------------------------------------------------------------------
 * FFXiMain's projection takes its shape from a float at +0x2F0 of its camera object, which the game
 * sets from the configured resolution as h / (w * 0.25 * 3): 1 for 4:3, and a window of another
 * shape stretches the scene. Ashita's aspect addon finds the setter by its code,
 * `mov eax, [global]; test eax, eax; je; fld [esp+4]; fmul [0.25]; fmul [3.0]`, where the global
 * points at the object; every frame puts the value back from the shape the frame is shown at. */
static float g_aspect;          /* width / height to project for; 0 follows the window, < 0 off */
static uint32_t g_aspect_global; /* 0 not looked for yet, 0xFFFFFFFF not found */

static void find_aspect_global(void)
{
    static const uint8_t PAT[] = { 0xA1, 0, 0, 0, 0, 0x85, 0xC0, 0x74, 0, 0xD9, 0x44, 0x24, 0x04, 0xD8, 0x0D,
                                   0, 0, 0, 0, 0xD8, 0x0D };
    static const uint8_t ANY[] = { 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0 };
    uint32_t start = rt_image_base + rt_image_text_rva, end = start + rt_image_text_size;
    for (uint32_t a = start; a + sizeof PAT <= end; ++a)
    {
        const uint8_t* p = GUEST_PTR(a);
        size_t i = 0;
        while (i < sizeof PAT && (ANY[i] || p[i] == PAT[i]))
            ++i;
        if (i == sizeof PAT)
        {
            g_aspect_global = rd32(a + 1);
            rt_log("[recomp] aspect ratio: global %08x (code at %08x), %s\n", g_aspect_global, a,
                g_aspect > 0 ? "fixed" : "following the window");
            return;
        }
    }
    rt_log("[recomp] aspect ratio: not found; the scene keeps the game's shape\n");
    g_aspect_global = 0xFFFFFFFFu;
}

static void fix_aspect(void)
{
    if (g_aspect < 0)
        return;
    if (!g_aspect_global)
        find_aspect_global();
    if (g_aspect_global == 0xFFFFFFFFu)
        return;
    uint32_t object = rd32(g_aspect_global);
    if (!object)
        return;
    float ratio = g_aspect;
    if (!(ratio > 0))
    {
        uint32_t w = 0, h = 0;
        d3d8_screen_size(&w, &h);
        if (!w || !h)
            return;
        ratio = (float)w / (float)h;
    }
    float v = 4.0f / 3.0f / ratio; /* the game's h / (w * 0.75) */
    uint32_t bits;
    memcpy(&bits, &v, 4);
    if (rd32(object + 0x2F0) != bits)
        wr32(object + 0x2F0, bits);
}

/* --- draw distance -------------------------------------------------------------------------------------
 * How far out FFXiMain draws is a scale it works out per kind (2026-09-03: 0x101892b0, 1.0 or a
 * quality setting's 0.5, never below a zone's floor) times one of two floats in its graphics
 * settings: the world's (0x10456ab8) and characters' (0x10456aa8), both 1.0 unless its settings
 * change them. Ashita's drawdistance addon finds them by the code that multiplies,
 * `mov eax, ecx; dec eax; je +8; fmul [world]; jmp +6; fmul [characters]`. Every frame puts our
 * factor on top of what the game last chose. The scene effects' settings file (draw,
 * draw_entities) overrides the factors while the game runs, for tuning. */
static float g_draw_world = 1.0f, g_draw_entities = 1.0f; /* --draw-distance */
static uint32_t g_draw_global[2];                          /* world, characters; 0 not looked for, ~0 not found */

static void find_draw_globals(void)
{
    static const uint8_t PAT[] = { 0x8B, 0xC1, 0x48, 0x74, 0x08, 0xD8, 0x0D, 0, 0, 0, 0, 0xEB, 0x06, 0xD8, 0x0D };
    static const uint8_t ANY[] = { 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0 };
    uint32_t start = rt_image_base + rt_image_text_rva, end = start + rt_image_text_size;
    for (uint32_t a = start; a + sizeof PAT + 4 <= end; ++a)
    {
        const uint8_t* p = GUEST_PTR(a);
        size_t i = 0;
        while (i < sizeof PAT && (ANY[i] || p[i] == PAT[i]))
            ++i;
        if (i == sizeof PAT)
        {
            g_draw_global[0] = rd32(a + 7), g_draw_global[1] = rd32(a + 15);
            rt_log("[recomp] draw distance: world %08x, characters %08x (code at %08x)\n", g_draw_global[0],
                g_draw_global[1], a);
            return;
        }
    }
    rt_log("[recomp] draw distance: not found; the game keeps its own\n");
    g_draw_global[0] = g_draw_global[1] = 0xFFFFFFFFu;
}

static void fix_draw_distance(void)
{
    static uint32_t game[2] = { 0x3F800000u, 0x3F800000u }, ours[2]; /* the game's last value, what we wrote */
    float live[2] = { gfx_fx_get("draw"), gfx_fx_get("draw_entities") };
    float k[2] = { live[0] > 0 ? live[0] : g_draw_world, live[1] > 0 ? live[1] : g_draw_entities };
    if (!g_draw_global[0])
    {
        if (k[0] == 1.0f && k[1] == 1.0f)
            return; /* nothing asked for: leave the game's code unsearched */
        find_draw_globals();
    }
    if (g_draw_global[0] == 0xFFFFFFFFu)
        return;
    for (int i = 0; i < 2; ++i)
    {
        uint32_t now = rd32(g_draw_global[i]);
        if (now != ours[i])
            game[i] = now; /* the game changed it (its settings) */
        float base, v;
        memcpy(&base, &game[i], 4);
        v = base * k[i];
        memcpy(&ours[i], &v, 4);
        if (now != ours[i])
            wr32(g_draw_global[i], ours[i]);
    }
}

/* a factor, or world x characters (2, 3x1.5); 0 if it is neither */
static int parse_draw_distance(const char* s, float* world, float* entities)
{
    char* end;
    double a = strtod(s, &end), b = a;
    if (*end == 'x' || *end == ',')
        b = strtod(end + 1, &end);
    if (*end || !(a >= 0.5 && a <= 20) || !(b >= 0.5 && b <= 20))
        return 0;
    *world = (float)a, *entities = (float)b;
    return 1;
}

/* w:h (16:9), wxh, w/h, or a ratio (1.778); 0 if it is none of those */
static double parse_shape(const char* s)
{
    char* end;
    double a = strtod(s, &end), b = 1.0;
    if (*end == ':' || *end == 'x' || *end == '/')
        b = strtod(end + 1, &end);
    if (*end || !(a > 0) || !(b > 0) || a / b < 0.5 || a / b > 8)
        return 0;
    return a / b;
}

/* --- nameplates ----------------------------------------------------------------------------------------
 * FFXiMain draws the names over characters' heads (2026-09-03: 0x10086210, given a point in the
 * world, the text and a size) as screen-space quads into the 3D scene's render target: every glyph
 * corner is its offset from the projected point times [esp+0x4c] across and [esp+0x50] down. It
 * sets those from the target's width and height, which the target's stretch to the window turns
 * into the window's, so a name keeps the shape it was drawn for only in a 4:3 window and widens
 * with the window (1.8 times at 3440x1440). The hook point (meta/builds.json "nameplate_scale")
 * is just after both are set: the across factor is brought back to a 4:3 window's, and both take
 * the size asked for. Text stays centred on the point, since offsets are from it. */
#ifdef FFXI_HOOK_NAMEPLATE_SCALE
extern GuestFn rt_hook_nameplate_scale;
#endif
static int g_nameplate_fix = 1;                     /* --nameplates: 1 the 4:3 shape, 0 as the game draws */
static float g_nameplate_sx = 1.0f, g_nameplate_sy = 1.0f; /* --nameplate-scale */

static void nameplate_scale(Guest* g)
{
    float kx = g_nameplate_sx, ky = g_nameplate_sy;
    if (g_nameplate_fix)
    {
        uint32_t w = 0, h = 0;
        d3d8_screen_size(&w, &h);
        if (w && h)
            kx *= (4.0f / 3.0f) * (float)h / (float)w;
    }
    wrf32(g->esp + 0x4c, rdf32(g->esp + 0x4c) * kx);
    wrf32(g->esp + 0x50, rdf32(g->esp + 0x50) * ky);
}

static void setup_nameplates(void)
{
    int wanted = g_nameplate_fix || g_nameplate_sx != 1.0f || g_nameplate_sy != 1.0f;
#ifdef FFXI_HOOK_NAMEPLATE_SCALE
    if (wanted)
        rt_hook_nameplate_scale = nameplate_scale;
    rt_log("[recomp] nameplates: %s, scale %gx%g\n", g_nameplate_fix ? "4:3 shape" : "as the game draws them",
        g_nameplate_sx, g_nameplate_sy);
#else
    (void)nameplate_scale;
    if (wanted)
        rt_log("[recomp] nameplates: build %s has no nameplate hook; they stay as the game draws them\n", FFXI_BUILD);
#endif
}

/* s, or sx x sy (1.25, 1x1.2); 0 if it is neither */
static int parse_scale(const char* s, float* sx, float* sy)
{
    char* end;
    double a = strtod(s, &end), b = a;
    if (*end == 'x' || *end == ':' || *end == ',')
        b = strtod(end + 1, &end);
    if (*end || !(a >= 0.25 && a <= 4) || !(b >= 0.25 && b <= 4))
        return 0;
    *sx = (float)a, *sy = (float)b;
    return 1;
}

/* w:h or a ratio (16:9, 1.778) as width / height, or off (0); 0 if it is none of these */
static int parse_aspect(const char* s, float* aspect)
{
    if (!strcmp(s, "off"))
        return *aspect = 0.0f, 1;
    char* end;
    double a = strtod(s, &end), b = 1.0;
    if (*end == ':' || *end == 'x' || *end == '/')
        b = strtod(end + 1, &end);
    if (*end || !(a > 0) || !(b > 0) || a / b < 0.5 || a / b > 8)
        return 0;
    *aspect = (float)(a / b);
    return 1;
}

static int g_profile_shims;
static int g_addons_on; /* the addon host (host/addons/): off with FFXI_ADDONS=0 */

static void present_hook(void)
{
    if (g_profile_shims)
    {
        static uint64_t last;
        uint64_t now = rt_monotonic_ns();
        thunk_prof_thread = plat_thread_id();
        if (!last)
            last = now;
        if (now - last >= 2000000000ull)
            thunk_prof_report(), last = now;
    }
    fix_aspect();
    fix_draw_distance();
    modern_frame();
    if (g_addons_on)
        addons_frame();
    discord_frame();
    if (!g_fps_global)
        find_fps_global();
    if (g_fps_global == 0xFFFFFFFFu)
        return;
    uint32_t object = rd32(g_fps_global);
    if (object && rd32(object + 0x30) != g_fps_divisor)
        wr32(object + 0x30, g_fps_divisor);
}

/* ffxi.reg, the base registry (--reg): beside host64, in an app bundle's
 * Resources, or at the top of the source tree host64 was built in (build/host64). */
static int bundled_registry(const char* argv0, char* out, size_t n)
{
    const char *end = argv0, *p;
    for (p = argv0; *p; ++p)
        if (*p == '/' || *p == '\\')
            end = p + 1;
    static const char* const WHERE[] = { "ffxi.reg", "../Resources/ffxi.reg", "../ffxi.reg" };
    for (size_t i = 0; i < sizeof WHERE / sizeof *WHERE; ++i)
    {
        PlatStat st;
        snprintf(out, n, "%.*s%s", (int)(end - argv0), argv0, WHERE[i]);
        if (plat_stat(out, &st))
            return 1;
    }
    return 0;
}

static void report_overlay(const char* name, unsigned files)
{
    rt_log("[recomp] dats: %s, %u files\n", name, files);
}

#ifdef __APPLE__
#include <SDL3/SDL.h>
#include <unistd.h>
#endif

int main(int argc, char** argv)
{
#ifdef __APPLE__
    /* started from Finder or the Dock (an app bundle, no terminal): the log goes to host64.log
     * beside the sign-in screen's files */
    if (app_bundled() && !isatty(2))
    {
        char* pref = SDL_GetPrefPath("FFXIRecompile", "FFXI");
        if (pref)
        {
            char log[1100];
            snprintf(log, sizeof log, "%shost64.log", pref);
            if (freopen(log, "w", stderr))
                setvbuf(stderr, NULL, _IOLBF, 0);
            freopen(log, "a", stdout);
            setvbuf(stdout, NULL, _IOLBF, 0);
            SDL_free(pref);
        }
    }
#endif
    static char app_game[1024], app_server[256], app_bg[1100], app_dats[1024], app_val[64];
    const char* game = NULL;
    const char* regs[8];
    unsigned nregs = 0;
    const char* overlay = NULL;
    const char* data_dir = NULL;
    const char* addon_harness = NULL; /* --addon-harness script: addons without the game (host/addons/harness.c) */
    const char* finals[8];
    unsigned nfinals = 0;
    const char* dats[8];
    unsigned ndats = 0;
    const char* packs[8];
    unsigned npacks = 0;
    const char* user_dir = NULL;
    uint32_t game_server = DEFAULT_GAME_SERVER;
    LsbLogin lsb = { 0, 54231, 54230, 54001, NULL, NULL, "", NULL, NULL, NULL };
    static char base_reg[1100];
    const char* server_name = NULL; /* --server as given, for the sign-in screen */
    int nameplates_given = 0, nameplate_scale_given = 0, ui_aspect_given = 0, draw_given = 0, fps_given = 0;
    float ui_aspect = 0.0f;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        if (!strcmp(argv[i], "--game"))
            game = argv[i + 1];
        else if (!strcmp(argv[i], "--reg") && nregs < 8)
            regs[nregs++] = argv[i + 1];
        else if (!strcmp(argv[i], "--reg-overlay"))
            overlay = argv[i + 1];
        else if (!strcmp(argv[i], "--reg-final") && nfinals < 8)
            finals[nfinals++] = argv[i + 1];
        else if (!strcmp(argv[i], "--data-dir"))
            data_dir = argv[i + 1];
        else if (!strcmp(argv[i], "--addon-harness"))
            addon_harness = argv[i + 1];

        else if (!strcmp(argv[i], "--dats") && ndats < 8)
            dats[ndats++] = argv[i + 1];
        else if (!strcmp(argv[i], "--textures") && npacks < 8)
            packs[npacks++] = argv[i + 1];
        else if (!strcmp(argv[i], "--user-dir"))
            user_dir = argv[i + 1];
        else if (!strcmp(argv[i], "--server") || !strcmp(argv[i], "--lobby"))
        {
            server_name = argv[i + 1];
            if (!net_resolve_ipv4(argv[i + 1], &game_server))
            {
                fprintf(stderr, "%s: cannot resolve %s\n", argv[i], argv[i + 1]);
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--user"))
            lsb.user = argv[i + 1];
        else if (!strcmp(argv[i], "--pass"))
            lsb.password = argv[i + 1];
        else if (!strcmp(argv[i], "--otp"))
            lsb.otp = argv[i + 1];
        else if (!strcmp(argv[i], "--login-token"))
            lsb.login_token = argv[i + 1];
        else if (!strcmp(argv[i], "--loader-version"))
        {
            int v[3];
            if (!lsb_parse_version(argv[i + 1], v))
            {
                fprintf(stderr, "--loader-version: major.minor.patch, the version the server expects (" LSB_LOADER_VERSION " by default)\n");
                return 2;
            }
            lsb.version = argv[i + 1];
        }
        else if (!strcmp(argv[i], "--fps-divisor"))
        {
            long d = strtol(argv[i + 1], NULL, 10);
            if (d < 1 || d > 60)
            {
                fprintf(stderr, "--fps-divisor: 1 (60 fps), 2 (30 fps, as shipped), ...\n");
                return 2;
            }
            g_fps_divisor = (uint32_t)d;
            fps_given = 1;
        }
        else if (!strcmp(argv[i], "--ui-aspect"))
        {
            if (!parse_aspect(argv[i + 1], &ui_aspect))
            {
                fprintf(stderr, "--ui-aspect: a shape as w:h (16:9), a ratio (1.778), or off\n");
                return 2;
            }
            ui_aspect_given = 1;
        }
        else if (!strcmp(argv[i], "--nameplates"))
        {
            if (strcmp(argv[i + 1], "fix") && strcmp(argv[i + 1], "off"))
            {
                fprintf(stderr, "--nameplates: fix (their 4:3 shape in any window) or off (as the game draws them)\n");
                return 2;
            }
            g_nameplate_fix = !strcmp(argv[i + 1], "fix");
            nameplates_given = 1;
        }
        else if (!strcmp(argv[i], "--nameplate-scale"))
        {
            if (!parse_scale(argv[i + 1], &g_nameplate_sx, &g_nameplate_sy))
            {
                fprintf(stderr, "--nameplate-scale: a size from 0.25 to 4 (1.25), or across x down (1x1.2)\n");
                return 2;
            }
            nameplate_scale_given = 1;
        }
        else if (!strcmp(argv[i], "--draw-distance"))
        {
            if (!parse_draw_distance(argv[i + 1], &g_draw_world, &g_draw_entities))
            {
                fprintf(stderr, "--draw-distance: a factor from 0.5 to 20 (1 as the game draws; 3), or world x characters (3x1.5)\n");
                return 2;
            }
            draw_given = 1;
        }
        else if (!strcmp(argv[i], "--authport") || !strcmp(argv[i], "--dataport") || !strcmp(argv[i], "--viewport"))
        {
            long port = strtol(argv[i + 1], NULL, 10);
            if (port <= 0 || port > 65535)
            {
                fprintf(stderr, "%s: a port number\n", argv[i]);
                return 2;
            }
            *(argv[i][2] == 'a' ? &lsb.auth_port : argv[i][2] == 'd' ? &lsb.data_port : &lsb.view_port) = (uint16_t)port;
        }
    }
    if (!game && app_default("FFXIGameFolder", app_game, sizeof app_game))
        game = app_game;
    if (!ndats && app_default("FFXIDats", app_dats, sizeof app_dats))
        dats[ndats++] = app_dats;
    if (!ui_aspect_given && app_default("FFXIUIAspect", app_val, sizeof app_val) && !parse_aspect(app_val, &ui_aspect))
        ui_aspect = 0.0f;
    user32_set_ui_aspect(ui_aspect);
    if (!nameplates_given && app_default("FFXINameplates", app_val, sizeof app_val))
        g_nameplate_fix = strcmp(app_val, "off") != 0;
    if (!nameplate_scale_given && app_default("FFXINameplateScale", app_val, sizeof app_val)
        && !parse_scale(app_val, &g_nameplate_sx, &g_nameplate_sy))
        g_nameplate_sx = g_nameplate_sy = 1.0f;
    if (!draw_given && app_default("FFXIDrawDistance", app_val, sizeof app_val)
        && !parse_draw_distance(app_val, &g_draw_world, &g_draw_entities))
        g_draw_world = g_draw_entities = 1.0f;
    if (!game)
    {
        fprintf(stderr, "usage: host64 --game <FINAL FANTASY XI folder> [--reg f.reg]... [--reg-overlay f.reg] [--reg-final f.reg]... [--data-dir folder] "
                        "[--server name] [--user name [--pass p] [--otp code] [--authport n] "
                        "[--dataport n] [--viewport n]] [--loader-version a.b.c] [--dats folder]... [--nameplates fix|off] [--nameplate-scale s] [--draw-distance k]\n");
        return 2;
    }
    if (!lsb.password)
        lsb.password = getenv("FFXI_PASSWORD");
    if (!addon_harness)
        discord_init();
    if (!addon_harness && !(lsb.user && (lsb.password || lsb.login_token)))
    {
        /* Nothing on the command line signs in: the sign-in screen, in the game's own art. Its
         * window becomes the game's. */
        SigninSetup su = { game, data_dir, server_name, lsb.user,
                           lsb.password, lsb.otp, lsb.auth_port != 54231 ? lsb.auth_port : 0,
                           lsb.data_port != 54230 ? lsb.data_port : 0, lsb.view_port != 54001 ? lsb.view_port : 0 };
        /* an app bundle's first-run defaults (appdefaults.h) */
        su.default_mode = -1, su.default_space = -1;
        su.user_dir = user_dir;
        su.loader_version = lsb.version;
        if (app_default("FFXIFullscreenSpace", app_val, sizeof app_val))
            su.default_space = atoi(app_val) != 0;
        if (app_default("FFXIServer", app_server, sizeof app_server))
            su.default_server = app_server;
        if (app_default("FFXIWindowMode", app_val, sizeof app_val))
            su.default_mode = atoi(app_val);
        if (app_default("FFXIResolution", app_val, sizeof app_val))
            sscanf(app_val, "%dx%d", &su.default_w, &su.default_h);
        if (app_default("FFXIMenuResolution", app_val, sizeof app_val))
            sscanf(app_val, "%dx%d", &su.default_menu_w, &su.default_menu_h);
        if (app_default("FFXIBackground", app_val, sizeof app_val) && app_resource(app_val, app_bg, sizeof app_bg))
            su.default_background = app_bg;
        SigninResult sr;
        int r = signin_run(&su, &sr);
        if (r == 0)
            return 0;
        if (r > 0)
        {
            game_server = sr.server;
            user32_adopt_window(sr.window);
            if (!nfinals)
                finals[nfinals++] = strdup(sr.settings_reg);
            /* its folder for host64's files and the game's saved settings, the bundled registry
             * under them */
            if (!data_dir)
                data_dir = strdup(sr.data_dir);
            if (!overlay)
            {
                char o[1100];
                snprintf(o, sizeof o, "%s%ssaved.reg", data_dir,
                    data_dir[0] && data_dir[strlen(data_dir) - 1] == '/' ? "" : "/");
                overlay = strdup(o);
            }
            if (!nregs && bundled_registry(argv[0], base_reg, sizeof base_reg))
                regs[nregs++] = base_reg;
            lsb.user = NULL; /* signed in */
        }
        else if (!lsb.user)
        {
            fprintf(stderr, "no sign-in screen here: --user name for a LandSandBoat server\n");
            return 2;
        }
    }
    if (lsb.user)
    {
        /* a LandSandBoat server: sign in before anything is loaded, so a refusal costs nothing */
        char pw[256], err[512];
        if (!lsb.password && lsb.login_token)
            lsb.password = ""; /* the token replaces it */
        if (!lsb.password)
        {
            if (!read_secret("Password: ", pw, sizeof pw))
            {
                fprintf(stderr, "--user: no password (--pass, FFXI_PASSWORD, or a terminal to ask on)\n");
                return 2;
            }
            lsb.password = pw;
        }
        lsb.server = game_server;
        int ok = lsb_login(&lsb, err, sizeof err);
        memset(pw, 0, sizeof pw);
        if (!ok)
        {
            fprintf(stderr, "sign-in failed: %s\n", err);
            return 1;
        }
    }

    if (!gwin_init())
    {
        fprintf(stderr, "cannot reserve the guest window\n");
        return 1;
    }
    gt_init();
    rt_set_native_handler(thunk_dispatch);
    /* The game sees Windows paths. On Windows they are the host's own; elsewhere the install
     * (FINAL FANTASY XI and the viewer folder, side by side as retail installs them) is mounted
     * where a retail install puts them. */
    char exe[700], path[700], guest_game[700], host_game[700];
    snprintf(host_game, sizeof host_game, "%s", game);
    for (size_t n = strlen(host_game); n > 1 && (host_game[n - 1] == '/' || host_game[n - 1] == '\\'); --n)
        host_game[n - 1] = 0;
    if (plat_path_sep == '\\')
        snprintf(guest_game, sizeof guest_game, "%s", host_game);
    else
    {
        char host_viewer[760];
        snprintf(guest_game, sizeof guest_game, "C:\\PlayOnline\\SquareEnix\\FINAL FANTASY XI");
        snprintf(host_viewer, sizeof host_viewer, "%s/../PlayOnlineViewer", host_game);
        vfs_mount(guest_game, host_game);
        vfs_mount("C:\\PlayOnline\\SquareEnix\\PlayOnlineViewer", host_viewer);
    }
    game = guest_game;
    snprintf(exe, sizeof exe, "%s\\..\\PlayOnlineViewer\\pol.exe", game);
    k32_init(game, exe);
    k32_io_init();
    k32_misc_init();
    ole_init();
    user32_init();
    d3d8_init();
    dsound_init();
    dinput_init();
    ws2_init();
    /* the game's hosts: the lobby through gamecore's resolver, every other one through
     * gethostbyname */
    ws2_set_game_server(game_server);
    gamecore_set_lobby(game_server);
    rt_log("[recomp] game hosts -> %u.%u.%u.%u\n", game_server >> 24, (game_server >> 16) & 255, (game_server >> 8) & 255,
        game_server & 255);
    reg_init(regs, nregs, overlay);
    for (unsigned i = 0; i < nfinals; ++i)
        reg_load_final(finals[i]);
    {
        /* The install folders are where the game is now, whatever the imported registry says (it
         * comes from another machine or folder); the game checks them (FFXI-9001). Retail writes
         * 0001 with a trailing backslash and 1000 without. */
        char p[760];
        snprintf(p, sizeof p, "%s\\", game);
        reg_set_string("HKEY_LOCAL_MACHINE\\SOFTWARE\\PlayOnlineUS\\InstallFolder", "0001", p);
        snprintf(p, sizeof p, "%s\\..\\PlayOnlineViewer", game);
        char viewer[760];
        if (vfs_full_path(p, viewer, sizeof viewer))
            snprintf(p, sizeof p, "%s", viewer);
        reg_set_string("HKEY_LOCAL_MACHINE\\SOFTWARE\\PlayOnlineUS\\InstallFolder", "1000", p);
    }
    vfs_init(game);
    if (user_dir)
    {
        char guest_user[760];
        snprintf(guest_user, sizeof guest_user, "%s\\USER", game);
        vfs_mount(guest_user, user_dir);
        rt_log("[recomp] USER -> %s\n", user_dir);
    }
    for (unsigned i = 0; i < ndats; ++i)
    {
        /* DAT overlays: files under their ROM*\ and sound*\ folders replace the install's */
        if (!vfs_add_overlay(dats[i], report_overlay))
            rt_log("[recomp] dats: no ROM or sound files in %s\n", dats[i]);
    }
    {
        /* texture packs: --textures, else <data dir>/textures if there is one */
        char def[1100];
        PlatStat st;
        if (!npacks && data_dir)
        {
            snprintf(def, sizeof def, "%s%ctextures", data_dir, plat_path_sep);
            if (plat_stat(def, &st))
                packs[npacks++] = def;
        }
        for (unsigned i = 0; i < npacks; ++i)
            d3d8_texture_pack(packs[i]);
    }
    {
        /* The game reads patch.ver from its folder and will not start without it; the lobby sees
         * the version inside. Installs launched without the viewer (private
         * servers) ship none: then one is made for this build's version, in --data-dir (else next
         * to the host), and mounted over the game's path. The install itself is never written. */
        char pv[760];
        PlatStat st;
        snprintf(pv, sizeof pv, "%s%cpatch.ver", host_game, plat_path_sep);
        if (!plat_stat(pv, &st))
        {
            uint8_t file[0x120];
            if (data_dir)
                snprintf(pv, sizeof pv, "%s%cpatch.%s.ver", data_dir, plat_path_sep, FFXI_VERSION);
            else
            {
                const char *dir_end = argv[0], *p;
                for (p = argv[0]; *p; ++p)
                    if (*p == '/' || *p == plat_path_sep)
                        dir_end = p + 1;
                snprintf(pv, sizeof pv, "%.*spatch.%s.ver", (int)(dir_end - argv[0]), argv[0], FFXI_VERSION);
            }
            PlatFile* f = gamecore_make_patch_ver(FFXI_VERSION, file) ? plat_file_open(pv, PLAT_WRITE | PLAT_CREATE | PLAT_TRUNCATE) : NULL;
            if (!f || plat_file_write(f, file, sizeof file) != sizeof file)
            {
                fprintf(stderr, "cannot write %s\n", pv);
                return 1;
            }
            plat_file_close(f);
            char guest_pv[760];
            snprintf(guest_pv, sizeof guest_pv, "%s\\patch.ver", game);
            vfs_mount(guest_pv, pv);
            printf("[recomp] no patch.ver in the install: version %s from %s\n", FFXI_VERSION, pv);
        }
    }
    gamecore_slots_init();
    gamecore_files_init();
    gamecore_presence_init();
    {
        char viewer[700];
        snprintf(viewer, sizeof viewer, "%s\\..\\PlayOnlineViewer", game);
        char full[700];
        if (vfs_full_path(viewer, full, sizeof full))
            gamecore_set_root(full);
    }

    /* the images first, before the heap spreads through the low window */
    snprintf(path, sizeof path, "%s%cFFXiMain.dll", host_game, plat_path_sep);
    if (!pe_load(path))
        return 1;
    snprintf(path, sizeof path, "%s%cFFXi.dll", host_game, plat_path_sep);
    if (!pe_load_module(path, &rt_module_ffxi, FFXI_BASE))
        return 1;
    k32_add_module("FFXi.dll", FFXI_BASE);
    ole_register_class(CLSID_GameMain, rt_image_base);
    ole_register_class(CLSID_FxFileManager, FFXI_BASE);
    ole_register_class(CLSID_FFXiEntry, FFXI_BASE);
    gamecore_init();
    d3d8_setup();
    d3d8_set_present_hook(present_hook);
    ModernSetup ms = { game, data_dir, &g_fps_divisor, fps_given, ui_aspect_given, nfinals ? finals[nfinals - 1] : NULL };
    modern_init(&ms);
    setup_nameplates();
    {
        /* the addon host: Ashita v4 and Windower 4 Lua addons, and our own */
        const char* off = getenv("FFXI_ADDONS");
        if (!off || strcmp(off, "0"))
        {
            const char* where = data_dir;
#ifdef __APPLE__
            char* pref = NULL;
            if (!where)
                where = pref = SDL_GetPrefPath("FFXIRecompile", "FFXI");
#endif
            AddonsSetup as = { where ? where : ".", host_game, dats, ndats };
            addons_init(&as);
            modern_set_addons(addons_menu());
            g_addons_on = 1;
#ifdef __APPLE__
            SDL_free(pref);
#endif
        }
        else
        {
            /* Discord's zone names are the addon host's DAT reader's */
            extern void xi_res_setup(const char* game_dir, const char* const* overlays, unsigned n);
            xi_res_setup(host_game, dats, ndats);
        }
    }
    if (getenv("FFXI_PROFILE") && getenv("FFXI_PROFILE")[0] && getenv("FFXI_PROFILE")[0] != '0')
        thunk_timer = gfx_prof_shim, g_profile_shims = 1; /* the profile splits the game's time into its code and our API calls */
    dsound_setup();
    dinput_setup();
    if (getenv("FFXI_RECOMP_MISSING"))
        thunk_report_missing();

    /* both CRTs, as the Windows loader would run them */
    uint32_t attach[3] = { rt_image_base, 1, 0 };
    if (!guest_call(rt_image_oep, 3, attach))
    {
        fprintf(stderr, "FFXiMain's DllMain failed\n");
        return 1;
    }
    uint32_t attach_ffxi[3] = { FFXI_BASE, 1, 0 };
    if (!guest_call(rt_module_ffxi.oep + *rt_module_ffxi.delta, 3, attach_ffxi))
    {
        fprintf(stderr, "FFXi.dll's DllMain failed\n");
        return 1;
    }

    if (addon_harness)
        return addons_harness(addon_harness);

    /* FFXiEntry, through FFXi.dll's class factory */
    gt_lock();
    uint32_t clsid = guest_bytes(CLSID_FFXiEntry, 16), iid_cf = guest_bytes(IID_IClassFactory, 16),
             iid_entry = guest_bytes(IID_IFFXiEntry, 16), out = gheap_alloc(4, 1);
    gt_unlock();
    uint32_t gco[3] = { clsid, iid_cf, out };
    uint32_t hr = guest_call(pe_export_at(FFXI_BASE, "DllGetClassObject"), 3, gco);
    uint32_t cf = rd32(out);
    if (hr || !cf)
    {
        fprintf(stderr, "FFXi.dll DllGetClassObject(FFXiEntry): %08x\n", hr);
        return 1;
    }
    uint32_t ci[3] = { 0, iid_entry, out };
    hr = com_call(cf, 3, 3, ci);
    uint32_t entry = rd32(out);
    if (hr || !entry)
    {
        fprintf(stderr, "IClassFactory::CreateInstance(IFFXiEntry): %08x\n", hr);
        return 1;
    }

    /* IFFXiEntry::GameStart(pCore, &pFFXiMessage): the game runs inside this call */
    rt_log("[recomp] GameStart\n");
#if defined(_WIN32)
    /* FFXI_SAMPLE=<file>: where this thread's time goes, sampled (tools/sample_report.py) */
    if (getenv("FFXI_SAMPLE") && getenv("FFXI_SAMPLE")[0])
        rt_log("[recomp] sampling the game thread to %s: %s\n", getenv("FFXI_SAMPLE"),
            sampler_start(getenv("FFXI_SAMPLE")) ? "on" : "cannot write it");
#endif
    uint32_t start[2] = { gamecore_object(), out };
    hr = com_call(entry, 3, 2, start);
    const char* message = NULL;
    int32_t code = gamecore_exit_code(&message);
    rt_log("[recomp] GameStart returned %08x; exit code %d%s%s\n", hr, code, message && *message ? ", message: " : "",
        message ? message : "");
    return hr ? 1 : 0;
}
