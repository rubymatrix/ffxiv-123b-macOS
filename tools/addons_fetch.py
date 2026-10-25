"""Fetch Ashita's and Windower's addons, libraries and resources into the addon host's folders.

  python3 tools/addons_fetch.py [--data-dir <folder>] [--only ashita|windower|<source>] [--list]

--only takes a kind or one source by name (--only windower-resources fetches Windower's res/ alone,
leaving the addons and libraries there as they are).

The addon host (host/addons/) runs Ashita v4 and Windower 4 addons with
those projects' own Lua libraries, unchanged. They aren't ours, so the repository never carries them:
this downloads each project's published archive at a pinned commit (PINS) and lays it out in the
data folder the way each project's own install is laid out:

  <data dir>/ashita/addons/...        Ashita's addons and addons/libs
  <data dir>/ashita/config/...        Ashita's configuration files (only files missing here)
  <data dir>/windower/addons/...      Windower's addons and addons/libs
  <data dir>/windower/res/...         Windower's resources (the res library reads them)

Files from the archives replace the copies here (updates), except configuration files the player
may have changed, which are only added when missing. Nothing else in the folders is touched, so the
player's own addons and settings stay. Needs network; uses only Python's standard library.
"""
import argparse
import io
import os
import sys
import tarfile
import urllib.request

DATA_DIR = os.environ.get('FFXI_DATA_DIR') or os.path.expanduser('~/Library/Application Support/FFXIRecompile/FFXI')

PINS = {
    'ashita': {
        'repo': 'AshitaXI/Ashita-v4beta',
        'commit': '4171c74c8ddb2ca2a31654f199e6c1cee40d7256',
        # archive folder -> (where under <data dir>/ashita, replace existing files?)
        'take': {'addons': ('addons', True), 'config': ('config', False)},
    },
    'windower-lua': {
        'repo': 'Windower/Lua',
        'commit': '5a35aeffa01aa47ea2a7898a7015fccb586728a0',
        'kind': 'windower',
        'take': {'addons': ('addons', True)},
    },
    'windower-resources': {
        'repo': 'Windower/Resources',
        'commit': '99f552fa7cdd8e4a65e3dfa5bbd7d9462ac60b89',
        'kind': 'windower',
        'take': {'resources_data': ('res', True)},
    },
}


def fetch(name, pin, data_dir):
    kind = pin.get('kind', name)
    url = 'https://codeload.github.com/%s/tar.gz/%s' % (pin['repo'], pin['commit'])
    print('%s: %s at %s' % (name, pin['repo'], pin['commit'][:10]), flush=True)
    with urllib.request.urlopen(url) as r:
        blob = r.read()
    root = os.path.join(data_dir, kind)
    written = kept = 0
    with tarfile.open(fileobj=io.BytesIO(blob), mode='r:gz') as t:
        for m in t.getmembers():
            if not m.isfile():
                continue
            parts = m.name.split('/', 2)  # <repo>-<commit>/<top>/<rest>
            if len(parts) < 3 or parts[1] not in pin['take']:
                continue
            where, replace = pin['take'][parts[1]]
            rel = parts[2]
            if '..' in rel.split('/'):
                continue
            dest = os.path.join(root, where, *rel.split('/'))
            if os.path.exists(dest) and not replace:
                kept += 1
                continue
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with t.extractfile(m) as src, open(dest, 'wb') as out:
                out.write(src.read())
            written += 1
    print('  %d files written under %s%s' % (written, root, ', %d kept' % kept if kept else ''))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data-dir', default=DATA_DIR)
    ap.add_argument('--only', choices=['ashita', 'windower'] + sorted(PINS))
    ap.add_argument('--list', action='store_true', help='print the pinned sources and stop')
    a = ap.parse_args()
    for name, pin in PINS.items():
        if a.only and a.only not in (name, pin.get('kind', name)):
            continue
        if a.list:
            print('%-20s https://github.com/%s/tree/%s' % (name, pin['repo'], pin['commit']))
            continue
        fetch(name, pin, a.data_dir)
    return 0


if __name__ == '__main__':
    sys.exit(main())
