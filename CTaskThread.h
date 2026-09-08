#pragma once
#include "framework.h"
#include "PauseController.h"
#include "TaskMemoryState.h"
#include "obj.h"
#include "structs.h"
#include <cstdarg>
#include <limits>
#include <regex>

// Keep the unqualified spelling used by the reference task declarations.
using regex = std::regex;

class CTaskThread {
public:
    CTaskThread(std::shared_ptr<tagWndInfo> wnd, std::shared_ptr<CThreadControl> control,
                const tagWndIni& ini, HWND logWindow = nullptr,
                TaskMemoryStatePtr memory = nullptr);
    virtual ~CTaskThread();
    bool Start();
    void Stop();
    void Suspend();
    void Resume();
    bool IsReady() const { return m_ready.load(); }
    bool IsRunning() const { return m_running.load(); }
    bool IsPaused() const { return m_control->IsPaused(); }
    DWORD ThreadId() const { return m_threadId.load(); }
    bool tClick(int x, int y);
    bool trClick(int x, int y);
    bool tMoveTo(int x, int y);
    bool FindPic(const CRect& area, const CString& file, CPoint& point, const CString& deltaColor = _T("000000"), double sim = 0.9, int dir = 0);
    bool FindColor(const CRect& area, const CString& color, CPoint& point, double sim = 0.9, int dir = 0);
    long GetColorNum(const CRect& area, const CString& color, double sim = 0.9);
    // Reference-project helpers: read a DWORD through a dmsoft expression
    // or from an absolute address in the target window.
    DWORD Read_Dw(CString cstr);
    DWORD Read_Dw(DWORD addr);

    double mGetAngle2(Point2f pa, Point2f pb);
    // Reference-project memory helpers. UpdateMP stores the copied address in
    // dwls so the following task code can use it just like the original code.
    bool UpdateMP(CString cstr, DWORD lon);
    bool UpdateMP(DWORD paddr, DWORD lon);
protected:
    // Fixed offsets formerly declared as macros in BianL.h.  Keeping them as
    // typed constants on the common base class makes the old names directly
    // available in CTaskThread, CNearby, CTaskScript and both helper threads.
    #define SGDC   25;
    #define ZHUTC  160 + 0x18
    #define ZHUTB  160 + 0x9B + 4  // jump-out address
    #define ZHUBC  160 + 0x61 // execution flag
    #define ZHUBB  160 + 0x9B + 8  // jump-out address
    #define FENGT  160 + 0x67  // jump-out address

    virtual bool OnLoop() = 0;
    virtual LPCTSTR ThreadName() const { return _T("CTaskThread"); }
    virtual bool OnEventThreadStart() { return true; }
    virtual void OnEventThreadConclude() {}
    // Keep the original framework's event-loop entry point: every iteration
    // reaches the most-derived OnLoop() through virtual dispatch.
    virtual bool OnEventThreadRun();
    bool WaitIfPaused();
    bool WaitFor(DWORD milliseconds);
    // Cooperative sleep used by task code. It checks stop state in 100 ms
    // slices and enters the pause gate after each slice.
    void SleepEx(DWORD milliseconds);
    // Keep CString messages raw while allowing the reference project's
    // printf-style calls, e.g. DebugLog(_T("value=%08x"), value).
    void DebugLog(LPCTSTR format, ...);
    void DebugLog(const CString& message);
    // Compatibility spelling from the original framework.
    void LogD(LPCTSTR format, ...);
    dmsoft* Dm() { return m_dm.get(); }
    const std::shared_ptr<tagWndInfo>& Window() const { return m_wnd; }
    TaskMemoryState& Memory() { return *m_memory; }
    const TaskMemoryState& Memory() const { return *m_memory; }
    bool WaitForMemoryReady();

    // Compatibility aliases for code being migrated from the reference
    // project: derived threads can use jz[ROLEB]. The storage is shared by
    // all three worker threads of one window, not global across windows.
    using AddressArray = std::array<LONGLONG, TaskMemoryState::AddressSlotCount>;
    using ExpressionArray = std::array<CString, TaskMemoryState::ExpressionSlotCount>;
    using WindowArray = std::array<LONGLONG, Fruit15Count>;
    static constexpr TaskMemoryState::AddressSlot ROLEB = TaskMemoryState::ROLEB;
    TaskMemoryStatePtr m_memory;
    AddressArray& jz;
    ExpressionArray& tsg;
    // Compatibility alias for the reference project's window table.
    WindowArray& ck;
    DWORD dwls = 0;
    std::shared_ptr<tagWndInfo> m_wnd;
    std::shared_ptr<CThreadControl> m_control;
    tagWndIni m_ini;
    std::unique_ptr<dmsoft> m_dm;
    std::atomic<bool> m_ready{ false };
    std::atomic<bool> m_running{ false };
    std::atomic<DWORD> m_threadId{ 0 };
    std::thread m_thread;
    HWND m_logWindow = nullptr;
private:
    void ThreadMain();
    void EmitFormattedDebugLog(LPCTSTR format, va_list args);
    void EmitDebugLog(const CString& text);
    CPoint ToScreen(int x, int y) const;
    CRect ToScreen(const CRect& area) const;

public:
    DWORD mmpid, mmainthread, mmhwnd;
    void havetest();

