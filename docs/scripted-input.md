# Scripted input (`XIV_INPUT_SCRIPT`)

Drives the client without a human: a timeline of key presses, gamepad buttons and sticks, mouse
clicks and screenshots, played into the game as if a player made them. Used for unattended
login + in-world tests.

Code: `runtime/portable/autoinput.c` (timeline, ticked once per presented frame from
`d3d9.c present()`), scripted pad in `runtime/portable/input.c`, window/cursor helpers in
`runtime/portable/user32.c`.

## How the input reaches the game

The client reads the keyboard three ways: window messages (`WM_KEYDOWN`/`WM_CHAR`),
`GetKeyboardState`, and DirectInput8 (`GetDeviceState` and the buffered `GetDeviceData`). The
gamepad is enumerated through DirectInput and, being an Xbox-style pad, read through XInput
(`xinput1_3` ordinal 2).

- **Keys and mouse** are synthesized SDL events pushed onto SDL's queue (`SDL_PushEvent`). The
  window pump turns them into all of the above exactly as it does real ones, so `down` holds a
  key everywhere until `up`. (The old `XIV_AUTOKEYS` path only posted `WM_KEYDOWN`+`WM_KEYUP`,
  which the menus ignore.)
- **Gamepad**: while `XIV_INPUT_SCRIPT` (or `XIV_VPAD=1`) is set, pad 0 exists from start-up as
  "Scripted pad" (DirectInput enumeration, XInput and DirectInput state). What the script holds is
  added on top of a real pad 0 if one is connected. `XIV_VPAD=0` turns the scripted pad off.
- **Mouse**: `click`/`move` also override what `GetCursorPos` reports.

## Script format

One action per line; `#` starts a comment.

```
<time> <command> [args]
```

`<time>` is milliseconds since the first frame, or `+ms` after the previous line. The file is
re-read every 100 ms: **lines appended while the game runs are played too**, and a `+ms` time on
an appended line counts from when it is read (so `+0` means now).

