// FrameLock hook DLL - controlled game clock + lossless frame-by-frame capture.
// Injected into the game. Virtualizes the common Windows time sources and steps
// them by exactly 1/FPS per presented frame while recording. Each frame is read
// back from the back buffer and written as a max-quality JPEG BEFORE the game is
// allowed to advance, so frames are never skipped or duplicated.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <d3d9.h>
#include <d3d10.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wincodec.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <oleauto.h>
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
    wchar_t buf[1024];
    va_list a; va_start(a, fmt); _vsnwprintf_s(buf, _TRUNCATE, fmt, a); va_end(a);
    std::lock_guard<std::mutex> l(g_logMx);
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_logPath, L"a, ccs=UTF-8") == 0 && f) {
        SYSTEMTIME st; GetLocalTime(&st);
        fwprintf(f, L"[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
        fclose(f);
    }
}
static void SetStatus(const wchar_t* s) { if (g_sh) wcsncpy_s(g_sh->status, s, _TRUNCATE); Log(L"%s", s); }
static void SetApi(const wchar_t* s) { if (g_sh && wcscmp(g_sh->api, s) != 0) wcsncpy_s(g_sh->api, s, _TRUNCATE); }

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

static std::atomic<bool>    g_rec{ false };      // recording -> clock is frozen/stepped
static std::atomic<int64_t> g_offset{ 0 };       // virtual - real while not recording
static int64_t              g_recBase = 0;       // virtual QPC at frame 0
static std::atomic<int64_t> g_frame{ 0 };        // current frame index
static int64_t              g_fpsNum = 24, g_fpsDen = 1;

static int64_t MulDiv64(int64_t a, int64_t b, int64_t c) { return (a / c) * b + ((a % c) * b) / c; }

static int64_t RealQPC() {
    LARGE_INTEGER li;
    if (o_RtlQPC) o_RtlQPC(&li);
    else if (o_QPC_kb) o_QPC_kb(&li);
    else if (o_QPC_k32) o_QPC_k32(&li);
    else QueryPerformanceCounter(&li);
    return li.QuadPart;
}
// exact integer tick position of frame n: floor(n * freq * den / num).
// Split quotient/remainder terms to avoid overflowing n * freq * den.
static int64_t FrameTicks(int64_t n) {
    const int64_t numerator = g_freq * g_fpsDen;
    const int64_t denominator = g_fpsNum;
    const int64_t whole = numerator / denominator;
    const int64_t rem = numerator % denominator;
    return n * whole + (n / denominator) * rem + ((n % denominator) * rem) / denominator;
}

