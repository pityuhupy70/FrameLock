# FrameLock Recorder

A frame-locked game recorder (like Redemption Editor's recording mode) that works with any game rather than one title. When you press **Record**, it takes control of the game's clock. Each frame, the game advances by exactly **1/FPS** seconds, the rendered frame is read back and saved as a JPEG, and only then is the game allowed to move on. A slow PC simply makes the game run slower; frames are never dropped or duplicated.

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
4. Set the **FPS** (e.g. `24`, `30`, `60`, `23.976`, `60000/1001`) and the **output folder**.
5. Press **RECORD**, or press **F9** inside the game. Press it again to stop.
6. Each take goes into its own folder: `OutputFolder\Take_YYYYMMDD_HHMMSS\frame_000000.jpg, frame_000001.jpg, ...`

To turn the frames into a video, use any editor that imports image sequences, or run ffmpeg:
double-click the `make_video.bat` that FrameLock writes into every locked take folder. It runs
`ffmpeg -framerate NUM/DEN -start_number 0 -i frame_%06d.jpg -c:v libx264 -crf 12 -pix_fmt yuv420p -r NUM/DEN output.mp4`
with the exact FPS of the take (e.g. `60/1` or `60000/1001`). Always pass `-framerate` with the recorded FPS:
ffmpeg's default for image sequences is 25 fps, which changes the playback speed. `take_info.txt` lists the
frame count, FPS and the expected game/video duration.

## How it works
- **Controlled clock:** the DLL hooks `QueryPerformanceCounter` (ntdll/kernelbase/kernel32), `GetTickCount`, `GetTickCount64`, `timeGetTime`, `GetSystemTimeAsFileTime` and `GetSystemTimePreciseAsFileTime`. While recording, every thread in the game sees time `T0 + frame × 1/FPS`, using exact integer math with no drift. Time does not move until the current frame is saved.
- **Capture:** the DLL hooks the frame presentation calls for DXGI (`Present`/`Present1`, covering D3D11 and D3D12), Direct3D 9 (`Present`, `PresentEx`, swap chain `Present`) and OpenGL (`wglSwapBuffers`, `SwapBuffers`). Before the frame is shown, the back buffer is copied to the CPU with a GPU sync, at the game's native render resolution. MSAA is resolved, and HDR/FP16 is converted to sRGB.
- **Saving:** frames are saved as JPEG with WIC at **quality 1.0 with 4:4:4 chroma** (the maximum). The game is blocked until the file is fully written, and only then is the clock advanced by one frame.
- If a capture fails, time is **not** advanced, so the same frame is retried. After 3 failures in a row, recording stops instead of skipping a frame.

## Limitations (things that cannot be fully controlled from outside)
- **Anti-cheat games** (EasyAntiCheat, BattlEye, Vanguard, etc.) block injection. **Do not use this tool in online or multiplayer games, because your account can be banned.** Use it in offline or single-player modes only.
- **Vulkan** games (experimental, not yet tested): capture runs through a FrameLock Vulkan layer, which only loads when the game is started with **"Launch game .exe and attach"**. Attaching to a Vulkan game that is already running cannot capture it. On first launch the launcher writes `FrameLock_VkLayer64.json`/`32.json` next to the DLL and registers it under `HKCU\Software\Khronos\Vulkan\ImplicitLayers`. The layer stays inactive unless `FRAMELOCK_VK_CAPTURE=1` is set, and the launcher sets that only for the game it starts. If the FrameLock folder is moved, the next launch removes the old registry entry (only if its manifest file no longer exists). Once a game has presented through the Vulkan layer, the DXGI hooks stand down for that process, so drivers that present Vulkan through DXGI (e.g. NVIDIA) are never captured or stepped twice.
- **Direct3D 10-only** games: capture is now implemented (it shares the DXGI `Present` hook and mirrors the D3D11 readback path). It has **not** yet been verified against a real D3D10 title — treat it as experimental until confirmed.
- Games that read the CPU timestamp directly (`rdtsc`) or tie physics, animation or video playback to real time in other ways may not stay perfectly in sync. Most engines use QPC, so they work.
- **Frame limiters or busy-waiting on the clock:** turn off the in-game FPS cap and VSync for best results. If a game spins waiting for time to pass, a watchdog nudges the clock by 1 ms after 2 s; the UI shows this as "Watchdog slips". Zero slips means a perfect take.
- Audio is not recorded, and game audio may sound wrong while recording.
- 10-bit HDR10 (PQ) output is saved without tone mapping. Turn HDR off in the game for correct colors.
- Log file: `%TEMP%\FrameLock_<pid>.log`.

## v2 fix notes (capture failure / clock deadlock)

**Root cause of the empty Take folder:** recording froze the virtual clock completely until the next Present, and the watchdog only released it after >200000 clock queries and >2 s. Games with a Sleep/QPC frame limiter never reached the next Present, so capture never ran. HRESULTs were not logged, so the log showed nothing.

**Modes**
- *Locked clock (offline export, default):* one simulation step per captured frame at the rational FPS. Time is never frozen: it runs at real speed inside the current step, capped at the end of the step, so frame limiters always reach Present. If the game cannot reach the next Present within the step (FPS cap below output FPS), the clock slips (logged and counted as "Clock slips") instead of hanging.
- *Real-time capture (checkbox):* the clock is not touched; only frames the game actually presents are saved. The final status reports the measured FPS. Nothing is duplicated or fabricated.

**Pipeline:** D3D11 = CopyResource into a 3-slot staging ring with non-blocking Map. D3D12 = current back buffer index, PRESENT->COPY_SOURCE->PRESENT barriers on the swapchain's queue, per-slot allocator/list/readback buffer, fence Signal; the encoder thread waits for the fence before Map. JPEG encoding runs on a worker thread with a 4-job bounded queue (backpressure, never drops). Frames are written as `.tmp` and then renamed. The frame index and clock advance only after a frame is queued.

**Logs:** `%TEMP%\FrameLock_<pid>.log`. Frames 0 and 1 log every stage (device, swapchain size/format, buffer index, state, copy submit, fence, Map, conversion, JPEG options, output path). Any failure logs `stage` + `HRESULT` and stops the take (the frames already saved stay contiguous).

**Self-test:** press F10 or click *Self-test: 120 frames*. It captures 120 frames and then checks: 120 files, sequential names `frame_000000..frame_000119`, non-zero sizes, WIC-decodable, no identical adjacent frames or duplicate hashes, no leftover `.tmp`. The result is shown in the status line and saved to `test_report.txt` in the Take folder. Use a moving scene, because a static or paused scene fails the repeat check by design.

**Not yet validated:** this version has not been compiled or run against real games. Test D3D11 and D3D12 games separately with the self-test before relying on it. Vulkan capture (layer) and the D3D12 swapchain->queue association are new and unverified. D3D12 MSAA back buffers cannot exist (flip-model swapchains require SampleDesc.Count = 1), so that check only guards against an invalid state. If a D3D12 swapchain was created before FrameLock attached and GetDevice(queue) fails, capture still falls back to the last DIRECT queue (logged as a WARNING). D3D9/OpenGL capture is still a synchronous readback (encoding is async) and has not been validated.


## v3 timing fix (video played too fast)

### Root causes
1. **Clock free-run / slip.** When the game spent longer than a budget inside one step, the v2 clock was
   released to run at real speed ("clock slip"). That game time was then covered by fewer captured frames,
   so the encoded video ran faster than the game.
2. **Clamp below the boundary.** Inside a step time was bounded *below* the next step boundary; games that
   wait for a full frame interval never saw it, which triggered the slip above.
3. **Advancing before readback.** D3D11/D3D12 advanced the clock as soon as the copy was queued; when a
   readback slot later failed or was recycled, steps had been simulated without a saved frame.
4. **Real-time takes encoded at the selected FPS.** A real-time take contains only the frames the game
   presented (e.g. 15 per second); encoding them at 60 fps plays 4x too fast.
5. **ffmpeg rate.** `-framerate` missing or different from the recorded FPS (ffmpeg defaults to 25).
6. **F9 from inside the game** could start a take with the FPS from the last *applied* settings rather than
   the value shown in the launcher.

### New behaviour (Locked mode)
- Game time of frame *n* is exactly `n / FPS` (computed from the frame index with 64-bit integer math, no
  accumulated rounding). Every captured frame advances the game by exactly one step: 1/FPS.
- Inside a step, time is held at the boundary for 20 ms real time, then creeps forward at real speed but is
  **clamped at the next boundary (inclusive)**. It can never pass it. There is no free-run anymore.
- The clock advances **only after** the frame was copied and read back into memory (D3D11 blocking Map,
  D3D12 fence wait, D3D9/OpenGL synchronous readback). A failed capture retries the *same* step (3 attempts,
  then the take stops without skipping an index).
- JPEG encoding runs on a worker with a bounded queue; if it falls behind the game simply waits (clock held).
- One uncaptured warm-up step before `frame_000000` so the first frame is also exactly 1/FPS after the previous one.
- So a game that renders at 15 fps in real time, exported at 60 fps, produces 60 unique frames per second of
  game time; recording just takes 4x longer than real time. No duplicated or dropped frames, no speed change.
- Additional time APIs are virtualized: QueryInterruptTime(Precise), QueryUnbiasedInterruptTime(Precise).
- Hotkeys (F9 / F10) and a stall monitor run on their own thread. If the game presents nothing for 3 s the
  status shows a WAITING message (counted as "Stall warnings") - time is still held, never skipped.
- Real-time mode is kept but labelled NOT frame-locked; its take_info.txt reports the measured FPS.

### Limitations
- An in-game FPS cap / VSync limiter **below** the output FPS waits for more than one step of game time and the
  game will appear frozen (the stall warning explains this). Disable the cap or choose an output FPS <= cap.
  FrameLock deliberately does not speed time up to escape the wait.
- Clocks that cannot be hooked from user mode are not virtualized: `rdtsc`, audio device clocks (e.g.
  IAudioClock-driven sync), and video players / cutscenes timed by the audio or a hardware clock.
- Audio is not recorded.
