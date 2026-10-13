# SRanipal on Frame

C++20 server and Dear ImGui client. The server owns capture, MNN CPU inference and OSC UDP output. The client reads status and parameters through local mapped memory and can pause inference or toggle OSC.

## Build

Run build commands from `runtime/`. Final packages are written to `../output/`.

CMake 3.25+, Git and a C++20 compiler. Dependencies are pinned and fetched during configuration; `FETCHCONTENT_SOURCE_DIR_MNN`, `FETCHCONTENT_SOURCE_DIR_GLFW` and `FETCHCONTENT_SOURCE_DIR_IMGUI` accept local source trees.

Windows x64: Visual Studio 2022 with Desktop development with C++ and a Windows SDK.

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --parallel
ctest --preset windows-x64
```

With Visual Studio 2026, add `-G "Visual Studio 18 2026"` to configuration. Executables are in `build/windows-x64/Release`.

Linux (including native AArch64): GCC 12+ or Clang 16+, Ninja, OpenGL and X11 development headers.

```sh
sudo apt-get install ninja-build libgl1-mesa-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
cmake --preset linux-release
cmake --build --preset linux-release --parallel
ctest --preset linux-release
```

Set `-DSOF_BUILD_CLIENT=OFF` for a headless server. The `linux-aarch64` preset cross-compiles the server using `aarch64-linux-gnu-gcc/g++`; `SOF_SYSROOT` can point to a deployment sysroot. Build the GUI natively on ARM64 or supply target graphics dependencies when cross-compiling it. Release builds enable application LTO when supported; no host-specific CPU flags are applied.

Stage deployment files with `cmake --install build/<preset> --config Release --component Runtime --prefix build/package`. This includes dependency notices and the MSVC runtime on Windows. Linux deployments need compatible system libraries; the GUI requires OpenGL 3.3. CPack configurations produce ZIP/TGZ archives.

## Run

```sh
sof-server --model /path/to/model.mnn --device /dev/video0
sof-client
```

On Windows, use `--device 0`. Run `sof-server --help` for capture, normalization, tensor selection, OSC and IPC options. Both processes must use the same `--ipc` name. Capture/model configuration is supplied at server startup; GUI controls apply live.

The files in `../assets/models/` are Git LFS JSON/NPZ weights, not converted MNN graphs. Their network architecture, preprocessing and eye/lip output mapping still need to be established. The generic [MNN adapter](src/inference/mnn.cpp) accepts one fixed float32 RGB image input, bilinearly resizes it, applies `(pixel - mean) * scale`, and exposes one selected float output as unnamed parameters. It is a scaffold contract, not a claim about those weights. No demo or synthetic inference is included.

## Platform boundaries

- Linux: [V4L2 MMAP streaming](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/mmap.html) through the kernel's `uvcvideo` driver. Initial format support is single-plane YUYV with limited-range BT.601 conversion. MJPEG/NV12 and camera colorimetry require format adapters. The user needs access to the video device.
- Windows: [Media Foundation source reader](https://learn.microsoft.com/en-us/windows/win32/medfound/processing-media-data-with-the-source-reader), asynchronous capture and RGB32 conversion. Requested native camera dimensions and frame rate must be available.
- IPC: fixed-size, versioned snapshots under a Windows named mutex or Linux `flock`; no process-shared C++ atomics. Server publication and client reads are nonblocking. One server owns each channel. Commands carry revisions with server acknowledgements; heartbeat expiry detects stopped servers. Linux regions are private per user and retained across restarts; Windows names are local to the session. This transports status/controls, not video.
- OSC: [OSC 1.0](https://opensoundcontrol.stanford.edu/spec-1_0.html) float arguments in a single UDP message per inference, in parameter order. Default address is `/sranipal/parameters`; destinations are numeric IPv4/IPv6 addresses. Output is bounded to 128 parameters and UDP sends are nonblocking.

Capture and inference use separate threads with a latest-frame mailbox. Buffers and host tensors are reused; inference skips stale frames rather than accumulating a queue. Frame age measures receipt-to-result latency, not exposure-to-result latency. The client polls status at roughly 30 Hz independently of camera/inference rates.

Core tests cover wire encoding, UDP delivery, image row stride, frame replacement and IPC commands across real processes. CI builds Windows x64 and native Linux ARM64, cross-compiles AArch64, and runs core sanitizers on Linux. Camera/model integration requires hardware and converted graphs.
