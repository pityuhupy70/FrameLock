# FrameLock Recorder (source build)

FrameLock is a Windows in-process recorder prototype. It can freeze a game's commonly used Windows clocks, sample a supported graphics back buffer on each intercepted presentation, and write sequential JPEG images before releasing the next clock step.

**This is not a universal recorder and is not a verified, ready-to-run executable.** It is a source project that must be built and tested on Windows. Engine-specific behavior, graphics-driver behavior, protected processes, and unhooked timing sources can prevent a game from working correctly.

## Current supported capture paths

| Graphics API | Frame capture | Notes |
|---|---:|---|
| Direct3D 9 | Attempted | Device/back-buffer formats and multisampling are handled where supported |
| Direct3D 10 | Attempted | Uses the DXGI swap-chain path and a staging texture |
| Direct3D 11 | Attempted | Uses a staging texture and GPU readback |
| Direct3D 12 | Attempted | Uses a tracked direct command queue; multi-queue engines may not work reliably |
| OpenGL (WGL) | Attempted | Reads the default back buffer; unusual framebuffer/presentation setups may fail |
| Vulkan | **Not implemented** | Clock hooks alone do not capture Vulkan images |
| Other APIs / proprietary paths | **Not implemented** | No generic engine-independent capture method exists here |

The capture hooks cover common Windows presentation entry points, not every possible renderer configuration. Fullscreen optimizations, multiple swap chains, unusual device setups, overlays, protected content, and driver differences can affect results. Anti-cheat or protected games may reject injection; do not inject into online or multiplayer games.

## Strict frame-step behavior

When recording starts, the hook freezes the virtual QPC-derived clock. It samples the selected back buffer at the intercepted presentation point, copies pixels to CPU memory, and synchronously waits for the JPEG writer to commit the file. **Only after a successful save** does it change the virtual clock to the next frame time:

`T(n) = T(start) + floor(n * QPC_frequency * FPS_denominator / FPS_numerator)`

This uses integer rational stepping to avoid cumulative floating-point drift. The former 1 ms watchdog correction has been removed because any unrequested clock increment violates exact frame stepping. If a game busy-waits for its clock to advance, it can stall while recording; this version deliberately does not fake time to get past that wait.

A failed capture is retried at the same virtual time. After three consecutive capture failures, recording stops. A JPEG write failure stops recording. Files are named `frame_000000.jpg`, `frame_000001.jpg`, etc. within a timestamped take folder.

**Important limits of the guarantee:** the recorder can serialize its own intercepted presentation/capture/save/clock-step sequence. It cannot force every engine to execute exactly one simulation update per presentation, control `RDTSC`/CPU cycle counters, virtualize every wait/timer/GPU timestamp, or prevent a game from rendering the same visual state twice. Some engines derive physics or animation from their own clocks or fixed-step loops. Thus it cannot promise mathematically perfect simulation equivalence for every game.

## Output

- User-selectable FPS, including rational entries such as `60000/1001` and common decimal aliases such as `23.976`.
- Back-buffer resolution for supported capture paths.
- JPEG quality configured to 1.0 and 4:4:4 chroma subsampling when supported by the Windows Imaging Component encoder.
- Sequential JPEGs in one folder per take.
- No audio capture.

JPEG remains a lossy format even at maximum encoder quality.

## Build without installing Visual Studio locally

A GitHub Actions workflow is included at `.github/workflows/windows-build.yml`. Upload the **contents of this FrameLock folder** to the root of a GitHub repository, keeping the `.github/workflows` directory. Then open **Actions** and run **Build FrameLock for Windows**. The Windows runner builds both architectures and publishes a `FrameLock-Windows` artifact. Persian step-by-step instructions are in `BUILD_ON_GITHUB_FA.txt`. This still requires a GitHub account and a successful remote build; prebuilt binaries are not included in this source ZIP.

## Build on Windows

1. Install Visual Studio 2022 with **Desktop development with C++**, the Windows SDK, and CMake.
2. Run `build.bat` from this folder. CMake downloads MinHook from GitHub during the first configure, so internet access is required.
3. Binaries are placed in `dist\`:
   - `FrameLock64.exe` and `FrameLockHook64.dll` for 64-bit targets.
   - `FrameLock32.exe` and `FrameLockHook32.dll` for 32-bit targets.
4. Keep each EXE beside its matching DLL.

The project has **not been compiled or tested in this environment**. A successful Windows build is still required before use.

## Use

1. Start a supported single-player game and load the scene.
2. Run the matching FrameLock executable. Elevation must match the game's process elevation.
3. Select the game process and click **Attach**.
4. Set FPS and an output folder.
5. Click **RECORD** or press **F9** in the game. Press again to stop.

A take is saved as `OutputFolder\Take_YYYYMMDD_HHMMSS\frame_000000.jpg`, etc.

To assemble a video with FFmpeg:

```bat
ffmpeg -framerate 24 -i frame_%06d.jpg -c:v libx264 -crf 12 -pix_fmt yuv420p out.mp4
```

Use the same `-framerate` value that was selected in FrameLock.

## Known limitations and safety

- Windows only; launcher and hook DLL must match the target game's architecture (32/64-bit).
- Anti-cheat systems and protected processes may block injection. Use only in offline/single-player games where injection is permitted.
- Vulkan capture is not implemented in this source version. Direct3D and OpenGL coverage is best-effort, not universal.
- Direct3D 12 uses a process-wide last-seen direct queue; games with multiple graphics queues or unusual synchronization may fail or capture incorrectly.
- Clock hooks cover selected exported Windows APIs, not every timer source. Games using `RDTSC`, custom timers, GPU timestamps, or real-time waits may not respect the virtual clock.
- A game that busy-waits on a frozen timer may stall. The recorder will not advance time by a non-frame interval to escape that wait.
- HDR/PQ output is not tone-mapped comprehensively. Disable HDR in the game for more predictable SDR JPEG colors.
- Game audio is not recorded.
- Log: `%TEMP%\FrameLock_<pid>.log`.

## Project files

- `src/launcher/main.cpp` — Win32 user interface and DLL injection.
- `src/hook/dllmain.cpp` — timer hooks, graphics hooks, GPU readback, and JPEG writer.
- `src/shared.h` — shared-memory control block.
- `CMakeLists.txt` / `build.bat` — build configuration.
