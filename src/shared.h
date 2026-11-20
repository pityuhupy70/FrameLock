// FrameLock - shared memory layout between launcher (UI) and hook DLL
#pragma once
#include <windows.h>

#define FL_MAGIC 0x464C4B32u                      // bumped: layout changed (v2)
#define FL_SHARED_FMT L"Local\\FrameLock_%lu"   // %lu = target process id

#define FL_MODE_LOCKED   0   // offline export: game clock stepped exactly 1/FPS per captured frame
#define FL_MODE_REALTIME 1   // real-time capture: clock untouched, every presented frame is saved

#pragma pack(push, 8)
struct FLShared {
    DWORD          magic;
    volatile LONG  hooksReady;      // DLL: 1 once time hooks are active
    volatile LONG  wantRecord;      // UI / F9 hotkey: 1 = record, 0 = stop
    volatile LONG  isRecording;     // DLL: actual state
    volatile LONG  fpsNum;          // e.g. 24 or 24000
    volatile LONG  fpsDen;          // e.g. 1 or 1001
    volatile LONG  width;           // last captured frame size
    volatile LONG  height;
    volatile LONG64 framesSaved;    // frames fully written (JPEG on disk) in current/last take
    volatile LONG64 slipEvents;     // clock releases (game busy-waited beyond one step)
    wchar_t        outDir[MAX_PATH];
    wchar_t        takeDir[MAX_PATH];
    wchar_t        api[64];         // graphics API detected
    wchar_t        status[512];     // last status / error message
    // ---- v2
    volatile LONG  mode;            // FL_MODE_LOCKED / FL_MODE_REALTIME (read at record start)
    volatile LONG  testFrames;      // >0: next take is a self-test of this many frames (auto-stop + verify)
    volatile LONG64 framesQueued;   // frames captured and queued for encoding
    volatile LONG  lastHr;          // last failing HRESULT (0 = none)
};
#pragma pack(pop)
