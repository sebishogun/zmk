/*
 * AuroraKey games — runtime orchestrator.
 *
 * Subscribes to layer-state-changed and position-state-changed.
 * On entering CONFIG_AURORAKEY_GAME_LAYER, dispatches enter / tick /
 * input to the active game module. On leaving, dispatches exit and
 * releases the per-key Studio overlay so normal layer rendering
 * resumes.
 *
 * Architecture is single-game-active-at-a-time: only one module's
 * Kconfig should be set per build. If multiple are set the runtime
 * picks the first compiled-in one (Snake → Conway → Connect4) — the
 * editor's codegen normally enforces single-select but this falls
 * back gracefully if a hand-edited config slips through.
 *
 * Re-renders every tick rather than tracking dirty regions: the
 * board has 80 keys max, paint cost is negligible (~80 atomic stores
 * + a single split-bt fanout the renderer batches), and re-render
 * recovers from any concurrent USB Studio writes that may have
 * scribbled on the game layer mid-frame.
 *
 * Copyright (c) 2026 The ZMK Contributors / AuroraKey
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(aurorakey_games, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/rgb_underglow_layer.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) &&                   \
    IS_ENABLED(CONFIG_EXPERIMENTAL_RGB_LAYER) && IS_ENABLED(CONFIG_AURORAKEY_GAME_FULL_SCREEN)
#include <zmk/split/central.h>
#define GAME_FANOUT_TO_PERIPHERAL 1
#else
#define GAME_FANOUT_TO_PERIPHERAL 0
#endif

#include "game_board.h"
#include "game_runtime.h"

#if IS_ENABLED(CONFIG_AURORAKEY_GAMES)

#define GAME_LAYER CONFIG_AURORAKEY_GAME_LAYER
#define EXIT_POS CONFIG_AURORAKEY_GAME_EXIT_POSITION

/* Glove80 thumb-cluster matrix positions used as game inputs.
 * RH cluster D-pad layout:
 *   top row    [ 55  56  57 ]   left up right
 *   bottom row [ 72  73  74 ]   . down .
 * Bottom-middle alone for down so the thumb can rest naturally. */
#define KEY_LH_START 52
#define KEY_LH_RESET 54
#define KEY_RH_LEFT 55
#define KEY_RH_UP 56
#define KEY_RH_RIGHT 57
#define KEY_RH_DOWN 73

static bool game_active = false;
static uint32_t game_layer_id = 0;

/* Per-key cache of the colour we last pushed over split-bt to the
 * peripheral. Re-painting the same colour is a no-op for the user
 * and adds wasteful traffic on the BLE link — at 80 cells × 5 Hz
 * tick = 400 writes/sec, the central's split-bt msgq fills up
 * (typical capacity ~16), the EAGAIN handler evicts the oldest
 * write, and most of the frame's pixels get dropped en route to
 * RH. Result: RH renders a corrupt half-frame or just falls back
 * to the layer's normal colours.
 *
 * With this dirty cache we only fanout cells whose colour actually
 * changed since the last paint. A typical Snake frame mutates ~5
 * cells (old tail off, new head on, maybe food), so fanout drops
 * from 80/tick to ~5/tick and the BLE link comfortably keeps up. */
static uint32_t last_pushed[ZMK_KEYMAP_LEN];
/* Sentinel value: no real packed colour can equal this (high bit set
 * + low bit clear in a slot reserved by GAME_COLOR macro). Seeding the
 * cache with sentinel forces the very first paint per cell to actually
 * call stage_set + fanout — we MUST create an explicit overlay entry
 * (even for OFF / black cells) because otherwise the renderer falls
 * through to the layer's DT-baked colour and the user sees stock
 * underglow bleeding through under the game. */
#define CACHE_UNKNOWN 0xFFFFFFFEu
static void cache_reset_unknown(void) {
    for (int i = 0; i < ZMK_KEYMAP_LEN; i++) {
        last_pushed[i] = CACHE_UNKNOWN;
    }
}

/* ─── Public paint API ─────────────────────────────────────────────── */

