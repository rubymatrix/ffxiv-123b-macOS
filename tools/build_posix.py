"""Build driver for POSIX hosts (clang): arm64 macOS for R3, Linux as a by-product.

  python3 tools/build_posix.py prepare --game <FINAL FANTASY XI folder>
        identify the player's build (meta/builds.json) and unpack FFXiMain.dll and FFXi.dll into
        generated/ (Square Enix code: generated/ is gitignored, never committed)
  python3 tools/build_posix.py boot64 --game <folder>    the boot test (R3.1)
  python3 tools/build_posix.py host64 --game <folder>    the game host, with SDL3
  python3 tools/build_posix.py gfxtest                   the graphics back end and the D3D8 front end,
        offscreen, without the game (tests/gfx_test.c, tests/d3d8_test.c)
  python3 tools/build_posix.py datuitest --game <folder>  the game's UI art read from its DATs (host/datui.c):
        parse checks, and renders in build/datui/ (tests/datui_test.c)
  python3 tools/build_posix.py app --game <folder> [--server name] [--resolution WxH]
        [--menu-resolution WxH] [--window-mode 0-3] [--background picture] [--fullscreen-space 0|1]
        [--nameplates fix|off] [--nameplate-scale s] [--ui-aspect w:h|off] [--draw-distance k] [--dats folder]
        [--sign-identity name]
        build/Final Fantasy XI.app: host64 with its libraries, ffxi.reg and the defaults above in
        its Info.plist (host/appdefaults.h), so it starts from Finder with no command line. The values
        go into the built app only: nothing names a server in the source.

The same sources as tools/build.py's boot64/host64 targets, with plat_posix.c for plat_win.c.
Needs: clang (Xcode command line tools) and python3 with capstone and pefile. SDL3 and mbedtls are
vendored in third_party/ and built with clang (tools/thirdparty.py). The game folder is the retail "FINAL FANTASY XI"
folder copied from a Windows install, with the viewer folder from the same install next to it.
"""
import argparse
import concurrent.futures
import os
import shlex
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build  # noqa: E402  (constants and source lists; nothing Windows-only runs on import)
import thirdparty  # noqa: E402

ROOT = build.ROOT
GEN_FFXI_IMAGE = build.FFXI_IMAGE
CFLAGS = ['-O2', '-std=c11', '-g', '-DRT_GUEST_WINDOW', '-fno-strict-aliasing', '-I', 'runtime', '-I', 'runtime/portable',
          '-I', 'generated', '-I', 'third_party/stb']
# the generated C: every label and local is emitted whether used or not
GEN_WARNINGS = ['-Wno-unused-label', '-Wno-unused-variable', '-Wno-unused-but-set-variable', '-Wno-unused-function',
                '-Wno-parentheses-equality', '-Wno-unreachable-code']
# the graphics back end: Metal on macOS (R3.2), none elsewhere yet
if sys.platform == 'darwin':
    GFX_SOURCES = ['runtime/portable/gfx_msl.c', 'runtime/portable/gfx_msl_shaders.c', 'runtime/portable/gfx_metal.m']
    GFX_LIBS = ['-framework', 'Metal', '-framework', 'QuartzCore', '-framework', 'Foundation',
                '-framework', 'Security']  # Security: the sign-in screen's saved passwords
else:
    GFX_SOURCES = ['runtime/portable/gfx_null.c']
    GFX_LIBS = []
HOST_SOURCES = ['runtime/portable/user32.c', 'runtime/portable/d3d8.c', 'runtime/portable/dsound.c',
                'runtime/portable/input.c', 'runtime/portable/dinput.c', 'runtime/portable/ws2.c', 'host/host64.c',
                'host/lsb_login.c', 'host/datui.c', 'host/uidraw.c', 'host/modern.c', 'host/discord.c', 'host/signin.c', 'host/sewave.c', 'host/ui_art.c', 'host/keychain.c', 'host/appdefaults.c'] + GFX_SOURCES
