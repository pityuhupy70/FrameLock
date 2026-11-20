// FrameLock hook DLL (v3) - frame-by-frame image sequence exporter.
// Two explicitly separate modes:
//  * LOCKED (offline export): game time is a pure function of the captured frame index:
//    frame n is simulated at exactly n/FPS. The clock can never pass the next frame boundary
//    until the current frame has been copied and read back into memory. Slow rendering /
//    readback / encoding only make the export take longer - never change playback speed.
//  * REALTIME: clock untouched, every frame the game actually presents is saved (no fabrication).
//    Not frame-locked; not for exact-speed export.
// Capture: backbuffer copy right before the original Present (completed frame), read back
// synchronously (D3D11 blocking Map / D3D12 fence wait / D3D9 / GL), encoded on a worker thread
// with a bounded queue (backpressure instead of dropping frames).
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <d3d9.h>
#include <d3d10.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wincodec.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cmath>
#include "MinHook.h"
#include "../shared.h"
#include "flcore.h"     // PF enum + core interface used by separate capture modules (vk_layer.cpp)

static HMODULE   g_hMod = nullptr;
static FLShared* g_sh = nullptr;
static wchar_t   g_logPath[MAX_PATH];
static std::mutex g_logMx;

// ============================================================ logging / status
static void Log(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list a; va_start(a, fmt); _vsnwprintf_s(buf, _TRUNCATE, fmt, a); va_end(a);
    std::lock_guard<std::mutex> l(g_logMx);
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_logPath, L"a, ccs=UTF-8") == 0 && f) {
        SYSTEMTIME st; GetLocalTime(&st);
        fwprintf(f, L"[%02d:%02d:%02d.%03d][T%05lu] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId(), buf);
        fclose(f);
    }
}
static void SetStatus(const wchar_t* s) { if (g_sh) wcsncpy_s(g_sh->status, s, _TRUNCATE); Log(L"%s", s); }
static void SetApi(const wchar_t* s) { if (g_sh && wcscmp(g_sh->api, s) != 0) { wcsncpy_s(g_sh->api, s, _TRUNCATE); Log(L"API: %s", s); } }

// ============================================================ hook helpers
static void* Proc(const wchar_t* mod, const char* fn) {
    HMODULE h = GetModuleHandleW(mod);
    return h ? (void*)GetProcAddress(h, fn) : nullptr;
}
static bool Hook(void* target, void* detour, void** orig, const char* name) {
    if (!target) return false;
    MH_STATUS s = MH_CreateHook(target, detour, orig);
    if (s == MH_ERROR_ALREADY_CREATED) return true;
    if (s != MH_OK) { Log(L"Hook create failed: %S (%d)", name, (int)s); return false; }
    s = MH_EnableHook(target);
    if (s != MH_OK) { Log(L"Hook enable failed: %S (%d)", name, (int)s); return false; }
    Log(L"Hooked %S", name);
    return true;
}

// ============================================================ virtual clock
typedef BOOLEAN(NTAPI* RtlQPC_t)(PLARGE_INTEGER);
typedef BOOL(WINAPI* QPC_t)(LARGE_INTEGER*);
typedef void(WINAPI* QIT_t)(PULONGLONG);
typedef BOOL(WINAPI* QUIT_t)(PULONGLONG);
static RtlQPC_t o_RtlQPC = nullptr;
static QPC_t    o_QPC_kb = nullptr, o_QPC_k32 = nullptr;

static int64_t  g_freq = 10000000;
static int64_t  g_qpc0 = 0;     // real QPC at init
static uint64_t g_tick0 = 0;    // GetTickCount64 at init
static uint32_t g_tgt0 = 0;     // timeGetTime at init
static int64_t  g_ft0 = 0;      // system FILETIME at init
static int64_t  g_it0 = 0;      // interrupt time (100 ns) at init
static int64_t  g_uit0 = 0;     // unbiased interrupt time (100 ns) at init
static int64_t  g_fpsNum = 24, g_fpsDen = 1;

static int64_t MulDiv64(int64_t a, int64_t b, int64_t c) { return (a / c) * b + ((a % c) * b) / c; }
static int64_t RealQPC() {
    LARGE_INTEGER li;
    if (o_RtlQPC) o_RtlQPC(&li);
    else if (o_QPC_kb) o_QPC_kb(&li);
    else if (o_QPC_k32) o_QPC_k32(&li);
    else QueryPerformanceCounter(&li);
    return li.QuadPart;
}
static double RealMs(int64_t ticks) { return ticks * 1000.0 / (double)g_freq; }
// exact tick position of step n: n * freq * den / num (integer math, no drift)
static int64_t FrameTicks(int64_t n) { return MulDiv64(n, g_freq * g_fpsDen, g_fpsNum); }

// LOCKED clock (v3). Simulation step n covers exactly the virtual interval [B(n), B(n+1)] with
//     B(n) = recBase + n * freq * den / num        (pure function of n - no wall-clock time involved)
// While step n is active (frame n-1 captured, frame n not yet captured):
//   * for the first kHold of real time after the previous Present returned the clock reads exactly B(n),
//     so the game's frame-start read gets dt == 1/FPS exactly;
//   * afterwards it may creep forward (so short waits / limiters at >= output FPS can finish), but it is
//     CLAMPED to B(n+1) and can never pass it until frame n has been captured and read back to memory.
// => N captured frames == exactly N/FPS seconds of game time, no matter how slow rendering, GPU readback,
//    JPEG encoding or the disk are. Slow rendering only makes the export take longer.
// v2 had a "free-run" watchdog that released the clock to real time when the game waited at the cap and
// folded that time into the next step (slip). That made captured frames cover more than 1/FPS of game
// time = video played too fast. It is gone. The clock is only released mid-step when the user explicitly
// stops a stalled take; that step is never captured, so output timing is never affected.
struct ClockState { int64_t base, step, anchor, hold; bool released; int64_t releaseAt; };
static SRWLOCK              g_clk = SRWLOCK_INIT;
static ClockState           g_cs = {};
static std::atomic<bool>    g_lock{ false };        // locked clock active
static std::atomic<int64_t> g_offset{ 0 };          // virtual - real while not locked
static int64_t              g_recBase = 0;
static std::atomic<int64_t> g_stepIdx{ 0 };
static std::atomic<bool>    g_clockReleased{ false };

static int64_t HoldTicks() { return g_freq / 50; }   // 20 ms of real time at exactly B(n)
static int64_t ClockEval(const ClockState& c, int64_t real) {
    if (c.released) { int64_t r = real - c.releaseAt; return c.base + c.step + (r > 0 ? r : 0); }
    int64_t e = real - c.anchor; if (e < 0) e = 0;
    if (e <= c.hold) return c.base;
    int64_t add = e - c.hold;
    return c.base + (add < c.step ? add : c.step);   // never beyond the next step boundary
}
static int64_t VirtQPC() {
    if (!g_lock.load(std::memory_order_acquire)) return RealQPC() + g_offset.load(std::memory_order_relaxed);
    AcquireSRWLockShared(&g_clk); ClockState c = g_cs; ReleaseSRWLockShared(&g_clk);
    return ClockEval(c, RealQPC());
}
static int64_t VirtDelta(int64_t unitsPerSec) {
    int64_t d = VirtQPC() - g_qpc0;
    return d >= 0 ? MulDiv64(d, unitsPerSec, g_freq) : -MulDiv64(-d, unitsPerSec, g_freq);
}
static void SetStep(int64_t n, int64_t real) {   // caller holds g_clk exclusively
    g_cs.base = g_recBase + FrameTicks(n);
    g_cs.step = FrameTicks(n + 1) - FrameTicks(n);
    g_cs.anchor = real; g_cs.hold = HoldTicks(); g_cs.released = false; g_cs.releaseAt = 0;
    g_stepIdx = n; g_clockReleased = false;
}
// Step 0 begins when the Present that started the take returns.
static void ClockStart() {
    AcquireSRWLockExclusive(&g_clk);
    int64_t real = RealQPC();
    g_recBase = real + g_offset.load();   // >= every value returned so far -> monotonic
    SetStep(0, real);
    g_lock.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_clk);
    Log(L"CLOCK: locked at step 0 (step = %lld ticks = 1/%.6f s, freq %lld)", (long long)g_cs.step, (double)g_fpsNum / g_fpsDen, (long long)g_freq);
}
// Called only after the Present whose frame was captured AND read back to memory: next exact step.
// B(n+1) depends on n only. Every value handed out during step n was <= B(n+1), so time stays monotonic.
static void ClockAdvance() {
    AcquireSRWLockExclusive(&g_clk);
    SetStep(g_stepIdx + 1, RealQPC());
    ReleaseSRWLockExclusive(&g_clk);
}
// Explicit user stop while the game is stalled inside a step (e.g. an FPS limiter waiting for more than
// 1/FPS): let time run again so the game reaches Present and the take can end. The current step is
// NOT captured (FrameStepPre stops the take first), so no recorded frame is affected.
static void ClockRelease() {
    AcquireSRWLockExclusive(&g_clk);
    if (g_lock.load() && !g_cs.released) {
        g_cs.released = true; g_cs.releaseAt = RealQPC(); g_clockReleased = true;
        Log(L"CLOCK: released on user stop during step %lld (this step will not be captured)", (long long)g_stepIdx.load());
    }
    ReleaseSRWLockExclusive(&g_clk);
}
static void ClockStop() {
    if (!g_lock.load()) return;
    AcquireSRWLockExclusive(&g_clk);
    int64_t real = RealQPC();
    int64_t v = ClockEval(g_cs, real);
    g_offset.store(v - real);           // continuous, monotonic after unlocking
    g_lock.store(false, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_clk);
    Log(L"CLOCK: unlocked after %lld steps (game time advanced exactly %lld steps of 1/FPS)", (long long)g_stepIdx.load(), (long long)g_stepIdx.load());
}

// ---- time detours (none of them call the original except for real QPC reads)
static BOOLEAN NTAPI Hk_RtlQPC(PLARGE_INTEGER p) { if (!p) return FALSE; p->QuadPart = VirtQPC(); return TRUE; }
static BOOL WINAPI Hk_QPC(LARGE_INTEGER* p) { if (!p) return FALSE; p->QuadPart = VirtQPC(); return TRUE; }
static DWORD WINAPI Hk_GetTickCount() { return (DWORD)(g_tick0 + VirtDelta(1000)); }
static ULONGLONG WINAPI Hk_GetTickCount64() { return (ULONGLONG)(g_tick0 + VirtDelta(1000)); }
static DWORD WINAPI Hk_timeGetTime() { return (DWORD)(g_tgt0 + VirtDelta(1000)); }
static void WINAPI Hk_GetSysTimeFT(LPFILETIME ft) {
    if (!ft) return;
    ULARGE_INTEGER u; u.QuadPart = (ULONGLONG)(g_ft0 + VirtDelta(10000000));
    ft->dwLowDateTime = u.LowPart; ft->dwHighDateTime = u.HighPart;
}
static void WINAPI Hk_QIT(PULONGLONG p) { if (p) *p = (ULONGLONG)(g_it0 + VirtDelta(10000000)); }
static void WINAPI Hk_QUITPrecise(PULONGLONG p) { if (p) *p = (ULONGLONG)(g_uit0 + VirtDelta(10000000)); }
static BOOL WINAPI Hk_QUIT(PULONGLONG p) {
    if (!p) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *p = (ULONGLONG)(g_uit0 + VirtDelta(10000000)); return TRUE;
}

