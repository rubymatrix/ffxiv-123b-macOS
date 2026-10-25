# ffxiv-123b-macOS

![In game](screenshots/ffxiv-in-game.webp)

FINAL FANTASY XIV 1.23b (the original 1.0 client) running natively on Apple silicon Macs. No Wine,
no Rosetta.

The game's 32-bit x86 executable is statically translated to C on your machine, from your own
install, and compiled as an arm64 macOS app. Windows, Direct3D 9, audio, input and networking are
replaced by a macOS platform layer (Direct3D 9 runs on Metal).

It connects to 1.23b private servers such as
[Project Meteor](https://github.com/Ioncannon/project-meteor-server).

## Requirements

- Apple silicon Mac (M1 or later)
- The FINAL FANTASY XIV retail disc or its image. The patcher brings it up to 1.23b.
- A 1.23b server running [meteor-web](https://github.com/rubymatrix/Project-Meteor-Web) for the
  version check, patches and login page

Apple's command line tools and Python 3 come with them. The installer gets them when missing.

## Install

Mount the disc (or its image) and double-click `install.command`. Or paste into Terminal:

```sh
curl -fsSL https://raw.githubusercontent.com/rubymatrix/ffxiv-classic-recomp/main/install.command | bash -s -- \
    --server <host> --account-url http://<host>:8090
```

The installer:

1. Gets Apple's command line tools if they are missing (a dialog opens: click Install).
2. Clones these sources into `~/Library/Application Support/FFXIV1_0/source`.
3. Unpacks the disc's InstallShield cabinets natively into
   `~/Library/Application Support/FFXIV1_0/FINAL FANTASY XIV`. None of the disc's code runs.
4. Translates and compiles the disc's `ffxivboot.exe` (launcher) and `ffxivupdater.exe` (patcher).
5. Fetches the server's patch signing certificate (`/patch/cert` on meteor-web).
6. Puts `FINAL FANTASY XIV.app` in `/Applications` (`~/Applications` if that is not writable) and
   opens it.

Options (after `bash -s --`):

| Option | What |
|---|---|
| `--server <host>` | The server's host. Square Enix's host names go here. Default `127.0.0.1` |
| `--account-url <url>` | The server's login page, e.g. `http://<host>:8090` |
| `--disc <folder>` | The mounted disc. Default: found under `/Volumes` |
| `--install-dir <folder>` | Where the game goes |
| `--app-dir <folder>` | Where the app goes |
| `--patch-cert <file>` | Use this certificate instead of fetching it |
| `--no-unpack` | The install folder is already unpacked |

Run it again to update the sources and resume a stopped install. `XIV_SOURCE`, `XIV_REPO` and
`XIV_BRANCH` override where the sources go, where they come from and which branch.

## Patch

Start `FINAL FANTASY XIV.app`. It runs the native ffxivboot (`tools/xiv_run.py`):

1. ffxivboot checks the version with the server and downloads the patches. It checks each patch
   against the server's certificate.
2. The native ffxivupdater applies them, up to 1.23b.
3. ffxivlogin opens the server's login page. Sign in and the game starts.

Each program is translated again when a patch replaces it. A watcher builds them in the background
as the patches land: the patched updater while patches download, ffxivlogin and the game while
they apply. So each is ready by the time it starts. The first patch run takes a while; later starts
skip straight to sign-in. Build output goes to `build/run.log` in the sources folder.

`tools/xiv_e2e.sh` tests all of it end to end, from the disc to the lobby's character list, in
folders of its own (a fresh clone, install, Documents and app folder).

## Build from source

For development, against an install already at 1.23b:

```sh
python3 -m venv .venv && .venv/bin/pip install capstone pefile pycryptodome
.venv/bin/python tools/build_xiv.py translate --exe /path/to/ffxivgame.exe
.venv/bin/python tools/build_xiv.py xivboot   --exe /path/to/ffxivgame.exe
```

This produces `build/xivboot`. To sign in and start it without the launcher:

```sh
.venv/bin/python tools/xiv_login.py --meteor --server http://127.0.0.1:8000 --user NAME \
    --game "/path/to/FINAL FANTASY XIV"
```

`--lobby` sets the lobby host; `--signup` creates the account first.

## No game files here

This repo contains no Square Enix code or data, and never will. The translated C is generated
locally from your own copy of the game and is not committed.

## Credits

The recompiler and runtime come from
[xi-on-anything](https://github.com/rubymatrix/xi-on-anything), the same approach applied to
FINAL FANTASY XI.

## License

MIT (see `LICENSE`). FINAL FANTASY is a registered trademark of Square Enix Holdings Co., Ltd.
This project is not affiliated with or endorsed by Square Enix.