# the addon host (host/addons/): C, C++ (ImGui) and its embedded Lua
ADDON_SOURCES = sorted('host/addons/' + f for f in os.listdir(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'host', 'addons'))
                       if f.endswith('.c') or f.endswith('.cpp')) + ['generated/addons_lua.c']
ADDON_LIBS = ('luajit', 'imgui', 'luasocket', 'lfs', 'sqlite')


def posix(p):
    return p.replace('\\', '/')


PORTABLE = [posix(p).replace('plat_win.c', 'plat_posix.c') for p in build.PORTABLE]


def run(cmd, **kw):
    line = ' '.join(shlex.quote(c) for c in cmd)
    print('>', line if len(line) < 200 else line[:200] + ' ...')
    subprocess.check_call(cmd, cwd=ROOT, **kw)


# FFXI_PROGRESS=1 (tools/setup.py): the phases and the compile's progress as lines of their own
PROGRESS = bool(os.environ.get('FFXI_PROGRESS'))


def phase(name):
    if PROGRESS:
        print('@phase ' + name, flush=True)


def sdl():
    """SDL3 from third_party/sdl3: (compile flags, link flags)."""
    thirdparty.build('sdl3')
    return thirdparty.flags('sdl3'), thirdparty.libs('sdl3')


def tls():
    """mbedtls from third_party/mbedtls (the LandSandBoat sign-in's TLS, host/lsb_login.c)."""
    thirdparty.build('mbedtls')
    return thirdparty.flags('mbedtls'), thirdparty.libs('mbedtls')


def newest_header():
    """The newest runtime/host header: a changed struct must rebuild everything that may include it
    (no per-file dependency tracking; the generated code has its own headers, which it rebuilds with)."""
    newest = 0
    for d in ('runtime', 'runtime/portable', 'runtime/win32', 'host'):
        full = os.path.join(ROOT, d)
        if os.path.isdir(full):
            for f in os.listdir(full):
                if f.endswith('.h'):
                    newest = max(newest, os.path.getmtime(os.path.join(full, f)))
    return newest


def stale(s, obj, headers):
    """Whether obj must be rebuilt: missing, older than its source, older than the newest runtime
    header (our own sources), or older than build.h when the source includes it (another build's
    addresses)."""
    src = os.path.join(ROOT, s)
    if not os.path.exists(obj):
        return True
    stamp = os.path.getmtime(src)
    if not s.startswith('generated/'):
        stamp = max(stamp, headers)
    if os.path.getmtime(obj) < stamp:
        return True
    if not s.startswith('generated/') and os.path.getmtime(obj) < os.path.getmtime(build.BUILD_H):
        with open(src, errors='replace') as f:
            return '"build.h"' in f.read()
    return False


def compile_stale(sources, objdir, extra):
    """Compiles every source whose object is missing or older than it (or than the newest runtime
    header, for our own sources), in parallel; returns the objects."""
    os.makedirs(os.path.join(ROOT, objdir), exist_ok=True)
    headers = newest_header()
    objs, jobs = [], []
    for s in sources:
        obj = os.path.join(objdir, os.path.splitext(os.path.basename(s))[0] + '.o')
        objs.append(obj)
        full_obj = os.path.join(ROOT, obj)
        if stale(s, full_obj, headers):
            flags = CFLAGS + extra + (GEN_WARNINGS if s.startswith('generated/') else [])
            cc = 'clang'
            if s.endswith('.m'):  # Objective-C: references counted by hand (gfx_metal.m)
                flags = [f for f in flags if f != '-std=c11'] + ['-fno-objc-arc']
            elif s.endswith('.cpp'):  # the addon host's ImGui side
                flags = [f for f in flags if f != '-std=c11'] + ['-std=c++17']
                cc = 'clang++'
            jobs.append([cc, '-c'] + flags + [s, '-o', obj])
    if jobs:
        print('compiling %d of %d' % (len(jobs), len(sources)))
        failed = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
            for n, (cmd, rc) in enumerate(zip(jobs, ex.map(lambda c: subprocess.call(c, cwd=ROOT), jobs)), 1):
                if rc:
                    failed.append(cmd[-3])
                if PROGRESS:
                    print('@progress %d %d' % (n, len(jobs)), flush=True)
        if failed:
            raise SystemExit('failed: ' + ', '.join(failed))
    return objs


