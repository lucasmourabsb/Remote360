/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <xecore/xboxkrnl.h>

/* Check the exported version before touching any version-specific kernel data. */
static int r360_kernel_supported(void)
{
	HMODULE kernel = 0;
	void *version = 0;
	if (XexGetModuleHandle("xboxkrnl.exe", &kernel) < 0 || !kernel)
		return 0;
	if (XexGetProcedureAddress(kernel, 344 /* XboxKrnlVersion */, &version) < 0 || !version)
		return 0;
	/* Major, Minor, Build, Qfe, each an unsigned native-endian 16-bit value. */
	return ((const volatile uint16_t *)version)[2] == 17559;
}
