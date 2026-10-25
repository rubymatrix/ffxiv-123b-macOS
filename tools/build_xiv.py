"""Build driver for FINAL FANTASY XIV 1.23b ffxivgame.exe on arm64 macOS (clang).

  python3 tools/build_xiv.py meta --exe <ffxivgame.exe> --ghidra <meteor-decomp/build/ghidra>
        export meta/ffxivgame.1.23b.meta.json from meteor-decomp's Ghidra project
        (discovery/ExportFFXIV.java, headless, read-only)
  python3 tools/build_xiv.py translate --exe <ffxivgame.exe>
        recomp/recomp.py --all into generated/ffxivgame (Square Enix code: gitignored)
  python3 tools/build_xiv.py xivboot --exe <ffxivgame.exe> [--run] [-- args...]
        the boot test (tests/xivboot.c): the translation plus the portable runtime
  python3 tools/build_xiv.py xivboot --exe <ffxivgame.exe> --debug [--opt -O0]
        build/xivboot-dbg for lldb (tools/xivdbg.sh): no LTO, -g on the translation so every
        source line is one /* guest-address: x86 */ instruction; objects in build/xiv-dbg

Objects are rebuilt when missing or older than their source or than the newest runtime header
(the generated code's too: guest.h holds the Guest layout every translation compiles against).
"""
import argparse
import concurrent.futures
import glob
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import thirdparty  # noqa: E402  (vendored SDL3 and unshield)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
META = 'meta/ffxivgame.1.23b.meta.json'
APPLY = 'meta/ffxivgame.1.23b.apply.json'
# functions the host may replace or watch (recomp.py --wraps; runtime/portable/xiv_ui.c)
WRAPS = ','.join('ui_%s=%s' % m for m in re.findall(r'^ANIM\((\w+), (0x[0-9a-f]+)\)', open(
    os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'runtime/portable/xiv_ui_wraps.h')).read(), re.M))  # Garlemald Client's code patches, translated in
WRAPS += ',key_repeat=0xd429b0'  # the keyboard's auto-repeat counter, paced in time (xiv_ui.c)
WRAPS += ',pad_repeat=0xd42940'  # the gamepad's auto-repeat counter, paced in time (xiv_ui.c)
WRAPS += ',window_arrange=0x925490'  # Sqwt Window arrange: the talk window's startup placement, centred (xiv_ui.c)
# hot functions run natively (runtime/portable/xiv_native.c)
WRAPS += ',crt_memcpy=0x9d4600,crt_memmove=0x9d5110,crt_memset=0x9d2110,const_cache_a=0x423b90,const_cache_b=0x423a40,mat4_vec4=0x42ec50'
# the dev ticket login: NetworkModule's ctor and Update (runtime/portable/xiv_direct_login.c, XIV_DIRECT_LOGIN)
WRAPS += ',net_ctor=0x4e0dc0,net_tick=0x4e30a0,net_session_login=0x4e1eb0'
# remote actors ground snap: ray start 1 above, and the snap log (runtime/portable/xiv_groundsnap.c, XIV_SNAPLOG)
WRAPS += ',snap_ground=0x660f70,actor_setpos=0x7ce080'
# the client's Lua C++ bindings, watched by the probe (runtime/portable/xiv_probe.c, XIV_PROBE)
WRAPS += ''.join(',lb_%s=0x%s' % (a, a) for a in re.findall(r'^LB\((\w+),', open(
    os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'runtime/portable/xiv_probe_binds.h')).read(), re.M))
GEN = 'generated/ffxivgame'
CFLAGS = ['-O2', '-mcpu=apple-m1', '-std=c11', '-g', '-DRT_GUEST_WINDOW', '-DRT_GUEST_FIXED=0x20000000000ull', '-fno-strict-aliasing', '-I', 'runtime', '-I',
          'runtime/portable', '-I', GEN]
