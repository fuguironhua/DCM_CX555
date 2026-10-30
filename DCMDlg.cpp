#include "pch.h"
#include "resource.h"
#include "DCMDlg.h"
#include "TlbbMultiOpenDlg.h"
#include <tlhelp32.h>
#include <cmath>
#include <cstdlib>
#include <gdiplus.h>
#include <shlwapi.h>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")

namespace {
// Per-row button columns in the window list.
constexpr int kRunColumn = 4;
constexpr int kPauseColumn = 5;
}

BEGIN_MESSAGE_MAP(CDCMDlg, CDialogEx)
    ON_BN_CLICKED(IDC_BUTTON_REFRESH, &CDCMDlg::OnRefresh)
    ON_BN_CLICKED(IDC_BUTTON_START, &CDCMDlg::OnStart)
    ON_BN_CLICKED(IDC_BUTTON_PAUSE, &CDCMDlg::OnPause)
    ON_BN_CLICKED(IDC_BUTTON_TEST, &CDCMDlg::OnTest)
    ON_BN_CLICKED(IDC_BUTTON_UPDATE_D, &CDCMDlg::OnUpdateD)
    ON_BN_CLICKED(IDC_BUTTON_MULTI_OPEN, &CDCMDlg::OnMultiOpen)
    ON_BN_CLICKED(IDC_BUTTON_CAPTURE, &CDCMDlg::OnCapture)
    ON_BN_CLICKED(IDC_BUTTON_PAUSE_VERIFY, &CDCMDlg::OnPauseVerify)
    ON_BN_CLICKED(IDC_BUTTON_DISTANCE, &CDCMDlg::OnDistance)
    ON_BN_CLICKED(IDC_BUTTON_GRAB_VERIFY, &CDCMDlg::OnGrabVerify)
    ON_BN_CLICKED(IDC_BUTTON_DETECT_TYPE, &CDCMDlg::OnDetectCaptchaType)
    ON_BN_CLICKED(IDC_BUTTON_ROUTE_FORWARD, &CDCMDlg::OnRunRouteForward)
    ON_BN_CLICKED(IDC_BUTTON_ROUTE_REVERSE, &CDCMDlg::OnRunRouteReverse)
    ON_BN_CLICKED(IDC_BUTTON_TASK_TOGGLE, &CDCMDlg::OnTaskToggle)
    ON_WM_CONTEXTMENU()
    ON_COMMAND(ID_DEBUG_SAVE, &CDCMDlg::OnDebugSave)
    ON_COMMAND(ID_DEBUG_CLEAR, &CDCMDlg::OnDebugClear)
    ON_WM_TIMER()
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_WINDOW_LIST, &CDCMDlg::OnListItemChanged)
    ON_NOTIFY(NM_CLICK, IDC_WINDOW_LIST, &CDCMDlg::OnWindowListClick)
    ON_NOTIFY(NM_CUSTOMDRAW, IDC_WINDOW_LIST, &CDCMDlg::OnWindowListCustomDraw)
    ON_NOTIFY(NM_DBLCLK, IDC_TASK_LIST, &CDCMDlg::OnTaskListDblClick)
    ON_NOTIFY(NM_DBLCLK, IDC_TASK_RUN_LIST, &CDCMDlg::OnTaskRunListDblClick)
    ON_MESSAGE(WM_APP + 101, &CDCMDlg::OnDmRegistrationMessage)
    ON_MESSAGE(WM_APP_WINDOW_THREAD_LOG, &CDCMDlg::OnWindowThreadLog)
    ON_MESSAGE(WM_APP + 103, &CDCMDlg::OnUpdateDFinished)
    ON_MESSAGE(WM_APP + 104, &CDCMDlg::OnGrabVerifyFinished)
END_MESSAGE_MAP()

CDCMDlg::CDCMDlg(CWnd* parent) : CDialogEx(IDD_MAIN_DIALOG, parent) {}

CDCMDlg::~CDCMDlg() {
    if (m_updateDThread.joinable()) m_updateDThread.join();
    m_windowToolsDm.reset();
    StopDmRegistration();
}

void CDCMDlg::DoDataExchange(CDataExchange* dx) {
    CDialogEx::DoDataExchange(dx);
    DDX_Control(dx, IDC_WINDOW_LIST, m_list);
    DDX_Control(dx, IDC_TASK_LIST, m_taskList);
    DDX_Control(dx, IDC_TASK_RUN_LIST, m_taskRunList);
    DDX_Control(dx, IDC_BUTTON_REFRESH, m_refresh);
    DDX_Control(dx, IDC_BUTTON_START, m_start);
    DDX_Control(dx, IDC_BUTTON_PAUSE, m_pause);
    DDX_Control(dx, IDC_BUTTON_TASK_TOGGLE, m_taskToggle);
    DDX_Control(dx, IDC_BUTTON_TEST, m_test);
    DDX_Control(dx, IDC_STATIC_STATUS, m_status);
    DDX_Control(dx, IDC_EDIT_DEBUG, m_debug);
}