static int64_t VirtQPC() {
    if (!g_rec.load(std::memory_order_acquire))
        return RealQPC() + g_offset.load(std::memory_order_relaxed);
    // Strict mode: time is frozen at one frame boundary. It advances only after
    // that frame has been captured and its JPEG has been committed to disk.
    // There is deliberately no watchdog time nudge: any such nudge would violate
    // exact 1/FPS stepping. Games that busy-wait for time to pass may stall here.
    return g_recBase + FrameTicks(g_frame.load(std::memory_order_acquire));
}
static int64_t VirtDelta(int64_t unitsPerSec) {
    int64_t d = VirtQPC() - g_qpc0;
    return d >= 0 ? MulDiv64(d, unitsPerSec, g_freq) : -MulDiv64(-d, unitsPerSec, g_freq);
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

// ============================================================ JPEG writer (WIC, quality 1.0, 4:4:4)
struct SaveJob { const wchar_t* path; int w; int h; const uint8_t* data; bool done; bool ok; };
static std::mutex g_wMx;
static std::condition_variable g_wCv;
static SaveJob* g_job = nullptr;

static bool EncodeJpeg(IWICImagingFactory* fac, const SaveJob& j) {
    IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr; IPropertyBag2* bag = nullptr;
    bool ok = false;
    do {
        if (FAILED(fac->CreateStream(&st))) break;
        if (FAILED(st->InitializeFromFilename(j.path, GENERIC_WRITE))) break;
        if (FAILED(fac->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc))) break;
        if (FAILED(enc->Initialize(st, WICBitmapEncoderNoCache))) break;
        if (FAILED(enc->CreateNewFrame(&fr, &bag))) break;
        PROPBAG2 opt = {}; VARIANT v; VariantInit(&v);
        opt.pstrName = (LPOLESTR)L"ImageQuality"; v.vt = VT_R4; v.fltVal = 1.0f; bag->Write(1, &opt, &v);
        opt.pstrName = (LPOLESTR)L"JpegYCrCbSubsampling"; v.vt = VT_UI1; v.bVal = (BYTE)WICJpegYCrCbSubsampling444; bag->Write(1, &opt, &v);
        if (FAILED(fr->Initialize(bag))) break;
        if (FAILED(fr->SetSize((UINT)j.w, (UINT)j.h))) break;
        WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
        if (FAILED(fr->SetPixelFormat(&pf))) break;
        UINT stride = (UINT)j.w * 3;
        if (FAILED(fr->WritePixels((UINT)j.h, stride, stride * (UINT)j.h, (BYTE*)j.data))) break;
        if (FAILED(fr->Commit())) break;
        if (FAILED(enc->Commit())) break;
        ok = true;
    } while (0);
    if (bag) bag->Release(); if (fr) fr->Release(); if (enc) enc->Release(); if (st) st->Release();
    return ok;
}
static DWORD WINAPI WriterThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* fac = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac));
    if (!fac) Log(L"WIC factory creation failed");
    for (;;) {
        SaveJob* j;
        { std::unique_lock<std::mutex> l(g_wMx); g_wCv.wait(l, [] { return g_job && !g_job->done; }); j = g_job; }
        bool ok = fac && EncodeJpeg(fac, *j);
        { std::lock_guard<std::mutex> l(g_wMx); j->ok = ok; j->done = true; g_job = nullptr; }
        g_wCv.notify_all();
    }
}
// Blocks the calling (render) thread until the JPEG is fully written to disk.
static bool SaveJpegSync(const wchar_t* path, int w, int h, const uint8_t* data) {
    SaveJob j = { path, w, h, data, false, false };
    std::unique_lock<std::mutex> l(g_wMx);
    g_wCv.wait(l, [] { return g_job == nullptr; });
    g_job = &j;
    g_wCv.notify_all();
    g_wCv.wait(l, [&] { return j.done; });
    return j.ok;
}

// ============================================================ pixel conversion -> BGR24
struct Frame {
    int w = 0, h = 0; std::vector<uint8_t> bgr;
    void alloc(int W, int H) { w = W; h = H; bgr.resize((size_t)W * H * 3); }
};
enum class PF { BGRA8, RGBA8, RGB10A2, RGBA16F, D9_A2R10G10B10, BGR565 };

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

// Flip-model swap chains rotate their render target. GetBuffer(0) is not
// necessarily the buffer currently being rendered, so query the current index
// when IDXGISwapChain3 is available (legacy discard swap chains fall back to 0).
static UINT CurrentDXGIBackBufferIndex(IDXGISwapChain* sc) {
    IDXGISwapChain3* sc3 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) {
        UINT index = sc3->GetCurrentBackBufferIndex();
        sc3->Release();
        return index;
    }
    return 0;
}