GEN_FLAGS = ['-O3', '-mcpu=apple-m1', '-flto=thin', '-w']  # speed over build time: these are the game's code
PORTABLE = ['runtime/runtime.c', 'runtime/portable/plat_posix.c', 'runtime/portable/gwin.c',
            'runtime/portable/gthread.c', 'runtime/portable/thunk.c', 'runtime/portable/pe.c',
            'runtime/portable/k32.c', 'runtime/portable/vfs.c',
            'runtime/portable/kobj.c', 'runtime/portable/k32_io.c',
            'runtime/portable/reg.c', 'runtime/portable/ole.c',
            'runtime/portable/k32_misc.c', 'runtime/portable/k32_xiv.c',
            'runtime/portable/xiv_misc.c', 'runtime/portable/fiber.c', 'runtime/portable/xiv_ui.c', 'runtime/portable/xiv_probe.c', 'runtime/portable/sampler_posix.c',
            'runtime/portable/xiv_native.c', 'runtime/portable/xiv_direct_login.c', 'runtime/portable/xiv_groundsnap.c',
            'runtime/portable/xiv_discord.c', 'runtime/portable/app_icon.m']


# Win32 shims on SDL3 (windowing, input, timers, sockets, audio) shared with FFXI.
HOST = ['runtime/portable/user32.c', 'runtime/portable/input.c', 'runtime/portable/autoinput.c', 'runtime/portable/ws2.c',
        'runtime/portable/dinput.c', 'runtime/portable/dsound.c', 'runtime/portable/d3d9.c', 'runtime/portable/d3dx9.c', 'runtime/portable/xaudio2.c',
        'runtime/portable/sm_msl.c', 'runtime/portable/gfx9_metal.m']
GFX_LIBS = ['-framework', 'Metal', '-framework', 'QuartzCore', '-framework', 'Foundation', '-weak_framework', 'MetalFX']


def run(cmd):
    line = ' '.join(shlex.quote(c) for c in cmd)
    print('>', line if len(line) < 200 else line[:200] + ' ...', flush=True)
    subprocess.check_call(cmd, cwd=ROOT)


# XIV_PROGRESS: "@progress <step> <done> <total>" lines for each step (translate, compile, link), which
# tools/xiv_run.py turns into its progress window
PROGRESS = bool(os.environ.get('XIV_PROGRESS'))


def run_lto(cmd, cache, modules):
    """The ThinLTO link with a progress bar: each module the link compiles writes its cache file in
    `cache`, so the files written since the link started count the modules done (of `modules`, the
    bitcode objects; ones reused from the cache are quick and not counted). A terminal gets a bar;
    a log a line per tenth."""
    import time
    line = ' '.join(shlex.quote(c) for c in cmd)
    print('>', line if len(line) < 200 else line[:200] + ' ...', flush=True)
    os.makedirs(os.path.join(ROOT, cache), exist_ok=True)
    start = time.time() - 1
    p = subprocess.Popen(cmd, cwd=ROOT)
    tty, shown = sys.stdout.isatty(), -1
    while p.poll() is None:
        time.sleep(0.5)
        try:
            done = sum(1 for e in os.scandir(os.path.join(ROOT, cache)) if e.stat().st_mtime >= start)
        except OSError:
            done = 0
        frac = min(done / modules, 1.0) if modules else 0
        elapsed = time.time() - start
        if PROGRESS:
            print('@progress link %d %d' % (min(done, modules), modules), flush=True)
            continue
        if tty:
            sys.stdout.write('\r  linking [%-40s] %3d%%  %d / %d modules  %ds ' % (
                '#' * int(frac * 40), 100 * frac, min(done, modules), modules, elapsed))
            sys.stdout.flush()
        elif int(frac * 10) > shown:
            shown = int(frac * 10)
            print('  linking %d%% (%d / %d modules, %ds)' % (100 * frac, min(done, modules), modules, elapsed), flush=True)
    if tty:
        print()
    if p.returncode:
        raise subprocess.CalledProcessError(p.returncode, cmd)
    print('  linked in %ds (modules not counted were reused from the cache)' % (time.time() - start), flush=True)