static void InstallTimeHooks() {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_freq = f.QuadPart;
    LoadLibraryW(L"winmm.dll");
    // capture baselines with the ORIGINAL functions before hooking
    g_qpc0 = RealQPC();
    g_tick0 = GetTickCount64();
    typedef DWORD(WINAPI* tgt_t)();
    if (auto p = (tgt_t)Proc(L"winmm.dll", "timeGetTime")) g_tgt0 = p();
    typedef void(WINAPI* ft_t)(LPFILETIME);
    FILETIME ft;
    if (auto p = (ft_t)Proc(L"kernel32.dll", "GetSystemTimePreciseAsFileTime")) p(&ft); else GetSystemTimeAsFileTime(&ft);
    g_ft0 = (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
    ULONGLONG t = 0;
    if (auto p = (QIT_t)Proc(L"kernelbase.dll", "QueryInterruptTime")) { p(&t); g_it0 = (int64_t)t; }
    if (auto p = (QUIT_t)Proc(L"kernelbase.dll", "QueryUnbiasedInterruptTime")) { if (p(&t)) g_uit0 = (int64_t)t; }
    else if (auto p2 = (QUIT_t)Proc(L"kernel32.dll", "QueryUnbiasedInterruptTime")) { if (p2(&t)) g_uit0 = (int64_t)t; }

    Hook(Proc(L"ntdll.dll", "RtlQueryPerformanceCounter"), (void*)Hk_RtlQPC, (void**)&o_RtlQPC, "RtlQueryPerformanceCounter");
    Hook(Proc(L"kernelbase.dll", "QueryPerformanceCounter"), (void*)Hk_QPC, (void**)&o_QPC_kb, "kernelbase!QueryPerformanceCounter");
    Hook(Proc(L"kernel32.dll", "QueryPerformanceCounter"), (void*)Hk_QPC, (void**)&o_QPC_k32, "kernel32!QueryPerformanceCounter");
    const wchar_t* mods[] = { L"kernelbase.dll", L"kernel32.dll" };
    for (auto m : mods) {
        Hook(Proc(m, "GetTickCount"), (void*)Hk_GetTickCount, nullptr, "GetTickCount");
        Hook(Proc(m, "GetTickCount64"), (void*)Hk_GetTickCount64, nullptr, "GetTickCount64");
        Hook(Proc(m, "GetSystemTimeAsFileTime"), (void*)Hk_GetSysTimeFT, nullptr, "GetSystemTimeAsFileTime");
        Hook(Proc(m, "GetSystemTimePreciseAsFileTime"), (void*)Hk_GetSysTimeFT, nullptr, "GetSystemTimePreciseAsFileTime");
        Hook(Proc(m, "QueryUnbiasedInterruptTime"), (void*)Hk_QUIT, nullptr, "QueryUnbiasedInterruptTime");
    }
    // Win10+ only (null elsewhere -> Hook() skips)
    Hook(Proc(L"kernelbase.dll", "QueryInterruptTime"), (void*)Hk_QIT, nullptr, "QueryInterruptTime");
    Hook(Proc(L"kernelbase.dll", "QueryInterruptTimePrecise"), (void*)Hk_QIT, nullptr, "QueryInterruptTimePrecise");
    Hook(Proc(L"kernelbase.dll", "QueryUnbiasedInterruptTimePrecise"), (void*)Hk_QUITPrecise, nullptr, "QueryUnbiasedInterruptTimePrecise");
    Hook(Proc(L"winmm.dll", "timeGetTime"), (void*)Hk_timeGetTime, nullptr, "winmm!timeGetTime");
    Hook(Proc(L"winmmbase.dll", "timeGetTime"), (void*)Hk_timeGetTime, nullptr, "winmmbase!timeGetTime");
}

// ============================================================ pixel conversion -> BGR24
struct Frame {
    int w = 0, h = 0; std::vector<uint8_t> bgr;
    void alloc(int W, int H) { w = W; h = H; bgr.resize((size_t)W * H * 3); }
};
// enum class PF lives in flcore.h (shared with vk_layer.cpp)

static uint8_t g_srgbLut[4097];
static void InitLut() {
    for (int i = 0; i <= 4096; i++) {
        double c = i / 4096.0;
        double s = c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
        g_srgbLut[i] = (uint8_t)(s * 255.0 + 0.5);
    }
}
static inline uint8_t LinToSrgb(float v) { if (!(v > 0.f)) return 0; if (v >= 1.f) return 255; return g_srgbLut[(int)(v * 4096.f)]; }
static inline float H2F(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023; float v;
    if (e == 0) v = ldexpf((float)m, -24);
    else if (e == 31) v = m ? 0.f : 65504.f;
    else v = ldexpf((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}
static void Convert(PF pf, const uint8_t* src, size_t pitch, int w, int h, uint8_t* dst, bool flipY = false) {
    for (int y = 0; y < h; y++) {
        const uint8_t* s = src + (size_t)(flipY ? (h - 1 - y) : y) * pitch;
        uint8_t* d = dst + (size_t)y * w * 3;
        if (pf == PF::BGR24) { memcpy(d, s, (size_t)w * 3); continue; }
        for (int x = 0; x < w; x++, d += 3) {
            switch (pf) {
            case PF::BGRA8: d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; s += 4; break;
            case PF::RGBA8: d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; s += 4; break;
            case PF::RGB10A2: { uint32_t v = *(const uint32_t*)s; d[2] = (uint8_t)((v & 1023) >> 2); d[1] = (uint8_t)(((v >> 10) & 1023) >> 2); d[0] = (uint8_t)(((v >> 20) & 1023) >> 2); s += 4; } break;
            case PF::D9_A2R10G10B10: { uint32_t v = *(const uint32_t*)s; d[0] = (uint8_t)((v & 1023) >> 2); d[1] = (uint8_t)(((v >> 10) & 1023) >> 2); d[2] = (uint8_t)(((v >> 20) & 1023) >> 2); s += 4; } break;
            case PF::RGBA16F: { const uint16_t* p = (const uint16_t*)s; d[2] = LinToSrgb(H2F(p[0])); d[1] = LinToSrgb(H2F(p[1])); d[0] = LinToSrgb(H2F(p[2])); s += 8; } break;
            case PF::BGR565: { uint16_t v = *(const uint16_t*)s; d[2] = (uint8_t)(((v >> 11) & 31) * 255 / 31); d[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63); d[0] = (uint8_t)((v & 31) * 255 / 31); s += 2; } break;
            }
        }
    }
}
static bool PFFromDXGI(DXGI_FORMAT f, PF& pf) {
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_TYPELESS: pf = PF::BGRA8; return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS: pf = PF::RGBA8; return true;
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS: pf = PF::RGB10A2; return true;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS: pf = PF::RGBA16F; return true;
    default: return false;
    }
}
static DXGI_FORMAT Typed(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}

// ============================================================ take state + failure reporting
enum class Api { None, D11, D12, D10, D9, GL };
static std::mutex        g_stepMx;
static std::atomic<bool> g_rec{ false };          // a take is active
static bool              g_lockMode = true;        // take mode (locked vs realtime)
static void*             g_target = nullptr;       // swapchain/device/window being recorded
static Api               g_api = Api::None;
static int               g_fails = 0;
static int64_t           g_nextIdx = 0;            // next frame index to capture (== frames queued)
static int64_t           g_testTarget = 0;
static int64_t           g_takeStartReal = 0;
static std::atomic<int>  g_takeId{ 0 };
static std::atomic<int>  g_failedTake{ -1 };       // take that hit an async (post-queue) failure
static std::atomic<bool> g_asyncFail{ false };
static std::wstring      g_takeDir;
static thread_local bool t_inPresent = false;

static void ReportAsyncFailure(int takeId, int64_t idx, const wchar_t* stage, HRESULT hr) {
    g_failedTake = takeId;
    if (g_sh) InterlockedExchange(&g_sh->lastHr, (LONG)hr);
    wchar_t m[512];
    swprintf_s(m, L"ERROR: frame_%06lld failed at stage '%s' (HRESULT 0x%08X). Recording stopped; frames 0..%lld are complete and contiguous.",
        (long long)idx, stage, (unsigned)hr, (long long)idx - 1);
    SetStatus(m);
    g_asyncFail = true;
}
static size_t BppOf(PF pf) { return pf == PF::RGBA16F ? 8 : pf == PF::BGR565 ? 2 : pf == PF::BGR24 ? 3 : 4; }

// ============================================================ encode queue (pixels are already in CPU memory)
// v3: every API reads the frame back SYNCHRONOUSLY inside the Present hook (GPU copy waited, pixels copied to
// RAM) before the clock may advance. Only JPEG encoding + disk writes happen on the worker thread, through a
// bounded queue (backpressure: the render thread waits, frames are never dropped). Waiting is harmless: the
// game clock is held at the step boundary while we wait.
struct Job {
    int takeId = 0; int64_t idx = -1; std::wstring dir;
    int w = 0, h = 0; PF pf = PF::BGR24; size_t pitch = 0; std::vector<uint8_t> raw;     // CPU pixels
    bool end = false; int64_t endCount = 0; int64_t testTarget = 0; bool realtime = false; double wallSec = 0;
    int64_t fpsNum = 24, fpsDen = 1;
};
static const size_t kMaxJobs = 4;     // bounded queue: producer waits (backpressure), never drops
static std::mutex g_jMx; static std::condition_variable g_jCv; static std::deque<Job> g_jobs;

static void Enqueue(Job&& j) {
    std::unique_lock<std::mutex> l(g_jMx);
    if (!j.end && g_jobs.size() >= kMaxJobs) {
        int64_t t0 = RealQPC();
        g_jCv.wait(l, [] { return g_jobs.size() < kMaxJobs; });
        Log(L"Backpressure: render thread waited %.1f ms for the encoder (frame %lld) - game clock held, no time lost", RealMs(RealQPC() - t0), (long long)j.idx);
    }
    g_jobs.push_back(std::move(j));
    l.unlock(); g_jCv.notify_all();
}

static bool EncodeJpeg(IWICImagingFactory* fac, const wchar_t* path, int w, int h, const uint8_t* data, bool v, HRESULT& hr, const wchar_t*& stage) {
    std::wstring tmp = std::wstring(path) + L".tmp";
    IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr; IPropertyBag2* bag = nullptr;
    bool ok = false;
    do {
        stage = L"JPEG CreateStream";      hr = fac->CreateStream(&st); if (FAILED(hr)) break;
        stage = L"JPEG create file";       hr = st->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE); if (FAILED(hr)) break;
        stage = L"JPEG CreateEncoder";     hr = fac->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc); if (FAILED(hr)) break;
        stage = L"JPEG encoder Initialize"; hr = enc->Initialize(st, WICBitmapEncoderNoCache); if (FAILED(hr)) break;
        stage = L"JPEG CreateNewFrame";    hr = enc->CreateNewFrame(&fr, &bag); if (FAILED(hr)) break;
        PROPBAG2 opt = {}; VARIANT var; VariantInit(&var);
        opt.pstrName = (LPOLESTR)L"ImageQuality"; var.vt = VT_R4; var.fltVal = 1.0f; HRESULT h1 = bag->Write(1, &opt, &var);
        opt.pstrName = (LPOLESTR)L"JpegYCrCbSubsampling"; var.vt = VT_UI1; var.bVal = (BYTE)WICJpegYCrCbSubsampling444; HRESULT h2 = bag->Write(1, &opt, &var);
        if (v || FAILED(h1) || FAILED(h2)) Log(L"JPEG options: quality hr=0x%08X, 4:4:4 hr=0x%08X (non-fatal)", (unsigned)h1, (unsigned)h2);
        stage = L"JPEG frame Initialize";  hr = fr->Initialize(bag); if (FAILED(hr)) break;
        stage = L"JPEG SetSize";           hr = fr->SetSize((UINT)w, (UINT)h); if (FAILED(hr)) break;
        stage = L"JPEG SetPixelFormat";
        WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR; hr = fr->SetPixelFormat(&pf); if (FAILED(hr)) break;
        if (pf != GUID_WICPixelFormat24bppBGR) { hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT; break; }
        UINT stride = (UINT)w * 3;
        stage = L"JPEG WritePixels";       hr = fr->WritePixels((UINT)h, stride, stride * (UINT)h, (BYTE*)data); if (FAILED(hr)) break;
        stage = L"JPEG frame Commit";      hr = fr->Commit(); if (FAILED(hr)) break;
        stage = L"JPEG encoder Commit";    hr = enc->Commit(); if (FAILED(hr)) break;
        ok = true;
    } while (0);
    if (bag) bag->Release(); if (fr) fr->Release(); if (enc) enc->Release(); if (st) st->Release();   // closes the file
    if (ok) {
        stage = L"rename .tmp -> .jpg";
        if (!MoveFileExW(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING)) { hr = HRESULT_FROM_WIN32(GetLastError()); ok = false; }
    }
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

static uint64_t Fnv1a(const uint8_t* p, size_t n) { uint64_t x = 1469598103934665603ull; for (size_t i = 0; i < n; i++) { x ^= p[i]; x *= 1099511628211ull; } return x; }

// Self-test verification: file count, sequential names, sizes, decodability, repeated hashes.
static void VerifyTake(IWICImagingFactory* fac, const std::wstring& dir, int64_t expect) {
    int64_t missing = 0, empty = 0, undecodable = 0, adjRepeat = 0, dupAny = 0, files = 0, tmpLeft = 0;
    std::unordered_set<uint64_t> seen; uint64_t prev = 0; std::vector<uint8_t> buf;
    for (int64_t i = 0; i < expect; i++) {
        wchar_t p[MAX_PATH + 32]; swprintf_s(p, L"%s\\frame_%06lld.jpg", dir.c_str(), (long long)i);
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExW(p, GetFileExInfoStandard, &fa)) { missing++; Log(L"TEST: missing %s", p); continue; }
        if (fa.nFileSizeLow == 0 && fa.nFileSizeHigh == 0) { empty++; Log(L"TEST: zero-size %s", p); continue; }
        IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fd = nullptr; IWICFormatConverter* cv = nullptr;
        HRESULT hr = fac ? fac->CreateDecoderFromFilename(p, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec) : E_POINTER;
        UINT w = 0, h = 0;
        if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &fd);
        if (SUCCEEDED(hr)) hr = fd->GetSize(&w, &h);
        if (SUCCEEDED(hr)) hr = fac->CreateFormatConverter(&cv);
        if (SUCCEEDED(hr)) hr = cv->Initialize(fd, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        if (SUCCEEDED(hr)) { buf.resize((size_t)w * h * 3); hr = cv->CopyPixels(nullptr, w * 3, (UINT)buf.size(), buf.data()); }
        if (cv) cv->Release(); if (fd) fd->Release(); if (dec) dec->Release();
        if (FAILED(hr)) { undecodable++; Log(L"TEST: cannot decode %s hr=0x%08X", p, (unsigned)hr); continue; }
        uint64_t hs = Fnv1a(buf.data(), buf.size());
        if (i > 0 && hs == prev) { adjRepeat++; Log(L"TEST: frame_%06lld identical to previous frame", (long long)i); }
        if (!seen.insert(hs).second) dupAny++;
        prev = hs;
    }
    WIN32_FIND_DATAW fd; HANDLE fh = FindFirstFileW((dir + L"\\frame_*.jpg").c_str(), &fd);
    if (fh != INVALID_HANDLE_VALUE) { do files++; while (FindNextFileW(fh, &fd)); FindClose(fh); }
    fh = FindFirstFileW((dir + L"\\*.tmp").c_str(), &fd);
    if (fh != INVALID_HANDLE_VALUE) { do tmpLeft++; while (FindNextFileW(fh, &fd)); FindClose(fh); }
    bool filesOk = missing == 0 && empty == 0 && undecodable == 0 && files == expect && tmpLeft == 0;
    bool pass = filesOk && adjRepeat == 0 && dupAny == 0;
    wchar_t m[512];
    swprintf_s(m, L"TEST %s: %lld/%lld files (frame_000000..frame_%06lld), missing %lld, zero-size %lld, undecodable %lld, leftover .tmp %lld, "
        L"repeated adjacent hashes %lld, duplicate hashes %lld%s",
        pass ? L"PASS" : L"FAIL", (long long)files, (long long)expect, (long long)expect - 1, (long long)missing, (long long)empty,
        (long long)undecodable, (long long)tmpLeft, (long long)adjRepeat, (long long)dupAny,
        (filesOk && !pass) ? L" (files OK; repeats mean a static/paused scene or a frame that did not change)" : L"");
    SetStatus(m);
    FILE* f = nullptr;
    if (_wfopen_s(&f, (dir + L"\\test_report.txt").c_str(), L"w, ccs=UTF-8") == 0 && f) { fwprintf(f, L"%s\n", m); fclose(f); }
}

