# Games v2 — multi-game runtime: Snake + Conway + Connect 4 (design + decisions)

Baseline restore point: tag `aurorakey-stable-v0.14.0` (`aaa2ae7e`) — Snake-complete,
single-game-per-build. Roll back here if v2 goes sideways.

**Scope of v2:** all three games **coexist in one build**, with **runtime switching** between
them from inside the game layer, **colored thumb-cluster controls**, a **shared GO-style splash**,
and a fix for the **color-clear bug**. Grounded in the actual code (`game_runtime.c/.h`,
`board_glove80.c`, `snake.c`) + editor codegen (`ZmkStudio/internal/zmk/conf.go`).

**Process:** local build + flash only for now. **No repo push** until tested on hardware.

## AS BUILT — deviations from the plan (hardware geometry)
- **Connect 4 is a full 6×5, rendered as ONE board spanning the wrist gap** (not two mirrored
  copies). The Glove80 LED grid (`board_glove80.c`) is irregular: no half has 6 full-height
  columns, and the RH thumb cluster blocks mirroring a 6-wide board. The clean solution is to use
  the 3 full-height grid columns of each half — **LH x2,3,4 + RH x9,10,11** (`c4_gridx[]`) — so the
  board straddles the wrist gap (3 cols each side), with board rows on grid y1-5 and the **cursor on
  the top row y0** (as requested). Both players use their own thumb controls on their turn; the
  cursor is shared. WIN_LEN 4, all directions work. Trade-off vs the original "two mirrored copies":
  that idea maxes at 4×4 on this grid, which is too small — so 6×5 spanning won.
- **Clear-bug fix shipped two ways:** runtime clears the 12 thumb keys on activate (off-grid bleed),
  and each game paints a coloured control legend; Connect 4 also `game_paint_clear()`s the full grid
  on enter so non-board cells don't bleed.
- Switch UX = **cycle on key 72** (no picker), with a name-glyph splash.

---

## 0. Grounded architecture facts (verified in code)

- **Dispatch is hard Kconfig `#if`, single-game (v1).** `game_runtime.c:159-188` calls `snake_*`
  directly under `#if IS_ENABLED(CONFIG_AURORAKEY_GAME_SNAKE)`. The header even states "no v-table —
  single-game-active-at-a-time is the v1 contract." → **multi-game requires a registry/vtable (v2).**
- **Render:** `game_paint(x,y,color)` (grid only) → `paint_one(pos,color)` (static, dirty-cached, BLE
  fanout). `game_paint_clear()` clears **grid cells only**. Colors `GAME_COLOR(0xRRGGBB)`.
- **Board geometry (`board_glove80.c`):** 14×6 grid. **In-grid positions:** 0–51, 58–68, 75–79.
  **NOT in grid (thumb cluster):** **52,53,54,55,56,57** (top rows) + **69,70,71,72,73,74** (bottom rows).
  LH = cols 0–5, wrist gap (−1) = cols 6–7, RH = cols 8–13.
- **Input whitelist (`game_runtime.c:293-308`)** forwards only `52,54,55,56,57,73`; `53`=exit; all else
  silently consumed. LH side is starved of game keys.
- **Tick:** `K_WORK_DELAYABLE`, dynamic `tick_ms()`, **20 ms floor**. **RNG:** `sys_rand32_get()`.
- **Activate (`:211`)** seeds `cache_reset_unknown()` then `dispatch_enter()`. **Deactivate (`:235`)**
  `dispatch_exit()` → `zmk_rgb_underglow_layer_clear()` + RH clear.
- **BLE fanout** is rate-limited (~100 writes/s). Dirty-cache keeps steady-state cheap. Static boards
  (Connect 4) are free; Conway's busy generations lean on the slow tick + dirty-cache.

### 🐞 The color-clear bug — ROOT CAUSE FOUND
`game_paint_clear()` (`:146-155`) loops `x∈[0,width) y∈[0,height)` → paints **only the 14×6 grid** OFF.
The **12 thumb-cluster keys (52–57, 69–74) are not in the grid**, so on enter they never get an explicit
OFF overlay entry → the renderer falls through to their **DT-baked layer color** → those keys keep
whatever color the layer had = "some colors not cleared when the game starts."
**Fix:** clear the **full 80-position keymap** on enter, not just the grid (see §2.3). This is also exactly
what we need to then *paint* the thumb controls.

---

## 1. v2 multi-game runtime (registry / vtable)