def generated(sub):
    d = os.path.join(ROOT, 'generated', sub)
    return ['generated/%s/%s' % (sub, f) for f in sorted(os.listdir(d)) if f.endswith('.c')]


def prepare(game):
    run([sys.executable, 'tools/prepare.py', '--game', game])  # and FFXi.dll


def translate():
    if build.BUILD is None:
        raise SystemExit('run: python3 tools/build_posix.py prepare --game <folder>')
    build.write_build_h()
    build.recomp('generated/all', ['--all'])


def boot64(game):
    translate()
    # objects live apart from the binaries (build/boot64 is the program); the translation's are
    # shared with host64
    objs = compile_stale(generated('all'), 'build/all64', ['-I', 'generated/all'])
    objs += compile_stale(PORTABLE + ['tests/boot64.c'], 'build/obj/boot64', [])
    run(['clang', '-o', 'build/boot64'] + objs + ['-lm', '-lpthread'])
    run(['build/boot64', build.RETAIL, game])


def addons():
    """The addon host's libraries and embedded Lua: (compile flags, link flags)."""
    run([sys.executable, 'tools/embed_lua.py'])
    cflags, libs = ['-I', 'host/addons', '-DIMGUI_USER_CONFIG="imconfig_xi.h"'], []
    for name in ADDON_LIBS:
        thirdparty.build(name)
        cflags += thirdparty.flags(name)
        libs += thirdparty.libs(name)
    return cflags, libs + ['-lc++']


def host64(game):
    translate()
    run([sys.executable, 'recomp/recomp.py', '--meta', build.FFXI_META, '--image', GEN_FFXI_IMAGE, '--retail',
         build.FFXI_RETAIL, '--module', 'ffxi', '--out', 'generated/ffxi', '--all'])
    sdl_cflags, sdl_libs = sdl()
    tls_cflags, tls_libs = tls()
    phase('compile')
    objs = compile_stale(generated('all'), 'build/all64', ['-I', 'generated/all'])
    objs += compile_stale(generated('ffxi'), 'build/ffxi64', ['-I', 'generated/ffxi'])
    addon_cflags, addon_libs = addons()
    objs += compile_stale(PORTABLE + HOST_SOURCES + ADDON_SOURCES, 'build/obj/host64', sdl_cflags + tls_cflags + addon_cflags)
    # -export_dynamic: addons' ffi.C finds D3DX and Win32 (host/addons/d3d_ffi.c, win32_ffi.c) with
    # dlsym(RTLD_DEFAULT), so their symbols stay in the executable's export table
    export = ['-Wl,-export_dynamic'] if sys.platform == 'darwin' else ['-rdynamic']
    run(['clang', '-o', 'build/host64'] + objs + sdl_libs + tls_libs + addon_libs + GFX_LIBS + ['-lm', '-lpthread'] + export)
    print('built build/host64; run: build/host64 --game %s --server <name>' % shlex.quote(game))


def gfxtest():
    """The back end alone (gfx_test), then the D3D8 front end on it through its COM thunks (d3d8_test)."""
    sdl_cflags, sdl_libs = sdl()
    objs = compile_stale(GFX_SOURCES + ['tests/gfx_test.c'], 'build/gfxtest', sdl_cflags)
    run(['clang', '-o', 'build/gfx_test'] + objs + sdl_libs + GFX_LIBS)
    run(['build/gfx_test'])
    objs = compile_stale(PORTABLE + GFX_SOURCES + ['runtime/portable/user32.c', 'runtime/portable/input.c',
                                                   'runtime/portable/d3d8.c', 'tests/d3d8_test.c'], 'build/d3d8test', sdl_cflags)
    run(['clang', '-o', 'build/d3d8_test'] + objs + sdl_libs + GFX_LIBS + ['-lm', '-lpthread'])
    run(['build/d3d8_test'])


