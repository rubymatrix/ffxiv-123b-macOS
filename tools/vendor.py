"""Vendor SDL3 into third_party/, so a player's Mac needs only clang to build it.

  python3 tools/vendor.py [sdl3 ...]

For the maintainer, not the player: needs cmake and network. For each library it downloads the
pinned release (checked against its SHA-256), configures it once with cmake to learn the exact
sources and compiler flags, compiles it with clang to find every header those sources include,
and copies only those files, with the license, to third_party/<name>/. The flags are written to
third_party/<name>/manifest.json, which tools/thirdparty.py builds from with plain clang: no cmake,
no Homebrew, no pkg-config on the player's machine.

To move to a new release, change its version, url and sha256 in LIBS and run this again.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

LIBS = {
    'sdl3': {
        'version': '3.4.16',
        'url': 'https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz',
        'sha256': '7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68',
        'license': ['LICENSE.txt'],
        # what the game uses: video (Cocoa + Metal view), events, gamepads, audio
        'cmake': ['-DSDL_SHARED=OFF', '-DSDL_STATIC=ON', '-DSDL_TESTS=OFF', '-DSDL_EXAMPLES=OFF', '-DSDL_INSTALL=OFF',
                  '-DSDL_CAMERA=OFF', '-DSDL_GPU=OFF', '-DSDL_RENDER=OFF', '-DSDL_SENSOR=OFF', '-DSDL_DIALOG=OFF',
                  '-DSDL_TRAY=OFF', '-DSDL_OPENGL=OFF', '-DSDL_OPENGLES=OFF', '-DSDL_VULKAN=OFF',
                  '-DSDL_DISKAUDIO=OFF', '-DSDL_DUMMYAUDIO=OFF', '-DSDL_DUMMYVIDEO=OFF', '-DSDL_OFFSCREEN=OFF',
                  '-DSDL_HIDAPI_LIBUSB=OFF', '-DSDL_DEPS_SHARED=OFF', '-DCMAKE_OSX_ARCHITECTURES=arm64'],
        'public': ['include'],
        'frameworks': ['CoreMedia', 'CoreVideo', 'Cocoa', 'IOKit', 'ForceFeedback', 'Carbon', 'CoreAudio',
                       'AudioToolbox', 'AVFoundation', 'Foundation', 'GameController', 'Metal', 'QuartzCore'],
        'weak_frameworks': ['UniformTypeIdentifiers', 'CoreHaptics'],
    },
}
# flags of cmake's that the manifest keeps (the rest are warnings, colors, pch and output paths)
KEEP_FLAGS = ('-D', '-fobjc-arc', '-fno-strict-aliasing')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def vendor(name, work):
    lib = LIBS[name]
    tarball = os.path.join(work, os.path.basename(lib['url']))
    print('%s %s: downloading' % (name, lib['version']))
    urllib.request.urlretrieve(lib['url'], tarball)
    if sha256(tarball) != lib['sha256']:
        raise SystemExit('%s: %s has SHA-256 %s, not the pinned %s' % (name, lib['url'], sha256(tarball), lib['sha256']))
    with tarfile.open(tarball) as t:
        top = t.getnames()[0].split('/')[0]
        t.extractall(work)
    src = os.path.realpath(os.path.join(work, top))
    bld = os.path.realpath(os.path.join(work, 'build-' + name))
    print('%s: cmake' % name)
    subprocess.run(['cmake', '-S', src, '-B', bld, '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON']
                   + lib['cmake'], check=True, stdout=subprocess.DEVNULL)

    def rel(p):
        """A path in the source tree -> relative to the vendored folder; generated -> gen/..."""
        p = os.path.realpath(p)
        if p.startswith(src + '/'):
            return p[len(src) + 1:]
        if p.startswith(bld + '/'):
            return 'gen/' + p[len(bld) + 1:]
        return None

    groups, order = {}, []
    for e in json.load(open(os.path.join(bld, 'compile_commands.json'))):
        f = rel(e['file'])
        if f is None or not f.endswith(('.c', '.m')) or f.startswith(('gen/', 'src/test/')):
            continue  # cmake's precompiled header, SDL's test library
        toks = e['command'].split()
        inc, after, flags = [], [], []
        i = 0
        while i < len(toks):
            t = toks[i]
            if t.startswith('-I'):
                inc.append(rel(t[2:]))
            elif t.startswith('-idirafter'):
                after.append(rel(t[len('-idirafter'):] or toks[i + 1]))
                i += 0 if len(t) > len('-idirafter') else 1
            elif t.startswith(KEEP_FLAGS) and t not in ('-DNDEBUG',):
                flags.append(t)
            i += 1
        key = (tuple(inc), tuple(after), tuple(sorted(set(f for f in flags if f != '-fobjc-arc'))))
        if key not in groups:
            groups[key] = []
            order.append(key)
        groups[key].append(f)

    # compile it once as the manifest says, to learn every file the sources read
    print('%s: trial build' % name)
    objs = os.path.join(work, 'obj-' + name)
    os.makedirs(objs)
    jobs = []
    for n, key in enumerate(order):
        inc, after, flags = key
        for f in groups[key]:
            arc = ['-fobjc-arc'] if f.endswith('.m') else []
            o = os.path.join(objs, '%d_%s.o' % (n, f.replace('/', '_')))
            jobs.append(['clang', '-O2', '-w', '-mmacosx-version-min=12.0'] + list(flags) + arc
                        + ['-I' + os.path.join(src if not d.startswith('gen/') else bld, d[4:] if d.startswith('gen/') else d)
                           for d in inc]
                        + sum((['-idirafter', os.path.join(src, d)] for d in after), [])
                        + ['-MD', '-c', os.path.join(src, f), '-o', o])
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as ex:
        for r in ex.map(lambda c: subprocess.run(c, capture_output=True, text=True), jobs):
            if r.returncode:
                raise SystemExit('%s: trial build failed:\n%s' % (name, r.stderr[-2000:]))
    keep = set(f for fs in groups.values() for f in fs)
    for d in os.listdir(objs):
        if d.endswith('.d'):
            deps = open(os.path.join(objs, d)).read().replace('\\\n', ' ').split(':', 1)[1].split()
            keep.update(r for r in map(rel, deps) if r)
    for d in lib['public']:  # every public header, used or not
        for dirpath, _, files in os.walk(os.path.join(src, d)):
            keep.update(rel(os.path.join(dirpath, f)) for f in files if f.endswith('.h'))
    keep.update(lib['license'])

    dest = os.path.join(ROOT, 'third_party', name)
    shutil.rmtree(dest, ignore_errors=True)
    for f in sorted(keep):
        origin = os.path.join(bld, f[4:]) if f.startswith('gen/') else os.path.join(src, f)
        os.makedirs(os.path.dirname(os.path.join(dest, f)), exist_ok=True)
        shutil.copy(origin, os.path.join(dest, f))
    # include dirs with no header left in them are dropped
    have = set(os.path.dirname(f) for f in keep)

    def live(d):
        return any(h == d or h.startswith(d + '/') for h in have)
    manifest = {
        'version': lib['version'],
        'public': lib['public'],
        'frameworks': lib['frameworks'],
        'weak_frameworks': lib['weak_frameworks'],
        'groups': [{'include': [d for d in key[0] if live(d)], 'idirafter': list(key[1]), 'flags': list(key[2]),
                    'sources': sorted(groups[key])} for key in order],
    }
    with open(os.path.join(dest, 'manifest.json'), 'w') as f:
        json.dump(manifest, f, indent=1)
        f.write('\n')
    print('%s: %d files, %d sources in third_party/%s' % (name, len(keep), sum(map(len, groups.values())), name))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('libs', nargs='*', default=list(LIBS), choices=list(LIBS))
    args = ap.parse_args()
    if not shutil.which('cmake'):
        raise SystemExit('vendor.py needs cmake (for the maintainer only; players never run it)')
    with tempfile.TemporaryDirectory() as work:
        for name in args.libs:
            vendor(name, work)


if __name__ == '__main__':
    sys.exit(main())