// Writes take_info.txt + make_video.bat so the sequence is always encoded at exactly the recorded FPS
// (ffmpeg's image2 default is 25 fps - encoding a 60 fps take with the default would also look wrong).
static void WriteTakeInfo(const Job& j) {
    FILE* f = nullptr;
    double fps = (double)j.fpsNum / (double)j.fpsDen;
    if (_wfopen_s(&f, (j.dir + L"\\take_info.txt").c_str(), L"w, ccs=UTF-8") == 0 && f) {
        if (j.realtime)
            fwprintf(f, L"mode=REALTIME (not frame-locked)\nframes=%lld\nwall_seconds=%.3f\nmeasured_fps=%.3f\n"
                L"note=Real-time takes contain only the frames the game presented. They are NOT %.3f fps; use Locked mode for exact-speed export.\n",
                (long long)j.endCount, j.wallSec, j.wallSec > 0 ? j.endCount / j.wallSec : 0.0, fps);
        else
            fwprintf(f, L"mode=LOCKED (1 simulation step of exactly 1/FPS per captured frame)\nframes=%lld\nfps=%lld/%lld\nfps_decimal=%.6f\n"
                L"game_seconds=%.6f\nvideo_seconds_at_fps=%.6f\nfirst=frame_000000.jpg\nlast=frame_%06lld.jpg\n",
                (long long)j.endCount, (long long)j.fpsNum, (long long)j.fpsDen, fps,
                j.endCount / fps, j.endCount / fps, (long long)(j.endCount > 0 ? j.endCount - 1 : 0));
        fclose(f);
    }
    if (j.realtime || j.endCount <= 0) return;
    if (_wfopen_s(&f, (j.dir + L"\\make_video.bat").c_str(), L"w") == 0 && f) {
        // -framerate N/D on the input = every image is exactly one frame of 1/FPS; output keeps the same rate (no dup/drop)
        fwprintf(f, L"@echo off\r\ncd /d \"%%~dp0\"\r\n"
            L"ffmpeg -y -framerate %lld/%lld -start_number 0 -i frame_%%%%06d.jpg -c:v libx264 -crf 12 -preset slow -pix_fmt yuv420p -r %lld/%lld output.mp4\r\n"
            L"echo Expected duration: %.3f s (%lld frames at %lld/%lld fps)\r\npause\r\n",
            (long long)j.fpsNum, (long long)j.fpsDen, (long long)j.fpsNum, (long long)j.fpsDen,
            j.endCount / fps, (long long)j.endCount, (long long)j.fpsNum, (long long)j.fpsDen);
        fclose(f);
    }
}

static DWORD WINAPI WorkerThread(LPVOID) {
    HRESULT hc = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* fac = nullptr;
    HRESULT hf = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac));
    Log(L"Encoder thread: CoInitializeEx hr=0x%08X, WIC factory hr=0x%08X", (unsigned)hc, (unsigned)hf);
    std::vector<uint8_t> bgr;
    for (;;) {
        Job j;
        { std::unique_lock<std::mutex> l(g_jMx); g_jCv.wait(l, [] { return !g_jobs.empty(); }); j = std::move(g_jobs.front()); g_jobs.pop_front(); }
        g_jCv.notify_all();
        if (j.end) {
            if (g_failedTake != j.takeId) {
                wchar_t m[800];
                double fps = (double)j.fpsNum / (double)j.fpsDen;
                if (j.realtime) swprintf_s(m, L"Finished (real-time, NOT frame-locked): %lld frames actually presented by the game in %.2f s (%.2f fps measured) -> %s",
                    (long long)j.endCount, j.wallSec, j.wallSec > 0 ? j.endCount / j.wallSec : 0.0, j.dir.c_str());
                else swprintf_s(m, L"Finished (locked): %lld unique frames = %lld steps of exactly 1/%.3f s = %.3f s of game time = %.3f s of video at %lld/%lld fps "
                    L"(rendering took %.1f s real time). Encode with make_video.bat in %s",
                    (long long)j.endCount, (long long)j.endCount, fps, j.endCount / fps, j.endCount / fps,
                    (long long)j.fpsNum, (long long)j.fpsDen, j.wallSec, j.dir.c_str());
                SetStatus(m);
                WriteTakeInfo(j);
            }
            if (j.testTarget > 0) VerifyTake(fac, j.dir, j.testTarget);
            continue;
        }
        if (j.takeId == g_failedTake) continue;   // keep output contiguous after a failure
        wchar_t path[MAX_PATH + 32]; swprintf_s(path, L"%s\\frame_%06lld.jpg", j.dir.c_str(), (long long)j.idx);
        bool v = j.idx < 2;
        const wchar_t* stage = L"WIC factory"; HRESULT e = hf; bool ok = false;
        if (fac) {
            stage = L"pixel conversion";
            bgr.resize((size_t)j.w * j.h * 3);
            Convert(j.pf, j.raw.data(), j.pitch, j.w, j.h, bgr.data());
            if (v) Log(L"frame %lld: converted %dx%d (pf %d) -> BGR24", (long long)j.idx, j.w, j.h, (int)j.pf);
            ok = EncodeJpeg(fac, path, j.w, j.h, bgr.data(), v, e, stage);
        }
        if (ok) {
            if (g_sh) { InterlockedExchange64(&g_sh->framesSaved, j.idx + 1); g_sh->width = j.w; g_sh->height = j.h; }
            if (v || j.idx % 100 == 0) Log(L"Saved %s", path);
        } else {
            Log(L"FAIL frame %lld path %s", (long long)j.idx, path);
            ReportAsyncFailure(j.takeId, j.idx, stage, e);
        }
    }
}

// ============================================================ Direct3D 11 capture (synchronous: copy + blocking Map)
static struct { ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; ID3D11Texture2D* staging = nullptr; ID3D11Texture2D* resolved = nullptr;
                D3D11_TEXTURE2D_DESC desc = {}; PF pf = PF::BGRA8; } g11;