BOOL CDCMDlg::OnInitDialog() {
    CDialogEx::OnInitDialog();
    SetWindowText(_T("DCM_CX555 - Task Framework"));
    m_list.SetExtendedStyle(m_list.GetExtendedStyle() | LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    m_list.InsertColumn(0, _T("HWND"), LVCFMT_LEFT, 50);
    m_list.InsertColumn(1, _T("Title"), LVCFMT_LEFT, 75);
    m_list.InsertColumn(2, _T("Process"), LVCFMT_LEFT, 55);
    m_list.InsertColumn(3, _T("Status"), LVCFMT_LEFT, 100);
    m_list.InsertColumn(kRunColumn, _T("任务"), LVCFMT_CENTER, 34);
    m_list.InsertColumn(kPauseColumn, _T("暂停"), LVCFMT_CENTER, 34);
    InitTaskLists();
    m_debug.LimitText(100000);
    IncreaseDebugFont();
    m_start.SetWindowText(_T("Start"));
    m_pause.SetWindowText(_T("Pause"));
    m_taskToggle.SetWindowText(_T("\u5f00\u59cb/\u7ed3\u675f"));
    m_test.SetWindowText(_T("\u6d4b\u8bd5"));
    // Use dialog units and the existing font; leave the original controls in place.
    CRect updateDRect(419, 0, 456, 20);
    MapDialogRect(&updateDRect);
    if (m_updateD.Create(_T("更新D"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                        updateDRect, this, IDC_BUTTON_UPDATE_D)) {
        m_updateD.SetFont(GetFont());
    }

    const auto addToolButton = [this](CButton& button, LPCTSTR title, UINT id, CRect rect) {
        MapDialogRect(&rect);
        if (button.Create(title, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                          rect, this, id)) button.SetFont(GetFont());
    };
    addToolButton(m_multiOpen, _T("多开"), IDC_BUTTON_MULTI_OPEN, CRect(419, 25, 456, 45));
    addToolButton(m_capture, _T("截图"), IDC_BUTTON_CAPTURE, CRect(360, 50, 405, 70));
    addToolButton(m_pauseVerify, _T("暂停验证窗口"), IDC_BUTTON_PAUSE_VERIFY, CRect(409, 50, 456, 70));
    addToolButton(m_distance, _T("计算MoveX"), IDC_BUTTON_DISTANCE, CRect(360, 75, 456, 93));
    addToolButton(m_grabVerify, _T("\u6293\u9a8c\u8bc1\u7801"), IDC_BUTTON_GRAB_VERIFY, CRect(360, 116, 407, 132));
    addToolButton(m_detectType, _T("\u8bc6\u522b\u7c7b\u578b"), IDC_BUTTON_DETECT_TYPE, CRect(409, 116, 456, 132));
    addToolButton(m_routeForward, _T("\u6b63\u5411\u8def\u7ebf\u8dd1"), IDC_BUTTON_ROUTE_FORWARD,
                  CRect(360, 98, 407, 116));
    addToolButton(m_routeReverse, _T("\u53cd\u5411\u8def\u7ebf\u8dd1"), IDC_BUTTON_ROUTE_REVERSE,
                  CRect(409, 98, 456, 116));

    CRect statusRect(247, 118, 453, 132);
    MapDialogRect(&statusRect);
    m_status.MoveWindow(statusRect);

    wchar_t module[MAX_PATH]{};
    GetModuleFileName(nullptr, module, _countof(module));
    const auto root = std::filesystem::path(module).parent_path();
    auto ini = root / L"\u5168\u5c40\u914d\u7f6e.ini";
    if (!std::filesystem::exists(ini)) ini = root / L"config" / L"\u5168\u5c40\u914d\u7f6e.ini";
    if (!m_config.Load(ini)) {
        SetStatus(_T("global config not found"));
        AppendDebug(_T("config: not found, defaults are active"), LogLevel::Warning);
    } else {
        CString configPath(ini.c_str());
        AppendDebug(_T("config loaded: ") + configPath, LogLevel::Normal);
    }

    StartDmRegistration();
    SetStatus(_T("dm registration is running in background"));
    CString dmModes;
    dmModes.Format(_T("dm modes: display=%s, mouse=%s, keypad=%s"), m_config.Window().display.GetString(), m_config.Window().mouse.GetString(), m_config.Window().keypad.GetString());
    AppendDebug(dmModes, LogLevel::Normal);
    m_taskSchedule = std::make_shared<CTaskSchedule>();
    m_taskManager = std::make_unique<CTaskManager>(m_config, GetSafeHwnd(), m_taskSchedule);
    RefreshWindows();
    SetTimer(1, 1000, nullptr);
    UpdateActionButtons();
    return TRUE;
}

void CDCMDlg::InitTaskLists() {
    const DWORD listStyle = LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES;
    m_taskList.SetExtendedStyle(m_taskList.GetExtendedStyle() | listStyle);
    m_taskRunList.SetExtendedStyle(m_taskRunList.GetExtendedStyle() | listStyle);
    m_taskList.InsertColumn(0, _T("\u5f85\u9009\u4efb\u52a1"), LVCFMT_CENTER, 60);
    m_taskRunList.InsertColumn(0, _T("\u6267\u884c\u4efb\u52a1"), LVCFMT_CENTER, 60);

    // Task names intentionally match the reference project's task palette.
    static const LPCTSTR taskNames[] = {
        _T("\u68cb\u5c40"), _T("\u53cd\u8d3c"), _T("\u8001\u4e00"), _T("\u8001\u4e8c"), _T("\u8001\u4e09"),
        _T("\u6253\u602a"), _T("\u96c6\u5408"), _T("\u65b0\u4e00"), _T("\u65b0\u4e8c"), _T("\u65b0\u4e09"),
        _T("\u65b0\u6d3b\u52a8"), _T("\u6740\u661f"), _T("\u71d5\u5b50"), _T("\u7f25\u7f08"), _T("\u6d1b\u52a0\u8840"),
        _T("\u5347\u7ea7"), _T("\u79cd\u5730"), _T("\u8dd1\u5546"), _T("\u4e0a\u67b6"), _T("\u6253\u56fe"),
        _T("\u6269\u5f20"), _T("\u8e74\u97a0"), _T("\u626b\u8361"), _T("\u51fa\u552e"), _T("\u5355\u53cd\u8d3c"),
        _T("\u6574\u7406"), _T("\u5355\u71d52")
    };

    for (int i = 0; i < static_cast<int>(_countof(taskNames)); ++i) {
        m_taskList.InsertItem(i, taskNames[i]);
    }
    HideHorizontalScrollBar(m_taskList);
    HideHorizontalScrollBar(m_taskRunList);
}

void CDCMDlg::HideHorizontalScrollBar(CListCtrl& list) {
    // The list-view may recreate the bar after rows are inserted. Remove the
    // style as well as hiding the scrollbar so the three-column layout stays
    // clean while the vertical scrollbar remains available.
    list.ShowScrollBar(SB_HORZ, FALSE);
    list.ModifyStyle(WS_HSCROLL, 0);
    list.ShowScrollBar(SB_HORZ, FALSE);
}

void CDCMDlg::IncreaseDebugFont() {
    CFont* currentFont = m_debug.GetFont();
    LOGFONT logFont{};
    if (!currentFont || currentFont->GetLogFont(&logFont) == 0) return;

    CClientDC dc(this);
    const int onePoint = (std::max)(1, MulDiv(1, dc.GetDeviceCaps(LOGPIXELSY), 72));
    logFont.lfHeight = logFont.lfHeight < 0
        ? logFont.lfHeight - onePoint
        : logFont.lfHeight + onePoint;

    if (m_debugFont.CreateFontIndirect(&logFont) != FALSE) {
        m_debug.SetFont(&m_debugFont, TRUE);
    }
}

void CDCMDlg::RefreshWindows() {
    const auto checked = CheckedHandles();
    m_windows = m_wndManager.Refresh(m_config);
    UpdateRows(checked);
    CString status;
    status.Format(_T("found %u window(s)"), static_cast<unsigned>(m_windows.size()));
    SetStatus(status);
    CString debug;
    debug.Format(_T("refresh: %u matching window(s)"), static_cast<unsigned>(m_windows.size()));
    AppendDebug(debug);
}

std::vector<std::shared_ptr<tagWndInfo>> CDCMDlg::CheckedWindows() const {
    std::vector<std::shared_ptr<tagWndInfo>> result;
    for (int i = 0; i < m_list.GetItemCount() && i < static_cast<int>(m_windows.size()); ++i) {
        if (m_list.GetCheck(i)) result.push_back(m_windows[i]);
    }
    return result;
}

std::vector<HWND> CDCMDlg::CheckedHandles() const {
    std::vector<HWND> result;
    for (const auto& wnd : CheckedWindows()) if (wnd) result.push_back(wnd->hWnd);
    return result;
}

void CDCMDlg::UpdateRows(const std::vector<HWND>& keepChecked) {
    m_list.DeleteAllItems();
    for (int i = 0; i < static_cast<int>(m_windows.size()); ++i) {
        const auto& w = m_windows[i];
        CString handle; handle.Format(_T("0x%p"), w->hWnd);
        const int row = m_list.InsertItem(i, handle);
        m_list.SetItemText(row, 1, w->title);
        m_list.SetItemText(row, 2, w->process);
        const CString state = m_taskManager ? m_taskManager->Status(w->hWnd) : CString(_T("stopped"));
        m_list.SetItemText(row, 3, state);
        UpdateRowButtons(row);
        m_list.SetCheck(row, std::find(keepChecked.begin(), keepChecked.end(), w->hWnd) != keepChecked.end());
    }
    HideHorizontalScrollBar(m_list);
}

void CDCMDlg::UpdateStatuses() {
    for (int i = 0; i < m_list.GetItemCount() && i < static_cast<int>(m_windows.size()); ++i) {
        const HWND hwnd = m_windows[i]->hWnd;
        m_list.SetItemText(i, 3, m_taskManager ? m_taskManager->Status(hwnd) : _T("stopped"));
        UpdateRowButtons(i);
    }
}

void CDCMDlg::UpdateRowButtons(int row) {
    if (row < 0 || row >= static_cast<int>(m_windows.size())) return;
    const std::vector<HWND> target{ m_windows[row]->hWnd };
    const bool sequenceRunning = m_taskManager && m_taskManager->IsSequenceRunning(target);
    const bool paused = m_taskManager && m_taskManager->IsPaused(target.front());
    const bool running = m_taskManager && m_taskManager->IsRunning(target.front());
    const LPCTSTR runText = sequenceRunning ? _T("结束") : _T("执行");
    const LPCTSTR pauseText = paused ? _T("继续") : _T("暂停");
    // Only touch the cells when the label changes to avoid flicker on the 1s timer.
    if (m_list.GetItemText(row, kRunColumn) != runText) m_list.SetItemText(row, kRunColumn, runText);
    if (m_list.GetItemText(row, kPauseColumn) != pauseText) m_list.SetItemText(row, kPauseColumn, pauseText);
    // Item data caches "runtime running" so custom draw can grey out the pause button.
    if ((m_list.GetItemData(row) != 0) != running) {
        m_list.SetItemData(row, running ? 1 : 0);
        m_list.RedrawItems(row, row);
    }
}

void CDCMDlg::UpdateActionButtons() {
    const auto checked = CheckedHandles();
    const bool hasChecked = !checked.empty();
    bool anyRunning = false;
    bool anyPaused = false;

    if (m_taskManager) {
        if (hasChecked) {
            for (const HWND hwnd : checked) {
                anyRunning = anyRunning || m_taskManager->IsRunning(hwnd);
                anyPaused = anyPaused || m_taskManager->IsPaused(hwnd);
            }
        } else {
            anyRunning = m_taskManager->RunningCount() > 0;
        }
    }

    m_start.SetWindowText(anyRunning ? _T("Stop") : _T("Start"));
    m_pause.SetWindowText(anyPaused ? _T("Resume") : _T("Pause"));
    m_pause.EnableWindow(hasChecked && anyRunning);
}

void CDCMDlg::SetStatus(const CString& text) { if (m_status.GetSafeHwnd()) m_status.SetWindowText(text); }
COLORREF CDCMDlg::DebugColor(LogLevel level) const {
    switch (level) {
    case LogLevel::Normal: return RGB(125, 125, 125);
    case LogLevel::Warning: return RGB(255, 128, 0);
    case LogLevel::Exception: return RGB(200, 0, 0);
    case LogLevel::Debug: return RGB(0, 128, 128);
    default: return RGB(0, 128, 128);
    }
}
void CDCMDlg::AppendDebug(const CString& text, LogLevel level) {
    if (!m_debug.GetSafeHwnd()) return;
    CString line; line.Format(_T("[%s] %s\r\n"), CTime::GetCurrentTime().Format(_T("%H:%M:%S")), text.GetString());
    const int end = m_debug.GetWindowTextLength();
    m_debug.SetSel(end, end);
    CHARFORMAT2 format{};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_COLOR;
    format.crTextColor = DebugColor(level);
    m_debug.SetSelectionCharFormat(format);
    m_debug.ReplaceSel(line);
    // Keep the newest message visible.  ReplaceSel moves the selection to the
    // end, but RichEdit can leave the viewport at the previous scroll
    // position when the control does not have focus.  Move the caret and force
    // both RichEdit's caret view and its vertical scrollbar to the end.
    const int latest = m_debug.GetWindowTextLength();
    m_debug.SetSel(latest, latest);
    m_debug.SendMessage(EM_SCROLLCARET, 0, 0);
    m_debug.LineScroll(m_debug.GetLineCount(), 0);
    m_debug.SendMessage(WM_VSCROLL, SB_BOTTOM, 0);
    // Queue one more pass after RichEdit has laid out the inserted line.  It
    // covers the case where the control defers its scrollbar recalculation
    // until the current window-message handler returns.
    m_debug.PostMessage(EM_SCROLLCARET, 0, 0);
    m_debug.PostMessage(WM_VSCROLL, SB_BOTTOM, 0);
}

void CDCMDlg::StartDmRegistration() {
    if (m_dmInitThread.joinable()) return;

    m_dmStop.store(false);
    m_dmInitDone.store(false);
    m_dmRegistered.store(false);
    AppendDebug(_T("dm: registration thread starting"), LogLevel::Normal);
    m_dmInitThread = std::thread(&CDCMDlg::DmRegistrationThreadMain, this);
}

void CDCMDlg::DmRegistrationThreadMain() {
    const DWORD threadId = GetCurrentThreadId();

    HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInitialized = SUCCEEDED(comResult);
    HMODULE dmRegModule = nullptr;
    std::unique_ptr<dmsoft> dm;
    bool registered = false;

    auto debug = [threadId](const CString& text) {
        CString line;
        line.Format(_T("[dm-init][tid=%lu] %s\r\n"), threadId, text.GetString());
        OutputDebugString(line);
    };

    debug(_T("thread started"));
    if (!comInitialized) {
        debug(_T("CoInitializeEx failed"));
    } else {
        wchar_t module[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, module, _countof(module));
        const std::filesystem::path root = length > 0 && length < _countof(module)
            ? std::filesystem::path(module).parent_path()
            : std::filesystem::path();
        const std::filesystem::path dmRegPath = root / L"DmReg.dll";
        const std::filesystem::path dmPath = root / L"dm.dll";

        if (root.empty() || !std::filesystem::exists(dmRegPath) || !std::filesystem::exists(dmPath)) {
            debug(_T("dm.dll or DmReg.dll not found"));
        } else {
            dmRegModule = LoadLibraryW(dmRegPath.c_str());
            if (!dmRegModule) {
                debug(_T("LoadLibrary(DmReg.dll) failed"));
            } else {
                using SetDllPathW = long(WINAPI*)(wchar_t*, long);
                const auto setDllPath = reinterpret_cast<SetDllPathW>(GetProcAddress(dmRegModule, "SetDllPathW"));
                if (!setDllPath) {
                    debug(_T("SetDllPathW export not found"));
                } else {
                    std::wstring dmPathString = dmPath.wstring();
                    const long pathResult = setDllPath(dmPathString.data(), 0);
                    CString pathMessage;
                    pathMessage.Format(_T("SetDllPathW returned %ld"), pathResult);
                    debug(pathMessage);

                    dm = std::make_unique<dmsoft>();
                    const CString key = _T("youxirenshengb828692afe4e2c0035c66b2c77a06a0f");
                    const CString version = dm->Ver();
                    const long result = dm->RegEx(
                        key.GetString(), version.GetString(),
                        _T("121.204.252.143|121.204.253.161|125.77.165.62|125.77.165.131"));
                    CString registerMessage;
                    registerMessage.Format(_T("RegEx returned %ld, version %s"), result, version.GetString());
                    debug(registerMessage);
                    registered = result == 1;
                    if (registered) {
                        dm->DmGuard(1, _T("block"));
                        dm->SetShowErrorMsg(0);
                        debug(_T("dm registered successfully; waiting for shutdown"));
                    }
                }
            }
        }
    }

    m_dmRegistered.store(registered);
    m_dmInitDone.store(true);
    if (registered) {
        PostMessage(WM_APP + 101, 1, 0);
    } else {
        PostMessage(WM_APP + 101, 0, 0);
    }

    {
        std::unique_lock<std::mutex> lock(m_dmMutex);
        m_dmCv.wait(lock, [this] { return m_dmStop.load(); });
    }

    debug(_T("shutdown requested"));
    dm.reset();
    if (dmRegModule) FreeLibrary(dmRegModule);
    if (comInitialized) CoUninitialize();
    debug(_T("thread stopped; dm object and DmReg.dll released"));
}

void CDCMDlg::StopDmRegistration() {
    if (!m_dmInitThread.joinable()) return;

    AppendDebug(_T("dm: stopping registration thread"), LogLevel::Normal);
    m_dmStop.store(true);
    m_dmCv.notify_all();
    m_dmInitThread.join();
    m_dmRegistered.store(false);
    m_dmInitDone.store(false);
    AppendDebug(_T("dm: registration thread stopped"), LogLevel::Normal);
}

LRESULT CDCMDlg::OnWindowThreadLog(WPARAM, LPARAM lParam) {
    auto* message = reinterpret_cast<CString*>(lParam);
    if (message) {
        AppendDebug(*message, LogLevel::Debug);
        delete message;
    }
    return 0;
}

LRESULT CDCMDlg::OnDmRegistrationMessage(WPARAM wParam, LPARAM) {
    if (wParam != 0) {
        SetStatus(_T("dm registered; ready"));
        AppendDebug(_T("dm: background registration completed"), LogLevel::Normal);
    } else {
        SetStatus(_T("dm registration failed"));
        AppendDebug(_T("dm: background registration failed"), LogLevel::Exception);
    }
    return 0;
}

void CDCMDlg::OnRefresh() { RefreshWindows(); }

void CDCMDlg::OnUpdateD() {
    if (m_updateDThread.joinable()) return;
    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module, _countof(module));
    const auto configDirectory = std::filesystem::path(module).parent_path() / L"config";
    // Same offset source and decimal CCEE values as GEN_CETEXT; names replace row numbers.
    const std::filesystem::path offsetsFile = L"D:\\Tui.h";
    m_updateD.EnableWindow(FALSE);
    SetStatus(_T("更新D：正在更新当前 CE 表"));
    AppendDebug(_T("更新D：按名称匹配，偏移来源 D:\\Tui.h [CCEE]；跳过激活记录。"));
    const HWND window = GetSafeHwnd();
    try {
        m_updateDThread = std::thread([this, window, configDirectory, offsetsFile] {
            m_updateDResult = CeTableUpdater::Update(configDirectory, offsetsFile);
            ::PostMessage(window, WM_APP + 103, 0, 0);
        });
    } catch (const std::exception&) {
        m_updateD.EnableWindow(TRUE);
        SetStatus(_T("更新D：启动工作线程失败"));
        AppendDebug(_T("更新D：启动工作线程失败"), LogLevel::Exception);
    }
}

LRESULT CDCMDlg::OnUpdateDFinished(WPARAM, LPARAM) {
    if (m_updateDThread.joinable()) m_updateDThread.join();
    m_updateD.EnableWindow(TRUE);
    SetStatus(m_updateDResult.success ? _T("更新D：处理完成，详见日志") : _T("更新D：失败，详见日志"));
    AppendDebug(CString(m_updateDResult.message.c_str()), m_updateDResult.success ? LogLevel::Normal : LogLevel::Exception);
    return 0;
}

void CDCMDlg::OnStart() {
    const auto windows = CheckedWindows();
    const auto checked = CheckedHandles();
    bool anyRunning = false;
    if (m_taskManager) {
        if (checked.empty()) {
            anyRunning = m_taskManager->RunningCount() > 0;
        } else {
            for (const HWND hwnd : checked) {
                if (m_taskManager->IsRunning(hwnd)) {
                    anyRunning = true;
                    break;
                }
            }
        }
    }

    if (anyRunning) {
        if (m_taskManager) m_taskManager->StopSequence(checked);
        m_taskManager->Stop(checked);
        UpdateRows(checked);
        SetStatus(checked.empty() ? _T("stopped all and released objects") : _T("stopped checked windows and released objects"));
        AppendDebug(checked.empty() ? _T("toggle: stop all runtimes") : _T("toggle: stop checked runtimes"));
        UpdateActionButtons();
        return;
    }

    if (windows.empty()) {
        SetStatus(_T("check windows first"));
        AppendDebug(_T("start ignored: no checked window"), LogLevel::Warning);
        UpdateActionButtons();
        return;
    }
    if (!m_dmInitDone.load()) {
        SetStatus(_T("dm registration is still running"));
        AppendDebug(_T("start ignored: dm registration is still running"), LogLevel::Warning);
        return;
    }
    if (!m_dmRegistered.load()) {
        SetStatus(_T("dm registration failed"));
        AppendDebug(_T("start ignored: dm registration failed"), LogLevel::Exception);
        return;
    }

    const bool ok = m_taskManager->Start(windows);
    UpdateRows(checked);
    SetStatus(ok ? _T("started: 3 threads per window") : _T("some windows failed to start"));
    AppendDebug(ok ? _T("toggle: start TaskScript + Helper + Yan") : _T("toggle: one or more runtimes failed"), ok ? LogLevel::Normal : LogLevel::Exception);
    UpdateActionButtons();
}

void CDCMDlg::OnPause() {
    const auto checked = CheckedHandles();
    if (checked.empty()) {
        SetStatus(_T("check windows first"));
        AppendDebug(_T("pause ignored: no checked window"), LogLevel::Warning);
        return;
    }

    bool anyRunning = false;
    bool anyPaused = false;
    for (const HWND hwnd : checked) {
        anyRunning = anyRunning || m_taskManager->IsRunning(hwnd);
        anyPaused = anyPaused || m_taskManager->IsPaused(hwnd);
    }
    if (!anyRunning) {
        SetStatus(_T("no running window"));
        AppendDebug(_T("pause ignored: selected windows are stopped"), LogLevel::Warning);
        return;
    }

    if (anyPaused) {
        m_taskManager->Resume(checked);
    } else {
        m_taskManager->Suspend(checked);
    }
    UpdateRows(checked);
    SetStatus(anyPaused ? _T("resumed") : _T("paused"));
    AppendDebug(anyPaused ? _T("toggle: selected runtimes resumed") : _T("toggle: selected runtimes paused"));
    UpdateActionButtons();
}

void CDCMDlg::OnTimer(UINT_PTR id) {
    if (id == 1) {
        UpdateStatuses();
        // A selected window with a running sequence owns the execution list;
        // the checked-window removal below then only keeps its counter in step.
        const bool viewingWindow = SyncTaskRunListWithSelection();
        if (m_taskSequenceStarted && m_taskManager) {
            const auto checked = CheckedHandles();
            if (!checked.empty()) {
                const size_t completed = m_taskManager->CompletedTaskCount(checked);
                if (viewingWindow) m_taskRowsRemoved = completed;
                while (m_taskRowsRemoved < completed && m_taskRunList.GetItemCount() > 0) {
                    const CString task = m_taskRunList.GetItemText(0, 0);
                    m_taskRunList.DeleteItem(0);
                    ++m_taskRowsRemoved;
                    AppendDebug(CString(_T("task completed and removed: ")) + task, LogLevel::Normal);
                }
                if (!m_taskManager->IsSequenceRunning(checked)) {
                    m_taskRunList.DeleteAllItems();
                    m_taskRowsRemoved = 0;
                    m_taskSequenceStarted = false;
                    SetStatus(_T("task sequence completed; execution list cleared"));
                    AppendDebug(_T("task sequence completed: execution list cleared"), LogLevel::Normal);
                }
            }
        }
        UpdateActionButtons();
    }
    CDialogEx::OnTimer(id);
}

void CDCMDlg::OnListItemChanged(NMHDR* pNMHDR, LRESULT* result) {
    UpdateActionButtons();
    const auto* change = reinterpret_cast<const NMLISTVIEW*>(pNMHDR);
    if (change && (change->uChanged & LVIF_STATE) != 0 &&
        ((change->uNewState ^ change->uOldState) & LVIS_SELECTED) != 0) {
        SyncTaskRunListWithSelection();
    }
    if (result) *result = 0;
}

bool CDCMDlg::SyncTaskRunListWithSelection() {
    const int row = m_list.GetNextItem(-1, LVNI_SELECTED);
    if (!m_taskManager || row < 0 || row >= static_cast<int>(m_windows.size())) {
        m_viewedSequenceHwnd = nullptr;
        return false;
    }
    const HWND hwnd = m_windows[row]->hWnd;
    if (!m_taskManager->IsSequenceRunning({ hwnd })) {
        // The window we were mirroring has finished its queue: clear the list
        // the same way the checked-window sequence does on completion.
        if (m_viewedSequenceHwnd == hwnd) {
            m_taskRunList.DeleteAllItems();
            CString message;
            message.Format(_T("window 0x%p: task sequence completed, execution list cleared"), hwnd);
            SetStatus(_T("window task sequence completed; execution list cleared"));
            AppendDebug(message, LogLevel::Normal);
        }
        // An idle window leaves the list alone so it can still be edited and run.
        m_viewedSequenceHwnd = nullptr;
        return false;
    }

    m_viewedSequenceHwnd = hwnd;
    const auto remaining = m_taskManager->RemainingTaskIds(hwnd);
    bool same = m_taskRunList.GetItemCount() == static_cast<int>(remaining.size());
    for (int i = 0; same && i < static_cast<int>(remaining.size()); ++i)
        same = static_cast<int>(m_taskRunList.GetItemData(i)) == remaining[i];
    if (same) return true;

    m_taskRunList.SetRedraw(FALSE);
    m_taskRunList.DeleteAllItems();
    for (const int taskId : remaining) {
        // Item data is the task palette row, as in OnTaskListDblClick.
        const int item = m_taskRunList.InsertItem(m_taskRunList.GetItemCount(), m_taskList.GetItemText(taskId, 0));
        m_taskRunList.SetItemData(item, static_cast<DWORD_PTR>(taskId));
    }
    m_taskRunList.SetRedraw(TRUE);
    m_taskRunList.Invalidate();
    return true;
}

void CDCMDlg::OnWindowListClick(NMHDR* pNMHDR, LRESULT* result) {
    if (result) *result = 0;
    const auto* item = reinterpret_cast<const NMITEMACTIVATE*>(pNMHDR);
    if (!item) return;
    LVHITTESTINFO hit{};
    hit.pt = item->ptAction;
    if (m_list.SubItemHitTest(&hit) < 0 || hit.iItem < 0) return;
    if (hit.iSubItem == kRunColumn) ToggleWindowSequence(hit.iItem);
    else if (hit.iSubItem == kPauseColumn) ToggleWindowPause(hit.iItem);
}

void CDCMDlg::OnWindowListCustomDraw(NMHDR* pNMHDR, LRESULT* result) {
    auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(pNMHDR);
    *result = CDRF_DODEFAULT;
    switch (draw->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        *result = CDRF_NOTIFYITEMDRAW;
        return;
    case CDDS_ITEMPREPAINT:
        *result = CDRF_NOTIFYSUBITEMDRAW;
        return;
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
        break;
    default:
        return;
    }
    if (draw->iSubItem != kRunColumn && draw->iSubItem != kPauseColumn) return;

    const int row = static_cast<int>(draw->nmcd.dwItemSpec);
    CRect cell;
    if (!m_list.GetSubItemRect(row, draw->iSubItem, LVIR_BOUNDS, cell)) return;
    const bool enabled = draw->iSubItem != kPauseColumn || m_list.GetItemData(row) != 0;

    CDC* dc = CDC::FromHandle(draw->nmcd.hdc);
    dc->FillSolidRect(cell, ::GetSysColor(COLOR_WINDOW));
    CRect button(cell);
    button.DeflateRect(2, 1, 2, 1);
    dc->DrawFrameControl(button, DFC_BUTTON, DFCS_BUTTONPUSH | (enabled ? 0 : DFCS_INACTIVE));
    const int oldMode = dc->SetBkMode(TRANSPARENT);
    const COLORREF oldColor = dc->SetTextColor(::GetSysColor(enabled ? COLOR_BTNTEXT : COLOR_GRAYTEXT));
    const CString text = m_list.GetItemText(row, draw->iSubItem);
    dc->DrawText(text, button, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    dc->SetTextColor(oldColor);
    dc->SetBkMode(oldMode);
    *result = CDRF_SKIPDEFAULT;
}

void CDCMDlg::ToggleWindowSequence(int row) {
    if (!m_taskManager || row < 0 || row >= static_cast<int>(m_windows.size())) return;
    const auto wnd = m_windows[row];
    const std::vector<HWND> target{ wnd->hWnd };
    CString message;

    if (m_taskManager->IsSequenceRunning(target)) {
        m_taskManager->StopSequence(target);
        // A manual stop keeps the remaining tasks in the list instead of clearing it.
        if (m_viewedSequenceHwnd == wnd->hWnd) m_viewedSequenceHwnd = nullptr;
        message.Format(_T("window 0x%p: task sequence ended"), wnd->hWnd);
        SetStatus(_T("window task sequence ended"));
        AppendDebug(message, LogLevel::Normal);
        UpdateStatuses();
        UpdateActionButtons();
        return;
    }

    const auto taskIds = TaskSequenceIds();
    if (taskIds.empty()) {
        SetStatus(_T("add tasks to the execution list first"));
        AppendDebug(_T("window task sequence ignored: execution list is empty"), LogLevel::Warning);
        return;
    }
    if (!m_dmInitDone.load()) {
        SetStatus(_T("dm registration is still running"));
        AppendDebug(_T("window task sequence ignored: dm registration is still running"), LogLevel::Warning);
        return;
    }
    if (!m_dmRegistered.load()) {
        SetStatus(_T("dm registration failed"));
        AppendDebug(_T("window task sequence ignored: dm registration failed"), LogLevel::Exception);
        return;
    }

    // Same flow as OnTaskToggle, but only for this row: start its three
    // worker threads if needed, then hand it the current execution list.
    if (!m_taskManager->Start({ wnd })) {
        message.Format(_T("window 0x%p: runtime failed to start"), wnd->hWnd);
        SetStatus(_T("window failed to start"));
        AppendDebug(message, LogLevel::Exception);
        UpdateStatuses();
        UpdateActionButtons();
        return;
    }
    m_taskManager->StartSequence(taskIds, target);
    message.Format(_T("window 0x%p: task sequence started, %u task(s)"), wnd->hWnd,
                   static_cast<unsigned>(taskIds.size()));
    SetStatus(_T("window task sequence started"));
    AppendDebug(message, LogLevel::Normal);
    UpdateStatuses();
    SyncTaskRunListWithSelection();
    UpdateActionButtons();
}

void CDCMDlg::ToggleWindowPause(int row) {
    if (!m_taskManager || row < 0 || row >= static_cast<int>(m_windows.size())) return;
    const HWND hwnd = m_windows[row]->hWnd;
    const std::vector<HWND> target{ hwnd };
    CString message;
    if (!m_taskManager->IsRunning(hwnd)) {
        message.Format(_T("window 0x%p: pause ignored, window is stopped"), hwnd);
        SetStatus(_T("window is not running"));
        AppendDebug(message, LogLevel::Warning);
        return;
    }

    const bool paused = m_taskManager->IsPaused(hwnd);
    if (paused) m_taskManager->Resume(target);
    else m_taskManager->Suspend(target);
    message.Format(paused ? _T("window 0x%p: resumed") : _T("window 0x%p: paused"), hwnd);
    SetStatus(paused ? _T("window resumed") : _T("window paused"));
    AppendDebug(message, LogLevel::Normal);
    UpdateStatuses();
    UpdateActionButtons();
}

std::vector<int> CDCMDlg::TaskSequenceIds() const {
    std::vector<int> result;
    result.reserve(static_cast<size_t>(m_taskRunList.GetItemCount()));
    for (int row = 0; row < m_taskRunList.GetItemCount(); ++row) {
        result.push_back(static_cast<int>(m_taskRunList.GetItemData(row)));
    }
    return result;
}

void CDCMDlg::OnTaskListDblClick(NMHDR* pNMHDR, LRESULT* result) {
    const auto* item = reinterpret_cast<const LPNMITEMACTIVATE>(pNMHDR);
    if (item && item->iItem >= 0 && item->iItem < m_taskList.GetItemCount()) {
        const CString task = m_taskList.GetItemText(item->iItem, 0);
        const int row = m_taskRunList.InsertItem(m_taskRunList.GetItemCount(), task);
        // Keep the reference palette's numeric task order beside the visible
        // text. The sequence dispatcher uses this value, not the displayed
        // label, so duplicate task rows remain supported.
        m_taskRunList.SetItemData(row, static_cast<DWORD_PTR>(item->iItem));
        CString message;
        message.Format(_T("task added: %s"), task.GetString());
        AppendDebug(message, LogLevel::Normal);
    }
    if (result) *result = 0;
}

void CDCMDlg::OnTaskToggle() {
    if (!m_taskSchedule) return;

    const auto checked = CheckedHandles();
    if (m_taskManager && m_taskManager->IsSequenceRunning(checked)) {
        m_taskManager->StopSequence(checked);
        m_taskSequenceStarted = false;
        SetStatus(_T("task sequence ended"));
        AppendDebug(_T("task sequence ended"), LogLevel::Normal);
        return;
    }

    const auto taskIds = TaskSequenceIds();
    if (taskIds.empty()) {
        SetStatus(_T("add tasks to the execution list first"));
        AppendDebug(_T("task sequence ignored: execution list is empty"), LogLevel::Warning);
        return;
    }

    const auto windows = CheckedWindows();
    if (windows.empty()) {
        SetStatus(_T("check windows first"));
        AppendDebug(_T("task sequence ignored: no checked window"), LogLevel::Warning);
        return;
    }
    if (!m_dmInitDone.load()) {
        SetStatus(_T("dm registration is still running"));
        AppendDebug(_T("task sequence ignored: dm registration is still running"), LogLevel::Warning);
        return;
    }
    if (!m_dmRegistered.load()) {
        SetStatus(_T("dm registration failed"));
        AppendDebug(_T("task sequence ignored: dm registration failed"), LogLevel::Exception);
        return;
    }

    // The task button is also a convenient entry point when the lower
    // runtime button has not been pressed yet. Start the three worker
    // threads first, then publish one immutable ordered sequence to them.
    const bool runtimeOk = m_taskManager && m_taskManager->Start(windows);
    if (!runtimeOk) {
        SetStatus(_T("some windows failed to start"));
        AppendDebug(_T("task sequence ignored: one or more runtimes failed"), LogLevel::Exception);
        UpdateRows(CheckedHandles());
        UpdateActionButtons();
        return;
    }

    m_taskManager->StartSequence(taskIds, checked);
    m_taskSequenceStarted = true;
    m_taskRowsRemoved = 0;
    SetStatus(_T("task sequence started"));
    CString message;
    message.Format(_T("task sequence started: %u task(s), checked windows"),
                   static_cast<unsigned>(taskIds.size()));
    AppendDebug(message, LogLevel::Normal);
    UpdateRows(CheckedHandles());
    UpdateActionButtons();
}

void CDCMDlg::OnTest() {
    const auto windows = CheckedWindows();
    if (windows.empty()) {
        SetStatus(_T("check windows first"));
        AppendDebug(_T("test ignored: no checked window"), LogLevel::Warning);
        return;
    }
    if (!m_taskManager) {
        SetStatus(_T("task manager is not ready"));
        AppendDebug(_T("test ignored: task manager is not ready"), LogLevel::Warning);
        return;
    }
    if (!m_dmInitDone.load()) {
        SetStatus(_T("dm registration is still running"));
        AppendDebug(_T("test ignored: dm registration is still running"), LogLevel::Warning);
        return;
    }
    if (!m_dmRegistered.load()) {
        SetStatus(_T("dm registration failed"));
        AppendDebug(_T("test ignored: dm registration failed"), LogLevel::Exception);
        return;
    }

    const size_t requested = m_taskManager->RequestTest(CheckedHandles());
    if (requested == 0) {
        SetStatus(_T("start the selected window first"));
        AppendDebug(_T("test ignored: selected windows are stopped"), LogLevel::Warning);
        return;
    }

    SetStatus(_T("Fub_Test requested"));
    CString message;
    message.Format(_T("test requested: Fub_Test queued for %u window(s)"),
                   static_cast<unsigned>(requested));
    AppendDebug(message, LogLevel::Normal);
}

void CDCMDlg::OnRunRouteForward() {
    RequestRouteRun(false);
}

void CDCMDlg::OnRunRouteReverse() {
    RequestRouteRun(true);
}

void CDCMDlg::RequestRouteRun(bool reverse) {
    const auto windows = CheckedWindows();
    if (windows.empty()) {
        SetStatus(_T("check windows first"));
        AppendDebug(_T("route ignored: no checked window"), LogLevel::Warning);
        return;
    }
    if (!m_taskManager) {
        SetStatus(_T("task manager is not ready"));
        AppendDebug(_T("route ignored: task manager is not ready"), LogLevel::Warning);
        return;
    }
    if (!m_dmInitDone.load()) {
        SetStatus(_T("dm registration is still running"));
        AppendDebug(_T("route ignored: dm registration is still running"), LogLevel::Warning);
        return;
    }
    if (!m_dmRegistered.load()) {
        SetStatus(_T("dm registration failed"));
        AppendDebug(_T("route ignored: dm registration failed"), LogLevel::Exception);
        return;
    }

    const size_t requested = m_taskManager->RequestRoute(reverse, CheckedHandles());
    if (requested == 0) {
        SetStatus(_T("start the selected window first"));
        AppendDebug(_T("route ignored: selected windows are stopped"), LogLevel::Warning);
        return;
    }

    const LPCTSTR direction = reverse ? _T("reverse") : _T("forward");
    CString status;
    status.Format(_T("%s route requested"), direction);
    SetStatus(status);
    CString message;
    message.Format(_T("%s route requested for %u window(s)"),
                   direction, static_cast<unsigned>(requested));
    AppendDebug(message, LogLevel::Normal);
}

void CDCMDlg::OnContextMenu(CWnd* wnd, CPoint point) {
    if (!wnd || wnd->GetSafeHwnd() != m_debug.GetSafeHwnd()) {
        CDialogEx::OnContextMenu(wnd, point);
        return;
    }

    if (point.x == -1 && point.y == -1) {
        CRect rect;
        m_debug.GetWindowRect(&rect);
        point = rect.CenterPoint();
    }

    CMenu menu;
    if (!menu.CreatePopupMenu()) return;
    menu.AppendMenu(MF_STRING, ID_DEBUG_SAVE, _T("保存文本"));
    menu.AppendMenu(MF_STRING, ID_DEBUG_CLEAR, _T("清空文本框"));
    menu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RIGHTBUTTON, point.x, point.y, this);
}

void CDCMDlg::OnDebugSave() {
    CFileDialog dialog(
        FALSE, _T("txt"), _T("debug.txt"),
        OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT,
        _T("文本文件 (*.txt)|*.txt|所有文件 (*.*)|*.*||"), this);
    if (dialog.DoModal() != IDOK) return;

    CString text;
    m_debug.GetWindowText(text);

#ifdef _UNICODE
    const int sourceLength = static_cast<int>(text.GetLength());
    const int byteLength = ::WideCharToMultiByte(
        CP_UTF8, 0, text.GetString(), sourceLength, nullptr, 0, nullptr, nullptr);
    std::string utf8;
    if (byteLength > 0) {
        utf8.resize(static_cast<size_t>(byteLength));
        ::WideCharToMultiByte(CP_UTF8, 0, text.GetString(), sourceLength,
                              utf8.data(), byteLength, nullptr, nullptr);
    }
#else
    const int wideLength = ::MultiByteToWideChar(
        CP_ACP, 0, text.GetString(), static_cast<int>(text.GetLength()),
        nullptr, 0);
    std::wstring wide;
    if (wideLength > 0) {
        wide.resize(static_cast<size_t>(wideLength));
        ::MultiByteToWideChar(CP_ACP, 0, text.GetString(),
                              static_cast<int>(text.GetLength()),
                              wide.data(), wideLength);
    }
    const int byteLength = wide.empty()
        ? 0
        : ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength,
                                nullptr, 0, nullptr, nullptr);
    std::string utf8;
    if (byteLength > 0) {
        utf8.resize(static_cast<size_t>(byteLength));
        ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength,
                              utf8.data(), byteLength, nullptr, nullptr);
    }
