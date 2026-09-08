#pragma once

#include "CTaskThread.h"

class CTaskHelperScript final : public CTaskThread {
public:
    CTaskHelperScript(std::shared_ptr<tagWndInfo> wnd, std::shared_ptr<CThreadControl> control,
                      const tagWndIni& ini, HWND logWindow = nullptr,
                      TaskMemoryStatePtr memory = nullptr)
        : CTaskThread(std::move(wnd), std::move(control), ini, logWindow, std::move(memory)) {}

protected:
    LPCTSTR ThreadName() const override { return _T("HelperScript"); }
    bool OnEventThreadStart() override { return WaitForMemoryReady(); }
    bool OnLoop() override;

private:
    std::atomic_bool m_roleBaseLogged{ false };
};
