# AuroraKey Studio RGB subsystem (draft)

Adds a custom `zmk.rgb` subsystem to ZMK Studio's RPC protocol so AuroraKey can
edit per-key colors live over USB / BLE without rebuilding firmware.

This directory holds source-code drafts. Nothing here is wired into the build
yet — it documents the patch set required to land the feature.

## What needs to happen

1. **Fork `zmkfirmware/zmk-studio-messages`** to `sebishogun/zmk-studio-messages`,
   branch `aurorakey-rgb`.
   - Copy `proto/rgb.proto` and `proto/rgb.options.in` into `proto/zmk/`.
   - Patch `proto/zmk/studio.proto`:
     ```
     message Request {
         uint32 request_id = 1;
         oneof subsystem {
             zmk.core.Request core = 3;
             zmk.behaviors.Request behaviors = 4;
             zmk.keymap.Request keymap = 5;
             zmk.rgb.Request rgb = 6;       // NEW
         }
     }
     message RequestResponse { ... zmk.rgb.Response rgb = 6; ... }
     message Notification    { ... zmk.rgb.Notification rgb = 6; ... }
     ```
   - Update CMake glob so nanopb generates `rgb.pb.[ch]`.

2. **Pin the fork** in this repo's `app/west.yml`:
   ```yaml
   - name: zmk-studio-messages
     remote: sebishogun
     revision: aurorakey-rgb
     path: modules/msgs/zmk-studio-messages
   ```
   Add `sebishogun` to `remotes:` if not already there.

3. **Wire the subsystem source**:
   - Move `src/rgb_subsystem.c` into `app/src/studio/rgb_subsystem.c`.
   - Add to `app/src/studio/CMakeLists.txt`:
     ```cmake
     target_sources_ifdef(CONFIG_ZMK_RGB_UNDERGLOW app PRIVATE rgb_subsystem.c)
     ```

4. **Implement the staging API in `rgb_underglow_layer.c`** (currently the
   subsystem assumes these helpers exist):
   - `int zmk_rgb_underglow_layer_stage_set(uint32_t layer_id, uint32_t pos, uint32_t color)`
   - `int zmk_rgb_underglow_layer_set_transparent(uint32_t layer_id, bool t)`
   - `int zmk_rgb_underglow_layer_get_color(uint32_t layer_id, uint32_t pos, uint32_t *out)`
   - `bool zmk_rgb_underglow_layer_is_transparent(uint32_t layer_id)`
   - `int zmk_rgb_underglow_layer_save(void)`
   - `void zmk_rgb_underglow_layer_discard(void)`
   - `void zmk_rgb_underglow_layer_clear(uint32_t layer_id)`
   - `void zmk_rgb_underglow_layer_reset_all(void)`

5. **JS client** (`web/static/editor-usb-actions.js` in the AuroraKey app):
   add `EditorUSBActions.setKeyColorLive(layerIndex, keyPos, color)` that calls
   `StudioRPC.request('rgb', { set_key_color: {...} })`.

## Color encoding

`color` is a 32-bit value: `0xEERRGGBB`.

| EE byte    | Meaning                                                 |
|------------|---------------------------------------------------------|
| `0x00`     | Solid color (RGB used as-is)                            |
| `0x01`     | Breathe effect, color = breathe color                   |
| `0x02`     | Pulse effect                                            |
| `0x04`     | Dim                                                     |
| `0xFF`     | Transparent — key falls through to base animation       |

Multiple effect bits can be OR'd if the runtime supports it.

## Why a separate subsystem (not extending `keymap`)

`keymap` is upstream and shared with the official ZMK Studio frontend — extending
it would create a divergence risk. A new `rgb` subsystem keeps AuroraKey
additions isolated. Stock Studio simply ignores subsystem 6.