def newest_header():
    newest = 0
    for d in ('runtime', 'runtime/portable'):
        for f in glob.glob(os.path.join(ROOT, d, '*.h')):
            newest = max(newest, os.path.getmtime(f))
    return newest


def compile_stale(sources, objdir, extra=(), gen_flags=None):
    os.makedirs(os.path.join(ROOT, objdir), exist_ok=True)
    headers = newest_header()
    # objects built with other flags (a moved guest base, PGO) are stale whatever their age
    stamp = os.path.join(ROOT, objdir, '.flags')
    sig = repr((CFLAGS, list(extra), gen_flags or GEN_FLAGS))
    if not os.path.exists(stamp) or open(stamp).read() != sig:
        headers = float('inf')
    objs, jobs = [], []
    for s in sources:
        obj = os.path.join(objdir, os.path.splitext(os.path.basename(s))[0] + '.o')
        objs.append(obj)
        full = os.path.join(ROOT, obj)
        src_time = max(os.path.getmtime(os.path.join(ROOT, s)), headers)
        if not os.path.exists(full) or os.path.getmtime(full) < src_time:
            flags = CFLAGS + list(extra) + ((gen_flags or GEN_FLAGS) if s.startswith('generated/') else [])
            if s.endswith('.m'):  # Objective-C, references counted by hand
                flags = [f for f in flags if f != '-std=c11'] + ['-fno-objc-arc']
            jobs.append(['clang', '-c'] + flags + [s, '-o', obj])
    if jobs:
        print('compiling %d of %d' % (len(jobs), len(sources)), flush=True)
        failed = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
            futures = {ex.submit(subprocess.call, c, cwd=ROOT): c for c in jobs}
            for n, f in enumerate(concurrent.futures.as_completed(futures), 1):
                if f.result():
                    failed.append(futures[f][-3])
                if PROGRESS:
                    print('@progress compile %d %d' % (n, len(jobs)), flush=True)
        if failed:
            raise SystemExit('compile failed: ' + ', '.join(failed))
    with open(stamp, 'w') as f:
        f.write(sig)
    return objs


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def meta(exe, ghidra):
    head = glob.glob('/opt/homebrew/opt/ghidra/libexec/support/analyzeHeadless')
    if not head:
        raise SystemExit('Ghidra not found (brew install ghidra)')
    run([head[0], os.path.abspath(ghidra), 'ffxivgame', '-process', 'ffxivgame.exe', '-noanalysis', '-readOnly',
         '-scriptPath', 'discovery', '-postScript', 'ExportFFXIV.java', os.path.join(ROOT, META), sha256(exe), '1.23b'])


def extra_entries(exe):
    """meta/extra_entries.json's functions for this build of an exe (recomp.py --entries)"""
    with open(os.path.join(ROOT, 'meta', 'extra_entries.json')) as f:
        known = json.load(f).get(sha256(exe))
    return ['--entries', ','.join(known['entries'])] if known else []


def translate(exe):
    run([sys.executable, 'recomp/recomp.py', '--meta', META, '--apply', APPLY, '--image', exe, '--retail', exe, '--out',
         GEN, '--all', '--wraps', WRAPS] + extra_entries(exe))


