#pragma once

#include "CTaskThread.h"

class CTaskHelperYan final : public CTaskThread {
public:
    CTaskHelperYan(std::shared_ptr<tagWndInfo> wnd, std::shared_ptr<CThreadControl> control,
                   const tagWndIni& ini, HWND logWindow = nullptr,
                   TaskMemoryStatePtr memory = nullptr)
        : CTaskThread(std::move(wnd), std::move(control), ini, logWindow, std::move(memory)) {}

protected:
    LPCTSTR ThreadName() const override { return _T("HelperYan"); }
    bool OnEventThreadStart() override {
        const bool ready = WaitForMemoryReady();
        return ready;
    }
    bool OnLoop() override;
};
