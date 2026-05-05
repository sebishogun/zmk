# Copyright (c) 2024 The ZMK Contributors
# SPDX-License-Identifier: MIT

zephyr_linker_sources(DATA_SECTIONS ../../include/linker/zmk-rpc-subsystems.ld)
zephyr_linker_sources(SECTIONS ../../include/linker/zmk-rpc-subsystem-handlers.ld)
zephyr_linker_sources(SECTIONS ../../include/linker/zmk-rpc-subsystem-settings-reset.ld)
zephyr_linker_sources(SECTIONS ../../include/linker/zmk-rpc-event-mappers.ld)
zephyr_linker_sources(SECTIONS ../../include/linker/zmk-rpc-transport.ld)

target_sources(app PRIVATE msg_framing.c)
target_sources(app PRIVATE rpc.c)
target_sources(app PRIVATE core.c)
target_sources(app PRIVATE behavior_subsystem.c)
target_sources(app PRIVATE core_subsystem.c)
target_sources(app PRIVATE keymap_subsystem.c)
target_sources_ifdef(CONFIG_ZMK_RGB_UNDERGLOW app PRIVATE rgb_subsystem.c)
target_sources_ifdef(CONFIG_ZMK_STUDIO_TRANSPORT_UART app PRIVATE uart_rpc_transport.c)
target_sources_ifdef(CONFIG_ZMK_STUDIO_TRANSPORT_BLE app PRIVATE gatt_rpc_transport.c)

# AuroraKey live-USB mutation extensions. Each handler file is gated
# on its own Kconfig symbol so disabling a feature drops the file
# entirely from the build — no dead handlers, no stack growth.
target_sources_ifdef(CONFIG_ZMK_STUDIO_HOLDTAP_MUTATION app PRIVATE holdtap_subsystem.c)
target_sources_ifdef(CONFIG_ZMK_STUDIO_MACRO_SLOT_POOL app PRIVATE macro_slot_subsystem.c)
target_sources_ifdef(CONFIG_ZMK_STUDIO_IDLE_DIMMER app PRIVATE idle_dimmer.c)