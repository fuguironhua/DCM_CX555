#pragma once
#include "framework.h"

struct tagWndInfo {
    int id = 0;
    HWND hWnd = nullptr;
    CString title;
    CString process;
    RECT rect{ 0, 0, 0, 0 };
};

struct Point2f {
    float x = 0.0f;
    float y = 0.0f;
};

// Nearby-object result kept per worker instance.  The field names mirror the
// reference project while avoiding its process-wide global result vector.
struct Monst {
    int ID = 0;
    DWORD Dibu = 0;
    float Juli = 0.0f;
    DWORD fen = 0;
    DWORD Owener = 0;
    CString cxue;
    CString mname;
    Point2f pos{};
    DWORD gdj = 0;
};

// Data returned for the currently recommended city-shop item.
// destination is the map name parsed from the tooltip's coordinate line.
struct PaoShangRecommendData {
    DWORD price = 0;
    DWORD id = 0;
    CString destination;
};

struct tagWndIni {
    CString process;
    CString title;
    CString className;
    CString display;
    CString mouse;
    CString keypad;
    CString publicDesc;
    long mode = 0;
    CString serverUrl;
};

struct tagTaskInfo {
    int id = 0;
    std::shared_ptr<tagWndInfo> wnd;
    class CTaskThread* pTaskThread = nullptr;
    class CTaskThread* pTaskHelperThread = nullptr;
    class CTaskThread* pTaskYanThread = nullptr;
    CString state = _T("stopped");
};