#endif

    CFile file;
    if (!file.Open(dialog.GetPathName(), CFile::modeCreate | CFile::modeWrite | CFile::typeBinary)) {
        AppendDebug(_T("debug output save failed"), LogLevel::Exception);
        return;
    }

    static const BYTE utf8Bom[] = { 0xEF, 0xBB, 0xBF };
    file.Write(utf8Bom, sizeof(utf8Bom));
    if (!utf8.empty()) {
        file.Write(utf8.data(), static_cast<UINT>(utf8.size()));
    }
    file.Close();

    CString message;
    message.Format(_T("debug output saved: %s"), dialog.GetPathName().GetString());
    AppendDebug(message, LogLevel::Normal);
}

void CDCMDlg::OnDebugClear() {
    m_debug.SetWindowText(_T(""));
}

void CDCMDlg::OnTaskRunListDblClick(NMHDR* pNMHDR, LRESULT* result) {
    const auto* item = reinterpret_cast<const LPNMITEMACTIVATE>(pNMHDR);
    if (item && item->iItem >= 0 && item->iItem < m_taskRunList.GetItemCount()) {
        const CString task = m_taskRunList.GetItemText(item->iItem, 0);
        m_taskRunList.DeleteItem(item->iItem);
        CString message;
        message.Format(_T("task removed: %s"), task.GetString());
        AppendDebug(message, LogLevel::Normal);
    }
    if (result) *result = 0;
}