// ============================================================ Direct3D 10 capture (DXGI swap chain)
static bool CaptureD3D10(IDXGISwapChain* sc, ID3D10Device* dev, Frame& fr) {
    ID3D10Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(CurrentDXGIBackBufferIndex(sc), __uuidof(ID3D10Texture2D), (void**)&bb)) || !bb) return false;
    D3D10_TEXTURE2D_DESC d; bb->GetDesc(&d);
    PF pf;
    if (!PFFromDXGI(d.Format, pf)) { bb->Release(); SetStatus(L"Unsupported back buffer format (D3D10)"); return false; }
    ID3D10Texture2D* src = bb;
    ID3D10Texture2D* resolved = nullptr;
    ID3D10Texture2D* staging = nullptr;
    bool ok = false;
    do {
        if (d.SampleDesc.Count > 1) {
            D3D10_TEXTURE2D_DESC rd = d;
            rd.SampleDesc.Count = 1; rd.SampleDesc.Quality = 0;
            rd.Format = Typed(d.Format); rd.Usage = D3D10_USAGE_DEFAULT;
            rd.BindFlags = 0; rd.CPUAccessFlags = 0; rd.MiscFlags = 0;
            rd.MipLevels = 1; rd.ArraySize = 1;
            if (FAILED(dev->CreateTexture2D(&rd, nullptr, &resolved))) break;
            dev->ResolveSubresource(resolved, 0, bb, 0, rd.Format);
            src = resolved;
        }
        D3D10_TEXTURE2D_DESC sd = d;
        sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0;
        sd.Usage = D3D10_USAGE_STAGING; sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D10_CPU_ACCESS_READ; sd.MiscFlags = 0;
        sd.MipLevels = 1; sd.ArraySize = 1;
        if (resolved) sd.Format = Typed(d.Format);
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging))) break;
        dev->CopyResource(staging, src);
        D3D10_MAPPED_TEXTURE2D mapped = {};
        if (FAILED(staging->Map(0, D3D10_MAP_READ, 0, &mapped))) break;
        fr.alloc((int)d.Width, (int)d.Height);
        Convert(pf, (const uint8_t*)mapped.pData, mapped.RowPitch, fr.w, fr.h, fr.bgr.data());
        staging->Unmap(0);
        ok = true;
    } while (0);
    if (staging) staging->Release();
    if (resolved) resolved->Release();
    bb->Release();
    return ok;
}

// ============================================================ Direct3D 11 capture
static bool CaptureD3D11(IDXGISwapChain* sc, ID3D11Device* dev, Frame& fr) {
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(CurrentDXGIBackBufferIndex(sc), __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return false;
    D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
    PF pf; if (!PFFromDXGI(d.Format, pf)) { bb->Release(); SetStatus(L"Unsupported back buffer format (D3D11)"); return false; }
    ID3D11DeviceContext* ctx = nullptr; dev->GetImmediateContext(&ctx);
    if (!ctx) { bb->Release(); return false; }
    ID3D11Texture2D* src = bb; ID3D11Texture2D* resolved = nullptr; ID3D11Texture2D* st = nullptr;
    bool ok = false;
    do {
        if (d.SampleDesc.Count > 1) {
            D3D11_TEXTURE2D_DESC rd = d; rd.SampleDesc.Count = 1; rd.SampleDesc.Quality = 0; rd.Format = Typed(d.Format);
            rd.Usage = D3D11_USAGE_DEFAULT; rd.BindFlags = 0; rd.CPUAccessFlags = 0; rd.MiscFlags = 0; rd.MipLevels = 1; rd.ArraySize = 1;
            if (FAILED(dev->CreateTexture2D(&rd, nullptr, &resolved))) break;
            ctx->ResolveSubresource(resolved, 0, bb, 0, rd.Format);
            src = resolved;
        }
        D3D11_TEXTURE2D_DESC sd = d; sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0; sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0; sd.MipLevels = 1; sd.ArraySize = 1;
        if (resolved) sd.Format = Typed(d.Format);
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, &st))) break;
        ctx->CopySubresourceRegion(st, 0, 0, 0, 0, src, 0, nullptr);
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) break;   // waits for the GPU to finish the frame
        fr.alloc((int)d.Width, (int)d.Height);
        Convert(pf, (const uint8_t*)m.pData, m.RowPitch, fr.w, fr.h, fr.bgr.data());
        ctx->Unmap(st, 0);
        ok = true;
    } while (0);
    if (st) st->Release(); if (resolved) resolved->Release(); if (ctx) ctx->Release(); bb->Release();
    return ok;
}

// ============================================================ Direct3D 12 capture
static std::mutex g_qMx;
static ID3D12CommandQueue* g_lastQueue = nullptr;

