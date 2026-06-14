# Thorn — Design Document

> A cinematic-platformer tribute to Blizzard's **Blackthorne** (1994), built in C
> on **raylib 6.0**. Launches with `./run.sh`, exactly like the sibling projects
> `../Chernobyl`, `../Chernobyl2`, and `../uapd`, and emits a recurring **JSON
> instrumentation log** so game state can be reconstructed after any bug.

Status: **v0.2 — M1 complete** (the four-room Sunken Mines area loaded from
external `.lvl` files: room graph + doors, keys/locked doors, lever→bridge,
checkpoints; full JSON instrumentation; `--selftest` + `--headless`). This
document is the north star for growing it into the full game.

---

## 1. What this is (and what it is not)

Thorn is an **original game** that faithfully reproduces the *mechanics and feel*
of Blackthorne — the weighty, no-jump traversal; the pump-shotgun that fires both
forward and **over the shoulder**; ducking into background **shadow alcoves** to
let bullets pass. Game mechanics are not copyrightable, and re-implementing them
is the whole point of a clone.

Everything that *is* protected — Blizzard's sprites, animation frames, level
layouts, music, story text, and proper nouns — is **not** used. Thorn ships
original programmer-art rendering, hand-authored levels, and original naming:

| Blackthorne (reference, not used) | Thorn (original) |
|---|---|
| Kyle "Blackthorne" Vlaros | **Kael Thorne** |
| Planet Tuul / kingdom of Androth | the world of **Vael** |
| Sarlac, the ka'dra usurper | **Maldrak the Usurper** |
| the Androth / Androthi people | the **Aurithi** |
| lightstone / darkstone | the **Daystone** / **Nightstone** |

Where this document says "faithful to the source," it means *the mechanic*, built
from scratch.

### Source material (verified facts about Blackthorne)

For grounding, the reference game — per its Wikipedia entry and the design-defining
genre conventions:

- **Developer / publisher / year:** Blizzard Entertainment; Interplay (SNES/DOS),
  Sega (32X); September 1994.
- **Genre:** cinematic platformer ("Flashback with an attitude"), action- and
  gunplay-focused rather than puzzle-focused.
- **Premise:** Kyle Vlaros, sent to Earth as a child with the lightstone, returns
  after 20 years to depose Sarlac, who seized the throne and twisted his people
  into monsters.
- **Signature mechanics:** a pump-action **shotgun** (upgradeable speed/power)
  that can be **fired backwards without turning around**; both player and enemies
  **press into walls/shadows to dodge bullets**; key/item collection through
  maze-like levels; **no jump** — traversal is running and climbing.
- **Structure:** 17 levels across 4 areas — **Androth mines, Karrellian
  forests/swamps, Wasteland desert, Shadow Keep** (the 32X adds a snowy fifth
  area). Difficulty ramps via stronger enemies and an upgraded shotgun.
- **Craft:** rotoscoped animation (1000+ frames) gave it unusually smooth, weighty
  movement for its era — that *weight* is the feel we are chasing.

Thorn mirrors this shape with original areas: **the Sunken Mines → the Mire →
the Ashlands → the Usurper's Keep**.

---

## 2. Design pillars

1. **Weight over agility.** Movement is deliberate and momentum-driven. There is
   **no jump**. Mastery is reading rooms and timing, not platforming dexterity.
2. **The gun is a conversation.** Shotgun duels are about facing, cover, and the
   over-the-shoulder back-shot. Enemies use the same rules you do.
3. **The third plane.** Every fight is 2D + a shadow plane: step back into an
   alcove and the foreground bullets miss. Cover is a verb.
4. **Rooms, not runs.** Levels are discrete connected rooms full of keys, levers,
   bombs, hazards, and freed Aurithi who help. Exploration over reflex.
5. **Total observability.** With `--debug`, the entire game state is streamed to a
   JSON log at a fixed rate. Any bug you can see, the log can explain.

---

## 3. Gameplay mechanics (detailed)

### 3.1 Locomotion (no jump)
- **States:** `IDLE`, `WALK`, `RUN`, `TURN`, `DUCK`, `COVER`, `CLIMB_UP`,
  `CLIMB_DOWN`, `FALL`, `FIRE_FWD`, `FIRE_BACK`, `HIT`, `DEAD`.
- **Momentum:** horizontal velocity accelerates toward a target speed and decays
  with ground friction; you skid slightly on stop and on turn. A **turn** has a
  short commit window (you pivot in place before moving the other way) — this is
  what makes the gun duels tense.
