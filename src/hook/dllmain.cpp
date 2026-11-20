// FrameLock hook DLL (v2) - frame-by-frame image sequence exporter.
// Two explicitly separate modes:
//  * LOCKED (offline export): the game clock is stepped by exactly 1/FPS per captured frame.
//    Time is NOT frozen: inside a step it advances normally (bounded below the next step
//    boundary), so frame limiters / waits complete and the game can always reach Present.
//  * REALTIME: clock untouched, every frame the game actually presents is saved (no fabrication).
// Capture: backbuffer copy is recorded right before the original Present (completed frame),
// read back asynchronously (D3D11 staging ring / D3D12 readback ring + fence) and encoded on a
// worker thread. Bounded queues give backpressure instead of dropping frames.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <d3d9.h>
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
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cmath>
#include "MinHook.h"
#include "../shared.h"

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
static RtlQPC_t o_RtlQPC = nullptr;
static QPC_t    o_QPC_kb = nullptr, o_QPC_k32 = nullptr;

static int64_t  g_freq = 10000000;
static int64_t  g_qpc0 = 0;     // real QPC at init
static uint64_t g_tick0 = 0;    // GetTickCount64 at init
static uint32_t g_tgt0 = 0;     // timeGetTime at init
static int64_t  g_ft0 = 0;      // system FILETIME at init
static int64_t  g_fpsNum = 24, g_fpsDen = 1;
static int64_t  g_frameTimeBase = 0;       // virtual QPC at take start

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

// Locked-clock state for the current step n. Virtual time seen by the game during step n:
//   base                      for the first `dead` real ticks after Present returned (exact step start)
//   base + (e - dead)         afterwards, advancing at real speed ...
//   base + step - 1           ... capped just below the next step boundary (monotonic, no overlap)
// If the game busy-waits on the clock while it is held at the cap (frame limiter waiting longer
// than one output step), the clock is released (free-run) after 0.5 s + 1000 queries so the game
// can never hang; the step is then logged as a slip (that frame's dt was longer than 1/FPS).
struct ClockState { int64_t base, step, anchor, dead; };
static SRWLOCK              g_clk = SRWLOCK_INIT;
static ClockState           g_cs = {};
static std::atomic<bool>    g_lock{ false };        // locked clock active
static std::atomic<int64_t> g_offset{ 0 };          // virtual - real while not locked
static int64_t              g_recBase = 0, g_slip = 0;
static std::atomic<int64_t> g_stepIdx{ 0 };
static std::atomic<bool>    g_freeRun{ false };
static std::atomic<int64_t> g_holdQ{ 0 };