static void D11ReleaseTex() {
    if (g11.staging) { g11.staging->Release(); g11.staging = nullptr; }
    if (g11.resolved) { g11.resolved->Release(); g11.resolved = nullptr; }
    g11.desc = {};
}
// The frame counts as captured only when its pixels are in CPU memory: CopyResource, then a BLOCKING Map
// (waits for the GPU copy). On failure nothing is queued and the clock is not advanced -> the same
// simulation step is rendered and captured again on the next Present (no skip, no duplicate).
static bool SubmitD3D11(IDXGISwapChain* sc, ID3D11Device* dev, int64_t idx, bool v, const wchar_t*& stage, HRESULT& hr) {
    ID3D11Texture2D* bb = nullptr; bool ok = false;
    do {
        if (g11.dev != dev) {
            D11ReleaseTex();
            if (g11.ctx) g11.ctx->Release(); if (g11.dev) g11.dev->Release();
            g11.ctx = nullptr; dev->AddRef(); g11.dev = dev; dev->GetImmediateContext(&g11.ctx);
            Log(L"[D3D11] device %p, feature level 0x%X", dev, (unsigned)dev->GetFeatureLevel());
        }
        stage = L"D3D11 SwapChain::GetBuffer(0)"; hr = sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb); if (FAILED(hr)) break;
        D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
        if (v) {
            DXGI_SWAP_CHAIN_DESC sd = {}; HRESULT hs = sc->GetDesc(&sd);
            Log(L"[D3D11] frame %lld: swapchain GetDesc hr=0x%08X %ux%u fmt %d buffers %u swapEffect %d | backbuffer(0) %ux%u fmt %d samples %u",
                (long long)idx, (unsigned)hs, sd.BufferDesc.Width, sd.BufferDesc.Height, (int)sd.BufferDesc.Format, sd.BufferCount, (int)sd.SwapEffect,
                d.Width, d.Height, (int)d.Format, d.SampleDesc.Count);
        }
        PF pf; stage = L"D3D11 back buffer format"; if (!PFFromDXGI(d.Format, pf)) { hr = E_NOTIMPL; break; }
        if (d.Width != g11.desc.Width || d.Height != g11.desc.Height || d.Format != g11.desc.Format || d.SampleDesc.Count != g11.desc.SampleDesc.Count) {
            D11ReleaseTex(); g11.desc = d; g11.pf = pf;
        }
        DXGI_FORMAT copyFmt = d.SampleDesc.Count > 1 ? Typed(d.Format) : d.Format;
        if (!g11.staging) {
            D3D11_TEXTURE2D_DESC sd = d; sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0; sd.Usage = D3D11_USAGE_STAGING; sd.Format = copyFmt;
            sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0; sd.MipLevels = 1; sd.ArraySize = 1;
            stage = L"D3D11 CreateTexture2D(staging)"; hr = dev->CreateTexture2D(&sd, nullptr, &g11.staging); if (FAILED(hr)) break;
        }
        ID3D11Texture2D* src = bb;
        if (d.SampleDesc.Count > 1) {
            if (!g11.resolved) {
                D3D11_TEXTURE2D_DESC rd = d; rd.SampleDesc.Count = 1; rd.SampleDesc.Quality = 0; rd.Format = copyFmt;
                rd.Usage = D3D11_USAGE_DEFAULT; rd.BindFlags = 0; rd.CPUAccessFlags = 0; rd.MiscFlags = 0; rd.MipLevels = 1; rd.ArraySize = 1;
                stage = L"D3D11 CreateTexture2D(MSAA resolve)"; hr = dev->CreateTexture2D(&rd, nullptr, &g11.resolved); if (FAILED(hr)) break;
            }
            g11.ctx->ResolveSubresource(g11.resolved, 0, bb, 0, copyFmt); src = g11.resolved;
        }
        g11.ctx->CopyResource(g11.staging, src);
        int64_t t0 = RealQPC();
        D3D11_MAPPED_SUBRESOURCE m = {};
        stage = L"D3D11 staging Map (wait for GPU copy)"; hr = g11.ctx->Map(g11.staging, 0, D3D11_MAP_READ, 0, &m);
        if (FAILED(hr)) { Log(L"[D3D11] staging Map failed hr=0x%08X, device removed reason 0x%08X", (unsigned)hr, (unsigned)dev->GetDeviceRemovedReason()); break; }
        Job j; j.takeId = g_takeId; j.idx = idx; j.dir = g_takeDir; j.w = (int)d.Width; j.h = (int)d.Height; j.pf = pf;
        j.pitch = (size_t)j.w * BppOf(pf); j.raw.resize(j.pitch * j.h);
        for (int y = 0; y < j.h; y++) memcpy(j.raw.data() + (size_t)y * j.pitch, (const uint8_t*)m.pData + (size_t)y * m.RowPitch, j.pitch);
        g11.ctx->Unmap(g11.staging, 0);
        if (v) Log(L"[D3D11] frame %lld: copied + read back in %.2f ms (rowPitch %u)", (long long)idx, RealMs(RealQPC() - t0), m.RowPitch);
        Enqueue(std::move(j));
        ok = true;
    } while (0);
    if (bb) bb->Release();
    return ok;
}

// ============================================================ Direct3D 12 capture (synchronous: copy + fence wait + Map)
static std::mutex g_qMx;
static ID3D12CommandQueue* g_lastQueue = nullptr;
// phase opus55: swapchain -> present queue, recorded when the swapchain is created (IDXGIFactory*::CreateSwapChain*:
// for D3D12 the pDevice argument IS the command queue the swapchain presents on). Keyed by the swapchain's COM
// identity (IUnknown*). Holds one reference on each queue. Guarded by g_qMx. Only swapchains created after the
// factory hooks were installed are known; otherwise SubmitD3D12 falls back as before.
static std::unordered_map<void*, ID3D12CommandQueue*> g_scQueue;
static void* ComIdentity(IUnknown* o) {
    IUnknown* u = nullptr;
    if (!o || FAILED(o->QueryInterface(IID_PPV_ARGS(&u))) || !u) return o;
    u->Release(); return u;          // pointer value only used as a key; the swapchain keeps itself alive
}
static void RememberSwapchainQueue(IUnknown* devOrQueue, IUnknown* sc) {
    if (!devOrQueue || !sc) return;
    ID3D12CommandQueue* q = nullptr;
    if (FAILED(devOrQueue->QueryInterface(IID_PPV_ARGS(&q))) || !q) return;   // D3D10/11 device: nothing to record
    void* key = ComIdentity(sc);
    {
        std::lock_guard<std::mutex> l(g_qMx);
        auto it = g_scQueue.find(key);
        if (it != g_scQueue.end()) { if (it->second && it->second != q) it->second->Release(); else if (it->second == q) q->Release(); it->second = q; }
        else g_scQueue.emplace(key, q);  // keeps the QueryInterface reference
    }
    Log(L"[D3D12] swapchain %p created on command queue %p (recorded as its present queue)", key, q);
}
static ID3D12CommandQueue* LookupSwapchainQueue(IUnknown* sc) {   // returns an AddRef'd queue or nullptr
    void* key = ComIdentity(sc);
    std::lock_guard<std::mutex> l(g_qMx);
    auto it = g_scQueue.find(key);
    if (it == g_scQueue.end() || !it->second) return nullptr;
    it->second->AddRef(); return it->second;
}
static struct { ID3D12Device* dev = nullptr; ID3D12Fence* fence = nullptr; UINT64 fv = 0; HANDLE ev = nullptr;
                ID3D12Resource* rb = nullptr; ID3D12CommandAllocator* al = nullptr; ID3D12GraphicsCommandList* cl = nullptr; UINT64 size = 0; } g12;

static void D12Retire() { g12.rb = nullptr; g12.al = nullptr; g12.cl = nullptr; g12.size = 0; }   // GPU may still use them: leak, never reuse
static HRESULT D12Ensure(ID3D12Device* dev, const wchar_t*& stage) {
    if (!g12.ev) { stage = L"CreateEvent"; g12.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr); if (!g12.ev) return HRESULT_FROM_WIN32(GetLastError()); }
    if (g12.dev == dev) return S_OK;
    // every previous copy was waited for synchronously, so the old objects are idle
    if (g12.cl) g12.cl->Release(); if (g12.al) g12.al->Release(); if (g12.rb) g12.rb->Release(); D12Retire();
    if (g12.fence) g12.fence->Release(); g12.fence = nullptr;
    if (g12.dev) g12.dev->Release(); g12.dev = nullptr;
    stage = L"D3D12 CreateFence";
    HRESULT hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g12.fence)); if (FAILED(hr)) return hr;
    dev->AddRef(); g12.dev = dev; g12.fv = 0;
    Log(L"[D3D12] capture context created for device %p", dev);
    return S_OK;
}
static bool SubmitD3D12(IDXGISwapChain* sc, int64_t idx, bool v, const wchar_t*& stage, HRESULT& hr) {
    IDXGISwapChain3* sc3 = nullptr; ID3D12CommandQueue* q = nullptr; ID3D12Device* dev = nullptr; ID3D12Resource* bb = nullptr;
    bool open = false, ok = false;
    do {
        stage = L"QueryInterface(IDXGISwapChain3)"; hr = sc->QueryInterface(IID_PPV_ARGS(&sc3)); if (FAILED(hr)) break;
        // The swapchain's own queue: the copy is then ordered after the game's frame and before Present.
        stage = L"SwapChain::GetDevice(ID3D12CommandQueue)"; hr = sc->GetDevice(IID_PPV_ARGS(&q));
        if ((FAILED(hr) || !q) && (q = LookupSwapchainQueue(sc)) != nullptr) {
            if (v) Log(L"[D3D12] GetDevice(queue) hr=0x%08X; using the queue %p recorded when this swapchain was created", (unsigned)hr, q);
            hr = S_OK;
        }
        if (FAILED(hr) || !q) {
            HRESULT h0 = hr;
            { std::lock_guard<std::mutex> l(g_qMx); q = g_lastQueue; if (q) q->AddRef(); }
            if (!q) { stage = L"find D3D12 queue (not D3D11/D3D12 swapchain?)"; if (SUCCEEDED(hr)) hr = E_NOINTERFACE; break; }
            Log(L"[D3D12] WARNING: GetDevice(queue) hr=0x%08X and swapchain not seen at creation (attached late?); falling back to last DIRECT queue %p seen in ExecuteCommandLists (may not be the present queue)", (unsigned)h0, q);
            hr = S_OK;
        }
        D3D12_COMMAND_QUEUE_DESC qd = q->GetDesc();
        stage = L"D3D12 queue type"; if (qd.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) { hr = E_INVALIDARG; break; }
        stage = L"CommandQueue::GetDevice"; hr = q->GetDevice(IID_PPV_ARGS(&dev)); if (FAILED(hr)) break;
        hr = D12Ensure(dev, stage); if (FAILED(hr)) break;
        UINT bi = sc3->GetCurrentBackBufferIndex();       // buffer about to be presented
        stage = L"SwapChain3::GetBuffer(current)"; hr = sc3->GetBuffer(bi, IID_PPV_ARGS(&bb)); if (FAILED(hr)) break;
        D3D12_RESOURCE_DESC rd = bb->GetDesc();
        if (v) {
            DXGI_SWAP_CHAIN_DESC sd = {}; HRESULT hs = sc->GetDesc(&sd);
            Log(L"[D3D12] frame %lld: device %p queue %p | swapchain GetDesc hr=0x%08X %ux%u fmt %d buffers %u | back buffer index %u: %llux%u fmt %d samples %u | state assumed PRESENT (required at Present)",
                (long long)idx, dev, q, (unsigned)hs, sd.BufferDesc.Width, sd.BufferDesc.Height, (int)sd.BufferDesc.Format, sd.BufferCount,
                bi, (unsigned long long)rd.Width, rd.Height, (int)rd.Format, rd.SampleDesc.Count);
        }
        PF pf; stage = L"D3D12 back buffer format"; if (!PFFromDXGI(rd.Format, pf)) { hr = E_NOTIMPL; break; }
        // D3D12 only supports flip-model swapchains, and DXGI requires SampleDesc.Count == 1 for flip model, so a
        // multisampled D3D12 back buffer cannot be created; engines resolve MSAA themselves before Present.
        // This check therefore only guards against an impossible/invalid state - no resolve path is needed.
        stage = L"D3D12 MSAA back buffer (invalid for flip-model swapchains)"; if (rd.SampleDesc.Count > 1) { hr = E_NOTIMPL; break; }
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {}; UINT64 total = 0;
        dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
        if (g12.size < total) {
            if (g12.rb) { g12.rb->Release(); g12.rb = nullptr; } g12.size = 0;
            D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1;
            bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            stage = L"D3D12 CreateCommittedResource(READBACK)";
            hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g12.rb)); if (FAILED(hr)) break;
            g12.size = total;
        }
        if (!g12.al) {
            stage = L"D3D12 CreateCommandAllocator"; hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g12.al)); if (FAILED(hr)) break;
            stage = L"D3D12 CreateCommandList"; hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g12.al, nullptr, IID_PPV_ARGS(&g12.cl)); if (FAILED(hr)) break;
            g12.cl->Close();
        }
        // The previous copy was fully waited for, so resetting the allocator is safe.
        stage = L"D3D12 CommandAllocator::Reset"; hr = g12.al->Reset(); if (FAILED(hr)) break;
        stage = L"D3D12 CommandList::Reset"; hr = g12.cl->Reset(g12.al, nullptr); if (FAILED(hr)) break;
        open = true;
        D3D12_RESOURCE_BARRIER b = {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = bb; b.Transition.Subresource = 0;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g12.cl->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = g12.rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION srcl = {}; srcl.pResource = bb; srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; srcl.SubresourceIndex = 0;
        g12.cl->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;   // restore for Present
        g12.cl->ResourceBarrier(1, &b);
        stage = L"D3D12 CommandList::Close"; hr = g12.cl->Close(); open = false; if (FAILED(hr)) break;
        ID3D12CommandList* lists[] = { g12.cl };
        q->ExecuteCommandLists(1, lists);
        UINT64 val = ++g12.fv;
        int64_t t0 = RealQPC();
        stage = L"D3D12 CommandQueue::Signal"; hr = q->Signal(g12.fence, val);
        if (FAILED(hr)) { Log(L"[D3D12] Signal failed: readback objects retired (GPU may still use them)"); D12Retire(); break; }
        // Wait right here for the GPU copy: the frame must be in memory before the game may simulate the next step.
        stage = L"D3D12 copy fence wait";
        if (g12.fence->GetCompletedValue() < val) {
            hr = g12.fence->SetEventOnCompletion(val, g12.ev);
            if (FAILED(hr)) { stage = L"D3D12 SetEventOnCompletion"; D12Retire(); break; }
            if (WaitForSingleObject(g12.ev, 10000) != WAIT_OBJECT_0) {
                hr = dev->GetDeviceRemovedReason(); if (SUCCEEDED(hr)) hr = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
                Log(L"[D3D12] fence timeout: completed=%llu wanted=%llu", g12.fence->GetCompletedValue(), val);
                D12Retire(); break;
            }
        }
        stage = L"D3D12 readback Map";
        D3D12_RANGE r = { (SIZE_T)fp.Offset, (SIZE_T)total }; void* mapped = nullptr;
        hr = g12.rb->Map(0, &r, &mapped); if (FAILED(hr)) break;
        Job j; j.takeId = g_takeId; j.idx = idx; j.dir = g_takeDir; j.w = (int)rd.Width; j.h = (int)rd.Height; j.pf = pf;
        j.pitch = (size_t)j.w * BppOf(pf); j.raw.resize(j.pitch * j.h);
        const uint8_t* base = (const uint8_t*)mapped + fp.Offset;
        for (int y = 0; y < j.h; y++) memcpy(j.raw.data() + (size_t)y * j.pitch, base + (size_t)y * fp.Footprint.RowPitch, j.pitch);
        D3D12_RANGE wr = { 0, 0 }; g12.rb->Unmap(0, &wr);
        if (v) Log(L"[D3D12] frame %lld: barrier PRESENT->COPY_SOURCE->PRESENT, CopyTextureRegion %ux%u rowPitch %u, fence %llu, read back in %.2f ms",
            (long long)idx, fp.Footprint.Width, fp.Footprint.Height, fp.Footprint.RowPitch, val, RealMs(RealQPC() - t0));
        Enqueue(std::move(j));
        ok = true;
    } while (0);
    if (open && g12.cl) g12.cl->Close();
    if (bb) bb->Release(); if (dev) dev->Release(); if (q) q->Release(); if (sc3) sc3->Release();
    return ok;
}