    // Per-thread compatibility containers from the reference project.
    std::vector<DWORD> varrou;
    std::vector<Monst> vMonst;
    Monst aMonst{};

    //---------------------封包+操作----------------------
    int wBenTuXuL2(float xx, float yy);
    int wOpenMan(DWORD did);//打开NPC名字
    void wXuanZg(DWORD moid);//选择怪物


    //---------------------遍历----------------------
	void NerbAr(DWORD Erc);//遍历周围所有
	int  NerbMonst2(DWORD Erc, regex txt_reg);//返回vector
	int  NerbMonst3(DWORD Erc, int leix);//返回vector  0怪物 
	int  NerbMonst3(DWORD Erc, int leix, CString txt_reg);//返回vector
	int  NerbXian(DWORD Erc, CString txt_reg);//陷阱
	int  NerbMonst222(DWORD Erc, regex txt_reg);//定点返回vector + 不要血量
	void NearbyMonst5(DWORD Erc);//打图用
    bool NerExist();
	bool NerExist(DWORD Mid);//是否在附近
	DWORD NerExist(CString cname);//返回地址
    regex NerbCs(float fjl, Point2f pwz, CString txt_reg);//查询初始化
    bool mCanAttack(DWORD zjdz,DWORD mondz);//是否可以攻击

    //---------------------人物属性----------------------
    bool wHaveRide(); // 是否至少存在一个已点亮、可用的坐骑图标
    // xuh is the zero-based active-state ordinal.  The return value is the
    // state ID; optional outputs receive the definition strings and object
    // address.  A return value of zero means that the chain was not resolved.
    DWORD mRead_Buffer(DWORD xuh, CString* name = nullptr,
                       CString* description = nullptr,
                       DWORD* stateObject = nullptr);//读取人物状态


    //---------------------窗口----------------------
    // 在 CEGUI WindowManager 的红黑树中按注册名查找窗口对象。
    // 返回值为节点 + 0x28 中保存的 Window*，未找到时返回 0。
    DWORD FindCeguiWindowByName(const CString& name);
    int IsWndGuot();//过图窗口是否打开
    DWORD cGetTime();//获取当前时间
    DWORD cGetNetIc();//获取当前网络图标序号
    // xuh is the zero-based item index in BagJ->[*+0x22C00].
    // Identify follows LifeAbility:IsIndentify: 0 = identified/not applicable,
    // 1 = unidentified. Bind returns the raw GetItemBindStatus result:
    // 0 = no bind, 1 = already bound, 2 = the category-dependent bind branch,
    // 3 = bind when equipped.
    int cRead_Identify(DWORD xuh);//读取物品鉴定
    int cRead_Bind(DWORD xuh);//读取物品绑定

    //---------------------周末活动----------------------
    DWORD mRead_Benj();//读取本金
    DWORD mRead_TodayActive();//通过周活跃窗口读取今日活跃值
    //DWORD mRead_Tuixh();//读取推荐序号
    PaoShangRecommendData mRead_TuiJg();//读取推荐价格、物品ID和目的地
    CString mGet_Kjda(CString cTim);//获取科举答案


    //---------------------跑商--------------------
    DWORD mRead_BangZw();//读取帮派职位


private:
    DWORD ReadCeguiNumericText(DWORD window);
    CString ReadCeguiText(DWORD window);
    static CString ExtractPaoShangDestination(const CString& text);
    bool ReadNearbyPlayerPosition(Point2f& position);
    DWORD CopyNearbyDescriptor(DWORD node, CString& expression);
    static float NearbyDistance(const Point2f& from, const Point2f& to);

    float m_nearbyDistanceLimit = (std::numeric_limits<float>::max)();
    Point2f m_nearbyReferencePosition{};
    bool m_nearbyReferencePositionValid = false;
};
