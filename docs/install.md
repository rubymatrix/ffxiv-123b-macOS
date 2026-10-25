# Installing

You need:

- A Mac with Apple silicon (M1 or later)
- The FINAL FANTASY XIV disc, or an image of it
- The address of a 1.23b server to play on

## 1. Run the installer

Mount the disc (double-click the disc image), then paste this into Terminal. Put your server's
address in place of `<host>`:

```sh
curl -fsSL https://raw.githubusercontent.com/rubymatrix/ffxiv-classic-recomp/main/install.command | bash -s -- \
    --server <host> --account-url http://<host>:8090
```

If Apple's command line tools are missing, a dialog pops up first. Click **Install** and wait; the
installer carries on by itself.

The installer copies the game off the disc, builds it for your Mac and puts
**FINAL FANTASY XIV.app** in your Applications folder. It takes a few minutes. Nothing from the
disc leaves your Mac.

## 2. Patch and play

Open **FINAL FANTASY XIV.app**. The first time, it downloads the game's patches from your server
and updates the game to 1.23b. This takes a while. After that it goes straight to the login page.

Sign in, and the game starts.

## Updating

Run the same line again. It picks up the newest version and carries on where it left off.

## Options

Add these after `bash -s --`:

| Option | What it does |
|---|---|
| `--server <host>` | The server to play on |
| `--account-url <url>` | The server's login page |
| `--disc <folder>` | Where the disc is, if it isn't found by itself |
| `--install-dir <folder>` | Where the game goes (default `~/Library/Application Support/FFXIV1_0`) |
| `--app-dir <folder>` | Where the app goes (default `/Applications`) |

## Something went wrong?

- **"needs a Mac with Apple silicon"**: this only runs on M1 or later.
- **Disc not found**: make sure it is mounted, or pass `--disc /Volumes/<disc name>`.
- **Patching stops**: check the server is up, then open the app again. It resumes.
- The build log is at `~/Library/Application Support/FFXIV1_0/source/build/run.log`.
- Still stuck? Ask on the [XI on Anything Discord](https://discord.gg/4vUKPgyvEp).