bool CDCMDlg::EnsureWindowToolsDm() {
    if (!m_dmRegistered.load()) {
        SetStatus(_T("请等待大漠注册完成"));
        return false;
    }
    if (!m_windowToolsDm) {
        m_windowToolsDm = std::make_unique<dmsoft>();
        m_windowToolsDm->SetShowErrorMsg(0);
    }
    if (m_windowToolsDm->Ver().IsEmpty()) {
        SetStatus(_T("大漠工具对象初始化失败"));
        m_windowToolsDm.reset();
        return false;
    }
    return true;
}

void CDCMDlg::OnMultiOpen() {
    CTlbbMultiOpenDlg dialog(this);
    dialog.DoModal();
    RefreshWindows();
}

void CDCMDlg::OnCapture() {
    const auto windows = CheckedWindows();
    if (windows.empty()) { SetStatus(_T("请先勾选要截图的窗口")); return; }
    if (!EnsureWindowToolsDm()) return;
    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module, _countof(module));
    const auto directory = std::filesystem::path(module).parent_path() / L"captures" /
        CTime::GetCurrentTime().Format(L"%Y%m%d").GetString();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) { SetStatus(_T("截图目录创建失败")); return; }
    unsigned captured = 0;
    for (const auto& window : windows) {
        if (!window || !IsWindow(window->hWnd)) continue;
        const HWND hwnd = window->hWnd;
        CString message;
        if (::IsIconic(hwnd) || !::IsWindowVisible(hwnd)) {
            message.Format(L"截图跳过 HWND=%p：请先恢复并显示窗口", hwnd);
            AppendDebug(message, LogLevel::Warning);
            continue;
        }
        const long dmHwnd = static_cast<long>(reinterpret_cast<INT_PTR>(hwnd));
        // Existing workers use foreground display. Keep their binding untouched.
        m_windowToolsDm->SetWindowState(dmHwnd, 1);
        ::Sleep(200);
        if (::GetAncestor(::GetForegroundWindow(), GA_ROOT) != ::GetAncestor(hwnd, GA_ROOT)) {
            AppendDebug(_T("截图跳过：目标窗口激活失败"), LogLevel::Warning);
            continue;
        }
        RECT client{};
        POINT origin{};
        if (!::GetClientRect(hwnd, &client) || !::ClientToScreen(hwnd, &origin) ||
            client.right <= 0 || client.bottom <= 0) continue;
        const CRect screen(GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
            GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
            GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN));
        if (origin.x < screen.left || origin.y < screen.top ||
            origin.x + client.right > screen.right || origin.y + client.bottom > screen.bottom) {
            AppendDebug(_T("截图跳过：请将窗口完整移入屏幕"), LogLevel::Warning);
            continue;
        }
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        SYSTEMTIME now{};
        GetLocalTime(&now);
        CString filename;
        filename.Format(L"%02u%02u%02u_%03u_pid%lu_hwnd%p_%llu.png",
            now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, pid, hwnd, GetTickCount64());
        const auto path = directory / filename.GetString();
        const bool ok = m_windowToolsDm->CapturePng(origin.x, origin.y,
            origin.x + client.right, origin.y + client.bottom, path.c_str()) == 1;
        if (ok) ++captured;
        message.Format(ok ? L"截图已保存：%s" : L"截图失败：%s", path.c_str());
        AppendDebug(message, ok ? LogLevel::Normal : LogLevel::Warning);
    }
    CString status;
    status.Format(L"截图完成 %u/%u，保存至 captures 目录", captured, static_cast<unsigned>(windows.size()));
    SetStatus(status);
}

