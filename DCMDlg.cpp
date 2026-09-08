#include "pch.h"
#include "resource.h"
#include "DCMDlg.h"

BEGIN_MESSAGE_MAP(CDCMDlg, CDialogEx)
    ON_BN_CLICKED(IDC_BUTTON_REFRESH, &CDCMDlg::OnRefresh)
    ON_BN_CLICKED(IDC_BUTTON_START, &CDCMDlg::OnStart)
    ON_BN_CLICKED(IDC_BUTTON_PAUSE, &CDCMDlg::OnPause)
    ON_BN_CLICKED(IDC_BUTTON_TEST, &CDCMDlg::OnTest)
    ON_BN_CLICKED(IDC_BUTTON_TASK_TOGGLE, &CDCMDlg::OnTaskToggle)
    ON_WM_CONTEXTMENU()
    ON_COMMAND(ID_DEBUG_SAVE, &CDCMDlg::OnDebugSave)
    ON_COMMAND(ID_DEBUG_CLEAR, &CDCMDlg::OnDebugClear)
    ON_WM_TIMER()
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_WINDOW_LIST, &CDCMDlg::OnListItemChanged)
    ON_NOTIFY(NM_DBLCLK, IDC_TASK_LIST, &CDCMDlg::OnTaskListDblClick)
    ON_NOTIFY(NM_DBLCLK, IDC_TASK_RUN_LIST, &CDCMDlg::OnTaskRunListDblClick)
    ON_MESSAGE(WM_APP + 101, &CDCMDlg::OnDmRegistrationMessage)
    ON_MESSAGE(WM_APP_WINDOW_THREAD_LOG, &CDCMDlg::OnWindowThreadLog)
END_MESSAGE_MAP()

CDCMDlg::CDCMDlg(CWnd* parent) : CDialogEx(IDD_MAIN_DIALOG, parent) {}

CDCMDlg::~CDCMDlg() { StopDmRegistration(); }

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
    m_list.InsertColumn(1, _T("Title"), LVCFMT_LEFT, 135);
    m_list.InsertColumn(2, _T("Process"), LVCFMT_LEFT, 70);
    m_list.InsertColumn(3, _T("Status"), LVCFMT_LEFT, 85);
    InitTaskLists();
    m_debug.LimitText(100000);
    IncreaseDebugFont();
    m_start.SetWindowText(_T("Start"));
    m_pause.SetWindowText(_T("Pause"));
    m_taskToggle.SetWindowText(_T("\u5f00\u59cb/\u7ed3\u675f"));
    m_test.SetWindowText(_T("\u6d4b\u8bd5"));

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
        const bool running = m_taskManager && m_taskManager->IsRunning(w->hWnd);
        const CString state = running ? (m_taskManager->IsPaused(w->hWnd) ? _T("paused (3)") : _T("running (3)")) : _T("stopped");
        m_list.SetItemText(row, 3, state);
        m_list.SetCheck(row, std::find(keepChecked.begin(), keepChecked.end(), w->hWnd) != keepChecked.end());
    }
    HideHorizontalScrollBar(m_list);
}

void CDCMDlg::UpdateStatuses() {
    for (int i = 0; i < m_list.GetItemCount() && i < static_cast<int>(m_windows.size()); ++i) {
        const HWND hwnd = m_windows[i]->hWnd;
        const bool running = m_taskManager && m_taskManager->IsRunning(hwnd);
        m_list.SetItemText(i, 3, running ? (m_taskManager->IsPaused(hwnd) ? _T("paused (3)") : _T("running (3)")) : _T("stopped"));
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
        if (m_taskSchedule) m_taskSchedule->Stop();
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
        UpdateActionButtons();
    }
    CDialogEx::OnTimer(id);
}

void CDCMDlg::OnListItemChanged(NMHDR*, LRESULT* result) {
    UpdateActionButtons();
    if (result) *result = 0;
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

    if (m_taskSchedule->IsRunning()) {
        m_taskSchedule->Stop();
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

    m_taskSchedule->Start(taskIds);
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

void CDCMDlg::OnCancel() {
    KillTimer(1);
    if (m_taskSchedule) m_taskSchedule->Stop();
    if (m_taskManager) {
        m_taskManager->Stop();
        m_taskManager.reset();
    }
    StopDmRegistration();
    CDialogEx::OnCancel();
}
