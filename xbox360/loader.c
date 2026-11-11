// SPDX-License-Identifier: GPL-3.0-or-later
/* Manual, nonpersistent loader for kernel 17559. */
#include <xecore/xboxkrnl.h>
#include "kernel_17559.h"

#define ORD_XexLoadImage 409
#define XEX_LOADIMG_FLAG_DLL 8
#define PROC_SYSTEM 2
#define ORD_XamLoaderLaunchTitleEx 421

static const char *const roots[] = {
	"\\Device\\Harddisk0\\Partition1", "\\Device\\Mass0",
	"\\Device\\Mass1", "\\Device\\Mass2",
};
static const char *const aurora_dirs[] = { "\\Apps\\Aurora", "\\Aurora", "" };

typedef NTSTATUS (*XexLoadImage_t)(const char *, uint32_t, uint32_t, HMODULE *);
typedef NTSTATUS (*XamLoaderLaunchTitleEx_t)(const char *, const char *, const char *, uint32_t);

__attribute__((used)) volatile uint32_t g_stage[12] = {0x52334c44 /* R3LD */};

/* No CRT, heap, stdio, constructors or static XAM imports are needed here. */
static int make_path(char *out, unsigned capacity, const char *root, const char *suffix)
{
	unsigned n = 0;
	while (*root && n + 1 < capacity)
		out[n++] = *root++;
	while (*suffix && n + 1 < capacity)
		out[n++] = *suffix++;
	out[n] = 0;
	return !*root && !*suffix;
}

static void mark_stage(uint32_t stage)
{
	g_stage[1] = stage;
	DbgPrint("Remote360 loader stage %u\n", stage);
}

static void sleep_s(int seconds)
{
	int64_t interval = -(int64_t)seconds * 10000000;
	KeDelayExecutionThread(1, 0, &interval);
}

static volatile uint8_t *thread_proc_type(void)
{
	uint32_t pcr;
	__asm__ volatile("mr %0, 13" : "=r"(pcr));
	return (volatile uint8_t *)(*(volatile uint32_t *)(pcr + 0x100) + 0x73);
}

static int file_exists(const char *path)
{
	uint16_t length = 0;
	while (path[length])
		length++;
	ANSI_STRING name = {length, (uint16_t)(length + 1), (char *)path};
	OBJECT_ATTRIBUTES attrs = {0, &name, OBJ_CASE_INSENSITIVE};
	FILE_NETWORK_OPEN_INFORMATION info;
	return NtQueryFullAttributesFile(&attrs, &info) >= 0 && !(info.FileAttributes & 0x10);
}

static void return_to_dashboard(void)
{
	HMODULE xam = 0;
	void *launch = 0;
	char path[256], mount[256];
	XexGetModuleHandle("xam.xex", &xam);
	XexGetProcedureAddress(xam, ORD_XamLoaderLaunchTitleEx, &launch);
	g_stage[4] = (uint32_t)launch;
	if (!launch)
		return;
	mark_stage(30);
	for (unsigned r = 0; r < sizeof(roots) / sizeof(roots[0]); r++) {
		for (unsigned d = 0; d < sizeof(aurora_dirs) / sizeof(aurora_dirs[0]); d++) {
			if (!make_path(mount, sizeof(mount), roots[r], aurora_dirs[d]) ||
			    !make_path(path, sizeof(path), mount, "\\Aurora.xex"))
				continue;
			if (file_exists(path)) {
				g_stage[5] = (uint32_t)((XamLoaderLaunchTitleEx_t)launch)(path, mount, 0, 0);
				if ((int32_t)g_stage[5] >= 0)
					return;
			}
		}
	}
	/* An unknown Aurora location must not leave the loader on a black screen. */
	g_stage[5] = (uint32_t)((XamLoaderLaunchTitleEx_t)launch)(
		"\\Device\\Flash\\dash.xex", "\\Device\\Flash", 0, 0);
}

__attribute__((noreturn)) void loader_main(void)
{
	HMODULE krnl = 0;
	void *p_load = 0;

	mark_stage(3);
	if (!r360_kernel_supported()) {
		g_stage[3] = 0xC00000BB; /* STATUS_NOT_SUPPORTED */
		goto dashboard;
	}
#ifdef R360_LOADER_PROBE
	/* Isolate startup and dashboard hand-off before loading any plugin code. */
	g_stage[10] = 1;
	mark_stage(4);
	sleep_s(3);
	goto dashboard;
#endif
	XexGetModuleHandle("xboxkrnl.exe", &krnl);
	XexGetProcedureAddress(krnl, ORD_XexLoadImage, &p_load);
	g_stage[6] = (uint32_t)p_load;
	if (p_load) {
		HMODULE module = 0;
		char path[256];
		/* Never install a second set of hooks over an already-loaded copy. */
		if (XexGetModuleHandle("remote360.xex", &module) >= 0 && module) {
			g_stage[9] = 1;
			goto dashboard;
		}
		g_stage[3] = 0xC0000034; /* STATUS_OBJECT_NAME_NOT_FOUND */
		int found = 0;
		for (unsigned i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
			if (!make_path(path, sizeof(path), roots[i],
				       "\\Apps\\Remote360\\remote360.xex"))
				continue;
			if (file_exists(path)) {
				g_stage[8] = i + 1;
				found = 1;
				break;
			}
		}
		if (!found)
			goto dashboard;
		volatile uint8_t *type = thread_proc_type();
		uint8_t old = *type;
		g_stage[7] = old;
		mark_stage(10);
		*type = PROC_SYSTEM;
		g_stage[3] = (uint32_t)((XexLoadImage_t)p_load)(path, XEX_LOADIMG_FLAG_DLL, 0,
								      &module);
		*type = old;
		mark_stage(20);
	}

dashboard:
	sleep_s(1);
	return_to_dashboard();
	for (;;) {
		sleep_s(1);
		g_stage[2]++;
	}
}