static int64_t DeadZone(int64_t step) { return std::min(step / 4, g_freq / 100); }   // <= 10 ms
static int64_t ClockEval(const ClockState& c, int64_t real, bool countQuery) {
    int64_t e = real - c.anchor; if (e < 0) e = 0;
    if (e <= c.dead) return c.base;
    int64_t cap = c.step - 1, add = e - c.dead;
    if (add < cap) return c.base + add;
    int64_t held = add - cap;
    if (g_freeRun.load(std::memory_order_relaxed)) return c.base + cap + held;
    if (countQuery) {
        int64_t q = ++g_holdQ;
        if (held > g_freq / 2 && q > 1000) {
            bool exp = false;
            if (g_freeRun.compare_exchange_strong(exp, true)) {
                if (g_sh) InterlockedIncrement64(&g_sh->slipEvents);
                Log(L"CLOCK: game busy-waits on the clock past one output step (FPS cap/limiter lower than output FPS?). "
                    L"Clock released to avoid a hang; step %lld will be longer than 1/FPS (slip).", (long long)g_stepIdx.load());
            }
            return c.base + cap + held;
        }
    }
    return c.base + cap;
}
static int64_t VirtQPC() {
    if (!g_lock.load(std::memory_order_acquire)) return RealQPC() + g_offset.load(std::memory_order_relaxed);
    AcquireSRWLockShared(&g_clk); ClockState c = g_cs; ReleaseSRWLockShared(&g_clk);
    return ClockEval(c, RealQPC(), true);
}
static int64_t VirtDelta(int64_t unitsPerSec) {
    int64_t d = VirtQPC() - g_qpc0;
    return d >= 0 ? MulDiv64(d, unitsPerSec, g_freq) : -MulDiv64(-d, unitsPerSec, g_freq);
}
// Step 0 begins when the Present that started the take returns.
static void ClockStart() {
    AcquireSRWLockExclusive(&g_clk);
    int64_t real = RealQPC();
    g_recBase = real + g_offset.load(); g_frameTimeBase = g_recBase; g_slip = 0; g_stepIdx = 0;
    g_cs.base = g_recBase; g_cs.step = FrameTicks(1); g_cs.anchor = real; g_cs.dead = DeadZone(g_cs.step);
    g_freeRun = false; g_holdQ = 0;
    g_lock.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_clk);
    Log(L"CLOCK: locked at step 0 (step = %lld ticks, freq %lld, dead zone %.2f ms)", (long long)g_cs.step, (long long)g_freq, RealMs(g_cs.dead));
}
// Called after the Present whose frame was safely queued: move to the next exact step.
static void ClockAdvance() {
    AcquireSRWLockExclusive(&g_clk);
    int64_t real = RealQPC();
    int64_t vnow = ClockEval(g_cs, real, false);
    int64_t n = g_stepIdx + 1;
    int64_t nb = g_recBase + FrameTicks(n) + g_slip;
    int64_t extra = 0;
    if (vnow >= nb) { extra = vnow - nb + 1; g_slip += extra; nb += extra; }   // keep time monotonic after a free-run
    g_cs.base = nb; g_cs.step = FrameTicks(n + 1) - FrameTicks(n); g_cs.anchor = real; g_cs.dead = DeadZone(g_cs.step);
    g_stepIdx = n; g_freeRun = false; g_holdQ = 0;
    ReleaseSRWLockExclusive(&g_clk);
    if (extra) Log(L"CLOCK: slip at step %lld: +%.3f ms (this step was not exactly 1/FPS)", (long long)(n - 1), RealMs(extra));
}
static void ClockStop() {
    if (!g_lock.load()) return;
    AcquireSRWLockExclusive(&g_clk);
    int64_t real = RealQPC();
    int64_t v = ClockEval(g_cs, real, false);
    g_offset.store(v - real);           // continuous, monotonic after unlocking
    g_lock.store(false, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_clk);
    Log(L"CLOCK: unlocked after %lld steps (total slip %.3f ms)", (long long)g_stepIdx.load(), RealMs(g_slip));
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

    Hook(Proc(L"ntdll.dll", "RtlQueryPerformanceCounter"), (void*)Hk_RtlQPC, (void**)&o_RtlQPC, "RtlQueryPerformanceCounter");
    Hook(Proc(L"kernelbase.dll", "QueryPerformanceCounter"), (void*)Hk_QPC, (void**)&o_QPC_kb, "kernelbase!QueryPerformanceCounter");
    Hook(Proc(L"kernel32.dll", "QueryPerformanceCounter"), (void*)Hk_QPC, (void**)&o_QPC_k32, "kernel32!QueryPerformanceCounter");
    const wchar_t* mods[] = { L"kernelbase.dll", L"kernel32.dll" };
    for (auto m : mods) {
        Hook(Proc(m, "GetTickCount"), (void*)Hk_GetTickCount, nullptr, "GetTickCount");
        Hook(Proc(m, "GetTickCount64"), (void*)Hk_GetTickCount64, nullptr, "GetTickCount64");
        Hook(Proc(m, "GetSystemTimeAsFileTime"), (void*)Hk_GetSysTimeFT, nullptr, "GetSystemTimeAsFileTime");
        Hook(Proc(m, "GetSystemTimePreciseAsFileTime"), (void*)Hk_GetSysTimeFT, nullptr, "GetSystemTimePreciseAsFileTime");
    }
    Hook(Proc(L"winmm.dll", "timeGetTime"), (void*)Hk_timeGetTime, nullptr, "winmm!timeGetTime");
    Hook(Proc(L"winmmbase.dll", "timeGetTime"), (void*)Hk_timeGetTime, nullptr, "winmmbase!timeGetTime");
}

// ============================================================ pixel conversion -> BGR24
struct Frame {
    int w = 0, h = 0; std::vector<uint8_t> bgr;
    void alloc(int W, int H) { w = W; h = H; bgr.resize((size_t)W * H * 3); }
};
enum class PF { BGRA8, RGBA8, RGB10A2, RGBA16F, D9_A2R10G10B10, BGR565, BGR24 };

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
enum class Api { None, D11, D12, D9, GL };
static std::mutex        g_stepMx;
static std::atomic<bool> g_rec{ false };          // a take is active
static bool              g_lockMode = true;        // take mode (locked vs realtime)
static void*             g_target = nullptr;       // swapchain/device/window being recorded
static Api               g_api = Api::None;
static int               g_fails = 0;
static int64_t           g_nextIdx = 0;            // next frame index to capture (== frames queued)
static int64_t           g_testTarget = 0;
static int64_t           g_takeStartReal = 0;
static int64_t           g_capturePtsTicks = 0;     // PTS sampled at the current Present
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

// ============================================================ async readback / encode queue
struct D12Slot { ID3D12Resource* rb = nullptr; ID3D12CommandAllocator* al = nullptr; ID3D12GraphicsCommandList* cl = nullptr; UINT64 size = 0; bool busy = false; };
struct Job {
    int takeId = 0; int64_t idx = -1; std::wstring dir;
    int w = 0, h = 0; PF pf = PF::BGR24; size_t pitch = 0; std::vector<uint8_t> raw;     // CPU pixels
    D12Slot* slot = nullptr; ID3D12Fence* fence = nullptr; ID3D12Device* dev = nullptr;  // D3D12 readback
    UINT64 fenceVal = 0, offset = 0, size = 0;
    int64_t ptsTicks = 0;
    bool end = false; int64_t endCount = 0; int64_t testTarget = 0; bool realtime = false; double wallSec = 0;
    int64_t endPtsTicks = 0, fpsNum = 24, fpsDen = 1;
};
static const size_t kMaxJobs = 4;     // bounded queue: producer waits (backpressure), never drops
static std::mutex g_jMx; static std::condition_variable g_jCv; static std::deque<Job> g_jobs;
static std::mutex g_slotMx; static std::condition_variable g_slotCv;