def xivboot(exe, run_it, extra, debug=False, opt='-O1', pgo=None):
    if not os.path.exists(os.path.join(ROOT, GEN, 'table.c')):
        translate(exe)
    gen = sorted(os.path.relpath(p, ROOT) for p in glob.glob(os.path.join(ROOT, GEN, '*.c')))
    if debug:
        # lldb build: line tables + locals for the translation, no LTO so functions keep their
        # own frames (b f_XXXXXXXX stops where you expect). Objects stay in build/xiv-dbg; the
        # binary's debug map points at them, so no dsymutil step is needed.
        objs = compile_stale(gen, 'build/xiv-dbg/gen', gen_flags=[opt, '-g', '-mcpu=apple-m1', '-w'])
        thirdparty.build('sdl3')
        objs += compile_stale(PORTABLE + HOST + ['tests/xivboot.c'], 'build/xiv-dbg/obj', thirdparty.flags('sdl3'))
        run(['clang', '-o', 'build/xivboot-dbg', '-g', '-mcpu=apple-m1']
            + objs + thirdparty.libs('sdl3') + GFX_LIBS + ['-lm', '-lpthread'])
        if run_it:
            run(['build/xivboot-dbg', exe] + extra)
        return
    # profile-guided optimization of the translation (--pgo gen, play, --pgo use): branchy code
    # whose hot paths the compiler cannot guess. gen builds into build/xiv-pgo and writes
    # build/pgo/*.profraw when XIV_PGO_FRAMES frames have been drawn; use merges them.
    pgo_flags, gendir = [], 'build/xiv/gen'
    if pgo == 'gen':
        os.makedirs(os.path.join(ROOT, 'build/pgo'), exist_ok=True)
        pgo_flags, gendir = ['-fprofile-generate=' + os.path.join(ROOT, 'build/pgo')], 'build/xiv-pgo/gen'
    elif pgo == 'use':
        prof = os.path.join(ROOT, 'build/pgo/merged.profdata')
        raws = glob.glob(os.path.join(ROOT, 'build/pgo/*.profraw'))
        if raws:
            run(['xcrun', 'llvm-profdata', 'merge', '-o', prof] + raws)
        if not os.path.exists(prof):
            raise SystemExit('--pgo use: no profile (build/pgo/merged.profdata); run a --pgo gen build first')
        pgo_flags, gendir = ['-fprofile-use=' + prof, '-Wno-profile-instr-out-of-date', '-Wno-profile-instr-unprofiled'], 'build/xiv-pgouse/gen'
    objs = compile_stale(gen, gendir, gen_flags=GEN_FLAGS + pgo_flags)
    thirdparty.build('sdl3')
    objs += compile_stale(PORTABLE + HOST + ['tests/xivboot.c'], 'build/xiv/obj', thirdparty.flags('sdl3'))
    # ThinLTO: the translated functions call each other across 291 files; inlining the small ones
    # into their callers needs the whole program (cache: relinks reuse unchanged modules)
    # -U: d3d9.c's profile dump finds the profile runtime only in --pgo gen builds
    # XIV_INLINE=<threshold>: LTO inlining experiments (import limit and inline threshold)
    inl = os.environ.get('XIV_INLINE')
    inl_flags = ['-Wl,-mllvm,-import-instr-limit=%d' % (int(inl) * 2), '-Wl,-mllvm,-inline-threshold=%s' % inl] if inl else []
    run_lto(['clang', '-o', 'build/xivboot', '-flto=thin', '-O3', '-mcpu=apple-m1', '-Wl,-U,___llvm_profile_write_file'] + pgo_flags + inl_flags + [
             '-Wl,-cache_path_lto,build/xiv/lto-cache', '-Wl,-mllvm,-threads=%d' % (os.cpu_count() or 4)]
            + objs + thirdparty.libs('sdl3') + GFX_LIBS + ['-lm', '-lpthread'], 'build/xiv/lto-cache', len(gen))
    xiv_bundle(exe)
    with open(os.path.join(ROOT, 'build', 'xivboot.sha256'), 'w') as f:  # which ffxivgame.exe (xiv_run.py)
        f.write(sha256(exe) + '\n')
    if run_it:
        run(['build/xivboot', exe] + extra)


# the launcher programs: no game hooks, their own translation each (tests/xivexe.c)
EXE_PORTABLE = [p for p in PORTABLE if not re.search(r'xiv_(ui|probe|native|direct_login|groundsnap|discord)\.c$', p)]
EXE_HOST = [p for p in HOST if 'xaudio2' not in p] + ['runtime/portable/k32_launch.c', 'runtime/portable/ui_appkit.m', 'runtime/portable/webbrowser.m', 'runtime/portable/winhttp.c']