// ============================================================ Direct3D 10 capture (synchronous: copy + blocking Map)
// D3D10 shares the DXGI IDXGISwapChain::Present hook with D3D11/D3D12, so the Present slot was already hooked;
// only this device-specific readback path was missing. It mirrors the D3D11 path (the D3D10 device is also its
// own immediate context, so CopyResource/ResolveSubresource/Map are called directly on the device/texture).
static struct { ID3D10Device* dev = nullptr; ID3D10Texture2D* staging = nullptr; ID3D10Texture2D* resolved = nullptr;
                D3D10_TEXTURE2D_DESC desc = {}; PF pf = PF::BGRA8; } g10;

static void D10ReleaseTex() {
    if (g10.staging) { g10.staging->Release(); g10.staging = nullptr; }
    if (g10.resolved) { g10.resolved->Release(); g10.resolved = nullptr; }
    g10.desc = {};
}
static bool SubmitD3D10(IDXGISwapChain* sc, ID3D10Device* dev, int64_t idx, bool v, const wchar_t*& stage, HRESULT& hr) {
    ID3D10Texture2D* bb = nullptr; bool ok = false;
    do {
        if (g10.dev != dev) {
            D10ReleaseTex();
            if (g10.dev) g10.dev->Release();
            dev->AddRef(); g10.dev = dev;
            Log(L"[D3D10] device %p", dev);
        }
        stage = L"D3D10 SwapChain::GetBuffer(0)"; hr = sc->GetBuffer(0, __uuidof(ID3D10Texture2D), (void**)&bb); if (FAILED(hr)) break;
        D3D10_TEXTURE2D_DESC d; bb->GetDesc(&d);
        if (v) {
            DXGI_SWAP_CHAIN_DESC sd = {}; HRESULT hs = sc->GetDesc(&sd);
            Log(L"[D3D10] frame %lld: swapchain GetDesc hr=0x%08X %ux%u fmt %d buffers %u | backbuffer(0) %ux%u fmt %d samples %u",
                (long long)idx, (unsigned)hs, sd.BufferDesc.Width, sd.BufferDesc.Height, (int)sd.BufferDesc.Format, sd.BufferCount,
                d.Width, d.Height, (int)d.Format, d.SampleDesc.Count);
        }
        PF pf; stage = L"D3D10 back buffer format"; if (!PFFromDXGI(d.Format, pf)) { hr = E_NOTIMPL; break; }
        if (d.Width != g10.desc.Width || d.Height != g10.desc.Height || d.Format != g10.desc.Format || d.SampleDesc.Count != g10.desc.SampleDesc.Count) {
            D10ReleaseTex(); g10.desc = d; g10.pf = pf;
        }
        DXGI_FORMAT copyFmt = d.SampleDesc.Count > 1 ? Typed(d.Format) : d.Format;
        if (!g10.staging) {
            D3D10_TEXTURE2D_DESC sd = d; sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0; sd.Usage = D3D10_USAGE_STAGING; sd.Format = copyFmt;
            sd.BindFlags = 0; sd.CPUAccessFlags = D3D10_CPU_ACCESS_READ; sd.MiscFlags = 0; sd.MipLevels = 1; sd.ArraySize = 1;
            stage = L"D3D10 CreateTexture2D(staging)"; hr = dev->CreateTexture2D(&sd, nullptr, &g10.staging); if (FAILED(hr)) break;
        }
        ID3D10Texture2D* src = bb;
        if (d.SampleDesc.Count > 1) {
            if (!g10.resolved) {
                D3D10_TEXTURE2D_DESC rd = d; rd.SampleDesc.Count = 1; rd.SampleDesc.Quality = 0; rd.Format = copyFmt;
                rd.Usage = D3D10_USAGE_DEFAULT; rd.BindFlags = 0; rd.CPUAccessFlags = 0; rd.MiscFlags = 0; rd.MipLevels = 1; rd.ArraySize = 1;
                stage = L"D3D10 CreateTexture2D(MSAA resolve)"; hr = dev->CreateTexture2D(&rd, nullptr, &g10.resolved); if (FAILED(hr)) break;
            }
            dev->ResolveSubresource(g10.resolved, 0, bb, 0, copyFmt); src = g10.resolved;
        }
        dev->CopyResource(g10.staging, src);
        int64_t t0 = RealQPC();
        D3D10_MAPPED_TEXTURE2D m = {};
        stage = L"D3D10 staging Map (wait for GPU copy)"; hr = g10.staging->Map(0, D3D10_MAP_READ, 0, &m);
        if (FAILED(hr)) break;
        Job j; j.takeId = g_takeId; j.idx = idx; j.dir = g_takeDir; j.w = (int)d.Width; j.h = (int)d.Height; j.pf = pf;
        j.pitch = (size_t)j.w * BppOf(pf); j.raw.resize(j.pitch * j.h);
        for (int y = 0; y < j.h; y++) memcpy(j.raw.data() + (size_t)y * j.pitch, (const uint8_t*)m.pData + (size_t)y * m.RowPitch, j.pitch);
        g10.staging->Unmap(0);
        if (v) Log(L"[D3D10] frame %lld: copied + read back in %.2f ms (rowPitch %u)", (long long)idx, RealMs(RealQPC() - t0), m.RowPitch);
        Enqueue(std::move(j));
        ok = true;
    } while (0);
    if (bb) bb->Release();
    return ok;
}

static bool SubmitDXGI(IDXGISwapChain* sc, int64_t idx, bool v, const wchar_t*& stage, HRESULT& hr) {
    ID3D11Device* d11 = nullptr;
    hr = sc->GetDevice(__uuidof(ID3D11Device), (void**)&d11);
    if (SUCCEEDED(hr) && d11) { SetApi(L"Direct3D 11"); g_api = Api::D11; bool r = SubmitD3D11(sc, d11, idx, v, stage, hr); d11->Release(); return r; }
    if (v) Log(L"SwapChain::GetDevice(ID3D11Device) hr=0x%08X -> trying D3D10", (unsigned)hr);
    ID3D10Device* d10 = nullptr;
    hr = sc->GetDevice(__uuidof(ID3D10Device), (void**)&d10);
    if (SUCCEEDED(hr) && d10) { SetApi(L"Direct3D 10"); g_api = Api::D10; bool r = SubmitD3D10(sc, d10, idx, v, stage, hr); d10->Release(); return r; }
    if (v) Log(L"SwapChain::GetDevice(ID3D10Device) hr=0x%08X -> trying D3D12", (unsigned)hr);
    SetApi(L"Direct3D 12"); g_api = Api::D12;
    return SubmitD3D12(sc, idx, v, stage, hr);
}
// D3D9 / OpenGL: synchronous GPU readback, encoding on the worker.
static bool QueueCpuFrame(Frame& f, int64_t idx) {
    if (f.w <= 0 || f.h <= 0) return false;
    Job j; j.takeId = g_takeId; j.idx = idx; j.dir = g_takeDir; j.w = f.w; j.h = f.h; j.pf = PF::BGR24; j.pitch = (size_t)f.w * 3; j.raw = std::move(f.bgr);
    Enqueue(std::move(j));
    return true;
}