static bool CaptureD3D12(IDXGISwapChain* sc, ID3D12Device* dev, ID3D12CommandQueue* q, Frame& fr) {
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) return false;
    bool ok = false; bool ownQ = false;
    ID3D12Resource* bb = nullptr; ID3D12Resource* rb = nullptr; ID3D12CommandAllocator* al = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr; ID3D12Fence* fence = nullptr; HANDLE ev = nullptr;
    do {
        if (!q) { std::lock_guard<std::mutex> l(g_qMx); q = g_lastQueue; if (q) { q->AddRef(); ownQ = true; } }
        if (!q) { SetStatus(L"D3D12: command queue not found yet (wait a few frames)"); break; }
        UINT idx = sc3->GetCurrentBackBufferIndex();
        if (FAILED(sc3->GetBuffer(idx, IID_PPV_ARGS(&bb)))) break;
        D3D12_RESOURCE_DESC rd = bb->GetDesc();
        PF pf; if (!PFFromDXGI(rd.Format, pf)) { SetStatus(L"Unsupported back buffer format (D3D12)"); break; }
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT64 total = 0;
        dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))) break;
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al)))) break;
        if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, IID_PPV_ARGS(&cl)))) break;
        if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) break;
        D3D12_RESOURCE_BARRIER b = {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = bb; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION srcl = {}; srcl.pResource = bb; srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; srcl.SubresourceIndex = 0;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cl->ResourceBarrier(1, &b);
        if (FAILED(cl->Close())) break;
        ID3D12CommandList* lists[] = { cl };
        q->ExecuteCommandLists(1, lists);
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        q->Signal(fence, 1);
        if (fence->GetCompletedValue() < 1) { fence->SetEventOnCompletion(1, ev); WaitForSingleObject(ev, 10000); }
        if (fence->GetCompletedValue() < 1) break;
        void* p = nullptr; D3D12_RANGE r = { 0, (SIZE_T)total };
        if (FAILED(rb->Map(0, &r, &p))) break;
        fr.alloc((int)rd.Width, (int)rd.Height);
        Convert(pf, (const uint8_t*)p + fp.Offset, fp.Footprint.RowPitch, fr.w, fr.h, fr.bgr.data());
        D3D12_RANGE wr = { 0, 0 }; rb->Unmap(0, &wr);
        ok = true;
    } while (0);
    if (ev) CloseHandle(ev); if (fence) fence->Release(); if (cl) cl->Release(); if (al) al->Release();
    if (rb) rb->Release(); if (bb) bb->Release(); if (ownQ) q->Release(); sc3->Release();
    return ok;
}

static bool CaptureDXGI(IDXGISwapChain* sc, Frame& fr) {
    ID3D11Device* d11 = nullptr;
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&d11)) && d11) {
        SetApi(L"Direct3D 11"); bool r = CaptureD3D11(sc, d11, fr); d11->Release(); return r;
    }
    ID3D10Device* d10 = nullptr;
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D10Device), (void**)&d10)) && d10) {
        SetApi(L"Direct3D 10"); bool r = CaptureD3D10(sc, d10, fr); d10->Release(); return r;
    }
    ID3D12CommandQueue* q = nullptr; ID3D12Device* d12 = nullptr;
    sc->GetDevice(__uuidof(ID3D12CommandQueue), (void**)&q);
    if (q) q->GetDevice(IID_PPV_ARGS(&d12)); else sc->GetDevice(IID_PPV_ARGS(&d12));
    if (d12) { SetApi(L"Direct3D 12"); bool r = CaptureD3D12(sc, d12, q, fr); d12->Release(); if (q) q->Release(); return r; }
    if (q) q->Release();
    SetStatus(L"Unsupported DXGI device type");
    return false;
}

// ============================================================ Direct3D 9 capture
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
    auto validWglProc = [](PROC p) -> PROC {
        const intptr_t v = (intptr_t)p;
        return (!p || v == 1 || v == 2 || v == 3 || v == -1) ? nullptr : p;
    };
    auto glBindFramebuffer = (glBind_t)validWglProc(pGPA("glBindFramebuffer"));
    if (!glBindFramebuffer) glBindFramebuffer = (glBind_t)validWglProc(pGPA("glBindFramebufferEXT"));
    auto glBindBuffer = (glBind_t)validWglProc(pGPA("glBindBuffer"));
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
    if (glBindFramebuffer) glBindFramebuffer(READ_FB, (unsigned)prevFbo);
    glReadBuffer((unsigned)prevRead);
    if (glBindBuffer) glBindBuffer(PACK_BUF, (unsigned)prevPbo);
    fr.alloc(w, h);
    size_t row = (size_t)w * 3;
    for (int y = 0; y < h; y++) std::memcpy(fr.bgr.data() + (size_t)y * row, tmp.data() + (size_t)(h - 1 - y) * row, row);  // GL is bottom-up
    return true;
}

