#!/usr/bin/env python3
"""Local SynthXEX 0.0.5 patch for system DLLs. Run inside a copy of synthxex/src.

- every image gets an image-base optional header (0x10201), as XDK-built images do
- DLLs get no TLS optional header (SynthXEX only writes a 0x40-slot stub) and no
  system-flags header (title flags); the XDK-built system DLLs on the console carry
  neither
"""


def sub(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, "w").write(s.replace(old, new))


SIG = ("int setOptHeaders(struct secInfoHeader *secInfoHeader, struct peData *peData, "
       "struct optHeaderEntries *optHeaderEntries, struct optHeaders *optHeaders")
sub("setdata/optheaders.h", SIG + ");", SIG + ", uint32_t moduleFlags);")
sub("setdata/optheaders.c", SIG + ")", SIG + ", uint32_t moduleFlags)")
sub("main.c", "setOptHeaders(secInfoHeader, peData, optHeaderEntries, optHeaders)",
    "setOptHeaders(secInfoHeader, peData, optHeaderEntries, optHeaders, xexHeader->moduleFlags)")

sub("setdata/optheaders.c", "    optHeaderEntries->count = 4;\n",
    "    // Local patch: image base for all, no TLS stub for DLLs (see synthxex_dll_patch.py)\n"
    "    bool isDll = (moduleFlags & XEX_MOD_FLAG_DLL) != 0;\n"
    "\n"
    "    optHeaderEntries->count = isDll ? 3 : 5;\n")

sub("setdata/optheaders.c",
    "    optHeaderEntries->optHeaderEntry[currentHeader].dataOrOffset = secInfoHeader->baseAddr + peData->entryPoint;\n"
    "    currentHeader++;\n",
    "    optHeaderEntries->optHeaderEntry[currentHeader].dataOrOffset = secInfoHeader->baseAddr + peData->entryPoint;\n"
    "    currentHeader++;\n"
    "\n"
    "    // Image base (0x10201)\n"
    "    optHeaderEntries->optHeaderEntry[currentHeader].id = XEX_OPT_ID_IMAGE_BASE;\n"
    "    optHeaderEntries->optHeaderEntry[currentHeader].dataOrOffset = secInfoHeader->baseAddr;\n"
    "    currentHeader++;\n")

sub("setdata/optheaders.c",
    "    // TLS info (0x20104)\n"
    "    optHeaderEntries->optHeaderEntry[currentHeader].id = XEX_OPT_ID_TLS_INFO;\n"
    "    setTLSInfo(&(optHeaders->tlsInfo));\n"
    "    currentHeader++;\n",
    "    // TLS info (0x20104)\n"
    "    if(!isDll)\n"
    "    {\n"
    "        optHeaderEntries->optHeaderEntry[currentHeader].id = XEX_OPT_ID_TLS_INFO;\n"
    "        setTLSInfo(&(optHeaders->tlsInfo));\n"
    "        currentHeader++;\n"
    "    }\n")

sub("setdata/optheaders.c",
    "    // System flags (0x30000)\n"
    "    optHeaderEntries->optHeaderEntry[currentHeader].id = XEX_OPT_ID_SYS_FLAGS;\n",
    "    // System flags (0x30000) - title flags (XAM hooks, gamepad disconnect, ...);\n"
    "    // the XDK-built system DLLs carry none\n"
    "    if(isDll)\n"
    "    { return SUCCESS; }\n"
    "\n"
    "    optHeaderEntries->optHeaderEntry[currentHeader].id = XEX_OPT_ID_SYS_FLAGS;\n")

sub("common/datastorage.h", "#define XEX_OPT_ID_IMPORT_LIBS     0x103FF\n",
    "#define XEX_OPT_ID_IMAGE_BASE      0x10201\n#define XEX_OPT_ID_IMPORT_LIBS     0x103FF\n")
print("patched")
