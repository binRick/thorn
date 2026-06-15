# Thorn

A cinematic-platformer tribute to Blizzard's **Blackthorne** (1994), written in C
on **raylib 6.0**. Weighty traversal (with a jump); a pump-shotgun that fires forward
**and over the shoulder**; ducking into background **shadow alcoves** to let
bullets pass. Original art, naming, and levels — see **[DESIGN.md](DESIGN.md)**.

Status: **v0.9.1** — playable end-to-end with a title menu (Easy/Normal/Hard,
persisted), an animated hero (idle/walk/fire/hurt/climb), a knife, a selectable
pause menu, dynamic lighting + per-area ambient particles + screen-shake, a
camera that leads where you face, floating damage numbers, "now entering" area
title cards, audio, a drop-in CC0 sprite pipeline, persistent pickups, and a
`make test` regression harness — on the M3 base: all four areas (Sunken Mines → the Mire → the
Ashlands → the Usurper's Keep) linked end-to-end, a Daystone-shard gate, NPC
gifts/hints, area passwords, and the **Maldrak boss**. On top of sprite art, a
sampled shotgun SFX, a **jump** (`Space`), and combat & items depth over the
Sunken Mines area — magazine/reserve ammo with auto-reload, shotgun power/speed upgrades,
placed bombs that blow cracked walls, moving lifts that carry you, three enemy
types (shooter / advancing brute / cover-using sentry), and procedural audio.
Built on the M1 room graph (doors, keys/locked doors, lever→bridge, checkpoints).
Actors and tiles now render as **original, code-generated pixel-art sprites**
(no external assets); `./build/thorn --dumpsprites` exports the sheet to a PNG.
A **visual FX layer** adds dynamic lighting + vignette, parallax backgrounds,
particles (sparks/blood/smoke/embers) and screen-shake — toggle with `F` or
`--nofx`. All original techniques (no copyrighted assets).

**Drop-in sprites:** actors load animated PNG strips from `assets/sprites/`
(falling back to the generated art if absent). The bundled set is original and
**CC0** — regenerate it with `--gen-assets`, or **reskin** by replacing those
PNGs with your own / any CC0 pack (same filenames). See `assets/sprites/CREDITS.txt`.

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
| **Jump** | `Space` |
| Fire forward | `J` / `Ctrl` |
| **Fire backward (over the shoulder)** | `K` |
| Knife (melee) | `V` |
| Place bomb / throw bomb | `E` / `T` |
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

Useful flags: `--continue` (resume from the last saved area) / `--password CODE`
(MINE/MIRE/ASH/KEEP), `--headless` (run with no window — deterministic capture for
CI/SSH), `--selftest` (validate the whole room graph), `--room PATH` / `--spawn ID`
(boot into a specific room/entrance), `--rate N` (snapshot cadence; `0` = every
frame), `--frames N` (run N then quit), `--shot N` (screenshot at frame N),
`--god`, `--no-enemies`, `--demo`, `--dumpsprites`, `--gen-assets` (regenerate the
CC0 sprite PNGs), `--nofx` (disable visual FX), `--skiptitle` (skip the menu),
`--diff N` (0 Easy / 1 Normal / 2 Hard; also set with Left/Right on the title).

## Layout

```
run.sh debug.sh Makefile   entry points + build (Makefile `raylib6` vendors raylib 6.0)
DESIGN.md README.md         design doc + this file
src/main.c                  the game (single translation unit)
levels/<area>/*.lvl         room files for the four areas (external level data)
assets/sprites/*.png        drop-in CC0 sprite strips (reskin by replacing these)
vendor/                     vendored raylib 6.0 (gitignored, rebuilt on demand)
```

See **[DESIGN.md](DESIGN.md)** for mechanics, the level format, the full event
schema, and the roadmap.
