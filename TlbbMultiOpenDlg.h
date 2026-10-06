#pragma once

#include "TlbbLauncher.h"

class CTlbbMultiOpenDlg : public CDialogEx
{
	DECLARE_DYNAMIC(CTlbbMultiOpenDlg)

public:
	explicit CTlbbMultiOpenDlg(CWnd* parent = nullptr);
	enum { IDD = IDD_TLBB_MULTI_OPEN };

protected:
    void OnOK() override {} // Enter must not close the dialog during a launch.
	virtual void DoDataExchange(CDataExchange* dataExchange);
	virtual BOOL OnInitDialog();
	virtual void OnCancel();

	afx_msg void OnBrowse();
	afx_msg void OnRefresh();
	afx_msg void OnStartGame();
	afx_msg LRESULT OnLaunchComplete(WPARAM wParam, LPARAM lParam);
	DECLARE_MESSAGE_MAP()

private:
	static UINT LaunchThreadProc(LPVOID parameter);
	void AppendLog(const CString& message);
	void RefreshCount();
	void SetLaunching(bool launching);

	CString m_rootPath;
	int m_instanceLimit;
	bool m_launching;
	CEdit m_logEdit;
	CButton m_startButton;
	CSpinButtonCtrl m_limitSpin;
};