void CDCMDlg::OnPauseVerify() {
    if (m_verifyPause.IsPaused()) {
        for (const auto& message : m_verifyPause.Resume()) AppendDebug(message, LogLevel::Normal);
    } else {
        if (!EnsureWindowToolsDm()) return;
        // Flag 1 = title; flag 16 = visible. Recheck the exact title before pausing.
        const CString windows = m_windowToolsDm->EnumWindow(0, L"CEF_Control_Example", L"", 1 + 16);
        int offset = 0;
        CString token;
        while (!(token = windows.Tokenize(L",", offset)).IsEmpty()) {
            const HWND hwnd = reinterpret_cast<HWND>(static_cast<UINT_PTR>(_tcstoui64(token, nullptr, 10)));
            CString message;
            const bool ok = m_verifyPause.PauseWindow(hwnd, message);
            AppendDebug(message, ok ? LogLevel::Normal : LogLevel::Warning);
        }
        if (!m_verifyPause.IsPaused()) AppendDebug(_T("未暂停：未找到匹配窗口或操作失败"), LogLevel::Warning);
    }
    m_pauseVerify.SetWindowText(m_verifyPause.IsPaused() ? L"恢复验证窗口" : L"暂停验证窗口");
    SetStatus(m_verifyPause.IsPaused() ? L"验证窗口进程已暂停，点击可恢复" : L"当前无本程序暂停的验证进程");
}

void CDCMDlg::OnDistance() {
    DetectCaptchaType();
}

namespace {

// Grab captcha data: read the verification process memory, save data blocks
// and decode embedded base64 images.
constexpr SIZE_T kGrabChunk = 1u << 20;
constexpr SIZE_T kGrabWindow = 512u << 10;
constexpr SIZE_T kGrabMaxRegion = 64u << 20;

int Base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::vector<unsigned char> DecodeBase64(const std::string& text) {
    std::vector<unsigned char> output;
    output.reserve(text.size() * 3 / 4 + 4);
    int accumulator = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=') break;
        const int value = Base64Value(c);
        if (value < 0) continue;
        accumulator = (accumulator << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<unsigned char>((accumulator >> bits) & 0xFF));
        }
    }
    return output;
}

bool BytesEqual(const unsigned char* data, size_t size, size_t offset, const void* pattern, size_t length) {
    return offset + length <= size && memcmp(data + offset, pattern, length) == 0;
}

struct GrabOutcome {
    unsigned blocks = 0;
    unsigned images = 0;
    unsigned regions = 0;
    CString type;
    CString id;
    CString markers;
};

void SaveGrabFile(const std::filesystem::path& directory, const wchar_t* name, const void* data, size_t size) {
    const auto path = directory / name;
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    ::WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr);
    ::CloseHandle(file);
}

void NoteMarker(GrabOutcome& outcome, const wchar_t* marker) {
    if (outcome.markers.Find(marker) >= 0) return;
    if (!outcome.markers.IsEmpty()) outcome.markers += L",";
    outcome.markers += marker;
}

void ParseJsonFields(const unsigned char* data, size_t size, GrabOutcome& outcome) {
    const std::string text(reinterpret_cast<const char*>(data), std::min<size_t>(size, 4096));
    const auto extract = [&text](const char* key, CString& target) {
        if (!target.IsEmpty()) return;
        const size_t position = text.find(key);
        if (position == std::string::npos) return;
        const size_t begin = position + strlen(key);
        const size_t end = text.find('"', begin);
        if (end == std::string::npos || end - begin > 64) return;
        target = CString(std::string(text, begin, end - begin).c_str());
    };
    extract("\"type\":\"", outcome.type);
    extract("\"id\":\"", outcome.id);
}

void ExtractImages(HANDLE process, uintptr_t hit, const std::filesystem::path& directory, GrabOutcome& outcome) {
    if (outcome.images >= 12) return;
    std::vector<unsigned char> window(kGrabWindow);
    const uintptr_t start = hit > 4096 ? hit - 4096 : 0;
    SIZE_T read = 0;
    if (!::ReadProcessMemory(process, reinterpret_cast<LPCVOID>(start), window.data(), window.size(), &read) || read < 64) return;
    const std::wstring text(reinterpret_cast<const wchar_t*>(window.data()), read / sizeof(wchar_t));
    size_t position = 0;
    while ((position = text.find(L"data:image/", position)) != std::wstring::npos && outcome.images < 12) {
        const size_t semicolon = text.find(L';', position);
        const size_t tag = text.find(L"base64,", position);
        if (semicolon == std::wstring::npos || tag == std::wstring::npos || tag < semicolon) { position += 11; continue; }
        const std::wstring subtype = text.substr(position + 11, semicolon - position - 11);
        const size_t begin = tag + 7;
        size_t end = begin;
        while (end < text.size()) {
            const wchar_t c = text[end];
            const bool base64Char = (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') ||
                (c >= L'a' && c <= L'z') || c == L'+' || c == L'/' || c == L'=';
            if (!base64Char) break;
            ++end;
        }
        std::string encoded;
        encoded.reserve(end - begin);
        for (size_t i = begin; i < end; ++i) encoded.push_back(static_cast<char>(text[i]));
        const auto bytes = DecodeBase64(encoded);
        if (bytes.size() > 256) {
            ++outcome.images;
            const std::wstring extension = subtype == L"jpeg" ? L"jpg" : subtype;
            wchar_t name[64]{};
            swprintf_s(name, L"image_%02u.%s", outcome.images, extension.c_str());
            SaveGrabFile(directory, name, bytes.data(), bytes.size());
        }
        position = end;
    }
}

void SaveHitWindow(HANDLE process, uintptr_t hit, const std::filesystem::path& directory, unsigned index,
                   const wchar_t* kind, GrabOutcome& outcome) {
    std::vector<unsigned char> window(kGrabWindow);
    const uintptr_t start = hit > 8192 ? hit - 8192 : 0;
    SIZE_T read = 0;
    if (!::ReadProcessMemory(process, reinterpret_cast<LPCVOID>(start), window.data(), window.size(), &read) || read == 0) return;
    wchar_t name[64]{};
    swprintf_s(name, L"block_%02u_%s.bin", index, kind);
    SaveGrabFile(directory, name, window.data(), read);
    ++outcome.blocks;
}

GrabOutcome ScanCaptchaProcess(DWORD pid, const std::filesystem::path& directory, ULONGLONG deadline) {
    GrabOutcome outcome;
    HANDLE process = ::OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!process) return outcome;
    static const char* kUtf8Markers[] = { "\"captcha\"", "{\"id\":\"", "gen/random", "check3" };
    static const wchar_t* kUtf16Markers[] = { L"data:image", L"WORD_ORDER_IMAGE_CLICK", L"WORD_IMAGE_CLICK",
        L"HONEYCOMB_IMAGE_CLICK", L"IMAGE_CLICK", L"SLIDER", L"ROTATE", L"CONCAT", L"JIGSAW", L"SCRAPE", L"CURVE" };
    std::vector<unsigned char> buffer(kGrabChunk);
    MEMORY_BASIC_INFORMATION info{};
    uintptr_t address = 0x10000;
    unsigned index = 0;
    while (address < 0x7FFF0000 && ::GetTickCount64() < deadline) {
        if (!::VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &info, sizeof(info))) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(info.BaseAddress);
        const SIZE_T size = info.RegionSize;
        const DWORD protect = info.Protect & 0xFF;
        const bool readable = info.State == MEM_COMMIT && protect != PAGE_NOACCESS && (info.Protect & PAGE_GUARD) == 0;
        if (readable && size >= 0x1000 && size <= kGrabMaxRegion) {
            ++outcome.regions;
            for (SIZE_T offset = 0; offset < size && ::GetTickCount64() < deadline; offset += kGrabChunk - 64) {
                const SIZE_T want = std::min<SIZE_T>(kGrabChunk, size - offset);
                SIZE_T read = 0;
                if (!::ReadProcessMemory(process, reinterpret_cast<LPCVOID>(base + offset), buffer.data(), want, &read) || read == 0) break;
                for (const char* marker : kUtf8Markers) {
                    const size_t length = strlen(marker);
                    for (size_t i = 0; i + length <= read; ++i) {
                        if (!BytesEqual(buffer.data(), read, i, marker, length)) continue;
                        ++index;
                        SaveHitWindow(process, base + offset + i, directory, index, L"json", outcome);
                        ParseJsonFields(buffer.data() + i, read - i, outcome);
                        i += length - 1;
                    }
                }
                for (const wchar_t* marker : kUtf16Markers) {
                    const size_t length = wcslen(marker) * sizeof(wchar_t);
                    for (size_t i = 0; i + length <= read; ++i) {
                        if (!BytesEqual(buffer.data(), read, i, marker, length)) continue;
                        NoteMarker(outcome, marker);
                        if (wcscmp(marker, L"data:image") == 0) {
                            ++index;
                            SaveHitWindow(process, base + offset + i, directory, index, L"image", outcome);
                            ExtractImages(process, base + offset + i, directory, outcome);
                        }
                        i += length - 1;
                    }
                }
            }
        }
        const uintptr_t next = base + size;
        if (next <= address) break;
        address = next;
    }
    ::CloseHandle(process);
    return outcome;
}