- **Two gaits:** a careful walk (tap / `Shift`) and a committed run (hold). The
  walk is for edges and aiming; the run covers ground.
- **Gravity & falling:** constant gravity; you fall off edges. A fall beyond a
  safe height deals damage; a fall into a **pit** is fatal. `FALL` interrupts most
  actions.

### 3.2 Climbing (vertical traversal without jumping)
- **Climb up:** facing a ledge one tier above the current floor, press **Up** to
  mantle onto it (a committed animation; you cannot fire mid-climb).
- **Climb down / drop:** at the lip of a drop, press **Down + toward the edge** to
  lower yourself or drop to the tier below.
- **Lifts/elevators:** platforms that move between stops when a lever is thrown or
  when ridden; you stand on them and they carry you. (Authored per level.)

### 3.3 Combat — the shotgun
- **Forward fire (`Fire`):** a short-range hitscan blast in the facing direction
  with a muzzle flash and a brief tracer. Damages the first enemy in the lane
  within range and roughly on the player's vertical band.
- **Back fire (`Fire-Back`) — signature:** a blast in the *opposite* direction
  **without turning**. Same damage, same range; the win condition for an enemy
  who walked up behind you.
- **Pump cadence:** a per-shot cooldown (the "pump"). Upgrades later reduce the
  cooldown (speed) and raise damage (power), faithful to the source.
- **Ammo:** shells are a resource shown in the HUD; picked up in the world. (The
  slice starts you stocked; depletion + reload tuning is M2.)
- **Cover interaction:** a target in `COVER` cannot be hit by foreground fire —
  yours or theirs.

### 3.4 Cover — the shadow plane
- Certain wall tiles are **shadow alcoves**. Standing in front of one and pressing
  **Up** steps you *back* into shadow (`COVER`); press **Down** or move to step
  out. While in cover you cannot move or fire, and foreground bullets pass through
  the lane harmlessly. Enemies use alcoves the same way, so flushing them out — or
  catching them stepping in/out — is core to fights.

### 3.5 Items & inventory
A single-row inventory; `Use` consumes/activates the selected item, `Cycle`
changes selection.
- **Health potion** — restores HP.
- **Bomb** — placed against a **cracked wall** to blow a new passage; also a
  thrown weapon vs. clustered enemies (M2).
- **Keys** — open color-matched doors.
- **Bridge key / device** — activates dormant lifts or bridges (level-specific).
- **Daystone shards** — the macguffin; collecting the set gates the finale.

