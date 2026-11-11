/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "compat.h"

/* Minimal subset used by the proven Xbox 360 virtual-XInput hook. */
struct gip_gamepad_state {
	u16 buttons;
	bool guide;
	u16 trigger_left, trigger_right;
	s16 stick_left_x, stick_left_y, stick_right_x, stick_right_y;
};

enum gip_gamepad_button {
	GIP_GP_BTN_MENU = BIT(2),
	GIP_GP_BTN_VIEW = BIT(3),
	GIP_GP_BTN_A = BIT(4),
	GIP_GP_BTN_B = BIT(5),
	GIP_GP_BTN_X = BIT(6),
	GIP_GP_BTN_Y = BIT(7),
	GIP_GP_BTN_DPAD_U = BIT(8),
	GIP_GP_BTN_DPAD_D = BIT(9),
	GIP_GP_BTN_DPAD_L = BIT(10),
	GIP_GP_BTN_DPAD_R = BIT(11),
	GIP_GP_BTN_BUMPER_L = BIT(12),
	GIP_GP_BTN_BUMPER_R = BIT(13),
	GIP_GP_BTN_STICK_L = BIT(14),
	GIP_GP_BTN_STICK_R = BIT(15),
};

