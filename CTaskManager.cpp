#include "pch.h"
#include "CTaskManager.h"
#include "CWndManager.h"

CWindowRuntime::CWindowRuntime(std::shared_ptr<tagWndInfo> wnd, const tagWndIni& ini, HWND logWindow, TaskSchedulePtr schedule)
    : m_memory(std::make_shared<TaskMemoryState>()),
      m_control(std::make_shared<CThreadControl>()),
      m_wnd(std::move(wnd)),
      m_ini(ini),
      m_schedule(schedule ? std::move(schedule) : std::make_shared<CTaskSchedule>()) {
    m_info.id = m_wnd ? m_wnd->id : 0;
    m_info.wnd = m_wnd;
    m_task = std::make_unique<CTaskScript>(m_wnd, m_control, m_ini, logWindow,
                                           m_schedule, m_memory);
    m_helper = std::make_unique<CTaskHelperScript>(m_wnd, m_control, m_ini,
                                                   logWindow, m_memory);
    m_yan = std::make_unique<CTaskHelperYan>(m_wnd, m_control, m_ini,
                                             logWindow, m_memory);
    m_info.pTaskThread = m_task.get();
    m_info.pTaskHelperThread = m_helper.get();
    m_info.pTaskYanThread = m_yan.get();
}
CWindowRuntime::~CWindowRuntime() { Stop(); }
bool CWindowRuntime::Start() {
    if (IsRunning()) return true;
    m_control->Resume();
    const bool a = m_task->Start();
    const bool b = m_helper->Start();
    const bool c = m_yan->Start();
    if (!(a && b && c)) { Stop(); return false; }
    m_info.state = _T("running (3 threads)");
    return true;
}
void CWindowRuntime::Stop() {
    m_control->Stop();
    if (m_yan) m_yan->Stop();
    if (m_helper) m_helper->Stop();
    if (m_task) m_task->Stop();
    m_info.state = _T("stopped");
}
void CWindowRuntime::Suspend() { m_control->Pause(); m_info.state = _T("paused (3 threads)"); }
void CWindowRuntime::Resume() { m_control->Resume(); m_info.state = _T("running (3 threads)"); }
void CWindowRuntime::RequestTest() { if (m_task && IsRunning()) m_task->RequestTest(); }
void CWindowRuntime::RequestRoute(bool reverse) { if (m_task && IsRunning()) m_task->RequestRoute(reverse); }
void CWindowRuntime::StartSequence(const std::vector<int>& taskIds) {
    if (m_schedule) m_schedule->Start(taskIds);
}
void CWindowRuntime::StopSequence() {
    if (m_schedule) m_schedule->Stop();
}
bool CWindowRuntime::IsSequenceRunning() const {
    return m_schedule && m_schedule->IsRunning();
}
CString CWindowRuntime::Status() const {
    if (!IsRunning()) return _T("stopped");
    if (IsPaused()) return _T("paused (3)");
    const CString task = m_task ? m_task->CurrentTaskName() : CString();
    if (task.IsEmpty()) return _T("running (3)");
    CString status;
    status.Format(_T("running: %s"), task.GetString());
    return status;
}
size_t CWindowRuntime::CompletedTaskCount() const {
    return m_task ? m_task->CompletedTaskCount() : 0;
}
bool CWindowRuntime::IsRunning() const { return m_task && m_helper && m_yan && (m_task->IsRunning() || m_helper->IsRunning() || m_yan->IsRunning()); }
CTaskManager::CTaskManager(const CAppConfig& config, HWND logWindow, TaskSchedulePtr schedule)
    : m_ini(config.Window()), m_logWindow(logWindow) { (void)schedule; }
