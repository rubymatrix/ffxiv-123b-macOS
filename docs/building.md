# Building by hand

For development, against a game folder already patched to 1.23b. Players don't need this: use the
[installer](install.md).

```sh
python3 -m venv .venv && .venv/bin/pip install capstone pefile pycryptodome
.venv/bin/python tools/build_xiv.py translate --exe /path/to/ffxivgame.exe
.venv/bin/python tools/build_xiv.py xivboot   --exe /path/to/ffxivgame.exe
```

This makes `build/xivboot`. To sign in and start it without the launcher:

```sh
.venv/bin/python tools/xiv_login.py --meteor --server http://127.0.0.1:8000 --user NAME \
    --game "/path/to/FINAL FANTASY XIV"
```

`--lobby` sets the lobby host. `--signup` makes the account first.

`tools/xiv_e2e.sh` tests the whole install, from the disc to the character list, in folders of its
own.
