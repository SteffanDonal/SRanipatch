# SRanipal on Frame

C++20 server and Dear ImGui client. The server owns capture, MNN CPU inference and OSC UDP output. The client reads status and parameters through local mapped memory and can pause inference or toggle OSC.

## Build

Run build commands from `runtime/`. Final packages are written to `../output/`.

CMake 3.25+, Git and a C++20 compiler. Dependencies are pinned and fetched during configuration; `FETCHCONTENT_SOURCE_DIR_MNN`, `FETCHCONTENT_SOURCE_DIR_OPENCV`, `FETCHCONTENT_SOURCE_DIR_GLFW` and `FETCHCONTENT_SOURCE_DIR_IMGUI` accept local source trees.

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

Set `-DSOF_BUILD_CLIENT=OFF` for a headless server. The `linux-aarch64` preset cross-compiles the server using `aarch64-linux-gnu-gcc/g++`; `SOF_SYSROOT` can point to a deployment sysroot. Build the GUI natively on ARM64 or supply target graphics dependencies when cross-compiling it. Release builds enable application LTO when supported; no host-specific CPU flags are applied.

Stage deployment files with `cmake --install build/<preset> --config Release --component Runtime --prefix build/package`. This includes dependency notices and the MSVC runtime on Windows. Linux deployments need compatible system libraries; the GUI requires OpenGL 3.3. CPack configurations produce ZIP/TGZ archives.

## Run

```sh
sof-server
sof-client
```

Run `sof-server --help` for model, OSC and IPC options. Both processes must use the same `--ipc` name. Model configuration is supplied at server startup; GUI controls apply live.

The server reads OSC defaults from `~/.config/sranipal-on-frame/config.ini` on Linux, or `%APPDATA%\sranipal-on-frame\config.ini` on Windows. Command-line options override the file. Supported keys are:

```ini
osc_host=127.0.0.1
osc_port=9000
osc_address=/sranipal/parameters
```

Linux GUI builds include both GLFW Wayland and X11 backends. GLFW normally selects the backend from the session environment. On Steam Frame with Gamescope 3.16.28, use Gamescope's XWayland display because its native `xdg-shell` path currently does not route mouse and close events reliably. `sh scripts/run-linux.sh` selects that path, starts the lip-inference server and client together, and always stops the server when the client exits or the job is interrupted.

Arguments to `scripts/run-linux.sh` are forwarded to the server. For example, `sh scripts/run-linux.sh --precision low` selects MNN's reduced-precision CPU path.

For the VIVE Facial Tracker on Windows:

```powershell
./build/windows-x64/Release/sof-server.exe --capture-only
./build/windows-x64/Release/sof-client.exe
```

Capture is intentionally fixed to the VIVE Facial Tracker: both platforms locate the single USB `0BB4:0321` capture device, apply its vendor initialization sequence, and open native 400×400 YUY2/YUYV at 60 fps. There are no device or format overrides. The client displays the packed stereo image or either camera separately. `sof-capture --frames 120 --output frame.pgm` checks the same capture path without inference or a GUI. The [device protocol](https://github.com/LordOfDragons/vivefacialtracker) supports 64- and 384-byte extension payloads; inference preserves the packed camera bytes used by SRanipal.

The lip model is a fixed MNN graph with a float32 `[1,2,100,100]` stereo input and 30-value internal output. The server filters the packed grayscale image, splits the stereo views, resizes them with cubic preprocessing, scales values to `[0,1]`, and converts the model output to SRanipal's ordered 37 lip shapes. The default pseudo-separable filter applies horizontal and vertical 5-pixel medians; `--median exact` selects SRanipal's full 5×5 median instead. FP32 inference is the default; `--precision low` trades accuracy for speed. `scripts/export-lip-onnx.py` reproduces the intermediate ONNX graph from the Git LFS NPZ archive; conversion to MNN uses MNN 3.2.0's `MNNConvert`.

## Platform boundaries

- Linux: [V4L2 MMAP streaming](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/mmap.html) through the kernel's `uvcvideo` driver. It discovers the capture node belonging to USB `0BB4:0321`, controls extension unit 4 through `UVCIOC_CTRL_QUERY`, and captures 400×400 YUYV at 60 fps while preserving the VIVE's raw luminance bytes. Two driver buffers are used and already-completed buffers are drained before copying the newest frame. The user needs access to the video device.
- Windows: [Media Foundation source reader](https://learn.microsoft.com/en-us/windows/win32/medfound/processing-media-data-with-the-source-reader) asynchronous native YUY2 capture. The camera is selected only by VIVE USB VID/PID. The source reader requests low-latency processing and a one-frame capture-source buffer. Vendor controls use `IKsControl`, with a KS proxy fallback when Media Foundation does not expose topology.
- IPC: fixed-size, versioned snapshots under a Windows named mutex or Linux `flock`; no process-shared C++ atomics. Server publication and client reads are nonblocking. One server owns each channel. Commands carry revisions with server acknowledgements; heartbeat expiry detects stopped servers. Linux regions are private per user and retained across restarts; Windows names are local to the session. Fixed 400×400 luminance previews are published at roughly 30 Hz, independently of capture rate. Status reads only copy metadata; preview reads skip unchanged frames. Client and server must use the same protocol version.
- OSC: [OSC 1.0](https://opensoundcontrol.stanford.edu/spec-1_0.html) float arguments in a single UDP message per inference, in SRanipal lip-shape order. Default address is `/sranipal/parameters`; destinations are numeric IPv4/IPv6 addresses. Output is bounded to 128 parameters and UDP sends are nonblocking.

Capture and inference use separate threads with a latest-frame mailbox. Buffers and host tensors are reused; inference skips stale frames rather than accumulating a queue. Driver timestamps measure start-of-exposure-to-result latency on the VIVE tracker (monotonic V4L2 time on Linux and QPC/MFTIME on Windows), with userspace receipt time as a fallback for incompatible drivers. Windows samples older than two frame intervals are discarded. Malformed or incomplete frames are discarded. When the tracker is absent, disconnected or stops producing valid frames, the server goes idle and resumes capture after a native USB/camera arrival notification. The client polls status at roughly 30 Hz independently of camera/inference rates.

Core tests cover wire encoding, UDP delivery, image row stride, luminance decoding, vendor command acknowledgement, frame replacement, mapped previews and IPC commands across real processes. CI builds Windows x64 and native Linux ARM64, cross-compiles AArch64, and runs core sanitizers on Linux. Camera/model integration requires hardware and converted graphs.
