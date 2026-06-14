# Thorn

A cinematic-platformer tribute to Blizzard's **Blackthorne** (1994), written in C
on **raylib 6.0**. Weighty, no-jump traversal; a pump-shotgun that fires forward
**and over the shoulder**; ducking into background **shadow alcoves** to let
bullets pass. Original art, naming, and levels — see **[DESIGN.md](DESIGN.md)**.

Status: **v0.3 — M2 complete**: combat & items depth on top of the Sunken Mines
area — magazine/reserve ammo with auto-reload, shotgun power/speed upgrades,
placed bombs that blow cracked walls, moving lifts that carry you, three enemy
types (shooter / advancing brute / cover-using sentry), and procedural audio.
Built on the M1 room graph (doors, keys/locked doors, lever→bridge, checkpoints).

## Run

```bash
./run.sh                 # builds vendored raylib 6.0 on first run, then the game
./run.sh --room levels/sunken_mines/shaft.lvl   # boot straight into a room
./run.sh --selftest      # validate the room graph and exit (no window)
./run.sh --demo          # attract/auto-play (deterministic input)
```

Brew only ships raylib 5.5, so the first `./run.sh` builds raylib **6.0** from
source into `vendor/` (a few minutes, once). After that, launches are instant.

## Controls

| Action | Keys |
|---|---|
| Move | `A`/`D` or `←`/`→` |
| Up — climb ledge / enter shadow / use door / free NPC / lever | `W` or `↑` |
| Down — climb down / leave shadow / duck | `S` or `↓` |
| Walk (careful) | `Shift` |
| Fire forward | `Space` / `J` |
| **Fire backward (over the shoulder)** | `K` |
| Place bomb (blows cracked walls) | `E` |
| Pause · Debug overlay | `P` · `` ` `` or `Tab` |
| Respawn · God · Hitboxes (dev) | `R` · `G` · `H` |
| Quit | `Esc` |

## Instrumentation (JSON log)

`run.sh` always launches with `--debug`, which streams newline-delimited JSON to
`./thorn-debug.log`: a recurring **~5 Hz `state` snapshot** (now including the
current `room` and `bridge` state) plus discrete events (`fire`, `hit`, `death`,
`climb`, `cover`, `pickup`, `door`, `transition`, `lever`, `checkpoint`, …). The
whole point: when you hit a bug, the log lets the game state be reconstructed
frame-by-frame.

```bash
tail -f thorn-debug.log | jq -c .                                   # live
jq -c 'select(.ev=="state") | [.t,.p.st,.p.hp,.p.x,.p.y]' thorn-debug.log
jq -c 'select(.ev=="death" or .ev=="hit" or .ev=="fire")' thorn-debug.log
./debug.sh               # build + run, tee the log to /tmp for sharing
```

Useful flags: `--headless` (run with no window — deterministic capture for
CI/SSH), `--selftest` (validate the room graph), `--room PATH` / `--spawn ID`
(boot into a specific room/entrance), `--rate N` (snapshot cadence; `0` = every
frame), `--frames N` (run N then quit), `--shot N` (screenshot at frame N),
`--god`, `--no-enemies`, `--demo`.

## Layout

```
run.sh debug.sh Makefile   entry points + build (Makefile `raylib6` vendors raylib 6.0)
DESIGN.md README.md         design doc + this file
src/main.c                  the game (single translation unit)
levels/sunken_mines/*.lvl   the first area's rooms (external level files)
vendor/                     vendored raylib 6.0 (gitignored, rebuilt on demand)
```

See **[DESIGN.md](DESIGN.md)** for mechanics, the level format, the full event
schema, and the roadmap.