// ============================================================ Direct3D 9 / OpenGL capture (synchronous readback)
static bool CaptureD9Surface(IDirect3DDevice9* dev, IDirect3DSurface9* bb, Frame& fr) {
    D3DSURFACE_DESC d; if (FAILED(bb->GetDesc(&d))) return false;
    PF pf;
    switch (d.Format) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: pf = PF::BGRA8; break;
    case D3DFMT_A2R10G10B10: pf = PF::D9_A2R10G10B10; break;
    case D3DFMT_R5G6B5: pf = PF::BGR565; break;
    default: SetStatus(L"Unsupported back buffer format (D3D9)"); return false;
    }
    IDirect3DSurface9* src = bb; IDirect3DSurface9* rt = nullptr; IDirect3DSurface9* sys = nullptr; bool ok = false;
    do {
        if (d.MultiSampleType != D3DMULTISAMPLE_NONE) {
            if (FAILED(dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr))) break;
            if (FAILED(dev->StretchRect(bb, nullptr, rt, nullptr, D3DTEXF_NONE))) break;
            src = rt;
        }
        if (FAILED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr))) break;
        if (FAILED(dev->GetRenderTargetData(src, sys))) break;
        D3DLOCKED_RECT lr; if (FAILED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) break;
        fr.alloc((int)d.Width, (int)d.Height);
        Convert(pf, (const uint8_t*)lr.pBits, (size_t)lr.Pitch, fr.w, fr.h, fr.bgr.data());
        sys->UnlockRect(); ok = true;
    } while (0);
    if (sys) sys->Release(); if (rt) rt->Release();
    return ok;
}
static bool CaptureD9Dev(IDirect3DDevice9* dev, Frame& fr) {
    SetApi(L"Direct3D 9");
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
    bool r = CaptureD9Surface(dev, bb, fr); bb->Release(); return r;
}
static bool CaptureD9Swap(IDirect3DSwapChain9* sw, Frame& fr) {
    SetApi(L"Direct3D 9");
    IDirect3DDevice9* dev = nullptr; IDirect3DSurface9* bb = nullptr;
    if (FAILED(sw->GetDevice(&dev)) || !dev) return false;
    bool r = false;
    if (SUCCEEDED(sw->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) { r = CaptureD9Surface(dev, bb, fr); bb->Release(); }
    dev->Release(); return r;
}

// ============================================================ OpenGL capture
typedef void(WINAPI* glGetIntegerv_t)(unsigned, int*);
typedef void(WINAPI* glReadBuffer_t)(unsigned);
typedef void(WINAPI* glPixelStorei_t)(unsigned, int);
typedef void(WINAPI* glReadPixels_t)(int, int, int, int, unsigned, unsigned, void*);
typedef void(WINAPI* glBind_t)(unsigned, unsigned);
typedef PROC(WINAPI* wglGetProcAddress_t)(LPCSTR);
typedef HGLRC(WINAPI* wglGetCurrentContext_t)();

static bool CaptureGL(HDC hdc, Frame& fr) {
    SetApi(L"OpenGL");
    auto pCur = (wglGetCurrentContext_t)Proc(L"opengl32.dll", "wglGetCurrentContext");
    auto pGPA = (wglGetProcAddress_t)Proc(L"opengl32.dll", "wglGetProcAddress");
    auto glGetIntegerv = (glGetIntegerv_t)Proc(L"opengl32.dll", "glGetIntegerv");
    auto glReadBuffer = (glReadBuffer_t)Proc(L"opengl32.dll", "glReadBuffer");
    auto glPixelStorei = (glPixelStorei_t)Proc(L"opengl32.dll", "glPixelStorei");
    auto glReadPixels = (glReadPixels_t)Proc(L"opengl32.dll", "glReadPixels");
    if (!pCur || !pGPA || !glGetIntegerv || !glReadBuffer || !glPixelStorei || !glReadPixels || !pCur()) return false;
    auto glBindFramebuffer = (glBind_t)pGPA("glBindFramebuffer");
    if (!glBindFramebuffer) glBindFramebuffer = (glBind_t)pGPA("glBindFramebufferEXT");
    auto glBindBuffer = (glBind_t)pGPA("glBindBuffer");
    HWND wnd = WindowFromDC(hdc); RECT rc;
    if (!wnd || !GetClientRect(wnd, &rc)) return false;
    int w = rc.right - rc.left, h = rc.bottom - rc.top; if (w <= 0 || h <= 0) return false;
    const unsigned READ_BUFFER = 0x0C02, PACK_ALIGN = 0x0D05, PACK_ROW_LEN = 0x0D02, PACK_SKIP_ROWS = 0x0D03, PACK_SKIP_PIX = 0x0D04,
        READ_FB = 0x8CA8, READ_FB_BINDING = 0x8CAA, PACK_BUF = 0x88EB, PACK_BUF_BINDING = 0x88ED, BACK = 0x0405, BGR = 0x80E0, UBYTE = 0x1401;
    int prevRead = 0, prevAlign = 4, prevRow = 0, prevSR = 0, prevSP = 0, prevFbo = 0, prevPbo = 0;
    glGetIntegerv(READ_BUFFER, &prevRead); glGetIntegerv(PACK_ALIGN, &prevAlign); glGetIntegerv(PACK_ROW_LEN, &prevRow);
    glGetIntegerv(PACK_SKIP_ROWS, &prevSR); glGetIntegerv(PACK_SKIP_PIX, &prevSP);
    if (glBindFramebuffer) { glGetIntegerv(READ_FB_BINDING, &prevFbo); glBindFramebuffer(READ_FB, 0); }
    if (glBindBuffer) { glGetIntegerv(PACK_BUF_BINDING, &prevPbo); glBindBuffer(PACK_BUF, 0); }
    glReadBuffer(BACK);
    glPixelStorei(PACK_ALIGN, 1); glPixelStorei(PACK_ROW_LEN, 0); glPixelStorei(PACK_SKIP_ROWS, 0); glPixelStorei(PACK_SKIP_PIX, 0);
    std::vector<uint8_t> tmp((size_t)w * h * 3);
    glReadPixels(0, 0, w, h, BGR, UBYTE, tmp.data());   // implicit GPU sync
    glPixelStorei(PACK_ALIGN, prevAlign); glPixelStorei(PACK_ROW_LEN, prevRow); glPixelStorei(PACK_SKIP_ROWS, prevSR); glPixelStorei(PACK_SKIP_PIX, prevSP);
    glReadBuffer((unsigned)prevRead);
    if (glBindFramebuffer) glBindFramebuffer(READ_FB, (unsigned)prevFbo);
    if (glBindBuffer) glBindBuffer(PACK_BUF, (unsigned)prevPbo);
    fr.alloc(w, h);
    size_t row = (size_t)w * 3;
    for (int y = 0; y < h; y++) memcpy(fr.bgr.data() + (size_t)y * row, tmp.data() + (size_t)(h - 1 - y) * row, row);  // GL is bottom-up
    return true;
}

// ============================================================ take control (runs inside Present hooks)
static int64_t           g_preroll = 0;            // warm-up steps before frame 0 (locked mode)
static int               g_otherPresents = 0;      // presents of other objects since the target last presented
static std::atomic<bool> g_targetBusy{ false };    // render thread is capturing / presenting the target frame

static bool StartTake(void* obj) {
    LONG n = g_sh->fpsNum, d = g_sh->fpsDen;
    if (n <= 0 || d <= 0) { n = 24; d = 1; }
    wchar_t base[MAX_PATH];
    wcsncpy_s(base, g_sh->outDir, _TRUNCATE);
    if (!base[0]) { GetEnvironmentVariableW(L"USERPROFILE", base, MAX_PATH); wcscat_s(base, L"\\Videos\\FrameLock"); }
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t dir[MAX_PATH];
    swprintf_s(dir, L"%s\\Take_%04d%02d%02d_%02d%02d%02d", base, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    int r = SHCreateDirectoryExW(nullptr, dir, nullptr);
    if (r != ERROR_SUCCESS && r != ERROR_ALREADY_EXISTS) {
        wchar_t m[600]; swprintf_s(m, L"ERROR: cannot create output folder %s (Win32 error %d)", dir, r); SetStatus(m);
        InterlockedExchange(&g_sh->wantRecord, 0); return false;
    }
    g_fpsNum = n; g_fpsDen = d;
    g_takeDir = dir; wcsncpy_s(g_sh->takeDir, dir, _TRUNCATE);
    g_lockMode = g_sh->mode != FL_MODE_REALTIME;
    g_testTarget = InterlockedExchange(&g_sh->testFrames, 0);
    InterlockedExchange64(&g_sh->framesSaved, 0); InterlockedExchange64(&g_sh->framesQueued, 0);
    InterlockedExchange64(&g_sh->slipEvents, 0); InterlockedExchange(&g_sh->lastHr, 0);
    g_takeId++; g_asyncFail = false; g_nextIdx = 0; g_fails = 0; g_target = obj; g_api = Api::None;
    g_preroll = g_lockMode ? 1 : 0; g_otherPresents = 0;
    g_takeStartReal = RealQPC();
    g_rec = true; InterlockedExchange(&g_sh->isRecording, 1);
    wchar_t m[700];
    swprintf_s(m, L"TAKE START: mode=%s, fps=%ld/%ld, test=%lld frames, target=%p -> %s",
        g_lockMode ? L"LOCKED (offline export: exactly 1/FPS of game time per captured frame)" : L"REAL-TIME (NOT frame-locked: only frames the game presents)",
        n, d, (long long)g_testTarget, obj, dir);
    SetStatus(m);
    return true;
}
static void StopTake(const wchar_t* why) {
    ClockStop();
    double wall = (RealQPC() - g_takeStartReal) / (double)g_freq;
    g_rec = false; g_target = nullptr;
    InterlockedExchange(&g_sh->isRecording, 0); InterlockedExchange(&g_sh->wantRecord, 0);
    Log(L"TAKE STOP (%s): %lld frames captured, waiting for encoder", why, (long long)g_nextIdx);
    Job e; e.end = true; e.takeId = g_takeId; e.dir = g_takeDir; e.endCount = g_nextIdx; e.testTarget = g_testTarget; e.realtime = !g_lockMode; e.wallSec = wall;
    e.fpsNum = g_fpsNum; e.fpsDen = g_fpsDen;
    Enqueue(std::move(e));
}

// Before the original Present. Returns 0 = nothing, 1 = take started, 2 = frame captured, 3 = warm-up step.
// Cap: bool(int64_t idx, bool verbose, const wchar_t*& stage, HRESULT& hr) - copies the completed frame and reads it
// back into CPU memory synchronously. Only when it returns true is the frame index (and, in Post, the clock) advanced.
template <class Cap> static int FrameStepPre(void* obj, Cap& cap) {
    if (!g_sh) return 0;
    std::lock_guard<std::mutex> lk(g_stepMx);
    bool want = InterlockedCompareExchange(&g_sh->wantRecord, 0, 0) != 0;
    if (!g_rec) return (want && StartTake(obj)) ? 1 : 0;
    if (obj != g_target) {
        // The recorded swapchain/window was replaced (resize, fullscreen toggle...). Follow the new one once the old
        // one has stopped presenting. Safe: the clock has not advanced, so no simulation step is lost.
        if (++g_otherPresents < 30) return 0;
        Log(L"Target %p stopped presenting; now recording %p (clock was held, no step lost)", g_target, obj);
        g_target = obj;
    }
    g_otherPresents = 0;
    g_targetBusy = true;                                 // cleared in FrameStepPost
    if (g_asyncFail) { StopTake(L"async failure"); return 0; }
    if (!want || g_clockReleased) { StopTake(L"user"); return 0; }
    if (g_preroll > 0) {
        // One uncaptured warm-up step: the game's first dt after locking is (real time since its previous frame).
        // Skipping that one step before frame_000000 makes every captured frame - including the first - exactly 1/FPS.
        g_preroll--; return 3;
    }
    int64_t n = g_nextIdx;
    const wchar_t* stage = L"capture"; HRESULT hr = E_FAIL;
    if (!cap(n, n < 2, stage, hr)) {                    // not captured: index and clock NOT advanced -> same step is re-rendered
        InterlockedExchange(&g_sh->lastHr, (LONG)hr);
        wchar_t m[512]; swprintf_s(m, L"Capture of frame_%06lld failed at stage '%s' (HRESULT 0x%08X), attempt %d/3 - simulation held, retrying the same step", (long long)n, stage, (unsigned)hr, g_fails + 1);
        SetStatus(m);
        if (++g_fails >= 3) { SetStatus(L"ERROR: capture failed 3 times in a row - recording stopped (see previous line for stage/HRESULT). No frame index was skipped."); StopTake(L"capture failure"); }
        return 0;
    }
    g_fails = 0;
    g_nextIdx = n + 1;                                   // frame n is in memory: only now may the simulation advance
    InterlockedExchange64(&g_sh->framesQueued, n + 1);
    if (g_testTarget > 0 && n + 1 >= g_testTarget) InterlockedExchange(&g_sh->wantRecord, 0);   // stops at next Present
    return 2;
}
static void FrameStepPost(int r) {
    if (g_lockMode) {                                    // locked mode only (real-time mode never touches the clock)
        if (r == 1) ClockStart();                        // step 0 starts after the start Present
        else if ((r == 2 || r == 3) && g_rec) ClockAdvance();   // exactly one step per captured (or warm-up) frame
    }
    g_targetBusy = false;
}
template <class Cap, class Call>
static auto PresentWrap(void* obj, Cap cap, Call call) -> decltype(call()) {
    if (t_inPresent) return call();          // nested present (e.g. Present -> Present1)
    t_inPresent = true;
    int r = FrameStepPre(obj, cap);
    auto res = call();
    FrameStepPost(r);
    t_inPresent = false;
    return res;
}

// ============================================================ core interface (flcore.h) for separate capture modules
// Thin, behavior-preserving wrappers: the Vulkan layer (vk_layer.cpp) uses exactly the same FrameStepPre/Post,
// nesting guard and encode queue as the in-file D3D/GL hooks.
bool FL_BeginPresent() { if (t_inPresent) return false; t_inPresent = true; return true; }
void FL_EndPresent() { t_inPresent = false; }
int FL_StepPre(void* target, FLCaptureFn fn, void* ctx) {
    auto cap = [&](int64_t i, bool v, const wchar_t*& s, HRESULT& h) { return fn(ctx, i, v, &s, &h); };
    return FrameStepPre(target, cap);
}
void FL_StepPost(int r) { FrameStepPost(r); }
void FL_QueueFrame(int64_t idx, int w, int h, PF pf, size_t pitch, std::vector<uint8_t>&& raw) {
    Job j; j.takeId = g_takeId; j.idx = idx; j.dir = g_takeDir; j.w = w; j.h = h; j.pf = pf; j.pitch = pitch; j.raw = std::move(raw);
    Enqueue(std::move(j));
}
size_t FL_BytesPerPixel(PF pf) { return BppOf(pf); }
void FL_Log(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list a; va_start(a, fmt); _vsnwprintf_s(buf, _TRUNCATE, fmt, a); va_end(a);
    Log(L"%s", buf);
}
void FL_SetApi(const wchar_t* s) { SetApi(s); }
int64_t FL_RealQPC() { return RealQPC(); }
double FL_RealMs(int64_t ticks) { return RealMs(ticks); }

// Hotkeys + stall monitor on its own thread (works even when the game is stuck waiting on the clock).
static DWORD WINAPI ControlThread(LPVOID) {
    bool p9 = false, p10 = false; int64_t warnedStep = -1;
    for (;;) {
        Sleep(10);
        if (!g_sh) continue;
        bool d9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0, d10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (d9 && !p9) InterlockedExchange(&g_sh->wantRecord, g_sh->wantRecord ? 0 : 1);
        if (d10 && !p10 && !g_rec) { InterlockedExchange(&g_sh->testFrames, 120); InterlockedExchange(&g_sh->wantRecord, 1); }
        p9 = d9; p10 = d10;
        if (!g_lock.load()) { warnedStep = -1; continue; }
        bool busy = g_targetBusy.load();
        // Stop requested while the game is stuck inside a step: let it reach Present so the take can end.
        if (!busy && InterlockedCompareExchange(&g_sh->wantRecord, 0, 0) == 0) { ClockRelease(); continue; }
        AcquireSRWLockShared(&g_clk); ClockState c = g_cs; int64_t step = g_stepIdx.load(); ReleaseSRWLockShared(&g_clk);
        if (busy || c.released) continue;
        double heldMs = RealMs(RealQPC() - c.anchor - c.hold - c.step);   // time spent at the step boundary
        if (heldMs > 3000.0 && warnedStep != step) {
            warnedStep = step;
            InterlockedIncrement64(&g_sh->slipEvents);   // now counts stall warnings (time is never slipped)
            wchar_t m[700];
            swprintf_s(m, L"WAITING before frame_%06lld: no new frame for %.1f s. Game time is held at the frame boundary and will NOT be skipped or sped up. "
                L"If the game looks frozen, it is waiting for more than 1/FPS of game time (in-game FPS cap / VSync limiter below the output FPS). "
                L"Disable the cap or pick an output FPS <= the cap. Press F9 to stop; saved frames stay valid.",
                (long long)g_sh->framesQueued, heldMs / 1000.0);
            SetStatus(m);
        }
    }
    return 0;
}

// ============================================================ present hooks
typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* Present1_t)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef void(STDMETHODCALLTYPE* ECL_t)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
typedef HRESULT(STDMETHODCALLTYPE* D9Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(STDMETHODCALLTYPE* D9PresentEx_t)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* D9SwPresent_t)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
typedef BOOL(WINAPI* Swap_t)(HDC);
typedef BOOL(WINAPI* SwapLayer_t)(HDC, UINT);

static Present_t o_Present = nullptr; static Present1_t o_Present1 = nullptr; static ECL_t o_ECL = nullptr;
static D9Present_t o_D9Present = nullptr, o_D9Present2 = nullptr; static D9PresentEx_t o_D9PresentEx = nullptr; static D9SwPresent_t o_D9SwPresent = nullptr;
static Swap_t o_wglSwap = nullptr, o_gdiSwap = nullptr; static SwapLayer_t o_wglSwapLayer = nullptr;

#define CPU_CAP(expr, api) [&](int64_t idx, bool, const wchar_t*& stage, HRESULT& hr) { g_api = api; Frame f; stage = L"synchronous readback"; hr = E_FAIL; if (!(expr)) return false; hr = S_OK; return QueueCpuFrame(f, idx); }

// review2/3: once the Vulkan layer has presented in this process (sticky; e.g. NVIDIA "layered on DXGI" routes Vulkan
// presents through DXGI on another thread), DXGI presents bypass FrameStepPre entirely, so the clock can never be
// stepped by both paths. Deliberately NOT time-windowed (review3): a single Vulkan present can block for > 1 s.
static void DxgiStandDownLog() {
    static std::atomic<bool> once{ false };
    if (!once.exchange(true)) Log(L"DXGI Present seen while the Vulkan layer is presenting: DXGI hooks stand down (Vulkan path owns capture/clock)");
}
static HRESULT STDMETHODCALLTYPE Hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (flags & DXGI_PRESENT_TEST) return o_Present(sc, sync, flags);
    if (FL_VkLayerPresenting()) { DxgiStandDownLog(); return o_Present(sc, sync, flags); }   // review2: Vulkan layer owns the take
    return PresentWrap(sc, [&](int64_t i, bool v, const wchar_t*& s, HRESULT& h) { return SubmitDXGI(sc, i, v, s, h); }, [&] { return o_Present(sc, sync, flags); });
}
static HRESULT STDMETHODCALLTYPE Hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) {
    if (flags & DXGI_PRESENT_TEST) return o_Present1(sc, sync, flags, pp);
    if (FL_VkLayerPresenting()) { DxgiStandDownLog(); return o_Present1(sc, sync, flags, pp); }   // review2
    return PresentWrap((IDXGISwapChain*)sc, [&](int64_t i, bool v, const wchar_t*& s, HRESULT& h) { return SubmitDXGI(sc, i, v, s, h); }, [&] { return o_Present1(sc, sync, flags, pp); });
}
static void STDMETHODCALLTYPE Hk_ECL(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
    if (d.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        std::lock_guard<std::mutex> l(g_qMx);
        if (g_lastQueue != q) { q->AddRef(); if (g_lastQueue) g_lastQueue->Release(); g_lastQueue = q; }
    }
    o_ECL(q, n, lists);
}
// phase opus55: swapchain creation hooks -> D3D12 present-queue association (see RememberSwapchainQueue)
typedef HRESULT(STDMETHODCALLTYPE* CSC_t)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* CSCHwnd_t)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
typedef HRESULT(STDMETHODCALLTYPE* CSCCore_t)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
typedef HRESULT(STDMETHODCALLTYPE* CSCComp_t)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
static CSC_t o_CSC = nullptr; static CSCHwnd_t o_CSCHwnd = nullptr; static CSCCore_t o_CSCCore = nullptr; static CSCComp_t o_CSCComp = nullptr;
static HRESULT STDMETHODCALLTYPE Hk_CSC(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out) {
    HRESULT hr = o_CSC(f, dev, d, out);
    if (SUCCEEDED(hr) && out && *out) RememberSwapchainQueue(dev, *out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_CSCHwnd(IDXGIFactory2* f, IUnknown* dev, HWND w, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fd, IDXGIOutput* o, IDXGISwapChain1** out) {
    HRESULT hr = o_CSCHwnd(f, dev, w, d, fd, o, out);
    if (SUCCEEDED(hr) && out && *out) RememberSwapchainQueue(dev, *out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_CSCCore(IDXGIFactory2* f, IUnknown* dev, IUnknown* win, const DXGI_SWAP_CHAIN_DESC1* d, IDXGIOutput* o, IDXGISwapChain1** out) {
    HRESULT hr = o_CSCCore(f, dev, win, d, o, out);
    if (SUCCEEDED(hr) && out && *out) RememberSwapchainQueue(dev, *out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_CSCComp(IDXGIFactory2* f, IUnknown* dev, const DXGI_SWAP_CHAIN_DESC1* d, IDXGIOutput* o, IDXGISwapChain1** out) {
    HRESULT hr = o_CSCComp(f, dev, d, o, out);
    if (SUCCEEDED(hr) && out && *out) RememberSwapchainQueue(dev, *out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_D9Present(IDirect3DDevice9* d, const RECT* s, const RECT* r, HWND w, const RGNDATA* rg) {
    return PresentWrap(d, CPU_CAP(CaptureD9Dev(d, f), Api::D9), [&] { return o_D9Present(d, s, r, w, rg); });
}
static HRESULT STDMETHODCALLTYPE Hk_D9Present2(IDirect3DDevice9* d, const RECT* s, const RECT* r, HWND w, const RGNDATA* rg) {
    return PresentWrap(d, CPU_CAP(CaptureD9Dev(d, f), Api::D9), [&] { return o_D9Present2(d, s, r, w, rg); });
}
static HRESULT STDMETHODCALLTYPE Hk_D9PresentEx(IDirect3DDevice9Ex* d, const RECT* s, const RECT* r, HWND w, const RGNDATA* rg, DWORD fl) {
    return PresentWrap((IDirect3DDevice9*)d, CPU_CAP(CaptureD9Dev(d, f), Api::D9), [&] { return o_D9PresentEx(d, s, r, w, rg, fl); });
}
static HRESULT STDMETHODCALLTYPE Hk_D9SwPresent(IDirect3DSwapChain9* sw, const RECT* s, const RECT* r, HWND w, const RGNDATA* rg, DWORD fl) {
    IDirect3DDevice9* dev = nullptr; sw->GetDevice(&dev); if (dev) dev->Release();   // device pointer used as target id
    return PresentWrap(dev ? (void*)dev : (void*)sw, CPU_CAP(CaptureD9Swap(sw, f), Api::D9), [&] { return o_D9SwPresent(sw, s, r, w, rg, fl); });
}
static BOOL WINAPI Hk_wglSwap(HDC hdc) {
    return PresentWrap(WindowFromDC(hdc), CPU_CAP(CaptureGL(hdc, f), Api::GL), [&] { return o_wglSwap(hdc); });
}
static BOOL WINAPI Hk_gdiSwap(HDC hdc) {
    return PresentWrap(WindowFromDC(hdc), CPU_CAP(CaptureGL(hdc, f), Api::GL), [&] { return o_gdiSwap(hdc); });
}
static BOOL WINAPI Hk_wglSwapLayer(HDC hdc, UINT planes) {
    return PresentWrap(WindowFromDC(hdc), CPU_CAP(CaptureGL(hdc, f), Api::GL), [&] { return o_wglSwapLayer(hdc, planes); });
}

// ============================================================ hook installers (dummy devices -> vtables)
static HWND DummyWnd() {
    static HWND w = nullptr; if (w) return w;
    WNDCLASSEXW wc = { sizeof(wc) }; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = g_hMod; wc.lpszClassName = L"FrameLockDummyWnd";
    RegisterClassExW(&wc);
    w = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, g_hMod, nullptr);
    return w;
}
// IDXGIFactory vtable: 10 CreateSwapChain. IDXGIFactory2: 15 CreateSwapChainForHwnd, 16 CreateSwapChainForCoreWindow,
// 24 CreateSwapChainForComposition (IUnknown 0-2, IDXGIObject 3-6, IDXGIFactory 7-11, IDXGIFactory1 12-13, IDXGIFactory2 14-24).
static bool HookDXGIFactory() {
    typedef HRESULT(WINAPI* CF1_t)(REFIID, void**);
    auto pCF = (CF1_t)Proc(L"dxgi.dll", "CreateDXGIFactory1"); if (!pCF) return false;
    IDXGIFactory1* f = nullptr;
    if (FAILED(pCF(IID_PPV_ARGS(&f))) || !f) { Log(L"CreateDXGIFactory1 failed: swapchain->queue association disabled"); return false; }
    void** vt = *(void***)f;
    bool ok = Hook(vt[10], (void*)Hk_CSC, (void**)&o_CSC, "IDXGIFactory::CreateSwapChain");
    IDXGIFactory2* f2 = nullptr;
    if (SUCCEEDED(f->QueryInterface(IID_PPV_ARGS(&f2))) && f2) {
        void** v2 = *(void***)f2;
        ok &= Hook(v2[15], (void*)Hk_CSCHwnd, (void**)&o_CSCHwnd, "IDXGIFactory2::CreateSwapChainForHwnd");
        Hook(v2[16], (void*)Hk_CSCCore, (void**)&o_CSCCore, "IDXGIFactory2::CreateSwapChainForCoreWindow");
        Hook(v2[24], (void*)Hk_CSCComp, (void**)&o_CSCComp, "IDXGIFactory2::CreateSwapChainForComposition");
        f2->Release();
    }
    f->Release();
    return ok;
}
static bool HookDXGI() {
    HMODULE m = LoadLibraryW(L"d3d11.dll"); if (!m) return false;
    auto pCreate = (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)GetProcAddress(m, "D3D11CreateDeviceAndSwapChain"); if (!pCreate) return false;
    DXGI_SWAP_CHAIN_DESC sd = {}; sd.BufferCount = 1; sd.BufferDesc.Width = 64; sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = DummyWnd();
    sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = pCreate(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    if (FAILED(hr)) hr = pCreate(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    if (FAILED(hr) || !sc) { Log(L"Dummy D3D11 swapchain failed 0x%08X", hr); return false; }
    void** vt = *(void***)sc;
    bool ok = Hook(vt[8], (void*)Hk_Present, (void**)&o_Present, "IDXGISwapChain::Present");
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) {
        Hook((*(void***)sc1)[22], (void*)Hk_Present1, (void**)&o_Present1, "IDXGISwapChain1::Present1"); sc1->Release();
    }
    sc->Release(); ctx->Release(); dev->Release();
    HookDXGIFactory();   // optional: D3D12 present-queue association; failure only disables that refinement
    return ok;
}
static bool HookD3D12Queue() {
    HMODULE m = GetModuleHandleW(L"d3d12.dll"); if (!m) return false;
    auto pCreate = (PFN_D3D12_CREATE_DEVICE)GetProcAddress(m, "D3D12CreateDevice"); if (!pCreate) return false;
    ID3D12Device* dev = nullptr;
    if (FAILED(pCreate(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))) || !dev) return false;
    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q = nullptr;
    bool ok = false;
    if (SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) && q) {
        ok = Hook((*(void***)q)[10], (void*)Hk_ECL, (void**)&o_ECL, "ID3D12CommandQueue::ExecuteCommandLists"); q->Release();
    }
    dev->Release();
    return ok;
}
static bool HookD3D9() {
    HMODULE m = GetModuleHandleW(L"d3d9.dll"); if (!m) return false;
    typedef IDirect3D9* (WINAPI* C9_t)(UINT); typedef HRESULT(WINAPI* C9Ex_t)(UINT, IDirect3D9Ex**);
    auto pC9 = (C9_t)GetProcAddress(m, "Direct3DCreate9"); auto pC9Ex = (C9Ex_t)GetProcAddress(m, "Direct3DCreate9Ex");
    D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = DummyWnd(); pp.BackBufferFormat = D3DFMT_UNKNOWN;
    const DWORD fl = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT;
    bool ok = false; void* devPresent = nullptr;
    if (pC9) {
        if (IDirect3D9* d3d = pC9(D3D_SDK_VERSION)) {
            IDirect3DDevice9* dev = nullptr;
            if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, pp.hDeviceWindow, fl, &pp, &dev)))
                d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, pp.hDeviceWindow, fl, &pp, &dev);
            if (dev) {
                void** vt = *(void***)dev; devPresent = vt[17];
                ok = Hook(vt[17], (void*)Hk_D9Present, (void**)&o_D9Present, "IDirect3DDevice9::Present");
                IDirect3DSwapChain9* sw = nullptr;
                if (SUCCEEDED(dev->GetSwapChain(0, &sw)) && sw) { Hook((*(void***)sw)[3], (void*)Hk_D9SwPresent, (void**)&o_D9SwPresent, "IDirect3DSwapChain9::Present"); sw->Release(); }
                dev->Release();
            }
            d3d->Release();
        }
    }
    if (pC9Ex) {
        IDirect3D9Ex* d3d = nullptr;
        if (SUCCEEDED(pC9Ex(D3D_SDK_VERSION, &d3d)) && d3d) {
            IDirect3DDevice9Ex* dev = nullptr;
            d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, pp.hDeviceWindow, fl, &pp, nullptr, &dev);
            if (dev) {
                void** vt = *(void***)dev;
                ok |= Hook(vt[121], (void*)Hk_D9PresentEx, (void**)&o_D9PresentEx, "IDirect3DDevice9Ex::PresentEx");
                if (vt[17] != devPresent) Hook(vt[17], (void*)Hk_D9Present2, (void**)&o_D9Present2, "IDirect3DDevice9Ex::Present");
                dev->Release();
            }
            d3d->Release();
        }
    }
    return ok;
}
static bool HookGL() {
    bool ok = Hook(Proc(L"opengl32.dll", "wglSwapBuffers"), (void*)Hk_wglSwap, (void**)&o_wglSwap, "wglSwapBuffers");
    Hook(Proc(L"opengl32.dll", "wglSwapLayerBuffers"), (void*)Hk_wglSwapLayer, (void**)&o_wglSwapLayer, "wglSwapLayerBuffers");
    Hook(Proc(L"gdi32.dll", "SwapBuffers"), (void*)Hk_gdiSwap, (void**)&o_gdiSwap, "gdi32!SwapBuffers");
    return ok;
}

// ============================================================ timebase diagnostics (read-only, phase opus55)
// User mode cannot trap rdtsc/rdtscp: they only fault when CR4.TSD is set, which is kernel-controlled and not set
// by Windows, so a VEH never sees them. The only options would be patching the game's code or a hypervisor -
// both out of scope. What we CAN do honestly is report evidence: count rdtsc (0F 31) / rdtscp (0F 01 F9) byte
// patterns in the executable's code sections and compare with the count expected from random bytes. This is a
// heuristic for the log only; it changes nothing.
static void ReportTimebases() {
    const uint8_t* base = (const uint8_t*)GetModuleHandleW(nullptr); if (!base) return;
    auto dos = (const IMAGE_DOS_HEADER*)base; if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew); if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    size_t c31 = 0, cF9 = 0, scanned = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uint8_t* p = base + sec->VirtualAddress; size_t n = sec->Misc.VirtualSize, off = 0;
        while (off < n) {
            MEMORY_BASIC_INFORMATION mbi; if (!VirtualQuery(p + off, &mbi, sizeof(mbi))) break;
            size_t end = (size_t)((const uint8_t*)mbi.BaseAddress + mbi.RegionSize - p); if (end > n) end = n;
            if (end <= off) break;
            DWORD pr = mbi.Protect & 0xFF;
            bool rd = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
                (pr == PAGE_EXECUTE_READ || pr == PAGE_EXECUTE_READWRITE || pr == PAGE_EXECUTE_WRITECOPY || pr == PAGE_READONLY || pr == PAGE_READWRITE || pr == PAGE_WRITECOPY);
            if (rd) {
                for (size_t k = off; k + 1 < end; k++) if (p[k] == 0x0F) {
                    if (p[k + 1] == 0x31) c31++;
                    else if (p[k + 1] == 0x01 && k + 2 < end && p[k + 2] == 0xF9) cF9++;
                }
                scanned += end - off;
            }
            off = end;
        }
    }
    double expect = scanned / 65536.0;
    Log(L"Timebase scan (heuristic): %zu bytes of exe code, rdtsc patterns %zu (random-byte expectation ~%.0f), rdtscp patterns %zu. "
        L"rdtsc/rdtscp, audio/GPU clocks and KUSER_SHARED_DATA reads cannot be virtualized from user mode; if counts are well above the expectation and "
        L"locked takes play back at the wrong speed, the engine probably times itself with the TSC.", scanned, c31, expect, cF9);
}

