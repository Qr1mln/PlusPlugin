# PlusPlugin
This is a plugin architecture that adds additional functions to applications through a DLL proxy.

## Build

### Windows (MSVC, the shipped path)
Requires VS 2022 Build Tools with the x64 C++ toolset and MASM (`ml64`).

```bat
python build.py                       :: version.dll -> proxy\version.dll
python build_plugin.py script         :: compile + install to %APPDATA%\TTVoice\plugins\script
python build_plugin.py script console
```

### Linux (mingw-w64 cross-compile)
Requires `nasm` and `g++-mingw-w64-x86-64` (plus `llvm` for export verification):

```bash
sudo apt install -y nasm g++-mingw-w64-x86-64 llvm

./build_linux.sh                  # version.dll + script.dll + console.dll
./build_linux.sh proxy            # proxy only
./build_linux.sh script console   # selected plugins
```

`build_linux.sh` regenerates `proxy/stubs.nasm` from `proxy/proxy_exports.h`
(17 forwarding jumps - Ubuntu has no `llvm-ml`), links with `-static` so the
artifacts depend on system DLLs only, and verifies every output (PE32+ x86-64 +
expected export count). Intermediate files land in `build/`.

Deployment is still done on Windows: copy the `.dll` to `%APPDATA%\TTVoice\`
(proxy) or `%APPDATA%\TTVoice\plugins\<name>\` (plugins). To hot reload a plugin
without restarting the host, set `"enabled": false` in its `plugin.json`, wait a
second, overwrite the dll, then set it back to `true`.