static void Enqueue(Job&& j) {
    std::unique_lock<std::mutex> l(g_jMx);
    if (!j.end && g_jobs.size() >= kMaxJobs) {
        int64_t t0 = RealQPC();
        g_jCv.wait(l, [] { return g_jobs.size() < kMaxJobs; });
        Log(L"Backpressure: render thread waited %.1f ms for the encoder (frame %lld)", RealMs(RealQPC() - t0), (long long)j.idx);
    }
    g_jobs.push_back(std::move(j));
    l.unlock(); g_jCv.notify_all();
}
static void ReleaseSlot(D12Slot* s) { { std::lock_guard<std::mutex> l(g_slotMx); s->busy = false; } g_slotCv.notify_all(); }

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

struct TimingPoint { int64_t idx; int64_t ptsTicks; };

// Writes an exact per-frame presentation timeline. JPEG sequences have no intrinsic FPS;
// this sidecar prevents variable-rate game footage from being played back at an assumed CFR.
static bool WriteTimingFiles(const std::wstring& dir, const std::vector<TimingPoint>& points,
                             int64_t endPtsTicks, int64_t fpsNum, int64_t fpsDen, bool realtime) {
    if (points.empty()) {
        Log(L"TIMING: no successfully encoded frames; timing sidecars were not written");
        return false;
    }
    std::vector<int64_t> pts(points.size()), durations(points.size());
    const int64_t first = points.front().ptsTicks;
    for (size_t i = 0; i < points.size(); ++i) {
        int64_t p = points[i].ptsTicks - first;
        if (p < 0) p = 0;
        if (i && p <= pts[i - 1]) p = pts[i - 1] + 1; // enforce strictly increasing PTS in the manifest
        pts[i] = p;
    }
    for (size_t i = 0; i + 1 < points.size(); ++i)
        durations[i] = std::max<int64_t>(1, pts[i + 1] - pts[i]);
    int64_t finalDuration = endPtsTicks - points.back().ptsTicks;
    if (finalDuration <= 0 && points.size() > 1)
        finalDuration = points.back().ptsTicks - points[points.size() - 2].ptsTicks;
    if (finalDuration <= 0 && fpsNum > 0 && fpsDen > 0)
        finalDuration = std::max<int64_t>(1, MulDiv64(g_freq, fpsDen, fpsNum));
    durations.back() = std::max<int64_t>(1, finalDuration);

    try {
        const std::filesystem::path base(dir);
        const std::filesystem::path csvPath = base / L"frame_timestamps.csv";
        const std::filesystem::path csvTmp = base / L"frame_timestamps.csv.tmp";
        std::ofstream csv(csvTmp, std::ios::binary | std::ios::trunc);
        if (!csv) { Log(L"TIMING: cannot create frame_timestamps.csv.tmp"); return false; }
        csv << "frame_index,pts_seconds,duration_seconds\n";
        csv << std::fixed << std::setprecision(9);
        for (size_t i = 0; i < points.size(); ++i) {
            csv << points[i].idx << ','
                << (long double)pts[i] / (long double)g_freq << ','
                << (long double)durations[i] / (long double)g_freq << '\n';
        }
        csv.close();
        if (!csv) { Log(L"TIMING: writing CSV failed"); return false; }
        if (!MoveFileExW(csvTmp.c_str(), csvPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            Log(L"TIMING: committing CSV failed Win32 error %lu", GetLastError()); return false;
        }

        // FFmpeg concat-demuxer input with per-frame durations; unlike -framerate N,
        // this preserves the captured game-time cadence, including slow frames/slips.
        const std::filesystem::path ffPath = base / L"frames.ffconcat";
        const std::filesystem::path ffTmp = base / L"frames.ffconcat.tmp";
        std::ofstream ff(ffTmp, std::ios::binary | std::ios::trunc);
        if (!ff) { Log(L"TIMING: cannot create frames.ffconcat.tmp"); return false; }
        ff << "ffconcat version 1.0\n";
        ff << std::fixed << std::setprecision(9);
        for (size_t i = 0; i < points.size(); ++i) {
            wchar_t name[64]; swprintf_s(name, L"frame_%06lld.jpg", (long long)points[i].idx);
            char asciiName[64] = {};
            WideCharToMultiByte(CP_UTF8, 0, name, -1, asciiName, (int)sizeof(asciiName), nullptr, nullptr);
            ff << "file '" << asciiName << "'\n";
            ff << "option framerate 1000000\n";
            ff << "duration " << (long double)durations[i] / (long double)g_freq << "\n";
        }
        // Repeating the last still lets the concat demuxer honor the final duration.
        wchar_t lastName[64]; swprintf_s(lastName, L"frame_%06lld.jpg", (long long)points.back().idx);
        char asciiLast[64] = {};
        WideCharToMultiByte(CP_UTF8, 0, lastName, -1, asciiLast, (int)sizeof(asciiLast), nullptr, nullptr);
        ff << "file '" << asciiLast << "'\n";
        ff << "option framerate 1000000\n";
        ff.close();
        if (!ff) { Log(L"TIMING: writing FFconcat file failed"); return false; }
        if (!MoveFileExW(ffTmp.c_str(), ffPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            Log(L"TIMING: committing FFconcat file failed Win32 error %lu", GetLastError()); return false;
        }
        Log(L"TIMING: wrote %llu frame timestamps (%s) -> %s\\frame_timestamps.csv and frames.ffconcat",
            (unsigned long long)points.size(), realtime ? L"real-time game clock" : L"locked simulation clock", dir.c_str());
        return true;
    } catch (...) {
        Log(L"TIMING: exception while writing timing sidecars");
        return false;
    }
}

static DWORD WINAPI WorkerThread(LPVOID) {
    HRESULT hc = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* fac = nullptr;
    HRESULT hf = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac));
    Log(L"Encoder thread: CoInitializeEx hr=0x%08X, WIC factory hr=0x%08X", (unsigned)hc, (unsigned)hf);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::vector<uint8_t> bgr;
    std::vector<TimingPoint> takeTiming; int timingTakeId = -1;
    for (;;) {
        Job j;
        { std::unique_lock<std::mutex> l(g_jMx); g_jCv.wait(l, [] { return !g_jobs.empty(); }); j = std::move(g_jobs.front()); g_jobs.pop_front(); }
        g_jCv.notify_all();
        if (j.end) {
            if (timingTakeId == j.takeId) WriteTimingFiles(j.dir, takeTiming, j.endPtsTicks, j.fpsNum, j.fpsDen, j.realtime);
            else Log(L"TIMING: no worker timing records found for take %d", j.takeId);
            if (g_failedTake != j.takeId) {
                wchar_t m[700];
                if (j.realtime) swprintf_s(m, L"Finished (real-time): %lld game frames in %.2f s (%.2f fps measured); see frames.ffconcat for correct speed -> %s",
                    (long long)j.endCount, j.wallSec, j.wallSec > 0 ? j.endCount / j.wallSec : 0.0, j.dir.c_str());
                else swprintf_s(m, L"Finished (locked clock): %lld frames = %lld simulation steps at %lld/%lld fps, %lld clock slips; timing saved -> %s",
                    (long long)j.endCount, (long long)j.endCount, (long long)j.fpsNum, (long long)j.fpsDen, g_sh ? (long long)g_sh->slipEvents : 0LL, j.dir.c_str());
                SetStatus(m);
            }
            if (j.testTarget > 0) VerifyTake(fac, j.dir, j.testTarget);
            takeTiming.clear(); timingTakeId = -1;
            continue;
        }
        if (timingTakeId != j.takeId) { takeTiming.clear(); timingTakeId = j.takeId; }
        if (j.takeId == g_failedTake) { if (j.slot) ReleaseSlot(j.slot); continue; }   // keep output contiguous after a failure
        wchar_t path[MAX_PATH + 32]; swprintf_s(path, L"%s\\frame_%06lld.jpg", j.dir.c_str(), (long long)j.idx);
        bool v = j.idx < 2;
        const wchar_t* stage = L"WIC factory"; HRESULT e = hf; bool ok = false; bool slotUnsafe = false;
        do {
            if (!fac) break;
            const uint8_t* src = j.raw.data(); size_t pitch = j.pitch; void* mapped = nullptr;
            if (j.slot) {
                stage = L"D3D12 copy fence wait"; e = S_OK;
                if (j.fence->GetCompletedValue() < j.fenceVal) {
                    e = j.fence->SetEventOnCompletion(j.fenceVal, ev);
                    if (FAILED(e)) { stage = L"D3D12 SetEventOnCompletion"; break; }
                    if (WaitForSingleObject(ev, 10000) != WAIT_OBJECT_0) {
                        e = j.dev ? j.dev->GetDeviceRemovedReason() : E_FAIL; if (SUCCEEDED(e)) e = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
                        Log(L"[D3D12] fence timeout: completed=%llu wanted=%llu", j.fence->GetCompletedValue(), j.fenceVal);
                        slotUnsafe = true; break;
                    }
                }
                if (v) Log(L"[D3D12] frame %lld: copy fence %llu completed", (long long)j.idx, j.fenceVal);
                stage = L"D3D12 readback Map";
                D3D12_RANGE r = { (SIZE_T)j.offset, (SIZE_T)j.size };
                e = j.slot->rb->Map(0, &r, &mapped); if (FAILED(e)) break;
                src = (const uint8_t*)mapped + j.offset;
                if (v) Log(L"[D3D12] frame %lld: Map OK (rowPitch %zu, %llu bytes)", (long long)j.idx, pitch, j.size);
            }
            stage = L"pixel conversion";
            bgr.resize((size_t)j.w * j.h * 3);
            Convert(j.pf, src, pitch, j.w, j.h, bgr.data());
            if (mapped) { D3D12_RANGE wr = { 0, 0 }; j.slot->rb->Unmap(0, &wr); }
            if (j.slot) { ReleaseSlot(j.slot); j.slot = nullptr; }
            if (v) Log(L"frame %lld: converted %dx%d (pf %d) -> BGR24", (long long)j.idx, j.w, j.h, (int)j.pf);
            ok = EncodeJpeg(fac, path, j.w, j.h, bgr.data(), v, e, stage);
        } while (0);
        if (j.slot) {
            if (slotUnsafe) { std::lock_guard<std::mutex> l(g_slotMx); j.slot->rb = nullptr; j.slot->al = nullptr; j.slot->cl = nullptr; j.slot->size = 0; } // GPU may still use them: leak, never reuse
            ReleaseSlot(j.slot);
        }
        if (ok) {
            takeTiming.push_back({ j.idx, j.ptsTicks });
            if (g_sh) { InterlockedExchange64(&g_sh->framesSaved, j.idx + 1); g_sh->width = j.w; g_sh->height = j.h; }
            if (v || j.idx % 100 == 0) Log(L"Saved %s", path);
        } else {
            Log(L"FAIL frame %lld path %s", (long long)j.idx, path);
            ReportAsyncFailure(j.takeId, j.idx, stage, e);
        }
    }
}