def program_meta(exe, name):
    """The address map for this build of a launcher program: the meta/<name>.<build>.meta.json whose
    sha256 is the exe's (the disc's builds and the patched ones differ), and that sha256."""
    want = sha256(exe)
    for p in sorted(glob.glob(os.path.join(ROOT, 'meta', name + '.*.meta.json'))):
        with open(p) as f:
            m = re.search(r'"sha256":\s*"([0-9a-f]{64})"', f.read(4096))
        if m and m.group(1) == want:
            return os.path.relpath(p, ROOT), want
    raise SystemExit('%s: no address map for this build (sha256 %s) in meta/' % (exe, want))


def program_current(exe, name):
    """Whether build/bin/<name> was built from this exe (build/bin/<name>.sha256 says what from)."""
    stamp = os.path.join(ROOT, 'build', 'bin', name + '.sha256')
    return (os.path.exists(os.path.join(ROOT, 'build', 'bin', name)) and os.path.exists(stamp)
            and open(stamp).read().strip() == sha256(exe))


def program(exe, name, run_it, extra):
    """build/bin/<name> (ffxivboot, ffxivlogin, ffxivupdater): the exe translated with its address map
    into generated/<name>-<sha256 prefix>, linked with the shims the game uses. The updater replaces
    these programs; build/bin/<name>.sha256 records which build this one is, for program_current."""
    meta_path, want = program_meta(exe, name)
    tag = '%s-%s' % (name, want[:12])
    gen_dir = 'generated/' + tag
    if not os.path.exists(os.path.join(ROOT, gen_dir, 'table.c')):
        run([sys.executable, 'recomp/recomp.py', '--meta', meta_path, '--image', exe,
             '--retail', exe, '--out', gen_dir, '--all'] + extra_entries(exe))
    gen = sorted(os.path.relpath(p, ROOT) for p in glob.glob(os.path.join(ROOT, gen_dir, '*.c')))
    objs = compile_stale(gen, 'build/%s/gen' % tag, gen_flags=['-O2', '-mcpu=apple-m1', '-w'])
    thirdparty.build('sdl3')
    objs += compile_stale(EXE_PORTABLE + EXE_HOST + ['tests/xivexe.c'], 'build/exe/obj', thirdparty.flags('sdl3'))
    os.makedirs(os.path.join(ROOT, 'build/bin'), exist_ok=True)
    if PROGRESS:
        print('@progress link 0 1', flush=True)
    run(['clang', '-o', 'build/bin/' + name, '-mcpu=apple-m1', '-Wl,-U,___llvm_profile_write_file'] + objs + thirdparty.libs('sdl3') + GFX_LIBS + ['-framework', 'WebKit', '-lm', '-lpthread'])
    if PROGRESS:
        print('@progress link 1 1', flush=True)
    with open(os.path.join(ROOT, 'build', 'bin', name + '.sha256'), 'w') as f:
        f.write(want + '\n')
    if run_it:
        run(['build/bin/' + name, exe] + extra)


XIV_APP = 'build/FINAL FANTASY XIV.app'
XIV_PLIST = '''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key><string>xivboot</string>
	<key>CFBundleIdentifier</key><string>local.ffxiv10.recomp</string>
	<key>CFBundleName</key><string>FINAL FANTASY XIV</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleShortVersionString</key><string>1.23b</string>
	<key>CFBundleVersion</key><string>1</string>
	<key>LSApplicationCategoryType</key><string>public.app-category.role-playing-games</string>
	<key>LSSupportsGameMode</key><true/>
	<key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
'''


