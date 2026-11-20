# FrameLock Recorder

A game frame-sequence recorder with two timing modes. The default **Preserve actual game speed** mode leaves the game clock untouched and records the real presentation time of every frame. Optional **Locked clock** mode advances the game simulation in rational 1/FPS steps for offline frame-by-frame export. Frames are saved as sequential JPEG files; timestamp sidecars preserve the actual timing when the game runs at a variable frame rate.

## Build (one time)
1. Install **Visual Studio 2022** (Community is fine) and choose the **Desktop development with C++** workload.
2. Double-click `build.bat`. It needs internet once to download MinHook.
3. The finished program is in `dist\`:
   - `FrameLock64.exe` + `FrameLockHook64.dll` for 64-bit games (almost all modern games)
   - `FrameLock32.exe` + `FrameLockHook32.dll` for old 32-bit games

Keep each `.exe` in the same folder as its `.dll`.

## Use
1. Start the game, preferably in **borderless/windowed** or fullscreen mode. Load into the scene you want.
2. Run `FrameLock64.exe`. Run it **as Administrator** if the game runs elevated.
3. Select the game in the list and press **Attach**. You can also use *Launch game .exe and attach*.
4. Set the **FPS** (e.g. `24`, `30`, `60`, `23.976`, `60000/1001`) for locked-clock mode, and choose the **output folder**. Real-time mode uses the game's actual timing.
5. Press **RECORD**, or press **F9** inside the game. Press it again to stop.
6. Each take goes into its own folder: `OutputFolder\Take_YYYYMMDD_HHMMSS\frame_000000.jpg, frame_000001.jpg, ...`, plus `frame_timestamps.csv` and `frames.ffconcat` timing files.

**To preserve the game's actual playback speed**, leave **Preserve actual game speed** checked (real-time mode). FrameLock records only frames the game really presents and writes their actual time intervals. Do not import the JPGs as a fixed-rate sequence (for example, `-framerate 24`) because that can make a 60-FPS capture play 2.5x too fast. Use the generated FFmpeg concat list instead:
`ffmpeg -safe 0 -f concat -i frames.ffconcat -fps_mode vfr -c:v libx264 -bf 0 -crf 12 -pix_fmt yuv420p out.mp4`

The FPS field controls locked-clock mode; in real-time mode the recorded timestamps determine playback cadence.

## How it works
- **Timing:** in real-time mode, the clock is untouched and each captured frame gets a QPC presentation timestamp. In locked-clock mode, the DLL hooks `QueryPerformanceCounter` (ntdll/kernelbase/kernel32), `GetTickCount`, `GetTickCount64`, `timeGetTime`, `GetSystemTimeAsFileTime` and `GetSystemTimePreciseAsFileTime`; game time advances in rational 1/FPS simulation steps. Any clock slips are reflected in the per-frame timestamps.
- **Capture:** the DLL hooks DXGI (`Present`/`Present1`, covering D3D11 and D3D12), Direct3D 9 and OpenGL presentation calls. D3D11/D3D12 use asynchronous readback paths; D3D12 waits for the copy fence before mapping the readback resource. D3D9/OpenGL readback remains synchronous. Captures use the game's render resolution.
- **Saving:** frames are encoded as JPEG with WIC at **quality 1.0 with 4:4:4 chroma** on a worker thread. The capture pipeline preserves frame order and applies backpressure rather than intentionally dropping frames. `frame_timestamps.csv` stores PTS/duration per saved image and `frames.ffconcat` provides an FFmpeg variable-frame-rate input list.
- If a capture fails, time is **not** advanced, so the same frame is retried. After 3 failures in a row, recording stops instead of skipping a frame.

## Limitations (things that cannot be fully controlled from outside)
- **Anti-cheat games** (EasyAntiCheat, BattlEye, Vanguard, etc.) block injection. **Do not use this tool in online or multiplayer games, because your account can be banned.** Use it in offline or single-player modes only.
- **Vulkan** games: the clock control works, but frame capture is not implemented in this version. Use the game's DX11/DX12 option if it has one.
- **Direct3D 10-only** games are not captured.
- Games that read the CPU timestamp directly (`rdtsc`) or tie physics, animation or video playback to real time in other ways may not stay perfectly in sync. Most engines use QPC, so they work.
- **Frame limiters or busy-waiting on the clock:** turn off the in-game FPS cap and VSync for best results. If a game spins waiting for time to pass, a watchdog nudges the clock by 1 ms after 2 s; the UI shows this as "Watchdog slips". Zero slips means a perfect take.
- Audio is not recorded, and game audio may sound wrong while recording.
- 10-bit HDR10 (PQ) output is saved without tone mapping. Turn HDR off in the game for correct colors.
- Log file: `%TEMP%\FrameLock_<pid>.log`.

## v2 fix notes (capture failure / clock deadlock)

**Root cause of the empty Take folder:** recording froze the virtual clock completely until the next Present, and the watchdog only released it after >200000 clock queries and >2 s. Games with a Sleep/QPC frame limiter never reached the next Present, so capture never ran. HRESULTs were not logged, so the log showed nothing.

**Modes**
- *Locked clock (optional offline export):* one simulation step per captured frame at the rational FPS. Time is never frozen: it runs at real speed inside the current step, capped at the end of the step, so frame limiters always reach Present. If the game cannot reach the next Present within the step (FPS cap below output FPS), the clock slips (logged and counted as "Clock slips") instead of hanging.
- *Real-time capture (default checkbox):* the clock is not touched; only frames the game actually presents are saved. Exact presentation timestamps and durations are written to `frame_timestamps.csv` and `frames.ffconcat`, so variable game FPS is not forced into a fixed-rate sequence. Nothing is duplicated or fabricated.

**Pipeline:** D3D11 = CopyResource into a 3-slot staging ring with non-blocking Map. D3D12 = current back buffer index, PRESENT->COPY_SOURCE->PRESENT barriers on the swapchain's queue, per-slot allocator/list/readback buffer, fence Signal; the encoder thread waits for the fence before Map. JPEG encoding runs on a worker thread with a 4-job bounded queue (backpressure, never drops). Frames are written as `.tmp` and then renamed. Per-frame timestamps are carried through asynchronous readback/encoding and written in frame order.

**Logs:** `%TEMP%\FrameLock_<pid>.log`. Frames 0 and 1 log every stage (device, swapchain size/format, buffer index, state, copy submit, fence, Map, conversion, JPEG options, output path). Any failure logs `stage` + `HRESULT` and stops the take (the frames already saved stay contiguous).

**Self-test:** press F10 or click *Self-test: 120 frames*. It captures 120 frames and then checks: 120 files, sequential names `frame_000000..frame_000119`, non-zero sizes, WIC-decodable, no identical adjacent frames or duplicate hashes, no leftover `.tmp`. The result is shown in the status line and saved to `test_report.txt` in the Take folder. Use a moving scene, because a static or paused scene fails the repeat check by design.

**Not yet validated:** this version has not been compiled or run against real games. Test D3D11 and D3D12 games separately with the self-test before relying on it. D3D12 MSAA back buffers, Vulkan, and D3D12 games where the swapchain queue cannot be queried (fallback to the last DIRECT queue, logged as a WARNING) are unsupported or unverified. D3D9/OpenGL capture is still a synchronous readback (encoding is async) and has not been validated.
