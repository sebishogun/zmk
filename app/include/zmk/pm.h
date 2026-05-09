/*
 * Copyright (c) 2023 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

int zmk_pm_suspend_devices(void);
void zmk_pm_resume_devices(void);

/* Enable wakeup + run RESUME on every device declared under the
 * `zmk,soft-off-wakeup-sources` DT node (kscan, encoders, …). MUST
 * be called BEFORE zmk_pm_suspend_devices() in any path that ends
 * in `sys_poweroff`: zmk_pm_suspend_devices skips wakeup-enabled
 * devices, leaving them resumed, which is what arms GPIO SENSE on
 * nRF52 and equivalents so a key press wakes the chip. Without this
 * the chip enters System OFF with no wake source — only RESET pin
 * (= power cycle) recovers. No-op when the DT node isn't declared
 * for this board. */
int zmk_pm_arm_wakers(void);

int zmk_pm_soft_off(void);