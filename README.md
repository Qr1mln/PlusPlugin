# PlusPlugin
This is a plugin architecture that adds additional functions to applications through a DLL proxy.

## Build

### Windows (MSVC, the shipped path)
Requires VS 2022 Build Tools with the x64 C++ toolset and MASM (`ml64`).

```bat
python framework_build.py             :: framework.dll -> framework\framework.dll
python proxy_build.py                  :: version.dll -> proxy\version.dll
python build_plugin.py console  :: compile + install to %APPDATA%\TTVoice\plugins\console
```

### Linux (mingw-w64 cross-compile)
Requires `nasm` and `g++-mingw-w64-x86-64` (plus `llvm` for export verification):

```bash
sudo apt install -y nasm g++-mingw-w64-x86-64 llvm

./build_linux.sh                  # version.dll + console.dll
./build_linux.sh proxy            # proxy only
./build_linux.sh console          # selected plugins
```

`build_linux.sh` regenerates `proxy/stubs.nasm` from `proxy/proxy_exports.h`
(17 forwarding jumps - Ubuntu has no `llvm-ml`), links with `-static` so the
artifacts depend on system DLLs only, and verifies every output (PE32+ x86-64 +
expected export count). Intermediate files land in `build/`.

Deployment is done on Windows with `python deploy.py` (`version.dll` - needs a
host restart) and `python deploy_plugin.py` (`framework.dll` + `editor2/dist`).
`framework.dll` hot reloads: `proxy/module_loader` watches the file, so the new
build is picked up within ~1s without restarting the host (the web editor's
WebSocket reconnects on its own). Any extra module listed under `[modules]` in
`version.ini` is loaded and hot reloaded the same way - the only contract is
`NotifyPlatform` + JSON.
