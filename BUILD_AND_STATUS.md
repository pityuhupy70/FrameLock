# FrameLock — status report (this phase)

## Honesty statement (read first)
This phase was done in a Linux sandbox with **no Windows toolchain, no Windows SDK, no GPU, no
DirectX/Vulkan/OpenGL runtime, and no games**. Therefore I **could not compile or run** the project here,
and I make **no claim that any binary was produced or that anything "works" at runtime.** The only way to get
actual tested x64/x86 builds is to run the included GitHub Actions workflow (or `build.bat` on a Windows
machine). Changes below were made and reviewed by reading the code; they still need a real compile + run to be
confirmed.

## What I changed in the source (implemented, not yet compiled/tested)
1. **Direct3D 10 capture path added** (`src/hook/dllmain.cpp`).
   - New `SubmitD3D10()` + `g10` context + `D10ReleaseTex()`, and a new branch in `SubmitDXGI()` that tries
     `ID3D11Device` → `ID3D10Device` → `ID3D12` on the swapchain.
   - Mirrors the existing (and already-shipped) D3D11 readback: `CopyResource` into a STAGING texture, MSAA
     resolve when needed, **blocking `Map`** so the pixels are in CPU RAM before the clock may advance, then
     the existing bounded encode queue.
   - The DXGI `Present` hook was already shared across D3D10/11/12, so only the device-specific readback was
     missing. This closes the previous "Direct3D 10-only games are not captured" gap.
   - `enum class Api` gained `D10`; added `#include <d3d10.h>` / `<d3d10_1.h>`.
2. **Comment correctness fix** in `FrameStepPost()` — the clock is advanced in **locked** mode (the old inline
   comment said "real-time", which was misleading). No behavior change.

No working features were removed and no unrelated features were added. The v3 timing model (one simulation
step = exactly 1/FPS per captured frame, clock clamped at the step boundary, advanced only after readback) is
unchanged.

## What I added (build/CI)
- **`.github/workflows/build.yml`** — builds **x64 and Win32** with Visual Studio 2022 + CMake on
  `windows-latest`, fetches MinHook (runners have internet), and uploads `FrameLock-x64` / `FrameLock-Win32`
  artifacts. This is the mechanism that produces real, compiler-verified builds.
- **`.gitignore`** for CMake/MSVC build output.

## How to get verified builds
1. Push this repo to GitHub → the workflow runs automatically (or trigger it from the Actions tab).
2. Download the `FrameLock-x64` / `FrameLock-Win32` artifacts (each has `FrameLock*.exe` + `FrameLockHook*.dll`).
3. Or on Windows: install VS 2022 ("Desktop development with C++") and run `build.bat`.
4. **If the D3D10 addition has any compile error, it is isolated to `SubmitD3D10` / the `SubmitDXGI` branch** —
   the CI log will point straight at it.

## Verified in this phase
- Static review only: brace/paren balance OK; new symbols resolve against the existing code; the D3D10 path is
  a structural mirror of the reviewed D3D11 path.
- **Not** verified: compilation, linking, and any runtime behavior. No self-test (F10) was run — that requires
  a real game.

## Honest limitations that remain (by design or not yet done)
- **"Universal" simulation-clock control is not achievable in general**, and this tool does not claim it. It
  virtualizes user-mode time APIs (QPC, GetTickCount(64), timeGetTime, GetSystem*Time*, interrupt-time). Games
  that derive simulation time from `rdtsc`, an audio device clock, GPU timestamps, a fixed internal timestep,
  or a network clock will **not** be driven deterministically by external hooks. The code already detects/
  reports stalls rather than fabricating frames — that is the correct behavior and should stay.
- **Vulkan capture is still not implemented** (clock virtualization still applies). This needs a real next-phase
  subsystem (see below).
- **D3D12 present-queue detection** falls back to the last DIRECT queue seen in `ExecuteCommandLists` when
  `GetDevice(ID3D12CommandQueue)` fails; on multi-queue engines that fallback may be wrong.
- **D3D12 MSAA back buffers** are rejected (`E_NOTIMPL`).
- **Anti-cheat** titles block injection — out of scope and must stay unsupported.
- **D3D10 path is experimental/unverified** until run against a real D3D10 game.