def datuitest(game):
    """host/datui.c against the install's DATs; renders the windows and lobby into build/datui/."""
    os.makedirs(os.path.join(ROOT, 'build', 'datui'), exist_ok=True)
    run(['clang', '-O2', '-std=c11', '-Wall', '-I', 'host', '-o', 'build/datui_test', 'tests/datui_test.c',
         'host/datui.c', '-lm'])
    run(['build/datui_test', '--game', game, '--out', 'build/datui'])


APP_NAME = 'Final Fantasy XI'


def plist_escape(v):
    return str(v).replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


def bundle_dylibs(exe, frameworks):
    """Copies the non-system libraries exe links (and theirs) into frameworks and points exe at
    them through @rpath, so the app runs without Homebrew."""
    os.makedirs(frameworks, exist_ok=True)
    def deps(path):
        out = subprocess.check_output(['otool', '-L', path], text=True).splitlines()[1:]
        return [l.split()[0] for l in out if l.strip()]
    todo, seen = [exe], set()
    while todo:
        img = todo.pop()
        for d in deps(img):
            name = os.path.basename(d)
            if not (d.startswith('/opt/') or d.startswith('/usr/local/')):
                continue
            dst = os.path.join(frameworks, name)
            if name not in seen:
                seen.add(name)
                shutil.copy(d, dst)
                os.chmod(dst, 0o755)
                subprocess.check_call(['install_name_tool', '-id', '@rpath/' + name, dst])
                todo.append(dst)
            if img != dst:
                subprocess.check_call(['install_name_tool', '-change', d, '@rpath/' + name, img])
    subprocess.check_call(['install_name_tool', '-add_rpath', '@executable_path/../Frameworks', exe])
    return sorted(seen)


# A self-signed code-signing certificate in the login keychain, made once per machine (README):
# signed with it, every build of the app is the same app to macOS, so the keychain's "Always Allow"
# for its saved passwords survives rebuilds. Ad hoc otherwise.
LOCAL_IDENTITY = 'FFXI Local Code Signing'


def signing_identity(given):
    if given or os.environ.get('FFXI_SIGN_IDENTITY'):
        return given or os.environ['FFXI_SIGN_IDENTITY']
    found = subprocess.run(['security', 'find-certificate', '-c', LOCAL_IDENTITY], capture_output=True)
    return LOCAL_IDENTITY if found.returncode == 0 else '-'