// ============================================================ Direct3D 11 capture (staging ring, non-blocking Map)
struct D11Ent { ID3D11Texture2D* tex = nullptr; int64_t idx = 0; int takeId = 0; int64_t ptsTicks = 0; std::wstring dir; };
static struct { ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; D11Ent e[3]; int head = 0, count = 0;
                D3D11_TEXTURE2D_DESC desc = {}; ID3D11Texture2D* resolved = nullptr; PF pf = PF::BGRA8; } g11;

// Map the oldest pending staging copy. Returns 1 = queued, 0 = not ready, -1 = error.
static int D11CollectOne(bool block) {
    if (!g11.count) return 0;
    D11Ent& e = g11.e[g11.head];
    D3D11_MAPPED_SUBRESOURCE m = {};
    HRESULT hr = g11.ctx->Map(e.tex, 0, D3D11_MAP_READ, block ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return 0;
    g11.head = (g11.head + 1) % 3; g11.count--;
    if (FAILED(hr)) {
        Log(L"[D3D11] staging Map failed hr=0x%08X, device removed reason 0x%08X", (unsigned)hr, (unsigned)g11.dev->GetDeviceRemovedReason());
        if (g_failedTake != e.takeId) ReportAsyncFailure(e.takeId, e.idx, L"D3D11 staging Map", hr);
        return -1;
    }
    Job j; j.takeId = e.takeId; j.idx = e.idx; j.ptsTicks = e.ptsTicks; j.dir = e.dir; j.w = (int)g11.desc.Width; j.h = (int)g11.desc.Height; j.pf = g11.pf;
    j.pitch = (size_t)j.w * BppOf(j.pf); j.raw.resize(j.pitch * j.h);
    for (int y = 0; y < j.h; y++) memcpy(j.raw.data() + (size_t)y * j.pitch, (const uint8_t*)m.pData + (size_t)y * m.RowPitch, j.pitch);
    g11.ctx->Unmap(e.tex, 0);
    if (e.idx < 2) Log(L"[D3D11] frame %lld: staging Map OK (%s, rowPitch %u)", (long long)e.idx, block ? L"blocking" : L"ready", m.RowPitch);
    Enqueue(std::move(j));
    return 1;
}
static bool D11Drain() { bool ok = true; while (g11.count) if (D11CollectOne(true) < 0) ok = false; return ok; }
static void D11ReleaseRing() {
    for (auto& e : g11.e) if (e.tex) { e.tex->Release(); e.tex = nullptr; }
    if (g11.resolved) { g11.resolved->Release(); g11.resolved = nullptr; }
    g11.head = g11.count = 0; g11.desc = {};
}
static bool SubmitD3D11(IDXGISwapChain* sc, ID3D11Device* dev, int64_t idx, bool v, const wchar_t*& stage, HRESULT& hr) {
    ID3D11Texture2D* bb = nullptr; bool ok = false;
    do {
        if (g11.dev != dev) {
            if (g11.dev) { D11Drain(); D11ReleaseRing(); g11.ctx->Release(); g11.dev->Release(); }
            dev->AddRef(); g11.dev = dev; dev->GetImmediateContext(&g11.ctx);
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
            D11Drain(); D11ReleaseRing(); g11.desc = d; g11.pf = pf;
        }
        stage = L"D3D11 collect finished copies";
        int r; while ((r = D11CollectOne(false)) > 0) {}
        if (r < 0) { hr = E_FAIL; break; }
        if (g11.count == 3) {                      // ring full: backpressure (wait for the oldest copy)
            int64_t t0 = RealQPC();
            if (D11CollectOne(true) < 0) { hr = E_FAIL; break; }
            Log(L"Backpressure: D3D11 waited %.1f ms for GPU copy (frame %lld)", RealMs(RealQPC() - t0), (long long)idx);
        }
        int slot = (g11.head + g11.count) % 3; D11Ent& e = g11.e[slot];
        DXGI_FORMAT copyFmt = d.SampleDesc.Count > 1 ? Typed(d.Format) : d.Format;
        if (!e.tex) {
            D3D11_TEXTURE2D_DESC sd = d; sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0; sd.Usage = D3D11_USAGE_STAGING; sd.Format = copyFmt;
            sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0; sd.MipLevels = 1; sd.ArraySize = 1;
            stage = L"D3D11 CreateTexture2D(staging)"; hr = dev->CreateTexture2D(&sd, nullptr, &e.tex); if (FAILED(hr)) break;
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
        g11.ctx->CopyResource(e.tex, src);
        e.idx = idx; e.takeId = g_takeId; e.ptsTicks = g_capturePtsTicks; e.dir = g_takeDir; g11.count++;
        if (v) Log(L"[D3D11] frame %lld: CopyResource -> staging slot %d submitted", (long long)idx, slot);
        ok = true;
    } while (0);
    if (bb) bb->Release();
    return ok;
}

// ============================================================ Direct3D 12 capture (readback ring + fence)
static std::mutex g_qMx;
static ID3D12CommandQueue* g_lastQueue = nullptr;
static struct { ID3D12Device* dev = nullptr; ID3D12Fence* fence = nullptr; UINT64 fv = 0; D12Slot s[3]; int next = 0; } g12;

static void D12WaitIdle() {
    std::unique_lock<std::mutex> l(g_slotMx);
    g_slotCv.wait(l, [] { for (auto& s : g12.s) if (s.busy) return false; return true; });
}
static HRESULT D12Ensure(ID3D12Device* dev, const wchar_t*& stage) {
    if (g12.dev == dev) return S_OK;
    D12WaitIdle();
    for (auto& s : g12.s) { if (s.cl) s.cl->Release(); if (s.al) s.al->Release(); if (s.rb) s.rb->Release(); s = D12Slot(); }
    if (g12.fence) g12.fence->Release(); g12.fence = nullptr;
    if (g12.dev) g12.dev->Release(); g12.dev = nullptr;
    stage = L"D3D12 CreateFence";
    HRESULT hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g12.fence)); if (FAILED(hr)) return hr;
    dev->AddRef(); g12.dev = dev; g12.fv = 0; g12.next = 0;
    Log(L"[D3D12] capture context created for device %p", dev);
    return S_OK;
}
static bool SubmitD3D12(IDXGISwapChain* sc, int64_t idx, bool v, const wchar_t*& stage, HRESULT& hr) {
    IDXGISwapChain3* sc3 = nullptr; ID3D12CommandQueue* q = nullptr; ID3D12Device* dev = nullptr; ID3D12Resource* bb = nullptr;
    D12Slot* s = nullptr; bool open = false, ok = false;
    do {
        stage = L"QueryInterface(IDXGISwapChain3)"; hr = sc->QueryInterface(IID_PPV_ARGS(&sc3)); if (FAILED(hr)) break;
        // The swapchain's own queue: the copy is then ordered after the game's frame and before Present.
        stage = L"SwapChain::GetDevice(ID3D12CommandQueue)"; hr = sc->GetDevice(IID_PPV_ARGS(&q));
        if (FAILED(hr) || !q) {
            HRESULT h0 = hr;
            { std::lock_guard<std::mutex> l(g_qMx); q = g_lastQueue; if (q) q->AddRef(); }
            if (!q) { stage = L"find D3D12 queue (not D3D11/D3D12 swapchain?)"; if (SUCCEEDED(hr)) hr = E_NOINTERFACE; break; }
            Log(L"[D3D12] WARNING: GetDevice(queue) hr=0x%08X; falling back to last DIRECT queue %p seen in ExecuteCommandLists (may not be the present queue)", (unsigned)h0, q);
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
        stage = L"D3D12 MSAA back buffer"; if (rd.SampleDesc.Count > 1) { hr = E_NOTIMPL; break; }
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {}; UINT64 total = 0;
        dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
        s = &g12.s[g12.next];
        {   // backpressure: wait until the worker has finished with this slot (its fence completed + unmapped)
            std::unique_lock<std::mutex> l(g_slotMx);
            if (s->busy) { int64_t t0 = RealQPC(); g_slotCv.wait(l, [&] { return !s->busy; }); Log(L"Backpressure: D3D12 waited %.1f ms for readback slot (frame %lld)", RealMs(RealQPC() - t0), (long long)idx); }
        }
        if (s->size < total) {
            if (s->rb) { s->rb->Release(); s->rb = nullptr; } s->size = 0;
            D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1;
            bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            stage = L"D3D12 CreateCommittedResource(READBACK)";
            hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s->rb)); if (FAILED(hr)) break;
            s->size = total;
        }
        if (!s->al) {
            stage = L"D3D12 CreateCommandAllocator"; hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s->al)); if (FAILED(hr)) break;
            stage = L"D3D12 CreateCommandList"; hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s->al, nullptr, IID_PPV_ARGS(&s->cl)); if (FAILED(hr)) break;
            s->cl->Close();
        }
        // Slot is idle => its previous copy fence completed, so resetting the allocator is safe.
        stage = L"D3D12 CommandAllocator::Reset"; hr = s->al->Reset(); if (FAILED(hr)) break;
        stage = L"D3D12 CommandList::Reset"; hr = s->cl->Reset(s->al, nullptr); if (FAILED(hr)) break;
        open = true;
        D3D12_RESOURCE_BARRIER b = {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = bb; b.Transition.Subresource = 0;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        s->cl->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = s->rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION srcl = {}; srcl.pResource = bb; srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; srcl.SubresourceIndex = 0;
        s->cl->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;   // restore for Present
        s->cl->ResourceBarrier(1, &b);
        stage = L"D3D12 CommandList::Close"; hr = s->cl->Close(); open = false; if (FAILED(hr)) break;
        ID3D12CommandList* lists[] = { s->cl };
        q->ExecuteCommandLists(1, lists);
        UINT64 val = ++g12.fv;
        stage = L"D3D12 CommandQueue::Signal"; hr = q->Signal(g12.fence, val);
        { std::lock_guard<std::mutex> l(g_slotMx); s->busy = true; }   // copy is in flight from here on: never reset/reuse before fence
        if (FAILED(hr)) { Log(L"[D3D12] Signal failed: slot %d retired (GPU may still use it)", g12.next); s->rb = nullptr; s->al = nullptr; s->cl = nullptr; s->size = 0; ReleaseSlot(s); break; }
        if (v) Log(L"[D3D12] frame %lld: barrier PRESENT->COPY_SOURCE->PRESENT, CopyTextureRegion %ux%u rowPitch %u -> slot %d submitted, fence value %llu",
            (long long)idx, fp.Footprint.Width, fp.Footprint.Height, fp.Footprint.RowPitch, g12.next, val);
        Job j; j.takeId = g_takeId; j.idx = idx; j.dir = g_takeDir; j.w = (int)rd.Width; j.h = (int)rd.Height; j.pf = pf;
        j.pitch = fp.Footprint.RowPitch; j.slot = s; j.fence = g12.fence; j.dev = g12.dev; j.fenceVal = val; j.offset = fp.Offset; j.size = total; j.ptsTicks = g_capturePtsTicks;
        g12.next = (g12.next + 1) % 3;
        Enqueue(std::move(j));
        ok = true;
    } while (0);
    if (open && s && s->cl) s->cl->Close();
    if (bb) bb->Release(); if (dev) dev->Release(); if (q) q->Release(); if (sc3) sc3->Release();
    return ok;
}

