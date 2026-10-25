# SRanipatch

Unofficial SRanipal runtime for the VIVE Facial Tracker & VRCFT on Windows and Steam Frame

C++20 server and Dear ImGui client. The server owns capture, MNN CPU inference and OSC UDP output. The client reads status and parameters through local mapped memory.

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

The server reads OSC defaults from `~/.config/sranipatch/config.ini` on Linux, or `%APPDATA%\sranipatch\config.ini` on Windows. Command-line options override the file. Supported keys are:

```ini
osc_host=127.0.0.1
osc_port=9000
osc_address=/sranipatch/parameters
```

Linux GUI builds include both GLFW Wayland and X11 backends. GLFW normally selects the backend from the session environment. On Steam Frame with Gamescope 3.16.28, use Gamescope's XWayland display because its native `xdg-shell` path currently does not route mouse and close events reliably. `sh scripts/run-linux.sh` selects that path, starts the lip-inference server and client together, and always stops the server when the client exits or the job is interrupted.

Arguments to `scripts/run-linux.sh` are forwarded to the server. For example, `sh scripts/run-linux.sh --precision low` selects MNN's reduced-precision CPU path.

For the VIVE Facial Tracker on Windows:

```powershell
./build/windows-x64/Release/sranipatch-server.exe
./build/windows-x64/Release/sranipatch-client.exe
```

Capture supports the VIVE Facial Tracker on Windows and Linux. The tracker must be accessible to the current user. Camera previews and tracking resume automatically after reconnection.

Inference uses the packed camera bytes and a pseudo-separable median filter. `--threads N` controls CPU threads; `--precision normal|low` selects MNN precision. Normal precision is the default.

See [model converter instructions](../assets/models/converter/README.md) to regenerate the runtime model from original SRanipal parameters.