/* paint_one writes a single pixel to BOTH the central's local
 * pending overlay AND the peripheral via the split-bt UPDATE_RGB_COLOR
 * opcode 0x01 fanout. Without the fanout step the RH-strip pixels
 * stay dark and the game only renders on LH — exactly what we hit
 * before this fix. Mirrors the Studio set_key_color RPC handler at
 * rgb_subsystem.c:37-44, which has stage_set + split_central_update_rgb_color
 * in the same function for the same reason.
 *
 * Dirty-cache: skip both the local stage_set AND the split-bt fanout
 * when the colour matches what we last pushed for this cell. Local
 * stage_set is cheap (atomic store + mask bit) but skipping it keeps
 * the renderer's per-tick work proportional to the diff, not the
 * board size. The fanout skip is the critical part — it stops the
 * msgq flood that drops RH frames. */
static void paint_one(int pos, uint32_t color) {
    if (pos < 0 || pos >= ZMK_KEYMAP_LEN) {
        return;
    }
    if (last_pushed[pos] == color) {
        return;
    }
    zmk_rgb_underglow_layer_stage_set(game_layer_id, (uint32_t)pos, color);
#if GAME_FANOUT_TO_PERIPHERAL
    int err = zmk_split_central_update_rgb_color(game_layer_id, (uint32_t)pos, color);
    if (err < 0) {
        /* msgq full (-EAGAIN) or other transient BLE error. Leave the
         * cache stale so next tick's paint pass retries this cell.
         * Central-side stage_set already succeeded so LH renders this
         * frame correctly; RH catches up within 1–2 ticks as the
         * peripheral msgq drains. */
        return;
    }
#endif
    last_pushed[pos] = color;
}

void game_paint(int x, int y, uint32_t color) {
    if (!game_active) {
        return;
    }
    if (x < 0 || x >= game_board.width || y < 0 || y >= game_board.height) {
        return;
    }
    paint_one(game_board.xy_to_pos(x, y), color);
}

void game_paint_clear(void) {
    if (!game_active) {
        return;
    }
    for (int y = 0; y < game_board.height; y++) {
        for (int x = 0; x < game_board.width; x++) {
            paint_one(game_board.xy_to_pos(x, y), GAME_COLOR_OFF);
        }
    }
}

/* ─── Module dispatch helpers ──────────────────────────────────────── */

static void dispatch_enter(void) {
#if IS_ENABLED(CONFIG_AURORAKEY_GAME_SNAKE)
    snake_enter();
#endif
}

static void dispatch_exit(void) {
#if IS_ENABLED(CONFIG_AURORAKEY_GAME_SNAKE)
    snake_exit();
#endif
}

static void dispatch_tick(void) {
#if IS_ENABLED(CONFIG_AURORAKEY_GAME_SNAKE)
    snake_tick();
#endif
}

static void dispatch_input(uint32_t position) {
#if IS_ENABLED(CONFIG_AURORAKEY_GAME_SNAKE)
    snake_input(position);
#endif
}

static int dispatch_tick_ms(void) {
#if IS_ENABLED(CONFIG_AURORAKEY_GAME_SNAKE)
    return snake_tick_ms();
#else
    return CONFIG_AURORAKEY_GAME_TICK_MS;
#endif
}

/* ─── Tick timer ───────────────────────────────────────────────────── */

static void tick_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(tick_work, tick_handler);

static void tick_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (!game_active) {
        return;
    }
    dispatch_tick();
    int next = dispatch_tick_ms();
    if (next < 20) {
        next = 20; /* hard floor — don't peg the work queue */
    }
    k_work_reschedule(&tick_work, K_MSEC(next));
}

/* ─── Activation / deactivation ───────────────────────────────────── */

static void activate(void) {
    if (game_active) {
        return;
    }
    game_layer_id = (uint32_t)zmk_keymap_layer_index_to_id((zmk_keymap_layer_index_t)GAME_LAYER);
    game_active = true;
    /* Don't call layer_clear here. layer_clear wipes the overlay
     * masks (no per-key entry for the cell) which causes the renderer
     * to fall back to the layer's DT-baked colour for unlit cells —
     * stock underglow bleeds through under the game. Instead we leave
     * the cache as CACHE_UNKNOWN so the first paint_one(OFF) on every
     * cell creates an explicit black overlay entry. The 80-cell warm-
     * up burst over the GO splash drains naturally through the BLE
     * msgq (with retry-on-EAGAIN in paint_one); steady-state cost
     * once the snake is moving is ~5 cells/tick. */
    cache_reset_unknown();
    LOG_INF("game enter (layer index=%d, layer_id=%u)", (int)GAME_LAYER, game_layer_id);
    dispatch_enter();
    /* First tick fires immediately so the user sees the initial frame
     * the moment they enter the layer; subsequent ticks honour the
     * module's requested interval. */
    k_work_reschedule(&tick_work, K_NO_WAIT);
}