### 1.1 The module struct
Replace per-game `#if` direct calls with a vtable each game exports:
```c
struct game_module {
    const char *name;          /* "Snake" / "Life" / "Connect 4"   */
    const uint8_t *glyph;      /* tiny menu icon/initial (shared glyph fmt) */
    void (*enter)(void);
    void (*exit)(void);
    void (*tick)(void);
    void (*input)(uint32_t pos);
    int  (*tick_ms)(void);     /* NULL → CONFIG_AURORAKEY_GAME_TICK_MS */
};
```
Each game file exports `const struct game_module <game>_module = { ... }`. The runtime builds the
compiled-in set:
```c
static const struct game_module *const games[] = {
#if IS_ENABLED(CONFIG_AURORAKEY_GAME_SNAKE)
    &snake_module,
#endif
#if IS_ENABLED(CONFIG_AURORAKEY_GAME_CONWAY)
    &conway_module,
#endif
#if IS_ENABLED(CONFIG_AURORAKEY_GAME_CONNECT4)
    &connect4_module,
#endif
};
static int active = 0;   /* index into games[] */
```
`dispatch_*` become `games[active]->*`. Backward-compatible: one game → array of 1, no menu.
RAM: all enabled games' static state coexists (~Snake 500 B + Conway 170 B + Connect4 250 B ≈ 1 KB). Fine.

### 1.2 Switching mechanism — CYCLE (decided)
- **No picker screen.** The game layer launches a game directly (the first compiled-in, or last-played).
- **Reserved CYCLE key = 72** advances to the next compiled-in game: Snake → Life → Connect 4 → Snake …
  On press: current `->exit()` → `game_clear_all()` → brief **name splash** (shared glyph renderer, §2.4,
  e.g. `[LIFE]`) → next `->enter()`. Single game compiled → key 72 is a no-op.
- `53` = EXIT (leave the layer entirely) stays as-is. CYCLE(72) is runtime-handled, intercepted before
  `dispatch_input`, so it works identically inside every game. (positions ⚠️ verify against the matrix.)
- Runtime owns `active` index + a tiny `splash` timer for the name flash; otherwise it's always in
  PLAYING mode (no MENU mode needed). The whitelist (§2.1) just needs to forward `72`.

### 1.3 Per-game thumb-key budget (drives the whitelist + MENU choice)
| Key | 52 | 53 | 54 | 55 | 56 | 57 | 69 | 70 | 71 | 72 | 73 | 74 |
|-----|----|----|----|----|----|----|----|----|----|----|----|----|
| role| start|EXIT|reset|R-left|R-up|R-right|L-left|L-mid|L-right|—|R-down|—|
| Snake | start/pause | exit | reset | ← | ↑ | → | | | | | ↓ | |
| Conway | start/pause | exit | clear | ← | ↑ | → | | step | random | | ↓ | |
| Connect4 | newgame | exit | reset | P2 ← | P2 drop | P2 → | P1 ← | P1 drop | P1 → | | | |
| **MENU** | | | | | | | | | | **72** | | |
→ **72 & 74 are free in all three**; reserve **72 = MENU**.

---

## 2. Shared rendering services (new — in game_runtime or a new game_render.c)

### 2.1 Extend the input whitelist
Forward the **full thumb cluster** (`52,54,55,56,57,69,70,71,72,73,74`) to the runtime, which first
checks MENU(`72`)/menu-mode itself, else `dispatch_input`. Keeps `53`=exit. Snake/Conway ignore extras.

### 2.2 Public position paint — `game_paint_pos()`
Export `void game_paint_pos(uint32_t pos, uint32_t color)` wrapping the existing static `paint_one`.
Lets games light **any** key (thumb controls, menu icons) — not just grid cells.

### 2.3 Full-canvas clear — `game_clear_all()` (fixes the bug)
```c
void game_clear_all(void) {            /* paint EVERY key OFF, not just the grid */
    for (int p = 0; p < ZMK_KEYMAP_LEN; p++) paint_one(p, GAME_COLOR_OFF);
}
```
Call this on enter (and on game-switch) instead of relying on grid-only clear. Establishes a true blank
80-key canvas → thumb keys no longer bleed their DT color. Keep warmup pacing (paint in bursts so the
BLE msgq drains) — extend warmup to cover all 80 positions, not just the grid.

### 2.4 Shared glyph / "GO" splash renderer
Extract Snake's `draw_glyph` (snake.c:164-174) + the GO splash into shared
`game_draw_glyph(x,y,rows,color)` / `game_draw_text(...)`. Reuse for: the GO countdown on game start
(all games), the menu icons, Connect-4 turn/winner banners, Conway phase hints. Snake keeps "GO"; menu
shows each `module->glyph`.

### 2.5 Colored thumb-cluster controls
After clearing, each game paints its **control legend** on the thumb keys via `game_paint_pos`, using a
shared palette so it matches the board feel:
- Directional/arrows = cyan `0x00CCFF`; SELECT/DROP = green `0x00FF66`; EXIT(`53`) = red `0xFF0000`;
  MENU(`72`) = white `0xFFFFFF`; secondary actions (reset/clear/step/random) = amber `0xFFAA00`.
- Connect 4: the **active** player's L/R/drop keys lit in their color (red/yellow), the idle player's dim.
- Repaint the legend whenever it changes (turn flip, phase change); dirty-cache keeps it cheap.

---

