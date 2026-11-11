/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "compat.h"

void xa_sleep_ms(unsigned int ms);
u64 xa_now_ms(void);
void xa_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int xa_debug;

