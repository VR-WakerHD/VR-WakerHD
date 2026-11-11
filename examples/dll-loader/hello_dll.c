// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// The smallest DLL for the game's DLL loader (docs/dll-loader.md): it shows a message box when the game loads it.
//   x86_64-w64-mingw32-clang -shared -o hello_dll.dll hello_dll.c -luser32
// then put hello_dll.dll in build/game/dlls/ (or start the game with --load-dll path\to\hello_dll.dll).
#include <windows.h>

static DWORD WINAPI hello(LPVOID unused) {
    (void)unused;
    MessageBoxA(NULL, "hello_dll.dll was loaded into Wind Waker HD", "DLL loader", MB_OK | MB_ICONINFORMATION);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        // DllMain runs under the loader lock: real work goes on a thread of its own
        HANDLE thread = CreateThread(NULL, 0, hello, NULL, 0, NULL);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