static bool SubmitDXGI(IDXGISwapChain* sc, int64_t idx, bool v, const wchar_t*& stage, HRESULT& hr) {
    ID3D11Device* d11 = nullptr;
    hr = sc->GetDevice(__uuidof(ID3D11Device), (void**)&d11);
    if (SUCCEEDED(hr) && d11) { SetApi(L"Direct3D 11"); g_api = Api::D11; bool r = SubmitD3D11(sc, d11, idx, v, stage, hr); d11->Release(); return r; }
    if (v) Log(L"SwapChain::GetDevice(ID3D11Device) hr=0x%08X -> trying D3D12", (unsigned)hr);
    SetApi(L"Direct3D 12"); g_api = Api::D12;
    return SubmitD3D12(sc, idx, v, stage, hr);
}
// D3D9 / OpenGL: synchronous GPU readback (unchanged), encoding moved to the worker.
static bool QueueCpuFrame(Frame& f, int64_t idx) {
    if (f.w <= 0 || f.h <= 0) return false;
    Job j; j.takeId = g_takeId; j.idx = idx; j.ptsTicks = g_capturePtsTicks; j.dir = g_takeDir; j.w = f.w; j.h = f.h; j.pf = PF::BGR24; j.pitch = (size_t)f.w * 3; j.raw = std::move(f.bgr);
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
static void PollHotkeys() {
    static bool p9 = false, p10 = false;
    bool d9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0, d10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (d9 && !p9) InterlockedExchange(&g_sh->wantRecord, g_sh->wantRecord ? 0 : 1);
    if (d10 && !p10 && !g_rec) { InterlockedExchange(&g_sh->testFrames, 120); InterlockedExchange(&g_sh->wantRecord, 1); }
    p9 = d9; p10 = d10;
}
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
    g_takeStartReal = RealQPC();
    g_frameTimeBase = g_takeStartReal + g_offset.load(std::memory_order_relaxed);
    g_capturePtsTicks = 0;
    g_rec = true; InterlockedExchange(&g_sh->isRecording, 1);
    wchar_t m[700];
    swprintf_s(m, L"TAKE START: mode=%s, fps=%ld/%ld, test=%lld frames, target=%p -> %s",
        g_lockMode ? L"LOCKED (offline export, 1 step per frame)" : L"REAL-TIME (only frames the game presents)", n, d, (long long)g_testTarget, obj, dir);
    SetStatus(m);
    return true;
}
static void StopTake(const wchar_t* why) {
    // Snapshot the final presentation time before draining readbacks or unlocking the clock.
    int64_t endPtsTicks = VirtQPC() - g_frameTimeBase;
    if (endPtsTicks < 0) endPtsTicks = 0;
    int64_t wallEnd = RealQPC();
    if (g_api == Api::D11 && !D11Drain()) D11ReleaseRing();   // queue remaining in-flight D3D11 copies
    ClockStop();
    double wall = (wallEnd - g_takeStartReal) / (double)g_freq;
    g_rec = false; g_target = nullptr;
    InterlockedExchange(&g_sh->isRecording, 0); InterlockedExchange(&g_sh->wantRecord, 0);
    Log(L"TAKE STOP (%s): %lld frames queued, waiting for encoder", why, (long long)g_nextIdx);
    Job e; e.end = true; e.takeId = g_takeId; e.dir = g_takeDir; e.endCount = g_nextIdx; e.testTarget = g_testTarget; e.realtime = !g_lockMode; e.wallSec = wall;
    e.endPtsTicks = endPtsTicks;
    e.fpsNum = g_fpsNum; e.fpsDen = g_fpsDen;
    Enqueue(std::move(e));
}