## What the next phase (your next run) should accomplish
1. **Compile via CI, fix any build errors**, then run the F10 120-frame self-test on real D3D11, D3D12, D3D9,
   OpenGL, and a D3D10 title; record pass/fail per API. Nothing should be called "working" before this.
2. **Vulkan capture.** Implement via `vkQueuePresentKHR` hook + swapchain image tracking + `vkCmdCopyImageToBuffer`
   into a host-visible buffer with a fence wait (same "readback before clock advance" contract). A proper Vulkan
   layer is the clean modular approach.
3. **D3D12 present queue** — track the queue actually used to present (associate queue↔swapchain at
   `CreateSwapChainForHwnd`/`Present`), and add D3D12 MSAA resolve.
4. **Modularity** — split `dllmain.cpp` into per-API capture modules behind a small `ICapture` interface so new
   APIs don't touch the core clock/queue.
5. **Determinism hardening** — optional `rdtsc` virtualization via a VEH on the `rdtsc`/`rdtscp` instruction for
   engines that read the TSC directly; detect and clearly report when an engine's timebase can't be hooked.


---

# Phase opus55: second half (continues the phase above; nothing above was undone)

## Honesty statement
Same sandbox limits as before: Linux only, no MSVC, Windows SDK, MinGW, Vulkan SDK, GPU, or games. GitHub could not be
reached either, so not even the Vulkan headers could be downloaded for a syntax check. **Nothing in this phase was
compiled or run.** Verification was limited to careful review against the Win32/DXGI/Vulkan specs, a
comment- and string-aware brace/paren/bracket balance check on every edited file (all balanced), and a check that every new
symbol is declared before use.

## Completed in this phase (implemented, NOT compiled/tested)
1. **Vulkan capture as an implicit Vulkan layer**: new `src/hook/vk_layer.cpp`, built into the same
   FrameLockHook{64,32}.dll.
   - Implements loader-layer interface v2: `vkNegotiateLoaderLayerInterfaceVersion` is exported via the new
     `src/hook/FrameLockHook.def`, so the export is undecorated on x86 too.
   - Intercepts: CreateInstance/DestroyInstance, CreateDevice/DestroyDevice, GetDeviceQueue(2),
     Create/DestroySwapchainKHR, and QueuePresentKHR.
   - Adds `TRANSFER_SRC` to the swapchain image usage when the surface supports it. If that create call fails, it retries
     the unmodified create, so the game never breaks.
   - Capture at present follows the same "pixels in RAM before the clock advances" contract as the other APIs:
     1. Barrier PRESENT_SRC->TRANSFER_SRC.
     2. `vkCmdCopyImageToBuffer` into a host-visible buffer.
     3. Barrier back to PRESENT_SRC, plus a host-read buffer barrier.
     4. Submit on the present queue. This submit waits on the game's present semaphores and signals our own semaphore.
     5. Block on a fence (10 s timeout), invalidate the memory if it isn't coherent, copy, then `FL_QueueFrame`.
     6. The present is then rewritten to wait on our semaphore.
   - If the submit fails, the game's semaphores are left untouched (spec guarantee), and the original present runs unchanged.
   - Supported formats: B8G8R8A8/R8G8B8A8 (UNORM and SRGB), A2B10G10R10, A2R10G10B10, R16G16B16A16_SFLOAT. Any other
     format fails with a clear stage message, and the core retries or stops as usual.
   - The CMake option `FL_ENABLE_VULKAN` (default ON) downloads Vulkan-Headers v1.3.290. Turning it OFF builds a stub,
     which is an escape hatch if the layer won't compile.
2. **Launcher** (`src/launcher/main.cpp`): new `RegisterVulkanLayer()`. It writes `FrameLock_VkLayer{64,32}.json`
   next to the DLL (absolute library_path, `enable_environment FRAMELOCK_VK_CAPTURE=1`, and the required
   `disable_environment`), then registers it in `HKCU\Software\Khronos\Vulkan\ImplicitLayers`. `LaunchGame()` sets
   `FRAMELOCK_VK_CAPTURE=1` only around its `CreateProcessW` call. A registration failure is non-fatal and shown to the user. `advapi32` is
   linked.
