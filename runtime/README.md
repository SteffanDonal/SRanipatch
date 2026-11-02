# SRanipatch

Unofficial SRanipal runtime for the VIVE Facial Tracker & VRCFT on Windows and Steam Frame

C++20 server and Dear ImGui client. The server owns capture, MNN CPU inference and OSC UDP output. The client reads status and parameters through local mapped memory.

## Install on Frame

```sh
curl -fsSL https://raw.githubusercontent.com/steffandonal/sranipatch/main/runtime/scripts/get.sh | bash
```

Downloads the latest stable Linux ARM64 release to `~/.local/share/sranipatch` and registers it with SteamVR. To choose a folder, append `-s -- --dir /path/to/sranipatch` to `bash`. SteamVR restart prompts warn that the Frame display will go black temporarily.

Linux installation requires USB Video (UVC) support, even without a camera attached. The installer offers to load an existing driver or, on SteamOS ARM64, set up [steam-frame-uvc](https://github.com/mavishtheblue/steam-frame-uvc) with your consent. Its source is stored in `${XDG_DATA_HOME:-$HOME/.local/share}/steam-frame-uvc`; upstream setup uses sudo and installs a system service. Declining or failed setup stops installation and preserves any existing registration. Uninstalling SRanipatch leaves UVC support installed.

For Windows x64, extract the ZIP from [Releases](https://github.com/steffandonal/sranipatch/releases/latest) and run `install.cmd`.

Package commands (`.cmd` on Windows):

- `start-server`: run tracking manually without registering with SteamVR. Server arguments are forwarded.
- `start-client`: open the desktop interface. Client arguments are forwarded.
- `install`: register this folder and offer a restart if SteamVR is running.
- `uninstall`: unregister this folder; settings and files remain.
- `update`: install the latest stable release, preserving settings and whether this folder is registered.
- `github`: open the project repository (`github.url` on Windows).

Updates briefly stop and relaunch running applications. SteamVR can stay active; only a changed driver revision prompts a restart. Old package directories are cleaned up on subsequent command launches when unlocked. Move an installed folder only after uninstalling, then run `install` at its new location. `--no-restart` (`-NoRestart` on Windows) suppresses restart prompts.

## Build

Run build commands from `runtime/`. Final packages are written to `../output/`.

CMake 3.25+, Git LFS and a C++20 compiler. Run `git lfs pull` after cloning to download binary assets. Dependencies are pinned and fetched during configuration; `FETCHCONTENT_SOURCE_DIR_MNN`, `FETCHCONTENT_SOURCE_DIR_OPENCV`, `FETCHCONTENT_SOURCE_DIR_GLFW` and `FETCHCONTENT_SOURCE_DIR_IMGUI` accept local source trees.

Windows x64: Visual Studio 2022 with Desktop development with C++ and a Windows SDK.

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --parallel
ctest --preset windows-x64
```

With Visual Studio 2026, add `-G "Visual Studio 18 2026"` to configuration. Executables are in `build/windows-x64/Release`.

Linux (including native AArch64): GCC 12+ or Clang 16+, Ninja, OpenGL, X11 and Wayland development headers.

```sh
sudo apt-get install ninja-build libgl1-mesa-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libwayland-dev libxkbcommon-dev wayland-protocols extra-cmake-modules
cmake --preset linux-release
cmake --build --preset linux-release --parallel
ctest --preset linux-release
```

Set `-DSRANIPATCH_BUILD_CLIENT=OFF` for a headless server. The `linux-aarch64` preset cross-compiles the server using `aarch64-linux-gnu-gcc/g++`; `SRANIPATCH_SYSROOT` can point to a deployment sysroot. Build the GUI natively on ARM64 or supply target graphics dependencies when cross-compiling it. Release builds enable application LTO when supported; no host-specific CPU flags are applied.

Stage deployment files with `cmake --install build/<preset> --config Release --component Runtime --prefix build/package`. This includes dependency notices and the MSVC runtime on Windows. Linux deployments need compatible system libraries; the GUI requires OpenGL 3.3. CPack configurations produce ZIP/TGZ archives.

## Run

```sh
sranipatch-server
sranipatch-client
```

Run `sranipatch-server --help` for model, OSC and IPC options. Both processes must use the same `--ipc` name. Model configuration is supplied at server startup.

The client previews live lip blendshapes using `head.glb` beside its executable. `sranipatch-client --head PATH` selects another model. The Blendshapes tab shows the head preview; the Camera tab shows the tracker image.

OSC output defaults to float arguments at `/sranipatch/parameters`. Set `output_vrc_native=true` to use the [VRChat face tracking protocol](https://vrc-beta-docs.netlify.app/avatars/face-tracking/osc/): a 102-byte blob at `/tracking/face/v1`. Unsupported expressions are zero.

The server reads OSC defaults from `~/.config/sranipatch/config.ini` on Linux, or `%APPDATA%\sranipatch\config.ini` on Windows. Saved changes apply automatically; invalid or missing files leave the last working configuration active. Removing a setting restores its default. Command-line options override the file. Supported keys are:

```ini
osc_host=127.0.0.1
osc_port=9337
output_vrc_native=false
```

On Linux, omitting `osc_host` or setting it to an empty value, `""`, or `null` discovers the Steam Link PC automatically. Discovery checks every two seconds. Without a peer, the server closes the camera and pauses inference; the model loads on first use and stays loaded for reconnection. An explicit IP overrides discovery. On Windows, these default values use `127.0.0.1`.

The client opens a desktop window by default. `--overlay` presents the same interface as a SteamVR dashboard overlay on Windows and Linux; SteamVR must be running. Dismissing the dashboard hides the interface. The overlay close control and SteamVR shutdown exit the client.

SteamVR integration is included in server builds; `-DSRANIPATCH_BUILD_DRIVER=OFF` disables it. Run the staged package's `install` command (`install.cmd` on Windows) to register it. Its driver settings provide tracking startup, dashboard visibility, status, retry, and restart controls. The dashboard defaults to disabled and remembers its setting. SteamVR stops servers it starts; manually started servers remain under manual control.

Linux GUI builds include GLFW Wayland and X11 backends; overlays use X11. `sh scripts/run-linux.sh` starts a manually owned server on the performance CPU cluster. SteamVR's dashboard toggle controls its overlay when connected.

Arguments to `scripts/run-linux.sh` are forwarded to the server. For example, `sh scripts/run-linux.sh --precision low` selects MNN's reduced-precision CPU path.

For the VIVE Facial Tracker on Windows:

```powershell
./build/windows-x64/Release/sranipatch-server.exe
./build/windows-x64/Release/sranipatch-client.exe
```

Capture supports the VIVE Facial Tracker on Windows and Linux. The tracker must be accessible to the current user. Camera previews and tracking resume automatically after reconnection.

Inference uses the packed camera bytes and a pseudo-separable median filter. `--threads N` controls CPU threads; `--precision normal|low` selects MNN precision. Normal precision is the default.

See [model converter instructions](../assets/models/converter/README.md) to regenerate the runtime model from original SRanipal parameters.
