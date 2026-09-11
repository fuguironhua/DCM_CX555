#pragma once

#include "framework.h"
#include "structs.h"
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
    MiniMap_Placename,
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

    // Indices retained from the reference project Fruit4 enum.
    enum BagSlot : size_t {
        baxue,
        balan,
        bacow,
        balsy,
        BagSlotCount
    };

    enum MamsSlot : size_t {
        Menpaid = 0,
        BagMax1 = 1,
        BagMax2 = 2,
        BagMax3 = 3,
        JiGj = 15,
        CoXuHa = 23,
        Cox1 = 24,
        Cox2 = 25,
        duibxh = 26,
        benbxh = 27,
        Jijl = 39,
        dxuem = 59,
        ddenj = 70,
        dZJID = 71,
        // Read_RWZT cache slots retained from the reference task thread.
        // They keep the last status value and the timestamp of the last
        // uncached read in the shared per-window memory state.
        dztsj = 72,
        dzbsj = 73,
        drwzt = 74,
        MamsSlotCount = 100
    };

    std::array<LONGLONG, AddressSlotCount> addresses{};
    std::array<CString, ExpressionSlotCount> expressions{};
    std::array<LONGLONG, Fruit15Count> windowAddresses{};
    std::array<DWORD, MamsSlotCount> mams{};
    std::array<BagItem, BagSlotCount> bag{};
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
        mams.fill(0);
        bag = {};
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

#define CLOSE_NPC _T("setmetatable(_G, {__index = Quest_Env});Quest_Close();")
#define DKRICH _T("setmetatable(_G, {__index = PlayerQuicklyEnter_Env});PlayerQuicklyEnter_Clicked(2);")
#define GBRICH _T("setmetatable(_G, {__index = ZhouHuoYue_Env});ZhouHuoYue_Close();")
#define DKSAOD _T("setmetatable(_G, {__index = PlayerQuicklyEnter_Env});PlayerQuicklyEnter_Clicked(1);")
#define GBSAOD _T("setmetatable(_G, {__index = SweepAll_Env});SweepAll_OnClosed();")
#define KGZDZD _T("setmetatable(_G, { __index = MiniMap_Env }); MiniMap_ZDZD();")
#define XUNLQR _T("setmetatable(_G, { __index = MessageBox_Self_Env }); MessageBox_Self_OK_Clicked();")
#define XUNLQR2 _T("setmetatable(_G, {__index = AcceptBox_Env}); AcceptBox_OK_Clicked();")
#define FUBHIL _T("setmetatable(_G, {__index = Higntlight_Env}); Higntlight_OnClose();")
#define CHTABL _T("setmetatable(_G, {__index = Packet_Env}); Packet_ChangeTabIndex(1);")
#define SHANFH _T("setmetatable(_G, {__index = CityShop_Env});this:Hide();")
#define BAGDAK _T("setmetatable(_G, {__index = MainMenuBar_Env});MainMenuBar_Packet_Clicked();")
#define BAGGUB _T("setmetatable(_G, {__index = Packet_Env});Packet_Close();")