### 3.6 NPCs — the freed Aurithi
Enslaved Aurithi stand in alcoves. Approach and press **Up** to free/talk; they
hand over an item or a hint (a key, a bomb, a password, "the lever is behind the
falls"). They are non-combatants and can be caught in crossfire — protecting them
is an optional objective.

### 3.7 Hazards & interactables
- **Pits** (fatal), **spike beds** (heavy damage), **force fields** (toggled by
  switches), **crushers** and **falling rock** (timed), **cracked walls** (bomb).
- **Levers / switches / pressure plates** toggle doors, bridges, lifts, fields.
- **Doors** (plain, locked, one-way) connect rooms; press **Up** to use.

### 3.8 Health, death, progression
- **HP** with a HUD bar; **lives** (or a continue) on death. Death plays a short
  animation, then respawns at the last **checkpoint** (a level entrance or a
  triggered marker). A **password** is shown on area completion (faithful retro
  touch) and also auto-saved.
- **Difficulty ramp:** later areas field tougher guards and gate progress behind
  multi-step item puzzles; the shotgun upgrades to keep pace.

### 3.9 Controls (default)

| Action | Keys |
|---|---|
| Move left / right | `←`/`→` or `A`/`D` |
| Up / climb / use / talk / enter shadow | `↑` or `W` |
| Down / climb down / exit shadow / duck | `↓` or `S` |
| Walk (careful) | `Shift` |
| **Fire forward** | `Space` or `J` |
| **Fire backward (over shoulder)** | `K` |
| Use item / cycle item | `E` / `Q` |
| Pause | `P` |
| Debug overlay | `` ` `` or `Tab` |
| Respawn (dev) | `R` |
| God mode / hitboxes (dev) | `G` / `H` |
| Quit | `Esc` |

---

## 4. World & level structure

- **Areas → rooms.** Four areas (Sunken Mines, the Mire, the Ashlands, the
  Usurper's Keep), each a graph of **rooms**. A room is one screen-or-wider tiled
  space with a fixed camera-follow.
- **Tile model.** Levels are tile grids (default **32 px** tiles), loaded from
  external `levels/<area>/<room>.lvl` text files (M1). Each file is a short
  `@`-directive header followed by the ASCII grid. A built-in fallback room boots
  if a file is missing, so the game never hard-fails. (Off-grid sides/top read as
  solid; below the grid is open void — a fatal fall.)

```
.lvl format (M1)
  Header directives (one per line, before the grid):
    @area  <name...>                         display area name
    @room  <name>                            display room name
    @door  <id> <targetRoom> <targetSpawn> [lock <color>]
           digit <id> in the grid is this door; on use it loads
           <targetRoom>.lvl and spawns the player at that room's door
           <targetSpawn>. "exit" as target clears the area. "lock <color>"
           requires a key of that colour to pass.
    @lever <id> bridge                        (documentation; see 'L'/'b' below)
    ;  comment line

  Grid legend:
    #  solid block            S  shadow alcove (cover)
    .  empty / air            ^  spike bed (hazard)
    b  bridge (solid only while a lever is thrown)
    0-9 door (id, see @door)   C  checkpoint
    K  gold key                B  bomb pickup
    H  health pickup           *  Daystone shard
    g  guard (enemy)           n  Aurithi NPC
    L  lever (extends bridges) P  player spawn (first room / fallback)
```

- **Room graph.** Doors link rooms by `(targetRoom, targetSpawn)`; the player
  spawns one tile *inside* the target door, facing in. Inventory (keys, bombs,
  shards, ammo, hp) persists across rooms; enemies/pickups are room-scoped.
- **Checkpoints.** Entering a room and touching a `C` tile both set the respawn
  point; death restores the checkpoint room + position with full health.
- **Validation.** `--selftest` loads every room in an area and checks that each
  door target room exists and contains the referenced spawn door, emitting a JSON
  report — a fast, deterministic guard against broken level wiring.
- **Camera.** Smooth horizontal follow with vertical easing, clamped to room
  bounds. Cinematic per-room snapping is an option flag for set-pieces.

---

## 5. Technical architecture

### 5.1 Stack & conventions (matches the sibling projects)
- **Language:** C11. **Renderer/IO:** raylib **6.0**, vendored as a static archive
  (`vendor/raylib/lib/libraylib.a`) so the build is self-contained — identical to
  Chernobyl2. Brew ships only raylib 5.5, so `make raylib6` clones the upstream
  `6.0` branch and builds it once.
- **Build:** `Makefile` (incremental) + `run.sh` (the entry point). Darwin links
  `OpenGL/Cocoa/IOKit/CoreVideo/CoreAudio`; Linux links `m/pthread/dl/rt/X11`.
- **Single translation unit.** Like the siblings, gameplay lives in `src/main.c`
  with helpers in `src/*.h`. This keeps the build trivial and the whole game
  greppable. Split into modules only when a unit clearly earns its own file.
- **ASCII-only `DrawText`** (raylib's default font is ASCII).
- **raylib 6 note:** v6 renamed skeletal-anim fields (`keyframeCount`); guarded by
  a `RAYLIB_VERSION_MAJOR` macro so the source still builds on 5.5 if needed. Thorn
  is 2D and largely independent of that, but the guard is kept for portability.

### 5.2 Game loop
- Fixed-timestep simulation (default **120 Hz** logic accumulator) decoupled from
  render; all motion is `dt`-scaled so physics is frame-rate independent. Render at
  the display's refresh (`SetTargetFPS` to the monitor / ProMotion).
- **Update order:** input → player state machine → physics/collision → entities
  (enemies, projectiles, lifts) → triggers/pickups → camera → instrumentation →
  render → HUD/overlay.

### 5.3 Data model (slice)
- `Player` (pos, vel, facing, state, state-timer, hp, ammo, flags: onGround,
  inCover, godMode, inventory).
- `Enemy[]` (pos, vel, facing, state, hp, fire-timer, alive).
- `Shot[]` ring buffer (origin, dir, age, owner, hit) for tracers/back-shots.
- `Level` (tile grid, width/height, spawn, lists of pickups/NPCs/levers/doors).
- Collision: **AABB vs. tile grid**, axis-separated (resolve X then Y) for stable
  ledges and walls.

### 5.4 Rendering (programmer art now, original sprites later)
- Two draw planes: a **background** pass (recessed/darker tiles, shadow alcoves)
  and a **foreground** pass (solids, actors, projectiles). Cover visibly tints the
  actor and shifts it into the background plane.
- Actors are drawn from primitives (torso/head/limbs/gun-barrel) so **state and
  facing are legible at a glance** — essential for debugging by eye. Swapping in
  sprite atlases later is a draw-function change, not an architecture change.
- HUD: HP bar, ammo, selected item, area/room name, and current player state.

### 5.5 Audio (M2)
- raylib audio: shotgun fire/pump, footsteps, hit/death, ambient per area, lever
  and door SFX. Out of scope for v0.1.

### 5.6 Directory layout
```
thorn/
  run.sh            # entry point: ensure raylib 6, build, run --debug
  debug.sh          # build + run, tee JSON log to /tmp, jq tips
  Makefile          # incremental build; `raylib6` vendors raylib 6.0
  DESIGN.md         # this document
  README.md         # quickstart + controls
  VERSION
  src/
    main.c          # the game (single TU)
    *.h             # helpers as they emerge (e.g. levelload.h)
  levels/           # external .lvl rooms (production format)
  vendor/
    raylib/         # vendored raylib 6.0 headers + static lib (gitignored)
    raylib-src/     # upstream checkout used to build it (gitignored)
  build/            # compiled binary (gitignored)
  thorn-debug.log   # JSON instrumentation stream (gitignored)
```

---

## 6. Instrumentation — the recurring JSON log

This is a first-class feature, not an afterthought. The goal: **when you report a
bug, the log alone should let state be reconstructed frame-by-frame.**

### 6.1 How it works
- `run.sh` always launches `./build/thorn --debug`.
- `--debug` opens `./thorn-debug.log` for writing (falls back to `stderr` if it
  can't). The format is **newline-delimited JSON (JSONL)** — one event object per
  line, each flushed immediately so a crash still leaves a complete prefix.
- Every record has a monotonic timestamp `t` (seconds since `InitWindow`) and an
  event tag `ev`, followed by event-specific fields:

```json
{"t":0.012,"ev":"boot","raylib":"6.0","build":"0.1.0"}
{"t":0.013,"ev":"window","w":1280,"h":720,"monitor":0}
{"t":0.020,"ev":"level","area":"Sunken Mines","room":"mine_01","w":40,"h":18,"enemies":2,"pickups":3}
```

### 6.2 The recurring state snapshot (the "recurring log")
At a fixed cadence (**~5 Hz**, i.e. every Nth simulation frame) the game emits a
full `state` record — the heartbeat that lets any moment be reconstructed:

```json
{"t":3.400,"ev":"state","frame":408,"fps":120,
 "p":{"x":612.0,"y":430.0,"vx":140.0,"vy":0.0,"face":1,"st":"RUN",
      "hp":80,"ammo":11,"ground":true,"cover":false},
 "cam":[480.0,360.0],
 "enemies":[{"i":0,"x":820,"y":430,"hp":40,"st":"AIM","face":-1},
            {"i":1,"x":1180,"y":238,"hp":0,"st":"DEAD","face":1}],
 "shots":1}
```

Cadence is tuneable (`--rate N` frames between snapshots; `0` = every frame for
microscopic repro). The snapshot is intentionally compact but complete: position,
velocity, facing, state, health, resources, ground/cover flags, camera, and a roster
of every enemy with health and state.

### 6.3 Discrete events (state transitions)
Emitted the instant something happens, so the log captures *causes*, not just the
sampled aftermath:

| `ev` | Fields | Meaning |
|---|---|---|
| `boot` / `window` / `shutdown` | versions, dims, frames | lifecycle |
| `level` | area, room, w, h, enemies, pickups | room loaded |
| `fire` | dir (`fwd`/`back`), x, y, face, hit, target | player shot + result |
| `enemyfire` | i, x, y, dir | a guard shot |
| `hit` | who (`player`/enemy i), dmg, hp | damage applied |
| `death` | who, x, y | actor died |
| `cover` | who, in (bool) | entered/left a shadow alcove |
| `climb` | dir (`up`/`down`), x, y | ledge mantle/drop |
| `fall` | dmg, fatal | fall damage / pit death |
| `pickup` | item, x, y | item collected |
| `use` | item, ok | inventory action |
| `door` / `lever` | id, state | interactable toggled |
| `npc` | id, gave | Aurithi freed / item granted |
| `respawn` | x, y | checkpoint restore |
| `pause` / `mode` | flag, value | pause, god, hitbox, noenemies |

### 6.4 Consuming the log (bug workflow)
```bash
# Tail it live, pretty:
tail -f thorn-debug.log | jq -c .

# Player's HP/state over time (find the frame a bug starts):
jq -c 'select(.ev=="state") | [.t,.p.st,.p.hp,.p.x,.p.y]' thorn-debug.log

# Everything around a death:
jq -c 'select(.ev=="death" or .ev=="hit" or .ev=="fire")' thorn-debug.log

# Did a back-shot connect?
jq -c 'select(.ev=="fire" and .dir=="back")' thorn-debug.log
```
The on-screen **debug overlay** (`` ` ``) shows the same snapshot fields plus the
last few discrete events, so the live view and the log always agree.

### 6.5 Dev affordances (debug build)
`--headless` (run the sim with no window — deterministic capture on SSH/CI),
`--selftest` (validate the room graph and exit), `--room PATH` / `--spawn ID`
(boot straight into a specific room/entrance), `--no-enemies`, `--god`, `--demo`
(scripted attract/auto-play), `--frames N` (run N frames then quit), `--shot N`
(screenshot at frame N), `--rate N` (snapshot cadence), plus in-game toggles
`G` god, `H` hitboxes, `N` no-enemies, `R` respawn. Toggles emit a `mode` event
so the log records the exact harness.

---

## 7. Build & run

```bash
./run.sh            # ensures raylib 6.0 is vendored, builds, runs with --debug
./run.sh --no-enemies   # extra args pass straight through to the game
./debug.sh          # same, but tees the JSON log to /tmp for sharing
make raylib6        # one-time: clone + build vendored raylib 6.0
make / make clean   # build / remove build artifacts
```
First run builds raylib 6.0 from source (a few minutes, once); subsequent runs are
incremental and instant when nothing changed.

---

## 8. Roadmap

- **M0 — Vertical slice. ✅ done.** One room; weighty no-jump movement; ledge
  climb up/down; shadow-cover; shotgun forward **and** back; guards with
  line-of-sight fire; HP/ammo/death/respawn; pickups; full JSON instrumentation
  + overlay.
- **M1 — Room graph + level files. ✅ done.** External `.lvl` loader + built-in
  fallback; the four-room **Sunken Mines** area; doors forming a room graph;
  gold key + locked door; lever→**bridge** over a shaft; checkpoints with
  checkpoint-respawn; `--selftest` graph validation; `--headless` deterministic
  capture. (Moving **lifts** — platforms that carry the player — were deferred to
  M2 with the rest of the dynamic-physics work; M1 ships bridges.)
- **M2 — Combat & items depth.** Ammo economy + reload, thrown/placed bombs &
  cracked walls, **moving lifts/platforms**, shotgun upgrades, enemy variety and
  smarter AI (use cover, flank), audio.
- **M3 — NPCs & narrative.** Freed Aurithi, hints/passwords, the Daystone-shard
  gate, area transitions, the four areas end-to-end.
- **M4 — Polish.** Original sprite art over the primitives, music, menus,
  options, Linux/Web/iOS build parity with the sibling projects.

## 9. Risks & open questions
- **Climb feel** is the hardest thing to make non-fiddly without a jump button;
  expect to iterate on the mantle detection thresholds (the instrumentation
  `climb`/`state` events exist precisely to tune this).
- **Cover vs. climb on `Up`** is context-disambiguated (ledge present → climb;
  on a shadow tile → cover; near a door/NPC/lever → use). If that feels
  ambiguous in play, split cover onto its own key.
- **Hitscan vs. projectile shells:** slice uses range-limited hitscan for crisp
  duels; revisit if upgrades/back-shots want travel-time tells.
- **Level format scope:** the ASCII grid is great for the slice and early areas;
  a richer external editor may be warranted by M3.

---

## Appendix A — JSON event schema (quick reference)
Every line: `{"t":<sec>, "ev":<tag>, ...}`. `state` is the recurring snapshot
(§6.2); all others are discrete (§6.3). `face`: `1`=right, `-1`=left. `st`: a
player/enemy state name (§3.1). Positions are world pixels; `cam` is the camera
top-left.

## Appendix B — Player state machine
`IDLE ⇄ WALK ⇄ RUN`; any ground state → `TURN` (brief) on reversing facing;
`IDLE/WALK → CLIMB_UP|CLIMB_DOWN` (committed) → ground; ground → `COVER` (on
shadow tile, `Up`) → ground (`Down`/move); `*` → `FIRE_FWD|FIRE_BACK` (brief,
returns to prior); leaving ground → `FALL` → ground (with possible `fall`
damage); any → `HIT` (brief) → prior; HP≤0 → `DEAD` → `respawn`.
