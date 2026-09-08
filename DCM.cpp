#include "pch.h"
#include "resource.h"
#include "DCMDlg.h"

class CDCMApp final : public CWinApp {
public:
    BOOL InitInstance() override {
        CWinApp::InitInstance();
        AfxEnableControlContainer();
        AfxInitRichEdit2();
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool comInitialized = SUCCEEDED(comResult);
        CDCMDlg dlg;
        m_pMainWnd = &dlg;
        dlg.DoModal();
        if (comInitialized) CoUninitialize();
        return FALSE;
    }
    DECLARE_MESSAGE_MAP()
};
BEGIN_MESSAGE_MAP(CDCMApp, CWinApp)
END_MESSAGE_MAP()
CDCMApp theApp;
