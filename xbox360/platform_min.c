// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdarg.h>

#include <xecore/xboxkrnl.h>

#include "../core/platform.h"

int xa_debug;

void xa_sleep_ms(unsigned int ms)
{
	int64_t interval = -(int64_t)ms * 10000;
	KeDelayExecutionThread(0, 0, &interval);
}

u64 xa_now_ms(void)
{
	int64_t t;
	KeQuerySystemTime(&t);
	return (u64)t / 10000;
}

/* The first build exposes counters in g_r360 instead of carrying the larger USB driver's
 * log-ring formatter. Keep the hook's diagnostic calls cheap and ABI-safe. */
void xa_log(const char *fmt, ...)
{
	(void)fmt;
}

