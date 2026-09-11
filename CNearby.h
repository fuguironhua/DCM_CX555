#pragma once

#include "CTaskThread.h"

class CNearby : public CTaskThread {
public:
    CNearby(std::shared_ptr<tagWndInfo> wnd, std::shared_ptr<CThreadControl> control,
            const tagWndIni& ini, HWND logWindow = nullptr,
            TaskMemoryStatePtr memory = nullptr)
        : CTaskThread(std::move(wnd), std::move(control), ini, logWindow, std::move(memory)) {}
    ~CNearby() override = default;

protected:
    // Keep this layer abstract, like the reference project. The concrete
    // task class owns the actual loop implementation.
    virtual bool OnLoop() override = 0;
    virtual bool ScanNearbyOnce();


public:
    void mChusrw();//初始化人物

};
