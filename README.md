# FrameLock Recorder — experimental v2 source

FrameLock is an experimental injected recorder that saves presented game frames as a numbered JPEG sequence. **This source package has not been built or tested against real games.** Do not treat compatibility or frame timing as confirmed until the D3D11 and D3D12 self-tests pass on your machine.

## Recording modes

- **Locked-clock export (default):** the DLL virtualizes selected Windows time APIs used by the game. At the start of each step, the virtual clock is held briefly (at most 10 ms), then advances at real speed and is capped just below the step boundary. After a frame is queued, the next rational-FPS step begins. If a game busy-polls a hooked clock while held at the boundary, a watchdog releases that clock after more than 0.5 seconds and 1,000 queries, and records a slip. This watchdog specifically detects clock polling; it is not a universal detector for every sleep-based or engine-level frame limiter. A slip means that step took longer than the requested interval.
- **Real-time capture:** the DLL leaves the hooked clocks alone and saves frames the game presents. It does not synthesize missing frames. The final status reports the measured presentation rate.

Locked-clock mode is a clock-hooking approximation, not an engine-level simulation API. It cannot control games that base simulation on unhooked timers, CPU timestamp instructions, their own fixed-step accumulator, or external clocks. It therefore cannot guarantee identical simulation behavior in every title.

## Capture and writing pipeline

- **D3D11:** copies the presented back buffer to a three-slot staging ring. Completed copies are mapped without waiting where possible; the ring applies backpressure if all slots are busy.
- **D3D12:** copies the current back buffer on the swapchain-associated direct queue, restores the resource to PRESENT, and uses per-slot allocators, command lists, readback buffers and a fence. The worker maps a slot only after fence completion is confirmed. MSAA back buffers are not supported.
- **D3D9 and OpenGL:** readback remains synchronous; JPEG encoding and file writing are delegated to the worker.
- **JPEG output:** WIC encodes frames on a worker thread. Files are written as `.tmp` and renamed to `.jpg` after commit. The capture queue is bounded; when it is full, the render thread waits rather than silently dropping a queued frame. GPU-slot backpressure can also make the render thread wait.

The frame index advances after the capture has been accepted into the readback/worker queue, not after the JPEG has finished writing. The worker writes jobs in order. If an asynchronous job fails, the take is stopped and later queued jobs from that take are discarded so that already-written files remain contiguous.

## Build locally

Requirements: Windows, Visual Studio 2022 with **Desktop development with C++**, CMake, and internet access during configuration so CMake can download MinHook.

Run `build.bat` from this folder. The output is placed in `dist\\`:

- `FrameLock64.exe` + `FrameLockHook64.dll` for 64-bit games
- `FrameLock32.exe` + `FrameLockHook32.dll` for 32-bit games

Keep each executable beside its matching DLL.

## Build with GitHub Actions (no local compiler required)

1. Create or open a GitHub repository and upload the contents of this folder, preserving `.github/workflows/windows-build.yml` and `src/`.
2. Open **Actions**, select **Build FrameLock for Windows**, and choose **Run workflow**.
3. When the run completes, download the `FrameLock-Windows` artifact. It contains both launcher/DLL pairs.

The GitHub runner performs the Windows builds and downloads MinHook. Your own computer does not need Visual Studio for this route, but the GitHub runner needs network access.

## Use

1. Start the game and load a moving scene. Borderless/windowed mode is a useful starting point.
2. Run the matching launcher. Run it elevated only when the game itself is elevated.
3. Select the game process and press **Attach** (or launch the game's executable from the launcher).
4. Set the FPS and output folder. Examples: `24`, `30`, `60`, `23.976`, `60000/1001`.
5. Press **RECORD** or F9 to start/stop. Enable **Real-time capture** for the real-time mode.
6. Each take is written under `OutputFolder\\Take_YYYYMMDD_HHMMSS\\` as `frame_000000.jpg`, `frame_000001.jpg`, and so on.

## Diagnostics and self-test

The log is `%TEMP%\\FrameLock_<pid>.log`. Frames 0 and 1 log detailed capture steps. Capture and worker failures report the stage and HRESULT where the API supplies one. The D3D9/OpenGL helper path still reports generic synchronous-readback failures rather than every underlying API error.

Press F10 or click **Self-test: 120 frames**. In a moving scene, it checks for 120 sequential filenames, non-empty files, WIC-decodable JPEGs, no identical adjacent frame hashes, and no leftover `.tmp` files. The result is shown in the status line and written to `test_report.txt` in the take folder. A static or paused scene will fail the repeated-frame check.

## Known limitations and unverified behavior

- D3D11 and D3D12 have **not** been validated against real games. Run the 120-frame self-test separately on at least one game using each API.
- D3D12 MSAA back buffers and Vulkan capture are unsupported.
- If the swapchain's D3D12 queue cannot be queried, the implementation falls back to the last DIRECT queue observed by the hook and logs a warning. That queue is not proven to be the swapchain's actual present queue; capture and resource synchronization are therefore not guaranteed in this fallback case.
- D3D9 and OpenGL capture paths are synchronous and have not been tested.
- Some games use time sources that are not hooked or manage their simulation timing independently. Locked-clock mode may produce uneven timing or slips in those games.
- The render thread can wait under queue or GPU-slot backpressure. The encoder itself runs on a separate worker thread, but the entire capture path is not guaranteed to be non-blocking.
- Audio is not captured. Anti-cheat-protected and online multiplayer games should not be used; DLL injection may violate their rules or trigger anti-cheat protections.
