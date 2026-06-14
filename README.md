# Thorn

A cinematic-platformer tribute to Blizzard's **Blackthorne** (1994), written in C
on **raylib 6.0**. Weighty, no-jump traversal; a pump-shotgun that fires forward
**and over the shoulder**; ducking into background **shadow alcoves** to let
bullets pass. Original art, naming, and levels — see **[DESIGN.md](DESIGN.md)**.

Status: **v0.1 — playable vertical slice** (one room with the full mechanic set
and complete JSON instrumentation).

## Run

```bash
./run.sh                 # builds vendored raylib 6.0 on first run, then the game
./run.sh --no-enemies    # extra flags pass straight through
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
| Use / cycle item | `E` / `Q` |
| Pause · Debug overlay | `P` · `` ` `` or `Tab` |
| Respawn · God · Hitboxes (dev) | `R` · `G` · `H` |
| Quit | `Esc` |

## Instrumentation (JSON log)

`run.sh` always launches with `--debug`, which streams newline-delimited JSON to
`./thorn-debug.log`: a recurring **~5 Hz `state` snapshot** plus discrete events
(`fire`, `hit`, `death`, `climb`, `cover`, `pickup`, `door`, …). The whole point:
when you hit a bug, the log lets the game state be reconstructed frame-by-frame.

```bash
tail -f thorn-debug.log | jq -c .                                   # live
jq -c 'select(.ev=="state") | [.t,.p.st,.p.hp,.p.x,.p.y]' thorn-debug.log
jq -c 'select(.ev=="death" or .ev=="hit" or .ev=="fire")' thorn-debug.log
./debug.sh               # build + run, tee the log to /tmp for sharing
```

Useful flags: `--rate N` (snapshot cadence in frames; `0` = every frame),
`--frames N` (run N frames then quit — deterministic capture), `--shot N`
(screenshot at frame N), `--god`, `--no-enemies`, `--demo`.

## Layout

```
run.sh debug.sh Makefile   entry points + build (Makefile `raylib6` vendors raylib 6.0)
DESIGN.md README.md         design doc + this file
src/main.c                  the game (single translation unit)
vendor/                     vendored raylib 6.0 (gitignored, rebuilt on demand)
```

See **[DESIGN.md](DESIGN.md)** for mechanics, the level format, the full event
schema, and the roadmap.