static void deactivate(void) {
    if (!game_active) {
        return;
    }
    LOG_INF("game exit");
    dispatch_exit();
    game_active = false;
    k_work_cancel_delayable(&tick_work);
    /* Release every pixel we painted — the game layer falls back to
     * whatever the editor configured (or the layer default). One
     * clear-layer fanout to RH so it stops rendering our pixels too.
     * On exit we WANT the DT fallback: the user is leaving the game
     * and the layer's normal appearance should return. */
    zmk_rgb_underglow_layer_clear(game_layer_id);
#if GAME_FANOUT_TO_PERIPHERAL
    zmk_split_central_rgb_clear_layer(game_layer_id);
#endif
    cache_reset_unknown();
}

/* ─── Event listeners ─────────────────────────────────────────────── */

static int on_layer_state(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    int top = zmk_keymap_highest_layer_active();
    if (top == GAME_LAYER) {
        activate();
    } else if (game_active) {
        deactivate();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(aurorakey_game_layer, on_layer_state);
ZMK_SUBSCRIPTION(aurorakey_game_layer, zmk_layer_state_changed);

static int on_position_state(const zmk_event_t *eh) {
    if (!game_active) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    /* Releases pass through silently — gameplay is press-driven; we
     * still want releases to clear any pressed-key state the kernel
     * tracks so we don't leave anything stuck after exiting the
     * layer. */
    if (!ev->state) {
        return ZMK_EV_EVENT_HANDLED;
    }
    /* Exit key: switch the keymap back to layer 0 (base). The layer-
     * state listener picks up the change and runs game_exit cleanup. */
    if ((int)ev->position == EXIT_POS) {
        zmk_keymap_layer_to(zmk_keymap_layer_index_to_id(0), false);
        return ZMK_EV_EVENT_HANDLED;
    }
    /* Game inputs: thumb-cluster keys handled by the active module. */
    switch (ev->position) {
    case KEY_LH_START:
    case KEY_LH_RESET:
    case KEY_RH_UP:
    case KEY_RH_DOWN:
    case KEY_RH_LEFT:
    case KEY_RH_RIGHT:
        dispatch_input(ev->position);
        return ZMK_EV_EVENT_HANDLED;
    default:
        /* Every other key on the game layer is silently consumed so
         * the user can't accidentally type letters mid-snake. The
         * dedicated layer + intercept-all is the production UX
         * contract: a game layer is for the game only. */
        return ZMK_EV_EVENT_HANDLED;
    }
}

ZMK_LISTENER(aurorakey_game_input, on_position_state);
ZMK_SUBSCRIPTION(aurorakey_game_input, zmk_position_state_changed);

/* Force-deactivate the game when the keyboard transitions to IDLE /
 * SLEEP. Two reasons:
 *  - The 50 ms tick work loop would otherwise keep firing right up
 *    until sys_poweroff, fighting the activity manager for the system
 *    work queue and adding scheduling pressure during sleep transition.
 *  - On wake (post-poweroff fresh boot) the keymap defaults to the
 *    base layer, so we wouldn't auto-activate anyway — but if the
 *    keymap retained the game layer somehow, deactivate-on-sleep
 *    means we cleanly cancel pending work and release the overlay
 *    before powering off, no half-state surviving the boundary. */
static int on_activity_state(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (game_active &&
        (ev->state == ZMK_ACTIVITY_IDLE || ev->state == ZMK_ACTIVITY_SLEEP)) {
        LOG_INF("game force-deactivate on activity=%d", (int)ev->state);
        deactivate();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(aurorakey_game_activity, on_activity_state);
ZMK_SUBSCRIPTION(aurorakey_game_activity, zmk_activity_state_changed);

#endif /* CONFIG_AURORAKEY_GAMES */
