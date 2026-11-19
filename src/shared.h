// FrameLock - shared memory layout between launcher (UI) and hook DLL
#pragma once
#include <windows.h>

#define FL_MAGIC 0x464C4B32u
#define FL_SHARED_FMT L"Local\\FrameLock_%lu"   // %lu = target process id

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
    volatile LONG64 framesSaved;    // frames written in current/last take
    wchar_t        outDir[MAX_PATH];
    wchar_t        takeDir[MAX_PATH];
    wchar_t        api[64];         // graphics API detected
    wchar_t        status[512];     // last status / error message
};
#pragma pack(pop)