## 3. Connect 4

- **Board** `COLS×ROWS` (default **6×5**), `WIN_LEN=4`, coded generally. **Mirrored on both halves**
  (needs `FULL_SCREEN=y`). **Cursor on the top row** (white, active player only). Colors: Red `0xFF0000`,
  Yellow `0xFFCC00`, cursor white, winning line flashes.
- **Controls (confirmed):** RH player `55/56/57` = ←/drop/→ ; LH player `69/70/71` = ←/drop/→ ;
  `54` reset, `53` exit, `72` menu. (positions ⚠️ verify)
- **Join handshake (1 vs 2 detection):** lobby → first DROP = Red → ~5 s wait; 2nd half joins = 2-player,
  else **AI** takes the open side. Double-tap DROP / RESET = start vs AI now. Simultaneous → LH = Red.
- **Turn flow:** active cursor moves cols, DROP = gravity drop → win/draw check → flip turn. AI turn has a
  ~400–700 ms "thinking" beat then animates.
- **Win:** scan 4 axes from placed cell, run ≥ `WIN_LEN`. Full board = draw. Flash winner → back to lobby.
- **AI (difficulty 1–9, all moves legal):** 1–2 random; 3–5 win/block/center heuristic (instant);
  6–9 negamax + α-β depth 4–10 (tens of k nodes, <100 ms on the M4F, center-ordered, simple window eval).
  Ship 1–5 now, 6–9 deep tier, default 5. Solved-perfect = overkill/RAM-heavy; depth-limited is the spot.
- State ~150–250 B.

## 4. Conway's Game of Life (no AI)

- **Whole keyboard** field (14×6 minus walls), `FULL_SCREEN=y`. Live = green (optional age grading),
  dead = off, cursor = white in edit.
- **Phases:** EDIT (D-pad moves cursor, DROP toggles cell) → RUN (auto-tick B3/S23) ⇄ PAUSE (`52`).
  STEP = `70`, CLEAR = `54`, RANDOM-soup = `71`, exit `53`, menu `72`.
- **Rules:** standard `B3/S23`, bounded edges v1 (wall cells permanently dead; wrap = later knob).
  Double-buffer `cur[84]+next[84]` ≈ 168 B. Optional auto-pause on stable/extinction.
- Tick = `TICK_MS` knob (~350 ms). Busy generations lean on dirty-cache; accept occasional RH lag.

---

## 5. Editor / codegen changes (`ZmkStudio`)

- **Multi-select now allowed.** `GamesKconfigOverrides`: drop the single-select assumption; emit
  `CONFIG_AURORAKEY_GAME_{SNAKE,CONWAY,CONNECT4}=y` for each enabled game + their per-game knobs.
- **Force `FULL_SCREEN=y`** whenever Conway or Connect 4 is enabled (they need both halves). Snake then
  runs full-screen too (dirty-cache makes it OK; note the historical teleport caveat).
- `GamesConfig`: replace the `Conway bool` / `Connect4 bool` stubs with `ConwayConfig{Enabled,TickMs,
  InitDensity,Wrap}` and `Connect4Config{Enabled,Cols,Rows,AILevel,TickMs}`.
- Editor UI (`games.templ` + `editor-games.js`): real Conway + Connect 4 sections (replace the disabled
  stubs); keep the byte-for-byte guarantee (emit nothing unless enabled AND ≥1 game selected).
- Tests (`conf_test.go`): multi-game emission, force-full-screen, per-game knob clamps.

---

## 6. Implementation order
1. **Runtime v2 core:** game_module vtable + games[] registry + active/mode + menu + MENU(72)/SELECT wiring.
2. **Shared services:** `game_paint_pos`, `game_clear_all` (clear-bug fix), glyph/GO extraction, thumb legend.
3. **Convert Snake** to the vtable (proves the refactor; no behavior change).
4. **Conway** (no AI).
5. **Connect 4** (board/turns/win → AI tiers → join handshake).
6. **Editor codegen + tests** (multi-select, force-full-screen, structs, UI).
7. **Build west image @ new fork HEAD → flash both halves → test.** No repo push until verified.

## 7. Local testing
- Firmware: build the west image (`docker/zmk/builder-west.Dockerfile`, `ZMK_REV`=local fork HEAD),
  workspace build, flash both halves. Hardware is the only test rig for LED/split/menu behavior.
- Editor: `conf_test.go` unit-tests the codegen without hardware.
- **Nothing is pushed** — all local until you've played it.

## 8. Decisions
**Confirmed:** cursor on top row ✓; controls map ✓; Conway whole-keyboard ✓; all-three coexist + runtime
switch ✓; colored thumb controls ✓; fix the clear bug ✓; reuse GO splash ✓; local-only/no-push ✓.
Switch UX = **CYCLE on key 72** (decided — no picker). MENU/menu-mode dropped. AI default level 5,
ship 1–5 first (6–9 deep tier follow-up).
