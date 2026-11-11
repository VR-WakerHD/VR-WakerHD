#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
# credit required, see LICENSE-VR.md.
# nvngx.dll_wwhd_nr.dll: the Vulkan forwarding proxy for the DLSS 5 NR model (tools/nr_snippet_proxy).
# Cross-compiled from Linux; kernel32 is the only Windows library it needs (zig cc supplies the import
# library). The prebuilt DLL is committed: players do not build tools. Rebuild only when the proxy
# source changes.
set -e
cd "$(dirname "$0")"
zig cc -target x86_64-windows-gnu -shared -O2 -s -o nvngx.dll_wwhd_nr.dll nr_snippet_proxy.c -lkernel32
rm -f nr_snippet_proxy.lib  # (zig leaves an import library next to the DLL; nothing needs it)
x86_64-w64-mingw32-objdump -p nvngx.dll_wwhd_nr.dll 2>/dev/null | grep -A20 "Export Table" || llvm-objdump -p nvngx.dll_wwhd_nr.dll | grep -A20 "Export Table" || true