| command | effect |
|---|---|
| `key K [hold_ms]` | press K, release after hold (default 120) |
| `down K` / `up K` | hold / release K |
| `pad BTN [hold_ms]` | press a pad button (default 150): `a b x y lb rb back start ls rs up down left right` |
| `paddown BTN` / `padup BTN` | hold / release |
| `stick l\|r X Y [hold_ms]` | stick position, -1..1, Y up = forward; without hold it stays until the next `stick` |
| `trigger l\|r V [hold_ms]` | trigger 0..1 |
| `move X Y` | cursor to X,Y as fractions (0..1) of the window |
| `click X Y [left\|right\|middle] [hold_ms]` | move there and click |
| `shot NAME` | back buffer to `<dir>/NAME.ppm` (`XIV_INPUT_SHOTS`, else the script's folder); `sips -s format png NAME.ppm --out NAME.png` |
| `log TEXT` | `[input] <ms>: TEXT` in the client log (sync point for shell loops) |
| `quit` | exit the client |

| `walkto X Z [tol] [timeout_ms] [Y]` | walk to world X,Z (default tol 1.0, timeout 60000; with Y, also within 1.2 of that height); see Navigation |
| `via X Z [tol] [timeout_ms] [Y]` | a waypoint: like `walkto` (default tol 1.5) but at full speed and the stick stays held for the next one |
| `walkpath X1 Z1 X2 Z2 ...` | `via` each point, `walkto` the last (what `tools/input/route.py --walkpath` prints) |
| `path X Z Y [X Z Y ...]` / `path clear` | collect a route (any number of lines) for the next `follow` |
| `follow [tol] [timeout_ms]` | walk the collected route (pure pursuit; default tol 0.8, timeout 180000); **what `route.py` prints by default** |
| `navstats` | log `navstats contacts=N stalls=M dist=D time=T` since the last one, and reset |
| `navset maxmag\|minmag V` | cap / floor the stick magnitude while walking (0.5 walks) |
| `face X Z` / `turn DEG` | turn toward a point / by DEG (positive = left, the game's rotation sense) |
| `pos` | `[input] <ms>: pos=x,y,z rot=r` (the client's own player position) |
| `stop` | end a running `walkto`/`face`/`waitfor`/`target`/`interact` now |
| `navtrace MS` | log the steering state every MS while walking (0 = off) |
| `target ID [tries] [key]` | press Tab (or key) until the client targets actor ID (hex); see Interaction |
| `untarget [key]` | press Escape (or key) until the client sends "no target" |
| `interact [key]` | press Return (or key) and wait for the client's EventStart (0x012D) |
| `choose N` | menu item N (1 = first): `Down` N-1 times, then `Return` |
| `waitfor in\|out\|any OPCODE [timeout_ms]` | wait for a game packet (hex opcode) since the previous action |
| `nettrace 0\|1` | log every game packet as `[net] t=<unix ms> in/out 0xOPCODE src=... len=... <payload>` (not 0xCA/0xCF/0x01/0x02); `t` is the server capture's clock |

`walkto`, `via`, `follow`, `face`, `turn`, `waitfor`, `target`, `untarget` and `interact` **block the
timeline**: the lines after them wait, and their times are shifted by however long the blocking
command took (so `+500 shot x` after a walk is 500 ms after the walk ends). Each one ends by
itself (done, timeout or stuck) and logs one result line (below); shell loops can wait for it in
`client.log`. `stop` and `quit` get through while one runs.

**Focus.** The game ignores keys, and the pad, while its window is not the active one. During a
scripted run the window is never told it lost focus (`user32.c`, `SDL_EVENT_WINDOW_FOCUS_LOST`
is dropped while `XIV_INPUT_SCRIPT` is set), so a run works with another app in front. Exception:
the title screen's "A Word to Our Players" dialog takes no keys when the window was never
focused. `login_walk.txt` clicks its Yes button instead.

K is an SDL key name (`Return`, `Escape`, `Left`, `W`, `Space`, `0`) or a Win32 virtual-key code
of two or more digits / hex (`0x0D`, `13`). Every action is logged as `[input] <ms>: ...`.

## Menu facts (1.23b, measured)

- Title: `Return` on Start opens "A Word to Our Players" with **No** highlighted: `Left`, then
  `Return`. Pad `A` on that dialog went back to the title (do not use it there).
- Character select opens on the first character; `Return` goes straight to loading (no confirm).
- In world, left stick and `W/A/S/D` both walk (camera-relative); the server sees 0x00CA position
  updates.

## Running it

```bash
cd ~/Projects/ffxiv-pc
./xivctl launch --no-build --input recomp/tools/input/login_walk.txt   # copies to recomp/build/run/input.txt
./xivctl wait 180                                                        # server-side: zoned in
./xivctl input '+0 stick l 0 1 3000' '+3500 shot after'                  # queue more, any time
```

Screenshots of an `xivctl` run land in `recomp/build/run/`. `recomp/tools/input/login_walk.txt`
is title -> character 1 -> world -> 3 s forward; its waits were measured on an M1 Max and may need
raising on a slower machine (look at its `0N_*.ppm` shots).

## Navigation (`walkto`, `face`, `turn`)

A closed loop in the client: every frame it reads the local player's position and facing from
guest memory and steers the scripted left stick.

- **Where it reads** (lldb on the live client, 2026-10-05; `runtime/portable/autoinput.c`
  `element_of`): `[0x01356e10]` is the actor manager. `+0x1783c` is my actor id. `+0x4e8` is the head
  of the `std::map<actorId, CharaElement*>` at `+0x4e4` (the lookup is `FUN_004d9970` → `FUN_004de190`).
  In a `CharaElement` (vtable `0x00fa7c50`), `+0x88` is the actor id, `+0x9c/+0xa0/+0xa4` is x/y/z
  (what the client reports in 0x00CA, updated every frame), and `+0xac` is the facing in radians
  (0 = +Z, +pi/2 = +X, the same value the server sees).
- **Steering**: the stick is camera-relative and mirrored against the rotation. Holding stick
  angle s (0 = up, +90 deg = right) makes the character face `c - s`, where c is the camera
  heading. The camera is not read. c is estimated as `facing + s` whenever the facing has settled
  with the stick held, so a follow camera is tracked. At the start of a command with no recent
  estimate, the camera is assumed to be behind the player. Full speed beyond 4 units, slower
  near the target, and slow while turning more than ~70 deg.
- **Results**: `[input] <ms>: walkto done|stuck|timeout pos=x,y,z rot=r dist=d target=X,Z (N ms)`.
  If the distance has not improved by 0.5 in 2.5 s, it slides sideways (right, then left, longer
  each time: `walkto blocked at ... detour right|left`). After 6 detours it gives up with `stuck`.
  `face`/`turn`: `face done|timeout pos=... rot=r want=w`. A light stick push turns the character
  and moves it a little (under ~0.5).
- **Routes: `tools/input/route.py` + `follow`.** `walkto` steers in a straight line, so walls,
  counters, railings and other floors stop it. For anything not in plain view, use a route from the
  Map Server's Detour navmesh (`Map Server/Navigation/ZoneNavMesh.cs`, agent radius 0.5).
  `tools/input/route.py X Y Z [--player NAME]` asks the running Map Server through the admin
  API's `/lua` (`TryFindPath` from the player's server position, no server change) and prints
  `path` lines (a point every metre) and a `follow`:

  ```bash
  recomp/tools/input/route.py 58.4 4.2 -1206.5 >> recomp/build/run/input.txt     # guild -> the Roost
  recomp/tools/input/route.py 11.85 8.75 -1263.45 >> recomp/build/run/input.txt  # and back
  recomp/tools/input/route.py 58.4 4.2 -1206.5 --via        # the older corner via/walkto lines
  ```

  Before the points are printed, route.py moves them:
  - away from walls: 16 navmesh rays per point aim for 1.1 m of clearance (`--clearance`), moving
    a point at most 0.6 (`--push`); the pushes are smoothed along the path;
  - to 1.4 m (`--npc-clear`) from every NPC and object of the zone and of the merged seamless zone,
    which the client collides with and the navmesh does not have;
  - to 1.4 m from the spots in `tools/input/obstacles.json`, which the client collides with but the
    navmesh lets through. One known spot: the rocks at the foot of the Roost ramp (x ~111-112,
    z ~-1198). They were found by `follow` stalling there and mapped with a fan of two-point
    `follow` walks (`path clear` / `path X Z Y X2 Z2 Y` / `follow 0.3 3000` / `pos`).
  Y picks the floor: the Roost's square (y 4) lies under the walkway from the guild (y 10 to 12).

- **`follow`: pure pursuit.** Every frame it projects the position onto the path near the
  current segment (preferring the same floor) and aims at a point further along. That point is
  as far as 2.5 m ahead, but only as far as the straight line to it stays within 0.3 m of the
  path, so a hairpin or a door frame is not cut. The stick drops to 0.75 or 0.55 where the path
  bends hard within the next few metres, and to 0.45 while the facing is still more than 50
  degrees off. Steering is integral on the facing error (as `walkto`) plus feed-forward of the
  aim point's swing, so corners do not overshoot. A fixed sideways stick walks a circle, because
  the camera follows the character (measured).
  - Turning round is not a stall. If the walk makes no progress along the path, or toward the
    end, for 0.9 s, the client has hit something the route does not know about. It logs
    `follow stalled at X,Y,Z ... obstacle near X,Z`, moves the path points near that spot 1.1 m
    or more away from it, backs off for 0.4 s and goes on. After 6 stalls it ends as `stuck`.
  - Results: `[input] <ms>: follow done|timeout|stuck pos=x,y,z dist=d along=a/total contacts=N
    stalls=M (T ms)`.
- **Wall contacts** are counted for `walkto`, `via` and `follow`. A contact is a 250 ms window
  at under 60% of running speed (~5/s) with the stick at full tilt for the last 500 ms, the
  facing on target, and no client frame over 80 ms. Seamless zone merges stall the client for a
  moment and are excluded. Each contact logs `contact at X,Y,Z (v/s of vref)`. `navstats` sums
  them.
- Positions to compare against: `meteor_sessions` (server) or `pos`.

Measured 2026-10-05 (Josh Dev, local Meteor, `navstats` per leg):

| route | old corner `via`/`walkto` | `follow` (final) |
|---|---|---|
| guild (11.9, -1263) -> Roost (58.4, -1206.5), 197 m | 2-4 contacts, 0-1 stalls, 45 s | 0 contacts, 0 stalls, 43-48 s (x2) |
| Roost -> guild | **stuck** at the ramp foot: 35 contacts, 66 detours, never arrived | 0 contacts, 0 stalls, 47 s (x2) |
| inside the guild: entrance -> (-6, -1262.5), 18 m between NPCs and benches | 2 contacts, 2 stalls | 0, 0, 4.3 s |
| and back | 0, 0 | 0, 0, 6.8 s |

Every guild <-> Roost walk switched zones on the server: `[seamless] Josh Dev: zone 206 -> 155 at
(~90, -1236)` and `zone 155 -> 206 at (~82.5, -1240)`.

## Interaction (`target`, `interact`, menus)

The client's game traffic is tapped in the socket shim (`ws2.c` `tap` → `autoinput_net`): the
world connection's packets (port `XIV_NETTAP_PORT`, default 54992) are split into game messages
and kept in a ring for `waitfor`/`target`/`interact`. Compressed packets are skipped. Meteor does
not compress them.

- `target 44D8000E` presses Tab and reads the **SetTarget 0x00CD** the client sends (payload
  u32 +0 = target id, +4 `E0000000`). It presses again (up to 20 tries) until the id matches.
  If the player already has that target, it returns at once. That check uses
  `CharaElement+0x12c`, the target the server echoes in 0x00D3. Tab cycles through what is on
  screen, so face or walk toward the NPC first. Use actor ids from `meteor_actors`: NPC names
  are not in the client's `CharaElement`. The result line is `[input] <ms>: target done
  id=0x...` or `target not found`.
- `interact` presses Return and waits up to 3 s for the client's **EventStart 0x012D** (the
  talk event). `[input] <ms>: interact started <eventName> target=0x...` or `interact no event`.
- Dialog text advances with `key Return`. Menus take `choose N` (cursor Down N-1, Return), or
  `key Up/Down` + `key Return`. Use `key Escape` to cancel.
- `waitfor` counts packets from the last action that could cause one (key, pad, stick,
  click, walk, target, interact). `shot`, `log` and `pos` in between do not reset it.
- Event state from the server: **RunEventFunction 0x0130** (the server asks the client to run a
  Lua event function, e.g. `delegateEvent ... talkDefault` or a menu), and **EndEvent 0x0131**
  (the event is over). The client's answer to a menu or choice is **EventUpdate 0x012E** (out).
  So `waitfor in 0x0131 15000` waits for the end of the conversation. `meteor_packets
  opcode=0x0130` shows the same thing server-side.

Measured with Gontrant (guildleve publisher, `0x44D8000E`, the Roost):

```
+0 target 44D8000E          # 5 Tabs: 0x00CD 44D80039, 44D8000B, 44D8003A, 44D8000A, 44D8000E
+0 interact                 # out 0x012D ... "talkDefault"; in 0x0130 talkDefault (the text box)
+1500 key Return            # advance: the menu (Battlecraft Leves ... Nothing.)
+1500 choose 7              # "Nothing."
+0 waitfor out 012E 4000    # EventUpdate with the choice
+0 waitfor in 0131 6000     # EndEvent talkDefault; the client then clears the target (0x00CD C0000000)
```
