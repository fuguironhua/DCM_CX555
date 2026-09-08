#pragma once

#include "framework.h"
#include <array>

// Window-name slots copied from the reference project's Fruit15 enum.
// They are kept as a separate per-window table so the migrated code can use
// the original ck[slot] indexing without reintroducing process-global state.
enum Fruit15 : size_t {
    Manager,
    PlayerFrame_HP,
    PlayerFrame_Icon,
    Packet_Frame,
    Frame_Zhy,
    Frame_Sd1,
    Frame_Sd2,
    MessageBox_Self_Frame,
    Frame_Time,
    Frame_Wl,
    BigBank_Frame,
    Shop_Frame,
    CityShop_Frame,
    GameSetup_Frame,
    SMSCode2_Frame,
    MiniMap_MiniMap_NetStatus,
    SceneTrans_Frame,
    MsgBall_Connect_Action,
    MessageBox_Self_Text,
    AcceptBox_Frame,
    AcceptBox_Text,
    PaoShang_Frame,
    PaoShang_Goumai_Tuijian1,
    PaoShang_Goumai_Tuijian2,
    PaoShang_Goumai_Tuijian3,
    PaoShang_ItemInfo1_Price,
    PaoShang_ItemInfo2_Price,
    PaoShang_ItemInfo3_Price,
    ZhouHuoYue_Condition_Text,
    Fruit15Count
};

// Address and expression storage shared by the three threads belonging to
// one window. This replaces the reference project's process-wide jz/tsg
// arrays while keeping the same slot names available to derived threads.
struct TaskMemoryState {
    enum AddressSlot : size_t {
        ROLEB, Mfen, BagJ, FenB, BagEcx, MyDiTu, WndOpen, RenW, RenWERC, RenWECX,
        XzZt, luaState, Sleepd, Gettick, TermThM, Duiwu, FFzhant, Strs, Createh, Kdebug,
        Gdebug, Hzt, Hzt2, Hztcc, Luadost, Paos, Shic, Nbsq, HHKK3PM, HHKK3P2, Wsasend,
        KuaTuX, ShicH, SaoD, HuoYe, jYcdiz, jcYcdi, Memccp, Hkwordd, Xguih, BagJH, BagJH2,
        Renms, Renmcall, Hksendd, Hkattjc, Bufqx, tlsj, psfx, sddd, bagbd, bagjd, sjjs,
        sjdqz, fbdz,zqpd , SceneManagerGlobal, SceneManagerCache, AddressSlotCount = 100
    };

    enum ExpressionSlot : size_t {
        tls1, tls2, tls3, workdir, mDiTu, luxi, ERC, ZJID, m_Name, m_Dqzt, m_Djxh,
        Shuxtb, shuxcw, shuxbg, Rwerc, GJID, gdgname, dz_Name, hkwords, rwxl, rwll,
        rwlq, mmsp, tdiax, tdqhct, tjrhct, wpcs, ExpressionSlotCount = 30
    };

    std::array<LONGLONG, AddressSlotCount> addresses{};
    std::array<CString, ExpressionSlotCount> expressions{};
    std::array<LONGLONG, Fruit15Count> windowAddresses{};
    LONGLONG gameModule = 0;
    LONGLONG ceguiModule = 0;
    LONGLONG kernel32Module = 0;
    LONGLONG ceguiBaseModule = 0;
    RECT clientRect{ 0, 0, 0, 0 };
    CString resourcePath;
    bool modulesReady = false;
    bool referenceWindowSize = false;
    bool initialized = false;

    void BeginInitialization() {
        std::scoped_lock lock(readyMutex);
        addresses.fill(0);
        expressions = {};
        windowAddresses.fill(0);
        gameModule = 0;
        ceguiModule = 0;
        kernel32Module = 0;
        ceguiBaseModule = 0;
        clientRect = RECT{ 0, 0, 0, 0 };
        resourcePath.Empty();
        modulesReady = false;
        referenceWindowSize = false;
        initialized = false;
    }

    void FinishInitialization(bool success) {
        {
            std::scoped_lock lock(readyMutex);
            modulesReady = success;
            initialized = true;
        }
        readyCondition.notify_all();
    }

    bool IsInitialized() const {
        std::scoped_lock lock(readyMutex);
        return initialized;
    }

    template <typename StopPredicate>
    bool WaitUntilInitialized(StopPredicate stopRequested) const {
        std::unique_lock<std::mutex> lock(readyMutex);
        readyCondition.wait(lock, [&] { return initialized || stopRequested(); });
        return initialized;
    }

private:
    mutable std::mutex readyMutex;
    mutable std::condition_variable readyCondition;
};

using TaskMemoryStatePtr = std::shared_ptr<TaskMemoryState>;
