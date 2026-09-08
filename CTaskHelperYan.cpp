#include "pch.h"
#include "CTaskHelperYan.h"

bool CTaskHelperYan::OnLoop() {
    SleepEx(200);
    return !m_control->IsStopping();
}
