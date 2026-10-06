#pragma once
#include "framework.h"
#include "resource.h"
#include "AppConfig.h"
#include "CTaskManager.h"
#include "CWndManager.h"
#include "CeTableUpdater.h"
#include "WindowTools.h"

class CDCMDlg final : public CDialogEx {
public:
    explicit CDCMDlg(CWnd* parent = nullptr);
    ~CDCMDlg() override;
    enum { IDD = IDD_MAIN_DIALOG };
protected:
    void DoDataExchange(CDataExchange* dx) override;
    BOOL OnInitDialog() override;
    void OnOK() override {}
    void OnCancel() override;
    DECLARE_MESSAGE_MAP()
private:
    void RefreshWindows();
    void InitTaskLists();
    void HideHorizontalScrollBar(CListCtrl& list);
    std::vector<std::shared_ptr<tagWndInfo>> CheckedWindows() const;
    std::vector<HWND> CheckedHandles() const;
    std::vector<int> TaskSequenceIds() const;
    void UpdateRows(const std::vector<HWND>& keepChecked);
    void UpdateStatuses();
    void UpdateActionButtons();
    void IncreaseDebugFont();
    void SetStatus(const CString& text);
    enum class LogLevel { Normal, Warning, Exception, Debug };
    void AppendDebug(const CString& text, LogLevel level = LogLevel::Debug);
    COLORREF DebugColor(LogLevel level) const;
    void StartDmRegistration();
    void StopDmRegistration();
    void DmRegistrationThreadMain();
    afx_msg void OnRefresh();
    afx_msg void OnStart();
    afx_msg void OnPause();
    afx_msg void OnTest();
    afx_msg void OnUpdateD();
    afx_msg void OnMultiOpen();
    afx_msg void OnCapture();
    afx_msg void OnPauseVerify();
    afx_msg void OnDistance();
    afx_msg void OnGrabVerify();
    afx_msg void OnDetectCaptchaType();
    void DetectCaptchaType();
    afx_msg void OnRunRouteForward();
    afx_msg void OnRunRouteReverse();
    void RequestRouteRun(bool reverse);
    bool EnsureWindowToolsDm();
    afx_msg LRESULT OnUpdateDFinished(WPARAM wParam, LPARAM lParam);
    afx_msg void OnContextMenu(CWnd* wnd, CPoint point);
    afx_msg void OnDebugSave();
    afx_msg void OnDebugClear();
    afx_msg void OnTimer(UINT_PTR id);
    afx_msg void OnListItemChanged(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnTaskListDblClick(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnTaskRunListDblClick(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnTaskToggle();
    afx_msg LRESULT OnDmRegistrationMessage(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnWindowThreadLog(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnGrabVerifyFinished(WPARAM wParam, LPARAM lParam);
    CListCtrl m_list;
    CListCtrl m_taskList;
    CListCtrl m_taskRunList;
    CButton m_refresh;
    CButton m_start;
    CButton m_pause;
    CButton m_taskToggle;
    CButton m_test;
    CButton m_updateD;
    CButton m_multiOpen;
    CButton m_capture;
    CButton m_pauseVerify;
    CButton m_distance;
    CButton m_grabVerify;
    CButton m_detectType;
    CButton m_routeForward;
    CButton m_routeReverse;
    std::unique_ptr<dmsoft> m_windowToolsDm;
    CVerifyProcessPause m_verifyPause;
    std::atomic<bool> m_grabRunning{ false };
    std::thread m_updateDThread;
    CeTableUpdater::Result m_updateDResult;
    CStatic m_status;
    CRichEditCtrl m_debug;
    CFont m_debugFont;
    CAppConfig m_config;
    CWndManager m_wndManager;
    TaskSchedulePtr m_taskSchedule;
    std::unique_ptr<CTaskManager> m_taskManager;
    std::thread m_dmInitThread;
    std::mutex m_dmMutex;
    std::condition_variable m_dmCv;
    std::atomic<bool> m_dmStop{ false };
    std::atomic<bool> m_dmInitDone{ false };
    std::atomic<bool> m_dmRegistered{ false };
    std::vector<std::shared_ptr<tagWndInfo>> m_windows;
    bool m_taskSequenceStarted = false;
    size_t m_taskRowsRemoved = 0;
};

