# Loading extra DLLs

The game can load any DLL you choose at start (a `.so` on Linux): overlays, hooks, tools, your own native code.
Unlike [code mods](setup-code-mods.md), these DLLs need no `wwhd_mod_init_v1` entry point and no mod package.
They are loaded with the Windows loader, so each DLL's `DllMain` runs; the game calls nothing else in them.

## Where the game looks

In this order, each DLL once (the same file named twice is loaded once):

1. **The `dlls` folder next to `wwhd.exe`**: every `*.dll` in it, sorted by file name. Name them `00_first.dll`,
   `10_second.dll` to choose the order. Subfolders are not searched, so a DLL's helper DLLs can sit in a subfolder.
2. **The `dlls` folder in the data folder**, if that is a different folder.
3. **Graphics > Extra DLLs**: the DLLs you chose there, each loaded from where it is (see below).
4. **`WWHD_LOAD_DLLS`**: paths separated by `;`, absolute or relative to the data folder.
   For example `WWHD_LOAD_DLLS=C:\tools\overlay.dll;mods\my_hook.dll`.
5. **`--load-dll <path>`** on the command line, as many times as you like. A relative path means the folder you
   started the game in.

For a source build run from `build/game`, that is `build/game/dlls/`.

## Choosing DLLs in Graphics

Both Graphics tabs have an **Extra DLLs** list:

- **In the game, F1 > Graphics > Extra DLLs.** **Add DLL...** opens a file picker; the DLL is saved to the list
  at once and loads at every start from the place you picked it. **Load now** also loads it into the running game
  (after the renderer and VR have started, so a DLL that must hook their start needs a restart instead). Each entry
  says whether it loaded this start, or why not. **Remove** takes effect at the next start.
- **In the VR launcher, Graphics > Extra DLLs.** **Add DLL...** (several at once is fine), **Remove**, then
  **Save**. A path whose file is gone is marked *not found*.

The list is the `extraDlls` key in `settings.ini`: full paths separated by `|`.

To start without any of them: `--no-dlls` or `WWHD_NO_DLLS=1`.

## When they load

After the log file starts and before the renderer, VR and the game start. A DLL that hooks Vulkan, OpenXR, SDL or
input functions is in place before they are created. The game's own memory (`mem::init`) and mods start after this,
so a DLL that needs the game running should wait for it on a thread of its own.

DLLs stay loaded until the game exits.

## The log

Each DLL gets a `[dlls]` line in the log (`captures/`), for example:

```
[dlls] loaded C:\...\build\game\dlls\hello_dll.dll
[dlls] C:\tools\missing.dll: not found
[dlls] C:\...\dlls\broken.dll: cannot load: The specified module could not be found (error 126)
[dlls] 1 loaded, 2 failed
```

A DLL that cannot be loaded is skipped; the game still starts. Error 126 usually means one of that DLL's own
dependencies is missing: put them next to it (its own folder is searched first). Error 193 means a 32-bit DLL; the
game is 64-bit.

## Writing one

[examples/dll-loader/hello_dll.c](../examples/dll-loader/hello_dll.c) is the smallest one: a message box at load.

```
x86_64-w64-mingw32-clang -shared -o hello_dll.dll examples/dll-loader/hello_dll.c -luser32
```

`DllMain` runs while Windows holds its loader lock: start a thread there for anything more than a few lines, and
don't load other libraries or wait for other threads inside `DllMain` itself.

## Safety

A loaded DLL runs with the game's full rights. Only load DLLs you built or trust.
