# SRanipatch

Tracking runtime for the VIVE Facial Tracker on Windows and Steam Frame. The server captures and sends tracking data; the client displays status and previews.

## Install on Frame

Start with the [headset setup guide](../README.md#setup) for Receiver installation and the Windows Firewall step required by Steam Frame.

```sh
curl -fsSL https://raw.githubusercontent.com/steffandonal/sranipatch/main/runtime/scripts/get.sh | bash
```

Installs to `~/.local/share/sranipatch` and registers with SteamVR. To choose a folder, append `-s -- --dir /path/to/sranipatch` to `bash`. Restarting SteamVR temporarily makes the Frame display go black.

Linux installation requires USB Video (UVC) support. If it is missing, the installer offers to set up [steam-frame-uvc](https://github.com/mavishtheblue/steam-frame-uvc); this can require sudo. Uninstalling SRanipatch leaves UVC support installed.

Package commands (`.cmd` on Windows):

- `start-server`: run tracking manually without registering with SteamVR. Server arguments are forwarded.
- `start-client`: open the desktop interface. Client arguments are forwarded.
- `install`: register this folder and offer a restart if SteamVR is running.
- `uninstall`: unregister this folder; settings and files remain.
- `update`: install the latest stable release, preserving settings and whether this folder is registered.
- `github`: open the project repository (`github.url` on Windows).

Move an installed folder only after uninstalling, then run `install` at its new location. `--no-restart` (`-NoRestart` on Windows) suppresses restart prompts.

## Build

Run build commands from `runtime/`. Final packages are written to `../output/`.

CMake 3.25+, Git LFS and a C++20 compiler are required. Run `git lfs pull` after cloning. Dependencies are fetched during configuration.

Windows x64: Visual Studio 2022 with Desktop development with C++ and a Windows SDK.

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --parallel
ctest --preset windows-x64
```

Executables are in `build/windows-x64/Release`.

Linux (including native AArch64): GCC 12+ or Clang 16+, Ninja, OpenGL, X11 and Wayland development headers.

```sh
sudo apt-get install ninja-build libgl1-mesa-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libwayland-dev libxkbcommon-dev wayland-protocols extra-cmake-modules
cmake --preset linux-release
cmake --build --preset linux-release --parallel
ctest --preset linux-release
```

Set `-DSRANIPATCH_BUILD_CLIENT=OFF` for a headless server. The `linux-aarch64` preset cross-compiles the server; `SRANIPATCH_SYSROOT` can point to a deployment sysroot.

Stage deployment files with `cmake --install build/<preset> --config Release --component Runtime --prefix build/package`. Linux deployments need compatible system libraries; the GUI requires OpenGL 3.3.

Create a package with `cpack --config build/<preset>/CPackConfig.cmake -C Release`. See [release instructions](../docs/releases.md) for publication.

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