CTaskManager::~CTaskManager() { Stop(); }
bool CTaskManager::Start(const std::vector<std::shared_ptr<tagWndInfo>>& windows) {
    bool allOk = true; std::scoped_lock lock(m_mutex);
    for (const auto& wnd : windows) {
        if (!wnd || !CWndManager::IsAlive(wnd->hWnd)) continue;
        auto it = m_runtimes.find(wnd->hWnd);
        if (it == m_runtimes.end()) {
            auto runtime = std::make_unique<CWindowRuntime>(wnd, m_ini, m_logWindow);
            if (!runtime->Start()) { allOk = false; continue; }
            m_runtimes.emplace(wnd->hWnd, std::move(runtime));
        } else if (!it->second->IsRunning()) {
            allOk = it->second->Start() && allOk;
        }
    }
    return allOk;
}
bool CTaskManager::Applies(HWND hwnd, const std::vector<HWND>& filter) const { return filter.empty() || std::find(filter.begin(), filter.end(), hwnd) != filter.end(); }
void CTaskManager::Stop(const std::vector<HWND>& windows) {
    std::scoped_lock lock(m_mutex);
    for (auto it = m_runtimes.begin(); it != m_runtimes.end();) { if (Applies(it->first, windows)) it = m_runtimes.erase(it); else ++it; }
}
void CTaskManager::Suspend(const std::vector<HWND>& windows) { std::scoped_lock lock(m_mutex); for (auto& [hwnd, runtime] : m_runtimes) if (Applies(hwnd, windows)) runtime->Suspend(); }
void CTaskManager::Resume(const std::vector<HWND>& windows) { std::scoped_lock lock(m_mutex); for (auto& [hwnd, runtime] : m_runtimes) if (Applies(hwnd, windows)) runtime->Resume(); }
size_t CTaskManager::RequestTest(const std::vector<HWND>& windows) {
    std::scoped_lock lock(m_mutex);
    size_t requested = 0;
    for (auto& [hwnd, runtime] : m_runtimes) {
        if (Applies(hwnd, windows) && runtime->IsRunning()) {
            runtime->RequestTest();
            ++requested;
        }
    }
    return requested;
}
size_t CTaskManager::RequestRoute(bool reverse, const std::vector<HWND>& windows) {
    std::scoped_lock lock(m_mutex);
    size_t requested = 0;
    for (auto& [hwnd, runtime] : m_runtimes) {
        if (Applies(hwnd, windows) && runtime->IsRunning()) {
            runtime->RequestRoute(reverse);
            ++requested;
        }
    }
    return requested;
}
void CTaskManager::StartSequence(const std::vector<int>& taskIds, const std::vector<HWND>& windows) {
    std::scoped_lock lock(m_mutex);
    for (auto& [hwnd, runtime] : m_runtimes)
        if (Applies(hwnd, windows) && runtime->IsRunning()) runtime->StartSequence(taskIds);
}
void CTaskManager::StopSequence(const std::vector<HWND>& windows) {
    std::scoped_lock lock(m_mutex);
    for (auto& [hwnd, runtime] : m_runtimes)
        if (Applies(hwnd, windows)) runtime->StopSequence();
}
bool CTaskManager::IsSequenceRunning(const std::vector<HWND>& windows) const {
    std::scoped_lock lock(m_mutex);
    for (const auto& [hwnd, runtime] : m_runtimes)
        if (Applies(hwnd, windows) && runtime->IsSequenceRunning()) return true;
    return false;
}
bool CTaskManager::IsRunning(HWND hwnd) const { std::scoped_lock lock(m_mutex); const auto it = m_runtimes.find(hwnd); return it != m_runtimes.end() && it->second->IsRunning(); }
bool CTaskManager::IsPaused(HWND hwnd) const { std::scoped_lock lock(m_mutex); const auto it = m_runtimes.find(hwnd); return it != m_runtimes.end() && it->second->IsPaused(); }
CString CTaskManager::Status(HWND hwnd) const { std::scoped_lock lock(m_mutex); const auto it = m_runtimes.find(hwnd); return it != m_runtimes.end() ? it->second->Status() : CString(_T("stopped")); }
size_t CTaskManager::CompletedTaskCount(const std::vector<HWND>& windows) const {
    std::scoped_lock lock(m_mutex);
    size_t minimum = (std::numeric_limits<size_t>::max)();
    bool found = false;
    for (const auto& [hwnd, runtime] : m_runtimes) {
        if (Applies(hwnd, windows)) {
            minimum = (std::min)(minimum, runtime->CompletedTaskCount());
            found = true;
        }
    }
    return found ? minimum : 0;
}
size_t CTaskManager::RunningCount() const { std::scoped_lock lock(m_mutex); size_t count = 0; for (const auto& [hwnd, runtime] : m_runtimes) if (runtime->IsRunning()) ++count; return count; }
