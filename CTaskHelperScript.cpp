#include "pch.h"
#include "CTaskHelperScript.h"

bool CTaskHelperScript::OnLoop() {
    if (!m_roleBaseLogged.exchange(true)) {
        CString message;
        message.Format(_T("HelperScript::OnLoop: jz[ROLEB] = 0x%llX"),
                       static_cast<unsigned long long>(jz[ROLEB]));
        DebugLog(message);
    }

    SleepEx(150);
    return !m_control->IsStopping();
}