def exe_icns(exe, out):
    """The exe's own icon (its RT_GROUP_ICON and RT_ICON resources, put back together as a .ico) as
    a .icns for an app bundle, made from the player's file at build time. False when it has none."""
    import struct
    import tempfile
    import pefile
    pe = pefile.PE(exe, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_RESOURCE']])
    types = {t.id: t for t in pe.DIRECTORY_ENTRY_RESOURCE.entries} if hasattr(pe, 'DIRECTORY_ENTRY_RESOURCE') else {}
    if 14 not in types or 3 not in types:
        return False

    def data(tid, name=None):
        for e in types[tid].directory.entries:
            if name is None or e.id == name:
                d = e.directory.entries[0].data.struct
                return pe.get_data(d.OffsetToData, d.Size)
        return b''
    grp = data(14)
    n = struct.unpack_from('<H', grp, 4)[0]
    images = [data(3, struct.unpack_from('<H', grp, 6 + 14 * i + 12)[0]) for i in range(n)]
    ico = bytearray(struct.pack('<HHH', 0, 1, n))
    off = 6 + 16 * n
    for i in range(n):
        ico += grp[6 + 14 * i:6 + 14 * i + 8] + struct.pack('<II', len(images[i]), off)
        off += len(images[i])
    for d in images:
        ico += d
    with tempfile.NamedTemporaryFile(suffix='.ico') as f:
        f.write(ico)
        f.flush()
        os.makedirs(os.path.dirname(out), exist_ok=True)
        return subprocess.call(['sips', '-s', 'format', 'icns', f.name, '--out', out], stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL) == 0


def xiv_bundle(game_exe=None):
    """build/xivboot inside an app bundle (a hard link), so macOS sees a game: Game Mode turns on
    when it runs full screen, and it shows ffxivgame.exe's icon. xiv_login.py starts this copy."""
    macos = os.path.join(ROOT, XIV_APP, 'Contents', 'MacOS')
    os.makedirs(macos, exist_ok=True)
    exe = os.path.join(macos, 'xivboot')
    if os.path.lexists(exe):
        os.remove(exe)
    os.link(os.path.join(ROOT, 'build', 'xivboot'), exe)
    plist = XIV_PLIST
    if game_exe and exe_icns(game_exe, os.path.join(ROOT, XIV_APP, 'Contents', 'Resources', 'AppIcon.icns')):
        plist = plist.replace('<key>CFBundleName</key>', '<key>CFBundleIconFile</key><string>AppIcon</string>\n\t<key>CFBundleName</key>')
    with open(os.path.join(ROOT, XIV_APP, 'Contents', 'Info.plist'), 'w') as f:
        f.write(plist)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('target', choices=('meta', 'translate', 'xivboot', 'exe'))
    ap.add_argument('--exe', required=True, help='ffxivgame.exe (1.23b, 2012.09.19.0001)')
    ap.add_argument('--ghidra', help='meteor-decomp/build/ghidra (meta)')
    ap.add_argument('--run', action='store_true')
    ap.add_argument('--debug', action='store_true', help='build/xivboot-dbg: -g translation, no LTO (for lldb)')
    ap.add_argument('--opt', default='-O1', help='optimization for the translation in --debug builds (-O0 keeps every register local visible but runs slowly)')
    ap.add_argument('--pgo', choices=('gen', 'use'), help='profile-guided optimization: an instrumented build, or one using its profile')
    ap.add_argument('--name', help='exe: which launcher program (ffxivboot, ffxivlogin, ffxivupdater); default from --exe')
    ap.add_argument('rest', nargs='*')
    a = ap.parse_args()
    exe = os.path.abspath(a.exe)
    if a.target == 'meta':
        meta(exe, a.ghidra or os.path.join(ROOT, '..', 'decomp', 'build', 'ghidra'))
    elif a.target == 'exe':
        program(exe, a.name or os.path.splitext(os.path.basename(exe))[0], a.run, a.rest)
    elif a.target == 'translate':
        translate(exe)
    else:
        xivboot(exe, a.run, a.rest, a.debug, a.opt, a.pgo)


if __name__ == '__main__':
    main()