// Before the original Present. Returns 0 = nothing, 1 = take started, 2 = frame queued.
// Cap: bool(int64_t idx, bool verbose, const wchar_t*& stage, HRESULT& hr) - submits capture of the completed frame.
template <class Cap> static int FrameStepPre(void* obj, Cap& cap) {
    if (!g_sh) return 0;
    std::lock_guard<std::mutex> lk(g_stepMx);
    PollHotkeys();
    bool want = InterlockedCompareExchange(&g_sh->wantRecord, 0, 0) != 0;
    if (!g_rec) return (want && StartTake(obj)) ? 1 : 0;
    if (obj != g_target) return 0;                       // other windows/swapchains
    if (g_asyncFail) { StopTake(L"async failure"); return 0; }
    if (!want) { StopTake(L"user"); return 0; }
    int64_t n = g_nextIdx;
    g_capturePtsTicks = VirtQPC() - g_frameTimeBase;
    if (g_capturePtsTicks < 0) g_capturePtsTicks = 0;
    const wchar_t* stage = L"capture"; HRESULT hr = E_FAIL;
    if (!cap(n, n < 2, stage, hr)) {                    // nothing queued: index and clock NOT advanced
        InterlockedExchange(&g_sh->lastHr, (LONG)hr);
        wchar_t m[512]; swprintf_s(m, L"Capture of frame_%06lld failed at stage '%s' (HRESULT 0x%08X), attempt %d/3", (long long)n, stage, (unsigned)hr, g_fails + 1);
        SetStatus(m);
        if (++g_fails >= 3) { SetStatus(L"ERROR: capture failed 3 times in a row - recording stopped (see previous line for stage/HRESULT). No frame index was skipped."); StopTake(L"capture failure"); }
        return 0;
    }
    g_fails = 0;
    g_nextIdx = n + 1;                                   // advance only after the frame is safely queued
    InterlockedExchange64(&g_sh->framesQueued, n + 1);
    if (g_testTarget > 0 && n + 1 >= g_testTarget) InterlockedExchange(&g_sh->wantRecord, 0);   // stops at next Present
    return 2;
}
static void FrameStepPost(int r) {
    if (!g_lockMode) return;                             // real-time: clock never touched
    if (r == 1) ClockStart();                            // step 0 starts after the start Present
    else if (r == 2 && g_rec) ClockAdvance();            // exactly one step per queued frame
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

static HRESULT STDMETHODCALLTYPE Hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (flags & DXGI_PRESENT_TEST) return o_Present(sc, sync, flags);
    return PresentWrap(sc, [&](int64_t i, bool v, const wchar_t*& s, HRESULT& h) { return SubmitDXGI(sc, i, v, s, h); }, [&] { return o_Present(sc, sync, flags); });
}
static HRESULT STDMETHODCALLTYPE Hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) {
    if (flags & DXGI_PRESENT_TEST) return o_Present1(sc, sync, flags, pp);
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
static DWORD WINAPI InitThread(LPVOID) {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    swprintf_s(g_logPath, L"%sFrameLock_%lu.log", tmp, GetCurrentProcessId());
    OpenShared();
    if (!g_sh) { Log(L"Shared memory failed"); return 0; }
    InitLut();
    CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
    if (MH_Initialize() != MH_OK) { SetStatus(L"ERROR: MinHook init failed"); return 0; }
    InstallTimeHooks();
    InterlockedExchange(&g_sh->hooksReady, 1);
    SetStatus(L"Hooks active (v2). Waiting for the game to render... Record = F9, 120-frame self-test = F10.");
    bool dxgi = false, d12 = false, d9 = false, gl = false; int tDxgi = 0, tD12 = 0, tD9 = 0, tGl = 0;
    for (;;) {   // graphics DLLs may be loaded later: keep scanning
        if (!dxgi && tDxgi < 5 && GetModuleHandleW(L"dxgi.dll")) { dxgi = HookDXGI(); tDxgi++; }
        if (!d12 && tD12 < 5 && GetModuleHandleW(L"d3d12.dll")) { d12 = HookD3D12Queue(); tD12++; }
        if (!d9 && tD9 < 5 && GetModuleHandleW(L"d3d9.dll")) { d9 = HookD3D9(); tD9++; }
        if (!gl && tGl < 5 && GetModuleHandleW(L"opengl32.dll")) { gl = HookGL(); tGl++; }
        if (GetModuleHandleW(L"vulkan-1.dll") && !dxgi && !d9 && !gl) SetApi(L"Vulkan (capture not supported in this version)");
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