// ============================================================ frame stepping (the core)
static std::mutex g_stepMx;
static void* g_target = nullptr;     // swapchain/device/window being recorded
static int g_fails = 0;
static wchar_t g_takeDir[MAX_PATH];
static thread_local bool t_inPresent = false;

static void PollHotkey() {
    static bool prev = false;
    bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (down && !prev) InterlockedExchange(&g_sh->wantRecord, g_sh->wantRecord ? 0 : 1);
    prev = down;
}
static void StartRecording(void* obj) {
    LONG n = g_sh->fpsNum, d = g_sh->fpsDen;
    if (n <= 0 || d <= 0) { n = 24; d = 1; }
    g_fpsNum = n; g_fpsDen = d;
    wchar_t base[MAX_PATH];
    wcsncpy_s(base, g_sh->outDir, _TRUNCATE);
    if (!base[0]) { GetEnvironmentVariableW(L"USERPROFILE", base, MAX_PATH); wcscat_s(base, L"\\Videos\\FrameLock"); }
    SYSTEMTIME st; GetLocalTime(&st);
    swprintf_s(g_takeDir, L"%s\\Take_%04d%02d%02d_%02d%02d%02d", base, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    int r = SHCreateDirectoryExW(nullptr, g_takeDir, nullptr);
    if (r != ERROR_SUCCESS && r != ERROR_ALREADY_EXISTS) {
        SetStatus(L"ERROR: cannot create output folder"); InterlockedExchange(&g_sh->wantRecord, 0); return;
    }
    wcsncpy_s(g_sh->takeDir, g_takeDir, _TRUNCATE);
    InterlockedExchange64(&g_sh->framesSaved, 0);
    g_recBase = RealQPC() + g_offset.load();   // freeze the clock at "now" (continuous)
    g_frame.store(0);
    g_target = obj; g_fails = 0;
    g_rec.store(true, std::memory_order_release);
    InterlockedExchange(&g_sh->isRecording, 1);
    wchar_t msg[600]; swprintf_s(msg, L"Recording at %ld/%ld fps -> %s", n, d, g_takeDir); SetStatus(msg);
}
static void StopRecording(bool okMsg) {
    int64_t v = VirtQPC();
    g_offset.store(v - RealQPC());   // keep time continuous after unfreezing
    g_rec.store(false, std::memory_order_release);
    g_target = nullptr;
    InterlockedExchange(&g_sh->isRecording, 0);
    if (okMsg) {
        wchar_t msg[700]; swprintf_s(msg, L"Finished: %lld frames saved to %s", (long long)g_sh->framesSaved, g_takeDir); SetStatus(msg);
    }
}

// Called on every Present BEFORE the original Present.
template <class Cap> static void FrameStep(void* obj, Cap& cap) {
    if (!g_sh) return;
    std::lock_guard<std::mutex> lk(g_stepMx);
    PollHotkey();
    bool want = InterlockedCompareExchange(&g_sh->wantRecord, 0, 0) != 0;
    if (!g_rec.load()) { if (want) StartRecording(obj); return; }
    if (obj != g_target) return;                        // ignore other windows/swapchains
    Frame f;
    if (!cap(f) || f.w <= 0 || f.h <= 0) {               // do NOT advance time: frame will be retried
        if (++g_fails >= 3) {
            SetStatus(L"ERROR: frame capture failed 3 times in a row - recording stopped (no frame was skipped).");
            InterlockedExchange(&g_sh->wantRecord, 0); StopRecording(false);
        }
        return;
    }
    g_fails = 0;
    int64_t n = g_frame.load();
    wchar_t path[MAX_PATH + 32];
    swprintf_s(path, L"%s\\frame_%06lld.jpg", g_takeDir, (long long)n);
    if (!SaveJpegSync(path, f.w, f.h, f.bgr.data())) {   // blocks until the file is on disk
        SetStatus(L"ERROR: could not write JPEG (disk full / no permission?) - recording stopped.");
        InterlockedExchange(&g_sh->wantRecord, 0); StopRecording(false); return;
    }
    g_sh->width = f.w; g_sh->height = f.h;
    InterlockedExchange64(&g_sh->framesSaved, n + 1);
    g_frame.store(n + 1, std::memory_order_release);   // advance game time by exactly 1/FPS
    if (!want) StopRecording(true);
}

template <class Cap, class Call>
static auto PresentWrap(void* obj, Cap cap, Call call) -> decltype(call()) {
    if (t_inPresent) return call();          // nested present (e.g. Present -> Present1)
    t_inPresent = true;
    FrameStep(obj, cap);
    auto r = call();
    t_inPresent = false;
    return r;
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

static HRESULT STDMETHODCALLTYPE Hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (flags & DXGI_PRESENT_TEST) return o_Present(sc, sync, flags);
    return PresentWrap(sc, [&](Frame& f) { return CaptureDXGI(sc, f); }, [&] { return o_Present(sc, sync, flags); });
}
static HRESULT STDMETHODCALLTYPE Hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) {
    if (flags & DXGI_PRESENT_TEST) return o_Present1(sc, sync, flags, pp);
    return PresentWrap((IDXGISwapChain*)sc, [&](Frame& f) { return CaptureDXGI(sc, f); }, [&] { return o_Present1(sc, sync, flags, pp); });
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
    return PresentWrap(d, [&](Frame& f) { return CaptureD9Dev(d, f); }, [&] { return o_D9Present(d, s, r, w, rg); });
}
static HRESULT STDMETHODCALLTYPE Hk_D9Present2(IDirect3DDevice9* d, const RECT* s, const RECT* r, HWND w, const RGNDATA* rg) {
    return PresentWrap(d, [&](Frame& f) { return CaptureD9Dev(d, f); }, [&] { return o_D9Present2(d, s, r, w, rg); });
}
static HRESULT STDMETHODCALLTYPE Hk_D9PresentEx(IDirect3DDevice9Ex* d, const RECT* s, const RECT* r, HWND w, const RGNDATA* rg, DWORD fl) {
    return PresentWrap((IDirect3DDevice9*)d, [&](Frame& f) { return CaptureD9Dev(d, f); }, [&] { return o_D9PresentEx(d, s, r, w, rg, fl); });
}
static HRESULT STDMETHODCALLTYPE Hk_D9SwPresent(IDirect3DSwapChain9* sw, const RECT* s, const RECT* r, HWND w, const RGNDATA* rg, DWORD fl) {
    IDirect3DDevice9* dev = nullptr; sw->GetDevice(&dev); if (dev) dev->Release();   // device pointer used as target id
    return PresentWrap(dev ? (void*)dev : (void*)sw, [&](Frame& f) { return CaptureD9Swap(sw, f); }, [&] { return o_D9SwPresent(sw, s, r, w, rg, fl); });
}
static BOOL WINAPI Hk_wglSwap(HDC hdc) {
    return PresentWrap(WindowFromDC(hdc), [&](Frame& f) { return CaptureGL(hdc, f); }, [&] { return o_wglSwap(hdc); });
}
static BOOL WINAPI Hk_gdiSwap(HDC hdc) {
    return PresentWrap(WindowFromDC(hdc), [&](Frame& f) { return CaptureGL(hdc, f); }, [&] { return o_gdiSwap(hdc); });
}
static BOOL WINAPI Hk_wglSwapLayer(HDC hdc, UINT planes) {
    return PresentWrap(WindowFromDC(hdc), [&](Frame& f) { return CaptureGL(hdc, f); }, [&] { return o_wglSwapLayer(hdc, planes); });
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
    CreateThread(nullptr, 0, WriterThread, nullptr, 0, nullptr);
    if (MH_Initialize() != MH_OK) { SetStatus(L"ERROR: MinHook init failed"); return 0; }
    InstallTimeHooks();
    InterlockedExchange(&g_sh->hooksReady, 1);
    SetStatus(L"Hooks active. Waiting for the game to render... Press Record or F9.");
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