std::vector<DWORD> ChildProcessIds(DWORD parent) {
    std::vector<DWORD> result;
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (::Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ParentProcessID == parent) result.push_back(entry.th32ProcessID);
        } while (::Process32NextW(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
    return result;
}

// Lightweight challenge-type detection: locate the captcha JSON in a process
// and read its type and id without saving any files.
bool LooksLikeChallengeId(const CString& id) {
    // 放宽为常见 id 字符集（字母/数字/下划线/短横线，长度 8~80），
    // 避免新型验证码因 id 不是"<类型>_<32位十六进制>"而被整体丢弃。
    const int length = id.GetLength();
    if (length < 8 || length > 80) return false;
    for (int i = 0; i < length; ++i) {
        const wchar_t c = id[i];
        const bool ok = (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') ||
            (c >= L'a' && c <= L'z') || c == L'_' || c == L'-';
        if (!ok) return false;
    }
    return true;
}

// Full challenge capture: find the captcha JSON in a process, then read the
// whole JSON window so the type, the id and the embedded images are available.
struct CaptchaChallenge {
    CString type;
    CString id;
    std::vector<unsigned char> backgroundImage;
    std::vector<unsigned char> templateImage;
    std::string attr1;
    int backgroundWidth = 0;
    int backgroundHeight = 0;
    int templateWidth = 0;
    int templateHeight = 0;
    // SLIDER2：拖动距离在 "data":{"attr":<数字>}。
    double sliderAttr = 0.0;
    bool hasSliderAttr = false;
};

bool JsonStringField(const std::string& text, const char* key, CString& value) {
    const size_t position = text.find(key);
    if (position == std::string::npos) return false;
    const size_t begin = position + strlen(key);
    const size_t end = text.find('"', begin);
    if (end == std::string::npos || end - begin > 64) return false;
    value = CString(std::string(text, begin, end - begin).c_str());
    return true;
}

bool JsonRawStringField(const std::string& text, const char* key, std::string& value) {
    const size_t position = text.find(key);
    if (position == std::string::npos) return false;
    const size_t begin = position + strlen(key);
    const size_t end = text.find('"', begin);
    if (end == std::string::npos || end - begin > 512) return false;
    value.assign(text, begin, end - begin);
    return true;
}

bool JsonIntField(const std::string& text, const char* key, int& value) {
    const size_t position = text.find(key);
    if (position == std::string::npos) return false;
    value = ::atoi(text.c_str() + position + strlen(key));
    return true;
}

bool JsonImageField(const std::string& text, const char* key, std::vector<unsigned char>& bytes) {
    const size_t position = text.find(key);
    if (position == std::string::npos) return false;
    const size_t tag = text.find("base64,", position);
    if (tag == std::string::npos) return false;
    const size_t begin = tag + 7;
    size_t end = begin;
    while (end < text.size()) {
        const char c = text[end];
        const bool base64Char = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || c == '+' || c == '/' || c == '=';
        if (!base64Char) break;
        ++end;
    }
    if (end <= begin) return false;
    bytes = DecodeBase64(std::string(text, begin, end - begin));
    return !bytes.empty();
}

bool ParseCaptchaChallenge(const std::string& json, CaptchaChallenge& challenge) {
    if (!JsonStringField(json, "\"id\":\"", challenge.id) || !LooksLikeChallengeId(challenge.id)) return false;
    if (!JsonStringField(json, "\"type\":\"", challenge.type) || challenge.type.IsEmpty()) {
        challenge.type = challenge.id.Left(challenge.id.GetLength() - 33);
    }
    JsonImageField(json, "\"backgroundImage\":\"", challenge.backgroundImage);
    JsonImageField(json, "\"templateImage\":\"", challenge.templateImage);
    JsonRawStringField(json, "\"attr1\":\"", challenge.attr1);
    // SLIDER2：拖动距离在 "data":{"attr":<数字>}。
    static const char kSliderAttrKey[] = "\"data\":{\"attr\":";
    const size_t sliderAttrPosition = json.find(kSliderAttrKey);
    if (sliderAttrPosition != std::string::npos) {
        challenge.sliderAttr = ::atof(json.c_str() + sliderAttrPosition + sizeof(kSliderAttrKey) - 1);
        challenge.hasSliderAttr = true;
    }
    JsonIntField(json, "\"backgroundImageWidth\":", challenge.backgroundWidth);
    JsonIntField(json, "\"backgroundImageHeight\":", challenge.backgroundHeight);
    JsonIntField(json, "\"templateImageWidth\":", challenge.templateWidth);
    JsonIntField(json, "\"templateImageHeight\":", challenge.templateHeight);
    return true;
}

bool FindCaptchaChallengeInProcess(DWORD pid, CaptchaChallenge& challenge, ULONGLONG deadline) {
    HANDLE process = ::OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!process) return false;
    static const char kIdKey[] = "{\"id\":\"";
    constexpr size_t kIdKeyLength = sizeof(kIdKey) - 1;
    constexpr SIZE_T kChallengeWindow = 1u << 20;
    std::vector<unsigned char> buffer(kGrabChunk + 64);
    std::vector<unsigned char> window(kChallengeWindow);
    MEMORY_BASIC_INFORMATION info{};
    uintptr_t address = 0x10000;
    bool found = false;
    while (!found && address < 0x7FFF0000 && ::GetTickCount64() < deadline) {
        if (!::VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &info, sizeof(info))) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(info.BaseAddress);
        const SIZE_T size = info.RegionSize;
        const DWORD protect = info.Protect & 0xFF;
        const bool writable = protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
            protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
        if (info.State == MEM_COMMIT && writable && (info.Protect & PAGE_GUARD) == 0 && size >= 0x1000) {
            for (SIZE_T offset = 0; offset < size && !found && ::GetTickCount64() < deadline; offset += kGrabChunk) {
                const SIZE_T want = std::min<SIZE_T>(kGrabChunk + 64, size - offset);
                SIZE_T read = 0;
                if (!::ReadProcessMemory(process, reinterpret_cast<LPCVOID>(base + offset), buffer.data(), want, &read) || read < 16) break;
                const std::string text(reinterpret_cast<const char*>(buffer.data()), read);
                size_t position = 0;
                while (!found && (position = text.find(kIdKey, position)) != std::string::npos) {
                    const uintptr_t jsonAddress = base + offset + position;
                    SIZE_T jsonRead = 0;
                    if (::ReadProcessMemory(process, reinterpret_cast<LPCVOID>(jsonAddress), window.data(), window.size(), &jsonRead) && jsonRead >= 64) {
                        const std::string json(reinterpret_cast<const char*>(window.data()), jsonRead);
                        if (ParseCaptchaChallenge(json, challenge)) found = true;
                    }
                    position += kIdKeyLength;
                }
            }
        }
        const uintptr_t next = base + size;
        if (next <= address) break;
        address = next;
    }
    ::CloseHandle(process);
    return found;
}

bool CaptureCaptchaChallenge(DWORD windowPid, CaptchaChallenge& challenge, DWORD& sourcePid, CString& error) {
    // 验证数据在 CEF 子进程（渲染进程）里的概率更高，先扫子进程再扫窗口
    // 进程本身；每个进程单独给时限，避免大进程耗尽总预算。
    std::vector<DWORD> pids;
    for (const DWORD child : ChildProcessIds(windowPid)) pids.push_back(child);
    pids.push_back(windowPid);
    constexpr ULONGLONG kPerProcessLimit = 6000;
    for (const DWORD pid : pids) {
        const ULONGLONG deadline = ::GetTickCount64() + kPerProcessLimit;
        if (FindCaptchaChallengeInProcess(pid, challenge, deadline)) {
            sourcePid = pid;
            return true;
        }
    }
    error = _T("captcha: challenge not found in verification process");
    return false;
}

// ROTATE solving: the circular template piece must be rotated until it matches
// the surrounding scene.  tac.min.js rotates the piece by
// moveX / (242 / 360) degrees, so the slider offset is moveX = angle * 242/360.
struct RotateResult {
    double angle = 0.0;
    double moveX = 0.0;
};

struct Image32 {
    int width = 0;
    int height = 0;
    int stride = 0;
    const unsigned char* pixels = nullptr;
    void Get(int x, int y, int& r, int& g, int& b, int& a) const {
        const unsigned char* p = pixels + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
        b = p[0]; g = p[1]; r = p[2]; a = p[3];
    }
};

constexpr double kRotatePi = 3.14159265358979323846;

void SampleBand(const Image32& image, double centerX, double centerY, double inner, double outer,
                int steps, std::vector<double>& profile) {
    for (int a = 0; a < steps; ++a) {
        const double angle = a * 2.0 * kRotatePi / steps;
        const double ca = std::cos(angle);
        const double sa = std::sin(angle);
        double sumR = 0, sumG = 0, sumB = 0;
        int count = 0;
        for (double r = inner; r <= outer; r += 1.0) {
            const int x = static_cast<int>(std::lround(centerX + r * ca));
            const int y = static_cast<int>(std::lround(centerY + r * sa));
            if (x < 0 || y < 0 || x >= image.width || y >= image.height) continue;
            int rr = 0, gg = 0, bb = 0, aa = 0;
            image.Get(x, y, rr, gg, bb, aa);
            sumR += rr; sumG += gg; sumB += bb;
            ++count;
        }
        if (count > 0) {
            profile[a * 3 + 0] = sumR / count;
            profile[a * 3 + 1] = sumG / count;
            profile[a * 3 + 2] = sumB / count;
        }
    }
}

bool SolveRotate(const Image32& background, const Image32& piece, RotateResult& result, CString& error) {
    // Piece center: bounding box of the opaque pixels.
    int minX = piece.width, minY = piece.height, maxX = -1, maxY = -1;
    long opaque = 0;
    for (int y = 0; y < piece.height; ++y) {
        for (int x = 0; x < piece.width; ++x) {
            int rr = 0, gg = 0, bb = 0, aa = 0;
            piece.Get(x, y, rr, gg, bb, aa);
            if (aa <= 32) continue;
            ++opaque;
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
        }
    }
    if (opaque < 500 || maxX <= minX || maxY <= minY) {
        error = _T("rotate: template piece not found");
        return false;
    }
    const double pieceCenterX = (minX + maxX) / 2.0;
    const double pieceCenterY = (minY + maxY) / 2.0;

    // Hole center: centroid of the black pixels near the image center.
    const double centerX = background.width / 2.0;
    const double centerY = background.height / 2.0;
    const double searchRadius = std::min<int>(background.width, background.height) * 0.35;
    double holeSumX = 0, holeSumY = 0;
    long holeCount = 0;
    for (int y = 0; y < background.height; ++y) {
        for (int x = 0; x < background.width; ++x) {
            const double dx = x - centerX;
            const double dy = y - centerY;
            if (dx * dx + dy * dy > searchRadius * searchRadius) continue;
            int rr = 0, gg = 0, bb = 0, aa = 0;
            background.Get(x, y, rr, gg, bb, aa);
            if (rr < 30 && gg < 30 && bb < 30) {
                holeSumX += x; holeSumY += y; ++holeCount;
            }
        }
    }
    if (holeCount < 500) {
        error = _T("rotate: background hole not found");
        return false;
    }
    const double holeCenterX = holeSumX / holeCount;
    const double holeCenterY = holeSumY / holeCount;

    // Hole radius: the largest ring that is almost entirely black.
    constexpr int kRingSteps = 720;
    double holeRadius = 0;
    const double maxRadius = std::min<double>(holeCenterX, holeCenterY) * 0.9;
    for (double r = 20; r <= maxRadius; r += 1.0) {
        int black = 0, total = 0;
        for (int a = 0; a < kRingSteps; ++a) {
            const double angle = a * 2.0 * kRotatePi / kRingSteps;
            const int x = static_cast<int>(std::lround(holeCenterX + r * std::cos(angle)));
            const int y = static_cast<int>(std::lround(holeCenterY + r * std::sin(angle)));
            if (x < 0 || y < 0 || x >= background.width || y >= background.height) continue;
            int rr = 0, gg = 0, bb = 0, aa = 0;
            background.Get(x, y, rr, gg, bb, aa);
            ++total;
            if (rr < 30 && gg < 30 && bb < 30) ++black;
        }
        if (total > 0 && black * 100 >= total * 95) holeRadius = r;
        else if (holeRadius > 0) break;
    }
    if (holeRadius < 20) {
        error = _T("rotate: hole radius not found");
        return false;
    }

    // Compare the piece content ring with the scene just outside the hole for
    // every candidate rotation; the best match is the required angle.
    constexpr int kSteps = 1440;  // 0.25 degree
    std::vector<double> pieceProfile(kSteps * 3, 0.0);
    std::vector<double> backgroundProfile(kSteps * 3, 0.0);
    SampleBand(piece, pieceCenterX, pieceCenterY, holeRadius * 0.68, holeRadius * 0.94, kSteps, pieceProfile);
    SampleBand(background, holeCenterX, holeCenterY, holeRadius + 21.0, holeRadius + 35.0, kSteps, backgroundProfile);

    const auto errorAt = [&](int shift, int stride) {
        double total = 0;
        for (int a = 0; a < kSteps; a += stride) {
            const int pa = ((a - shift) % kSteps + kSteps) % kSteps;
            for (int c = 0; c < 3; ++c) {
                const double d = pieceProfile[pa * 3 + c] - backgroundProfile[a * 3 + c];
                total += d * d;
            }
        }
        return total;
    };
    int bestStep = 0;
    double bestError = errorAt(0, 4);
    for (int step = 4; step < kSteps; step += 4) {
        const double e = errorAt(step, 4);
        if (e < bestError) { bestError = e; bestStep = step; }
    }
    int fineBest = bestStep;
    double fineError = -1.0;
    for (int step = bestStep - 6; step <= bestStep + 6; ++step) {
        const int wrapped = ((step % kSteps) + kSteps) % kSteps;
        const double e = errorAt(wrapped, 1);
        if (fineError < 0.0 || e < fineError) { fineError = e; fineBest = wrapped; }
    }
    result.angle = fineBest * 360.0 / kSteps;
    result.moveX = result.angle * 242.0 / 360.0;
    return true;
}

