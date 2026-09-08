#pragma once
#include "AppConfig.h"

#include "CTaskHelperScript.h"
#include "CTaskHelperYan.h"
#include "CTaskScript.h"

class CWindowRuntime final {
public:
    CWindowRuntime(std::shared_ptr<tagWndInfo> wnd, const tagWndIni& ini, HWND logWindow = nullptr,
                   TaskSchedulePtr schedule = nullptr);
    ~CWindowRuntime();

    bool Start();
    void Stop();
    void Suspend();
    void Resume();
    void RequestTest();
    bool IsPaused() const { return m_control->IsPaused(); }
    bool IsRunning() const;
    tagTaskInfo& Info() { return m_info; }
    const tagTaskInfo& Info() const { return m_info; }

private:
    TaskMemoryStatePtr m_memory;
    std::shared_ptr<CThreadControl> m_control;
    std::shared_ptr<tagWndInfo> m_wnd;
    tagWndIni m_ini;
    std::unique_ptr<CTaskScript> m_task;
    std::unique_ptr<CTaskHelperScript> m_helper;
    std::unique_ptr<CTaskHelperYan> m_yan;
    tagTaskInfo m_info;
};

class CTaskManager final {
public:
    explicit CTaskManager(const CAppConfig& config, HWND logWindow = nullptr,
                          TaskSchedulePtr schedule = nullptr);
    ~CTaskManager();

    bool Start(const std::vector<std::shared_ptr<tagWndInfo>>& windows);
    void Stop(const std::vector<HWND>& windows = {});
    void Suspend(const std::vector<HWND>& windows = {});
    void Resume(const std::vector<HWND>& windows = {});
    size_t RequestTest(const std::vector<HWND>& windows = {});
    bool IsRunning(HWND hwnd) const;
    bool IsPaused(HWND hwnd) const;
    size_t RunningCount() const;

private:
    bool Applies(HWND hwnd, const std::vector<HWND>& filter) const;
    tagWndIni m_ini;
    HWND m_logWindow = nullptr;
    TaskSchedulePtr m_schedule;
    mutable std::mutex m_mutex;
    std::unordered_map<HWND, std::unique_ptr<CWindowRuntime>> m_runtimes;
};