3. **First step of the ICapture split**: new `src/hook/flcore.h`, which `vk_layer.cpp` uses as its only interface to the core.
   - It holds the `PF` enum (moved out of dllmain.cpp) plus `FL_BeginPresent/FL_EndPresent`, `FL_StepPre/FL_StepPost`,
     `FL_QueueFrame`, `FL_BytesPerPixel`, `FL_Log`, `FL_SetApi`, `FL_RealQPC/FL_RealMs`, and `FL_VkLayerActive`.
   - The wrappers are implemented in dllmain.cpp, under "core interface (flcore.h)" right after `PresentWrap`. They
     call exactly the same `FrameStepPre/FrameStepPost`/`Enqueue` code, so behavior is unchanged.
4. **D3D12 present-queue association** (dllmain.cpp): new `HookDXGIFactory()`, called at the end of `HookDXGI()`.
   - It hooks IDXGIFactory::CreateSwapChain (vtable 10) and IDXGIFactory2::CreateSwapChainForHwnd/CoreWindow/Composition
     (vtable 15/16/24).
   - For D3D12, `pDevice` is the command queue, so `RememberSwapchainQueue()` records swapchain (COM identity)
     -> queue in `g_scQueue`.
   - `SubmitD3D12` resolution order is now: `GetDevice(queue)` -> creation-time map -> old `g_lastQueue` fallback (WARNING).
5. **D3D12 MSAA: resolved by analysis, not code.** D3D12 only supports flip-model swapchains, and DXGI requires
   SampleDesc.Count == 1 for those, so an MSAA D3D12 back buffer cannot exist. A resolve path would be unreachable. The
   check stays, with a corrected stage message and comment.
6. **rdtsc: the previous plan was corrected.** A VEH cannot trap rdtsc/rdtscp in user mode, because they only fault when CR4.TSD is set,
   which is kernel-controlled. Instead there is a new read-only `ReportTimebases()`, called in InitThread after
   `InstallTimeHooks()`. It counts rdtsc/rdtscp byte patterns in the exe's committed, readable code and logs them against the
   random-byte expectation (bytes/65536). It is a heuristic for diagnosis and changes nothing.
7. The InitThread Vulkan status message now explains that Vulkan capture requires launching the game from FrameLock. It
   shows only when vulkan-1.dll is loaded, the layer is not active, no API has been captured yet, and no take is running.
8. README limitations updated to match.

## Known risks / caveats in the new code (check these first when testing)
- **Semaphore reuse heuristic:** the layer uses a ring of 16 semaphores per device. Core Vulkan cannot tell when a present's
  wait has finished. The robust fix is `VK_EXT_swapchain_maintenance1` present fences.
- **Single swapchain per present:** only `pSwapchains[0]` is captured. On 32-bit, the 64-bit swapchain handle is truncated
  to form the take target id.
- **Swapchains that predate the layer** (impossible when launched from FrameLock) and swapchain images without
  TRANSFER_SRC support fail with a clear stage message.
- **HDR10 (ST2084) swapchains** are encoded as if they were SDR, and a warning is logged.
- **Drivers that present Vulkan through DXGI on another thread** (e.g. NVIDIA "layered on DXGI" present
  method) could make both the layer and the DXGI hook step the clock. The `t_inPresent` guard only covers same-thread nesting.
  If F10 shows double stepping, ignore DXGI presents while `FL_VkLayerActive()` is true.
- **Child processes** of a launched game inherit `FRAMELOCK_VK_CAPTURE` and also load the layer, which is harmless.
- **`g_scQueue` entries are never removed.** That's one queue reference per created swapchain, a negligible leak. A
  reused swapchain address is simply overwritten.
- **Header specifics to confirm on the first compile:** `vk_layer.h` from Vulkan-Headers v1.3.290 (`VkLayerDeviceCreateInfo`,
  `VK_LOADER_DATA_CALLBACK`, `LAYER_NEGOTIATE_INTERFACE_STRUCT`, and whether the header already prototypes
  `vkNegotiateLoaderLayerInterfaceVersion`; the definition matches the standard signature either way).

