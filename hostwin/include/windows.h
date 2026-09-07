/* LEGOLAND portable build -- the slice of <windows.h> the game sources
 * include directly (only input.c does). Everything else declares its Win32
 * types locally. Implementations come from the host shim, not from here. */
#ifndef LL_HOSTWIN_WINDOWS_H
#define LL_HOSTWIN_WINDOWS_H

typedef void*        HWND;
typedef void*        HCURSOR;
typedef unsigned int UINT;
typedef unsigned int WPARAM;
typedef long         LPARAM;
typedef unsigned long DWORD;
typedef int          BOOL;

typedef struct tagPOINT {
    long x;
    long y;
} POINT;

typedef struct tagMSG {
    HWND   hwnd;
    UINT   message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD  time;
    POINT  pt;
} MSG;

#define PM_NOREMOVE 0x0000
#define PM_REMOVE   0x0001

BOOL    PeekMessageA(MSG*, HWND, UINT, UINT, UINT);
BOOL    GetMessageA(MSG*, HWND, UINT, UINT);
BOOL    TranslateMessage(const MSG*);
long    DispatchMessageA(const MSG*);
HCURSOR SetCursor(HCURSOR);
BOOL    WaitMessage(void);

#endif