bool ComputeRotateAngle(const CaptchaChallenge& challenge, RotateResult& result, CString& error) {
    if (challenge.backgroundImage.empty() || challenge.templateImage.empty()) {
        error = _T("rotate: challenge images missing");
        return false;
    }
    Gdiplus::GdiplusStartupInput startupInput;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startupInput, nullptr) != Gdiplus::Ok) {
        error = _T("rotate: gdiplus startup failed");
        return false;
    }
    bool ok = false;
    IStream* backgroundStream = ::SHCreateMemStream(challenge.backgroundImage.data(), static_cast<UINT>(challenge.backgroundImage.size()));
    IStream* pieceStream = ::SHCreateMemStream(challenge.templateImage.data(), static_cast<UINT>(challenge.templateImage.size()));
    Gdiplus::Bitmap* background = backgroundStream ? Gdiplus::Bitmap::FromStream(backgroundStream) : nullptr;
    Gdiplus::Bitmap* piece = pieceStream ? Gdiplus::Bitmap::FromStream(pieceStream) : nullptr;
    if (background && piece) {
        Gdiplus::Rect backgroundRect(0, 0, static_cast<INT>(background->GetWidth()), static_cast<INT>(background->GetHeight()));
        Gdiplus::Rect pieceRect(0, 0, static_cast<INT>(piece->GetWidth()), static_cast<INT>(piece->GetHeight()));
        Gdiplus::BitmapData backgroundData{};
        Gdiplus::BitmapData pieceData{};
        if (background->LockBits(&backgroundRect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &backgroundData) == Gdiplus::Ok &&
            piece->LockBits(&pieceRect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &pieceData) == Gdiplus::Ok) {
            Image32 backgroundView;
            backgroundView.width = static_cast<int>(backgroundData.Width);
            backgroundView.height = static_cast<int>(backgroundData.Height);
            backgroundView.stride = static_cast<int>(backgroundData.Stride);
            backgroundView.pixels = static_cast<const unsigned char*>(backgroundData.Scan0);
            Image32 pieceView;
            pieceView.width = static_cast<int>(pieceData.Width);
            pieceView.height = static_cast<int>(pieceData.Height);
            pieceView.stride = static_cast<int>(pieceData.Stride);
            pieceView.pixels = static_cast<const unsigned char*>(pieceData.Scan0);
            ok = SolveRotate(backgroundView, pieceView, result, error);
            piece->UnlockBits(&pieceData);
            background->UnlockBits(&backgroundData);
        } else {
            error = _T("rotate: lock image bits failed");
        }
    } else {
        error = _T("rotate: decode image failed");
    }
    delete background;
    delete piece;
    if (backgroundStream) backgroundStream->Release();
    if (pieceStream) pieceStream->Release();
    Gdiplus::GdiplusShutdown(token);
    return ok;
}

// CURVE3 solving: the background JPEG contains a groove that matches the
// double-Bezier path drawn by the slider.  tac.min.js scales the control
// points by moveX/300 (x) and moveX/180 (y); the answer is the moveX whose
// curve best fits the groove (score: bright rims, dark centre).
struct Curve3Coordinates {
    double leftX = 0.0, leftY = 0.0;
    double rightX = 0.0, rightY = 0.0;
    double ctrl1X = 0.0, ctrl1Y = 0.0;
    double ctrl2X = 0.0, ctrl2Y = 0.0;
    double jointX = 0.0, jointY = 0.0;
};

struct Curve3Result {
    int moveX = -1;
    int runnerUpMoveX = -1;
    double score = 0.0;
};

struct Curve3Point {
    double x = 0.0;
    double y = 0.0;
};

struct GrayImage {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels;
};

double SampleCurve3Gray(const GrayImage& image, double x, double y) {
    x = std::clamp(x, 0.0, static_cast<double>(image.width - 1));
    y = std::clamp(y, 0.0, static_cast<double>(image.height - 1));
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int x1 = std::min<int>(x0 + 1, image.width - 1);
    const int y1 = std::min<int>(y0 + 1, image.height - 1);
    const double fx = x - x0;
    const double fy = y - y0;
    const auto at = [&](int px, int py) {
        return static_cast<double>(image.pixels[static_cast<size_t>(py) * image.width + px]);
    };
    const double top = at(x0, y0) * (1.0 - fx) + at(x1, y0) * fx;
    const double bottom = at(x0, y1) * (1.0 - fx) + at(x1, y1) * fx;
    return top * (1.0 - fy) + bottom * fy;
}

void Curve3Segment(const Curve3Point& start, const Curve3Point& control, const Curve3Point& end,
                   double t, Curve3Point& point, Curve3Point& tangent) {
    const double oneMinusT = 1.0 - t;
    point.x = start.x * (oneMinusT * oneMinusT) + control.x * (2.0 * oneMinusT * t) + end.x * (t * t);
    point.y = start.y * (oneMinusT * oneMinusT) + control.y * (2.0 * oneMinusT * t) + end.y * (t * t);
    tangent.x = (control.x - start.x) * (2.0 * oneMinusT) + (end.x - control.x) * (2.0 * t);
    tangent.y = (control.y - start.y) * (2.0 * oneMinusT) + (end.y - control.y) * (2.0 * t);
}

double ScoreCurve3MoveX(const GrayImage& gray, const Curve3Coordinates& coordinates, int moveX) {
    const double moveScaleX = moveX / 300.0;
    const double moveScaleY = moveX / 180.0;
    const Curve3Point left{coordinates.leftX, coordinates.leftY};
    const Curve3Point right{coordinates.rightX, coordinates.rightY};
    const Curve3Point ctrl1{coordinates.ctrl1X * moveScaleX, coordinates.ctrl1Y * moveScaleY};
    const Curve3Point ctrl2{coordinates.ctrl2X * moveScaleX, coordinates.ctrl2Y * moveScaleY};
    const Curve3Point joint{coordinates.jointX * moveScaleX, coordinates.jointY * moveScaleY};
    const double imageScaleX = gray.width / 300.0;
    const double imageScaleY = gray.height / 180.0;
    const double normalStep = std::min<double>(imageScaleX, imageScaleY) * 0.5;
    std::vector<double> profile(49, 0.0);
    int sampleCount = 0;
    for (int segment = 0; segment < 2; ++segment) {
        const Curve3Point& start = segment == 0 ? left : joint;
        const Curve3Point& control = segment == 0 ? ctrl1 : ctrl2;
        const Curve3Point& end = segment == 0 ? joint : right;
        for (int step = 0; step < 100; ++step) {
            const double t = step / 99.0;
            Curve3Point point;
            Curve3Point tangent;
            Curve3Segment(start, control, end, t, point, tangent);
            point.x *= imageScaleX;
            point.y *= imageScaleY;
            tangent.x *= imageScaleX;
            tangent.y *= imageScaleY;
            const double length = std::hypot(tangent.x, tangent.y);
            if (length < 1e-6) continue;
            const double normalX = -tangent.y / length;
            const double normalY = tangent.x / length;
            for (int offset = -24; offset <= 24; ++offset) {
                profile[static_cast<size_t>(offset + 24)] += SampleCurve3Gray(gray,
                    point.x + normalX * offset * normalStep,
                    point.y + normalY * offset * normalStep);
            }
            ++sampleCount;
        }
    }
    if (sampleCount == 0) return -1.0e18;
    for (double& value : profile) value /= sampleCount;
    const auto peak = [&](size_t begin, size_t end) {
        return *std::max_element(profile.begin() + static_cast<std::ptrdiff_t>(begin),
            profile.begin() + static_cast<std::ptrdiff_t>(end));
    };
    const auto average = [&](size_t begin, size_t end) {
        double total = 0;
        for (size_t index = begin; index < end; ++index) total += profile[index];
        return total / static_cast<double>(end - begin);
    };
    const double leftPeak = peak(15, 22);
    const double rightPeak = peak(27, 34);
    const double center = average(22, 27);
    const double outer = (average(0, 10) + average(39, 49)) / 2.0;
    const double pipe = (leftPeak + rightPeak) / 2.0;
    return (pipe - center) + (pipe - outer) - 0.45 * std::abs(leftPeak - rightPeak);
}

bool ComputeCurve3MoveX(const CaptchaChallenge& challenge, Curve3Result& result, CString& error) {
    if (challenge.backgroundImage.empty() || challenge.attr1.empty() ||
        challenge.backgroundWidth <= 0 || challenge.backgroundHeight <= 0) {
        error = _T("curve3: challenge data missing");
        return false;
    }
    std::vector<std::string> parts;
    {
        size_t begin = 0;
        for (;;) {
            const size_t end = challenge.attr1.find('.', begin);
            parts.push_back(challenge.attr1.substr(begin,
                end == std::string::npos ? std::string::npos : end - begin));
            if (end == std::string::npos) break;
            begin = end + 1;
        }
    }
    if (parts.size() < 20) {
        error = _T("curve3: attr1 layout is invalid");
        return false;
    }
    Curve3Coordinates coordinates;
    try {
        const auto pair = [&](size_t index) {
            return std::stod(parts[index] + "." + parts[index + 1]);
        };
        coordinates.leftX = pair(0) * 300.0;
        coordinates.leftY = pair(2) * 180.0;
        coordinates.rightX = pair(4) * 300.0;
        coordinates.rightY = pair(6) * 180.0;
        coordinates.ctrl1X = pair(8) * 300.0 / challenge.backgroundWidth;
        coordinates.ctrl1Y = pair(10) * 180.0 / challenge.backgroundHeight;
        coordinates.ctrl2X = pair(12) * 300.0 / challenge.backgroundWidth;
        coordinates.ctrl2Y = pair(14) * 180.0 / challenge.backgroundHeight;
        coordinates.jointX = pair(16) * 300.0 / challenge.backgroundWidth;
        coordinates.jointY = pair(18) * 180.0 / challenge.backgroundHeight;
    } catch (...) {
        error = _T("curve3: attr1 number conversion failed");
        return false;
    }

    Gdiplus::GdiplusStartupInput startupInput;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startupInput, nullptr) != Gdiplus::Ok) {
        error = _T("curve3: gdiplus startup failed");
        return false;
    }
    bool ok = false;
    GrayImage gray;
    IStream* stream = ::SHCreateMemStream(challenge.backgroundImage.data(),
        static_cast<UINT>(challenge.backgroundImage.size()));
    Gdiplus::Bitmap* bitmap = stream ? Gdiplus::Bitmap::FromStream(stream) : nullptr;
    if (bitmap) {
        Gdiplus::Rect rect(0, 0, static_cast<INT>(bitmap->GetWidth()), static_cast<INT>(bitmap->GetHeight()));
        Gdiplus::BitmapData data{};
        if (bitmap->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) == Gdiplus::Ok) {
            gray.width = static_cast<int>(data.Width);
            gray.height = static_cast<int>(data.Height);
            gray.pixels.assign(static_cast<size_t>(gray.width) * gray.height, 0);
            for (int y = 0; y < gray.height; ++y) {
                const unsigned char* row = static_cast<const unsigned char*>(data.Scan0) +
                    static_cast<size_t>(y) * data.Stride;
                for (int x = 0; x < gray.width; ++x) {
                    const unsigned char* p = row + static_cast<size_t>(x) * 4;
                    gray.pixels[static_cast<size_t>(y) * gray.width + x] =
                        static_cast<unsigned char>((299 * p[2] + 587 * p[1] + 114 * p[0]) / 1000);
                }
            }
            bitmap->UnlockBits(&data);
            ok = true;
        } else {
            error = _T("curve3: lock image bits failed");
        }
    } else {
        error = _T("curve3: decode image failed");
    }
    delete bitmap;
    if (stream) stream->Release();
    Gdiplus::GdiplusShutdown(token);
    if (!ok) return false;

    std::vector<std::pair<double, int>> scores;
    scores.reserve(243);
    for (int moveX = 0; moveX <= 242; ++moveX) {
        scores.emplace_back(ScoreCurve3MoveX(gray, coordinates, moveX), moveX);
    }
    std::sort(scores.begin(), scores.end(), [](const auto& left, const auto& right) {
        return left.first > right.first;
    });
    if (scores.empty()) {
        error = _T("curve3: no candidate scores");
        return false;
    }
    result.score = scores[0].first;
    result.moveX = scores[0].second;
    if (scores.size() > 1) result.runnerUpMoveX = scores[1].second;
    return true;
}

struct Slider2Result {
    int moveX = 0;          // 需要拖动的距离（图像像素，来自 data.attr）
    double attr = 0.0;      // 题目 data.attr 原值
    int holeCenterX = -1;   // 背景图中检测到的缺口中心 X（-1 = 未检测到）
};

// 普通「拖动滑块」没有 data.attr：拖动距离 = 缺口中心 - 滑块初始中心。
// 初始中心为背景图像素值，可按实测微调。
constexpr int kSliderPieceStartX = 74;