## What remains: exact continuation points for the next AI
1. **Build (first!).** Push and run `.github/workflows/build.yml`.
   - Likely error sites: `vk_layer.cpp` (header names), `HookDXGIFactory`/`Hk_CSC*` typedefs in dllmain.cpp, and
     `RegisterVulkanLayer` in main.cpp.
   - If the Vulkan part blocks the build, configure with `-DFL_ENABLE_VULKAN=OFF` to unblock everything else.
   - Confirm with `dumpbin /exports FrameLockHook32.dll` that `vkNegotiateLoaderLayerInterfaceVersion` is undecorated.
2. **Runtime F10 self-test per API**, recording pass/fail in this file: D3D9, D3D10, D3D11, D3D12 (check that the log
   shows "recorded as its present queue"), OpenGL, and Vulkan via Launch (the log should show "[Vulkan] layer: instance ...
   created" and "swapchain created ... transferSrc 1").
   - For Vulkan, also run with `VK_LOADER_DEBUG=layer` to confirm the loader loads the manifest.
   - Nothing is "working" until it passes this test.
3. **Finish the ICapture split.** Move `SubmitD3D9`, `SubmitD3D10`, `SubmitD3D11`, `SubmitD3D12`, and the GL readback
   out of dllmain.cpp into `capture_d3d9.cpp`, `capture_d3d1x.cpp`, `capture_d3d12.cpp`, and `capture_gl.cpp`, using only
   `flcore.h` (extend it with whatever they need, e.g. `Frame`/`QueueCpuFrame`). `vk_layer.cpp` is the reference pattern. The core
   (clock, take, encoder, hotkeys) stays in dllmain.cpp.
4. **Vulkan hardening** after the first test: `VK_EXT_swapchain_maintenance1` present fences for semaphore reuse,
   multi-swapchain presents, more formats (e.g. A1R5G5B5 or R5G6B5 if ever seen), and HDR10 tone-mapping.
5. **Optional:** remove `g_scQueue` entries on swapchain release (needs a Release hook or a private-data destructor
   callback via `SetPrivateDataInterface`).
6. **Do NOT redo:** D3D10 path, CI workflow, D3D12 MSAA resolve (impossible), or VEH rdtsc (impossible). Real TSC control
   would need code patching or a hypervisor, which is out of scope. Report it via the `ReportTimebases()` log instead.


## Phase opus55 - review pass (follow-up, still NOT compiled)
Same limits: Linux sandbox, no MSVC/Windows SDK/MinGW/Vulkan headers, no network, no GPU or games. Nothing here has
been compiled or run. This pass is a code review of the opus55 changes plus small defensive fixes.

### Changed in this pass (vk_layer.cpp only)
- vkGetDeviceProcAddr no longer hands out the vkCreateSwapchainKHR / vkDestroySwapchainKHR / vkQueuePresentKHR
  intercepts when the next layer/driver returns NULL for them (VK_KHR_swapchain not enabled on that device).
  Before, an app that queried them anyway would get our function and crash on a null next-pointer.
- Null guards in FL_vkCreateSwapchainKHR, FL_vkDestroySwapchainKHR, FL_vkGetDeviceQueue, FL_vkGetDeviceQueue2
  (unknown device / missing next function) instead of dereferencing null.
- Braces, parentheses and brackets still balance.

### Reviewed and found consistent (no change)
- Layer negotiation (interface v2), VK_LAYER_LINK_INFO chain advance, VK_LOADER_DATA_CALLBACK use for our command
  buffer, dispatch-key maps, swapchain image tracking, PRESENT_SRC -> TRANSFER_SRC -> PRESENT_SRC barriers,
  tightly packed copy (pitch = w * bpp), fence wait + invalidate for non-coherent memory, semaphore hand-off so the
  present waits on our signal once our submit consumed the game's semaphores.
- Manifest has both enable_environment and disable_environment (the loader ignores implicit layers without
  disable_environment).
- .def export + __stdcall: link.exe matches the undecorated .def name to _vkNegotiateLoaderLayerInterfaceVersion@4.
  Still confirm with dumpbin /exports on the 32-bit DLL.
- D3D12 swapchain->queue map reference counting (replace/duplicate cases) and the lookup order in SubmitD3D12.
- IDXGIFactory / IDXGIFactory2 vtable slots 10, 15, 16, 24.

### Open points to check on Windows (in addition to the opus55 list)
- Bitness: RegisterVulkanLayer registers only the layer matching the launcher (64 or 32). A 32-bit Vulkan game must
  be launched from the 32-bit launcher, or the loader will not find a loadable layer for it.
- The HKCU ImplicitLayers entry stays after FrameLock exits. That is harmless (gated by FRAMELOCK_VK_CAPTURE), but
  moving or deleting the FrameLock folder leaves a stale entry; consider removing it on uninstall.
- After a 10 s fence timeout the present still waits on our semaphore, which may never signal if the GPU is hung.
  That matches the "stop the take" path but should be seen once in a forced-timeout test.


## Phase review2 (follow-up to opus55) - still NOT compiled or run

Same sandbox limits as before: Linux, no MSVC, Windows SDK, MinGW, Vulkan headers or network. The only automated check was brace, parenthesis and bracket balance on every edited file (after stripping comments and strings). It passes. Treat everything as unverified until CI builds it and F10 passes.

### Reviewed (no change needed)
- vk_layer.cpp in full, all 567 original lines:
  - Loader negotiation (interface v2) and the VkLayerDeviceLink / VkLayerInstanceLink chain walk.
  - pfnSetDeviceLoaderData on command buffers.
  - Barriers: PRESENT_SRC to TRANSFER_SRC and back, plus the host-read buffer barrier.
  - The `#undef CreateSemaphore` collision fix.
  - 10-bit channel mapping: A2B10G10R10 = DXGI R10G10B10A2, A2R10G10B10 = D3D9 layout.
  - sRGB handled as raw 8-bit, zero-extent (minimized) guard, swapchain retry without TRANSFER_SRC, and replaceWait when there are 0 wait semaphores.
- The core calls the capture callback exactly once per present (FrameStepPre has no inner retry), so the game's wait semaphores are never consumed twice.
- RememberSwapchainQueue reference counting is correct (replace, same-queue and new paths). HookDXGIFactory vtable slots 10/15/16/24 match the IDXGIFactory2 layout.
- RegisterVulkanLayer: library_path is JSON-escaped, the registry value name is the full manifest path (DWORD 0), and the env var is set only around CreateProcessW.

### Changed in this phase
1. **vk_layer.cpp: null-call guard.** The device capOk check covered only 7 of about 25 next-in-chain functions, so VkCapture could call a null pointer (for example ResetCommandPool or InvalidateMappedMemoryRanges).
   - DevData now has `capOk`, which checks every function the capture path calls, plus memoryTypeCount and queue families.
   - VkCapture fails cleanly (E_NOTIMPL, stage message) when capOk is false.
   - DestroyDevice only tears down capture objects when capOk is true.
2. **Double-step mitigation (was "known issue / do if F10 shows double steps").**
   - New `bool FL_VkLayerPresenting()` in flcore.h, implemented in vk_layer.cpp outside the FL_NO_VULKAN block. In the stub build it is always false.
   - It returns true while the layer has seen a vkQueuePresentKHR within the last 1 s.
   - Hk_Present and Hk_Present1 in dllmain.cpp then call the original present directly and log once (DxgiStandDownLog).
   - Side effect: if the Vulkan path is failing for a game (for example an unsupported format), the DXGI path no longer acts as an accidental fallback for that game. The failure is reported through the Vulkan stage message instead.

### Still open (unchanged from opus55)
- After a 10 s fence timeout, the present still waits on our signal semaphore. If the GPU never finishes, the game hangs exactly as it would have anyway.
- The retired fence, pool and buffer are leaked on purpose.
- 16-semaphore ring reuse: the proper fix is VK_EXT_swapchain_maintenance1.
- Only the first swapchain per present is captured, and the 32-bit target id is truncated.
- HDR10 is saved as SDR.
- g_scQueue entries are never removed.
- Bitness: only the layer matching the launcher's bitness is registered.
- A stale HKCU ImplicitLayers value remains if the folder is moved. It is harmless because it is gated by enable_environment, but it could be cleaned up.

### Next AI: do exactly this, in order
1. CI build (.github/workflows/build.yml, x64 + Win32).
   - Likely error spots: Vulkan header names in vk_layer.cpp, the Hk_CSC* typedefs, RegisterVulkanLayer, and the new FL_VkLayerPresenting linkage (declared in flcore.h, defined in vk_layer.cpp, used in dllmain.cpp).
   - If Vulkan blocks the build, use -DFL_ENABLE_VULKAN=OFF.
   - Run `dumpbin /exports` on FrameLockHook32.dll and check that vkNegotiateLoaderLayerInterfaceVersion has no decoration.
2. F10 self-test per API: D3D9, D3D10, D3D11, D3D12, OpenGL, and Vulkan started via Launch.
   - For Vulkan, also run with VK_LOADER_DEBUG=layer.
   - On NVIDIA, check the log for the "DXGI hooks stand down" line and confirm there are no double steps.
   - Record pass/fail here.
3. Per-API split into capture_*.cpp files using only flcore.h, following the pattern of vk_layer.cpp.
4. Vulkan hardening: swapchain_maintenance1 present fences, multiple swapchains, more formats, HDR10 tone-mapping.
5. Optional: g_scQueue cleanup and stale-registry cleanup.

### Do NOT redo
The D3D10 path, the CI workflow, a D3D12 MSAA resolve (impossible), rdtsc interception through an exception handler (impossible), the opus55 null guards in the swapchain/queue intercepts, or this phase's capOk and DXGI stand-down changes.

## Phase review3 (follow-up to review2) - still NOT compiled or run

Same sandbox limits as before: Linux, no MSVC, no Windows SDK, no MinGW, no Vulkan headers, no network. The only automated check was bracket/brace/parenthesis balance on every edited file (after stripping comments, strings and char literals). It passes. Everything below is unverified until CI builds it and F10 passes.

### Review of review2 (findings)
- **capOk null-call guard: correct.** It covers every next-in-chain pointer that VkCapture and EnsureCaptureObjects call, plus memoryTypeCount and queue families, and it is checked before any capture object is created, so skipping DestroyCaptureObjects when capOk is false cannot leak anything that exists. `semNext % semRing.size()` cannot divide by zero: EnsureCaptureObjects fills the ring (or returns false) before VkCapture uses it.
- **FL_VkLayerPresenting linkage: correct.** Defined outside the FL_NO_VULKAN block; FL_RealQPC/FL_RealMs are non-static wrappers in dllmain.cpp.
- **Launcher: correct.** JsonEscapeUtf8 (backslash + quote after UTF-8 conversion) is sufficient for Windows paths (control characters cannot occur in them). Registry value name = full manifest path, DWORD 0. FRAMELOCK_VK_CAPTURE is set only around CreateProcessW.
- "18 files" in the ZIP = 12 files + 6 directory entries. Consistent with earlier phases.
- **BUG (fixed below): the review2 DXGI stand-down used a 1 s window measured from the START of the last vkQueuePresentKHR.** During a take one Vulkan present can block for well over 1 s (FL_QueueFrame backpressure when the encode queue is full, dllmain.cpp g_jCv.wait; the capture fence wait allows up to 10 s). After 1 s inside that present, Hk_Present/Hk_Present1 resumed normal operation, so on NVIDIA "layered on DXGI" the driver thread's DXGI present went through PresentWrap and stepped the clock as well: the exact double step the change was meant to prevent, and most likely under heavy encoding (high resolution, slow disk). Loading screens with no presents for > 1 s had the same gap.

### Changed in this phase
1. **vk_layer.cpp / flcore.h / dllmain.cpp: sticky DXGI stand-down.**
   - `g_vkLastPresentQpc` (1 s window) replaced by `std::atomic<bool> g_vkHasPresented`, set (release) on the first vkQueuePresentKHR with swapchainCount >= 1, BEFORE calling down the chain, and never cleared.
   - `FL_VkLayerPresenting()` = g_vkHasPresented && g_vkLayerActive. Stub build: always false (the layer never runs).
   - New one-time log line: `[Vulkan] layer: first vkQueuePresentKHR seen; DXGI Present hooks now stand down for this process`. The existing `DXGI hooks stand down` line in dllmain.cpp is still logged once when a DXGI present is actually bypassed.
   - Side effect (same as review2, now permanent for the process): once a game has presented through the FrameLock Vulkan layer, DXGI capture never acts as a fallback for that process. Vulkan failures are reported through the Vulkan stage message.
2. **main.cpp RegisterVulkanLayer: stale registry cleanup** (was "still open").
   - The ImplicitLayers key is now opened with KEY_SET_VALUE | KEY_QUERY_VALUE.
   - Before registering, values are enumerated; a value is deleted only if its name ends in `\` + VK_MANIFEST of the launcher's own bitness, it is not the current manifest path (case-insensitive), AND the file no longer exists. Other existing FrameLock copies and unrelated layers are never touched. Deletion happens after enumeration. Errors are ignored (best effort, non-fatal).

### Still open
- After a 10 s fence timeout, the present still waits on our signal semaphore. If the GPU never finishes, the game hangs exactly as it would have anyway.
- The retired fence, pool and buffer are leaked on purpose after a timeout.
- 16-semaphore ring reuse: the proper fix is VK_EXT_swapchain_maintenance1 present fences.
- Only the first swapchain per present is captured, and the 32-bit target id is truncated.
- HDR10 is saved as SDR.
- g_scQueue (D3D12 swapchain -> queue) entries are never removed. A correct fix needs a swapchain-lifetime signal (e.g. a private-data destructor via SetPrivateDataInterface, or hooking Release); not attempted without a compiler.
- Bitness: only the layer matching the launcher's bitness is registered. Registering both needs care because both manifests would be on the loader's implicit-layer list; check whether the target loader honours manifest `library_arch` (file_format_version 1.2.1) before doing this.
- If EnsureCaptureObjects fails part-way through creating the 16 semaphores, the ring stays smaller and is not refilled. It still works (smaller ring); low priority.

### Next AI: do exactly this, in order
1. **CI build** (.github/workflows/build.yml, x64 + Win32).
   - Likely error spots: Vulkan header names in vk_layer.cpp, the Hk_CSC* typedefs in dllmain.cpp, RegisterVulkanLayer (new RegEnumValueW block), and the FL_VkLayerPresenting linkage (declared in flcore.h, defined in vk_layer.cpp, used in dllmain.cpp).
   - If Vulkan blocks the build, use -DFL_ENABLE_VULKAN=OFF.
   - Run `dumpbin /exports` on FrameLockHook32.dll and check that vkNegotiateLoaderLayerInterfaceVersion has no decoration.
2. **F10 self-test** per API: D3D9, D3D10, D3D11, D3D12, OpenGL, and Vulkan started via Launch.
   - For Vulkan, also run with VK_LOADER_DEBUG=layer.
   - On NVIDIA, check the log for both stand-down lines and confirm there are no double steps.
   - **New for review3:** also do one Vulkan take on NVIDIA at a high resolution or with a slow output folder, so the encode queue fills and single presents block for > 1 s. Confirm frame indices and clock steps still match 1:1.
   - Move the old FrameLock folder once and relaunch: confirm the stale HKCU ImplicitLayers value is gone and the new one is present.
   - Record pass/fail here.
3. Per-API split into capture_*.cpp files using only flcore.h, following the pattern of vk_layer.cpp.
4. Vulkan hardening: swapchain_maintenance1 present fences, multiple swapchains, more formats, HDR10 tone-mapping.
5. Optional: g_scQueue cleanup; dual-bitness layer registration.

### Do NOT redo
The D3D10 path, the CI workflow, a D3D12 MSAA resolve (impossible), rdtsc interception through an exception handler (impossible), the opus55 null guards in the swapchain/queue intercepts, review2's capOk guard, or this phase's sticky stand-down and stale-registry cleanup. Do not go back to a time-windowed stand-down.

## Phase review4 (NOT compiled, NOT run - static brace/paren balance check only)
- Child-process fix: the hook DLL initialises its core only if Local\FrameLock_<pid> already exists. Children that inherit FRAMELOCK_VK_CAPTURE stay dormant and pass through until FrameLock attaches. The launcher starts games with CREATE_SUSPENDED and creates the mapping before ResumeThread.
- Both Vulkan layer manifests (64 and 32) are registered when both DLLs are next to the launcher. They use file_format_version 1.2.1 and library_arch. Stale cleanup covers both. The launcher warns about a bitness mismatch.
- CI: vkOFF diagnostic leg, dumpbin export and machine checks, combined FrameLock-dist package job.
- Full details and next steps: HANDOFF_REVIEW4.md