// ============================================================ init
static void OpenShared() {
    wchar_t name[64]; swprintf_s(name, FL_SHARED_FMT, GetCurrentProcessId());
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(FLShared), name);
    bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    if (!map) return;
    g_sh = (FLShared*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(FLShared));
    if (g_sh && (!existed || g_sh->magic != FL_MAGIC)) {
        ZeroMemory(g_sh, sizeof(FLShared)); g_sh->magic = FL_MAGIC; g_sh->fpsNum = 24; g_sh->fpsDen = 1;
    }
}
// review4 (child-process fix): the launcher (Attach or 'Launch game', the latter while the game is still suspended)
// always creates Local\FrameLock_<pid> BEFORE the DLL can run in that process. If the mapping does not exist, the DLL was
// loaded by the Vulkan loader into a process FrameLock did not start - typically a child of the launched game that
// inherited FRAMELOCK_VK_CAPTURE=1 (crash reporter, shader compiler, the real game behind a launcher stub). Such a process
// must not get clock hooks, hotkeys or a take: stay dormant (the layer passes everything through because g_sh is null)
// and initialise only if FrameLock later attaches to that PID (Attach creates the mapping, LoadLibrary just bumps the
// refcount, and this poll then sees the mapping). This also makes Vulkan games behind launcher stubs capturable: start the
// stub with 'Launch game', then Attach to the real game process.
static bool SharedExists() {
    wchar_t name[64]; swprintf_s(name, FL_SHARED_FMT, GetCurrentProcessId());
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!h) return false;
    CloseHandle(h);
    return true;
}
static DWORD WINAPI InitThread(LPVOID) {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    swprintf_s(g_logPath, L"%sFrameLock_%lu.log", tmp, GetCurrentProcessId());
    if (!SharedExists()) {
        Log(L"No FrameLock session for PID %lu (e.g. child process that inherited FRAMELOCK_VK_CAPTURE): dormant, pass-through until FrameLock attaches", GetCurrentProcessId());
        while (!SharedExists()) Sleep(500);
        Log(L"FrameLock session appeared for this process: initialising");
    }
    OpenShared();
    if (!g_sh) { Log(L"Shared memory failed"); return 0; }
    InitLut();
    CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
    if (MH_Initialize() != MH_OK) { SetStatus(L"ERROR: MinHook init failed"); return 0; }
    InstallTimeHooks();
    ReportTimebases();
    CreateThread(nullptr, 0, ControlThread, nullptr, 0, nullptr);
    InterlockedExchange(&g_sh->hooksReady, 1);
    SetStatus(L"Hooks active (v3 frame-locked). Waiting for the game to render... Record = F9, 120-frame self-test = F10.");
    bool dxgi = false, d12 = false, d9 = false, gl = false; int tDxgi = 0, tD12 = 0, tD9 = 0, tGl = 0;
    for (;;) {   // graphics DLLs may be loaded later: keep scanning
        if (!dxgi && tDxgi < 5 && GetModuleHandleW(L"dxgi.dll")) { dxgi = HookDXGI(); tDxgi++; }
        if (!d12 && tD12 < 5 && GetModuleHandleW(L"d3d12.dll")) { d12 = HookD3D12Queue(); tD12++; }
        if (!d9 && tD9 < 5 && GetModuleHandleW(L"d3d9.dll")) { d9 = HookD3D9(); tD9++; }
        if (!gl && tGl < 5 && GetModuleHandleW(L"opengl32.dll")) { gl = HookGL(); tGl++; }
        if (GetModuleHandleW(L"vulkan-1.dll") && !FL_VkLayerActive() && g_api == Api::None && !g_rec)
            SetApi(L"Vulkan loaded, FrameLock Vulkan layer NOT active: Vulkan capture needs the game started with 'Launch game .exe and attach' (ignore if the game renders with D3D/OpenGL)");
        Sleep(1000);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hMod = h;
        DisableThreadLibraryCalls(h);
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