bool ComputeSlider2MoveX(const CaptchaChallenge& challenge, Slider2Result& result, CString& error)
{
    // SLIDER2 有 data.attr 直接给出拖动距离；普通滑块（无 attr）退回图像法。
    result.attr = challenge.sliderAttr;
    if (challenge.hasSliderAttr) {
        result.moveX = static_cast<int>(challenge.sliderAttr + 0.5);
    }

    if (challenge.backgroundImage.empty()) {
        if (challenge.hasSliderAttr) return true;
        error = _T("slider: background image missing");
        return false;
    }

    Gdiplus::GdiplusStartupInput startupInput;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startupInput, nullptr) != Gdiplus::Ok) {
        if (challenge.hasSliderAttr) return true;
        error = _T("slider: gdiplus startup failed");
        return false;
    }

    IStream* stream = ::SHCreateMemStream(challenge.backgroundImage.data(),
                                          static_cast<UINT>(challenge.backgroundImage.size()));
    Gdiplus::Bitmap* bitmap = stream ? Gdiplus::Bitmap::FromStream(stream) : nullptr;
    if (bitmap) {
        const INT width = static_cast<INT>(bitmap->GetWidth());
        const INT height = static_cast<INT>(bitmap->GetHeight());
        Gdiplus::Rect rect(0, 0, width, height);
        Gdiplus::BitmapData data{};
        if (width > 0 && height > 0 &&
            bitmap->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) == Gdiplus::Ok) {
            // 缺口 = 暗色、低饱和的密集块：统计每列符合的像素数，
            // 再用约 104 宽的滑动窗口找最密集区间，取中心作为缺口中心 X。
            std::vector<int> columnCounts(static_cast<size_t>(width), 0);
            const BYTE* row = static_cast<const BYTE*>(data.Scan0);
            for (INT y = 0; y < height; ++y) {
                const BYTE* pixel = row + static_cast<ptrdiff_t>(y) * static_cast<ptrdiff_t>(data.Stride);
                for (INT x = 0; x < width; ++x, pixel += 4) {
                    const BYTE b = pixel[0], g = pixel[1], r = pixel[2];
                    const BYTE maxChannel = (std::max)(r, (std::max)(g, b));
                    const BYTE minChannel = (std::min)(r, (std::min)(g, b));
                    if (maxChannel < 95 &&
                        (maxChannel == 0 || (maxChannel - minChannel) * 100 / maxChannel < 35)) {
                        ++columnCounts[static_cast<size_t>(x)];
                    }
                }
            }

            const int window = 104;
            long long running = 0;
            long long bestSum = 0;
            int bestStart = -1;
            for (INT x = 0; x < width; ++x) {
                running += columnCounts[static_cast<size_t>(x)];
                if (x >= window) running -= columnCounts[static_cast<size_t>(x - window)];
                if (x >= window - 1 && running > bestSum) {
                    bestSum = running;
                    bestStart = x - window + 1;
                }
            }
            if (bestStart >= 0 && bestSum > 3000) {
                result.holeCenterX = bestStart + window / 2;
            }
        }
        delete bitmap;
    }
    if (stream) stream->Release();
    Gdiplus::GdiplusShutdown(token);

    if (!challenge.hasSliderAttr) {
        if (result.holeCenterX < 0) {
            error = _T("slider: hole not detected and attr missing");
            return false;
        }
        result.moveX = result.holeCenterX - kSliderPieceStartX;
        if (result.moveX < 0) result.moveX = 0;
    }
    return true;
}

}  // namespace

void CDCMDlg::OnGrabVerify() {
    if (m_grabRunning.exchange(true)) {
        AppendDebug(_T("captcha: previous grab is still running"), LogLevel::Warning);
        return;
    }
    if (!EnsureWindowToolsDm()) { m_grabRunning.store(false); return; }
    const CString windows = m_windowToolsDm->EnumWindow(0, L"CEF_Control_Example", L"", 1 + 16);
    HWND target = nullptr;
    int offset = 0;
    CString token;
    while (!(token = windows.Tokenize(L",", offset)).IsEmpty()) {
        const HWND hwnd = reinterpret_cast<HWND>(static_cast<UINT_PTR>(_tcstoui64(token, nullptr, 10)));
        if (::IsWindow(hwnd) && ::IsWindowVisible(hwnd)) { target = hwnd; break; }
    }
    if (!target) {
        AppendDebug(_T("captcha: CEF_Control_Example window not found"), LogLevel::Warning);
        m_grabRunning.store(false);
        return;
    }
    DWORD pid = 0;
    ::GetWindowThreadProcessId(target, &pid);
    if (!pid || pid == ::GetCurrentProcessId()) {
        AppendDebug(_T("captcha: invalid verification window process"), LogLevel::Warning);
        m_grabRunning.store(false);
        return;
    }
    wchar_t module[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, module, _countof(module));
    const CString stamp = CTime::GetCurrentTime().Format(L"%Y%m%d_%H%M%S");
    const auto directory = std::filesystem::path(module).parent_path() / L"captures" / L"captcha" /
        (std::wstring(L"grab_") + stamp.GetString());
    const HWND self = GetSafeHwnd();
    SetStatus(L"正在抓取验证码数据...");
    std::thread([self, pid, target, directory]() {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        CString text;
        if (error) {
            text = _T("captcha: cannot create output directory");
        } else {
            const ULONGLONG started = ::GetTickCount64();
            CaptchaChallenge challenge;
            CString captureError;
            DWORD sourcePid = 0;
            if (CaptureCaptchaChallenge(pid, challenge, sourcePid, captureError)) {
                const ULONGLONG elapsed = ::GetTickCount64() - started;
                unsigned saved = 0;
                if (!challenge.backgroundImage.empty()) {
                    SaveGrabFile(directory, L"background.jpg", challenge.backgroundImage.data(),
                                 challenge.backgroundImage.size());
                    ++saved;
                }
                if (!challenge.templateImage.empty()) {
                    SaveGrabFile(directory, L"template.png", challenge.templateImage.data(),
                                 challenge.templateImage.size());
                    ++saved;
                }
                if (!challenge.attr1.empty()) {
                    SaveGrabFile(directory, L"attr1.txt", challenge.attr1.data(), challenge.attr1.size());
                    ++saved;
                }
                if (challenge.hasSliderAttr) {
                    CStringA sliderAttrText;
                    sliderAttrText.Format("%.1f", challenge.sliderAttr);
                    SaveGrabFile(directory, L"slider_attr.txt",
                                 sliderAttrText.GetString(), sliderAttrText.GetLength());
                    ++saved;
                }
                CString info;
                info.Format(L"type=%s\r\nid=%s\r\nsource_pid=%lu\r\n"
                            L"background=%dx%d\r\ntemplate=%dx%d\r\nelapsed_ms=%llu\r\n",
                    challenge.type.GetString(), challenge.id.GetString(), sourcePid,
                    challenge.backgroundWidth, challenge.backgroundHeight,
                    challenge.templateWidth, challenge.templateHeight, elapsed);
                const CStringA infoBytes(info);
                SaveGrabFile(directory, L"challenge.txt", infoBytes.GetString(), infoBytes.GetLength());
                CString panelMessage;
                std::filesystem::path panelPath;
                CVerifyMemoryCapture::Save(target, pid, directory, panelPath, panelMessage);
                text.Format(L"captcha: type=%s id=%s pid=%lu files=%u elapsed=%llu ms\n%s\ndir=%s",
                    challenge.type.GetString(), challenge.id.GetString(), sourcePid, saved + 1, elapsed,
                    panelMessage.GetString(), directory.c_str());
            } else {
                const ULONGLONG elapsed = ::GetTickCount64() - started;
                text.Format(L"captcha: %s (elapsed=%llu ms)", captureError.GetString(), elapsed);
            }
        }
        auto* message = new CString(text);
        if (!::PostMessageW(self, WM_APP + 104, 0, reinterpret_cast<LPARAM>(message))) delete message;
    }).detach();
}

LRESULT CDCMDlg::OnGrabVerifyFinished(WPARAM, LPARAM lParam) {
    std::unique_ptr<CString> message(reinterpret_cast<CString*>(lParam));
    if (message) {
        AppendDebug(*message, LogLevel::Normal);
        SetStatus(*message);
    }
    m_grabRunning.store(false);
    return 0;
}

void CDCMDlg::OnDetectCaptchaType() {
    DetectCaptchaType();
}

void CDCMDlg::DetectCaptchaType() {
    if (m_grabRunning.exchange(true)) {
        AppendDebug(_T("captcha: previous memory task is still running"), LogLevel::Warning);
        return;
    }
    if (!EnsureWindowToolsDm()) { m_grabRunning.store(false); return; }
    const CString windows = m_windowToolsDm->EnumWindow(0, L"CEF_Control_Example", L"", 1 + 16);
    HWND target = nullptr;
    int offset = 0;
    CString token;
    while (!(token = windows.Tokenize(L",", offset)).IsEmpty()) {
        const HWND hwnd = reinterpret_cast<HWND>(static_cast<UINT_PTR>(_tcstoui64(token, nullptr, 10)));
        if (::IsWindow(hwnd) && ::IsWindowVisible(hwnd)) { target = hwnd; break; }
    }
    if (!target) {
        AppendDebug(_T("captcha: CEF_Control_Example window not found"), LogLevel::Warning);
        m_grabRunning.store(false);
        return;
    }
    DWORD pid = 0;
    ::GetWindowThreadProcessId(target, &pid);
    if (!pid || pid == ::GetCurrentProcessId()) {
        AppendDebug(_T("captcha: invalid verification window process"), LogLevel::Warning);
        m_grabRunning.store(false);
        return;
    }
    const HWND self = GetSafeHwnd();
    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module, _countof(module));
    const auto directory = std::filesystem::path(module).parent_path() / L"res";
    SetStatus(L"正在从内存保存验证码图片...");
    std::thread([self, pid, target, directory]() {
        CaptchaChallenge challenge;
        CString error;
        DWORD sourcePid = 0;
        CString text;
        CString savedMessage;
        std::filesystem::path savedPath;
        if (!CVerifyMemoryCapture::Save(target, pid, directory, savedPath, savedMessage)) {
            savedMessage += L"；继续识别类型";
        }
        if (CaptureCaptchaChallenge(pid, challenge, sourcePid, error)) {
            text.Format(L"captcha type: %s\nid: %s\npid: %lu",
                challenge.type.GetString(), challenge.id.GetString(), sourcePid);
            if (challenge.type == L"ROTATE") {
                RotateResult rotate;
                CString rotateError;
                if (ComputeRotateAngle(challenge, rotate, rotateError)) {
                    CString line;
                    line.Format(L"\nrotate: angle=%.1f deg  moveX=%.1f / 242", rotate.angle, rotate.moveX);
                    text += line;
                } else {
                    text += L"\n" + rotateError;
                }
            } else if (challenge.type == L"CURVE3") {
                Curve3Result curve;
                CString curveError;
                if (ComputeCurve3MoveX(challenge, curve, curveError)) {
                    CString line;
                    line.Format(L"\ncurve3: moveX=%d (runner-up %d, score=%.2f, range 0-242)",
                        curve.moveX, curve.runnerUpMoveX, curve.score);
                    text += line;
                } else {
                    text += L"\n" + curveError;
                }
            } else if (challenge.type.Left(7) == L"SLIDER") {
                Slider2Result slider;
                CString sliderError;
                if (ComputeSlider2MoveX(challenge, slider, sliderError)) {
                    CString line;
                    if (slider.holeCenterX >= 0) {
                        line.Format(L"\nslider2(%s): moveX=%d (attr=%.1f)  holeCenterX=%d  diff=%d",
                            challenge.type.GetString(), slider.moveX, slider.attr,
                            slider.holeCenterX, slider.holeCenterX - slider.moveX);
                    } else {
                        line.Format(L"\nslider2(%s): moveX=%d (attr=%.1f)  hole not detected",
                            challenge.type.GetString(), slider.moveX, slider.attr);
                    }
                    text += line;
                } else {
                    text += L"\n" + sliderError;
                }
            } else {
                text += L"\n（该类型暂未实现计算）";
            }
        } else {
            text = error;
        }
        if (!savedMessage.IsEmpty()) text = savedMessage + L"\n" + text;
        auto* message = new CString(text);
        if (!::PostMessageW(self, WM_APP + 104, 0, reinterpret_cast<LPARAM>(message))) delete message;
    }).detach();
}

void CDCMDlg::OnCancel() {
    for (const auto& message : m_verifyPause.Resume()) AppendDebug(message, LogLevel::Normal);
    if (m_verifyPause.IsPaused()) {
        SetStatus(_T("验证进程恢复失败，请再次点击恢复验证窗口后退出"));
        return;
    }
    if (m_updateDThread.joinable()) m_updateDThread.join();
    KillTimer(1);
    if (m_taskSchedule) m_taskSchedule->Stop();
    if (m_taskManager) {
        m_taskManager->Stop();
        m_taskManager.reset();
    }
    m_windowToolsDm.reset();
    StopDmRegistration();
    CDialogEx::OnCancel();
}