def app(game, a):
    """host64 as build/Final Fantasy XI.app, with the first-run defaults in its Info.plist."""
    host64(game)
    phase('app')
    bundle = os.path.join(ROOT, 'build', APP_NAME + '.app')
    shutil.rmtree(bundle, ignore_errors=True)
    contents = os.path.join(bundle, 'Contents')
    macos, res = os.path.join(contents, 'MacOS'), os.path.join(contents, 'Resources')
    os.makedirs(macos)
    os.makedirs(res)
    exe = os.path.join(macos, APP_NAME)
    shutil.copy(os.path.join(ROOT, 'build', 'host64'), exe)
    shutil.copy(os.path.join(ROOT, 'ffxi.reg'), res)
    shutil.copy(os.path.join(ROOT, 'assets', 'icon.icns'), res)
    keys = {'FFXIGameFolder': game}
    if a.server:
        keys['FFXIServer'] = a.server
    if a.resolution:
        keys['FFXIResolution'] = a.resolution
    if a.menu_resolution:
        keys['FFXIMenuResolution'] = a.menu_resolution
    if a.window_mode is not None:
        keys['FFXIWindowMode'] = a.window_mode
    if a.fullscreen_space is not None:
        keys['FFXIFullscreenSpace'] = a.fullscreen_space
    if a.nameplates:
        keys['FFXINameplates'] = a.nameplates
    if a.nameplate_scale:
        keys['FFXINameplateScale'] = a.nameplate_scale
    if a.ui_aspect:
        keys['FFXIUIAspect'] = a.ui_aspect
    if a.draw_distance:
        keys['FFXIDrawDistance'] = a.draw_distance
    if a.dats:
        keys['FFXIDats'] = os.path.abspath(os.path.expanduser(a.dats))
    if a.background:
        # the sign-in screen reads PNG, JPEG and BMP; anything else (WebP) becomes a PNG
        src, ext = os.path.expanduser(a.background), os.path.splitext(a.background)[1].lower()
        name = 'background' + (ext if ext in ('.png', '.jpg', '.jpeg', '.bmp') else '.png')
        if name.endswith('.png') and ext != '.png':
            run(['sips', '-s', 'format', 'png', src, '--out', os.path.join(res, name)])
        else:
            shutil.copy(src, os.path.join(res, name))
        keys['FFXIBackground'] = name
    extra = ''.join('\t<key>%s</key>%s\n' % (k, '<integer>%d</integer>' % v if isinstance(v, int)
                                              else '<string>%s</string>' % plist_escape(v)) for k, v in keys.items())
    with open(os.path.join(contents, 'Info.plist'), 'w') as f:
        f.write(APP_INFO_PLIST.replace('@NAME@', APP_NAME).replace('@EXTRA@', extra))
    libs = bundle_dylibs(exe, os.path.join(contents, 'Frameworks'))
    identity = signing_identity(a.sign_identity)
    run(['codesign', '--force', '--deep', '--sign', identity, bundle])
    if identity == '-':
        print('note: signed ad hoc: macOS asks again for the saved password after each rebuild; '
              'a code-signing identity in the keychain (%s) keeps its answer' % LOCAL_IDENTITY)
    print('built %s (%s bundled; defaults: %s)' % (bundle, ', '.join(libs) or 'no libraries',
                                                    ', '.join('%s=%s' % kv for kv in keys.items())))


APP_INFO_PLIST = '''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key><string>@NAME@</string>
	<key>CFBundleIdentifier</key><string>com.rubymatrix.xi-on-mac</string>
	<key>CFBundleName</key><string>@NAME@</string>
	<key>CFBundleDisplayName</key><string>@NAME@</string>
	<key>CFBundleIconFile</key><string>icon</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleShortVersionString</key><string>0.1</string>
	<key>CFBundleVersion</key><string>1</string>
	<key>LSMinimumSystemVersion</key><string>12.0</string>
	<key>LSApplicationCategoryType</key><string>public.app-category.role-playing-games</string>
	<key>NSHighResolutionCapable</key><true/>
@EXTRA@</dict>
</plist>
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('target', choices=['prepare', 'boot64', 'host64', 'gfxtest', 'datuitest', 'app'])
    ap.add_argument('--game', default=os.path.expanduser('~/SquareEnix/FINAL FANTASY XI'))
    # app: its first-run defaults (host/appdefaults.h)
    ap.add_argument('--server')
    ap.add_argument('--resolution')
    ap.add_argument('--menu-resolution')
    ap.add_argument('--window-mode', type=int, choices=[0, 1, 2, 3])
    ap.add_argument('--background')
    ap.add_argument('--sign-identity')
    ap.add_argument('--fullscreen-space', type=int, choices=[0, 1])
    ap.add_argument('--nameplates', choices=['fix', 'off'])
    ap.add_argument('--nameplate-scale')
    ap.add_argument('--draw-distance')
    ap.add_argument('--ui-aspect')
    ap.add_argument('--dats')
    args = ap.parse_args()
    if args.target == 'gfxtest':
        return gfxtest()
    game = os.path.abspath(args.game)
    if not os.path.exists(os.path.join(game, 'FFXiMain.dll')):
        raise SystemExit('no FFXiMain.dll in %s (--game)' % game)
    if args.target == 'datuitest':
        return datuitest(game)
    if args.target == 'app':
        return app(game, args)
    if args.target == 'prepare':
        prepare(game)
    elif args.target == 'boot64':
        boot64(game)
    else:
        host64(game)


if __name__ == '__main__':
    main()
