#pragma once
#include "framework.h"
#include "PauseController.h"
#include "TaskMemoryState.h"
#include "obj.h"
#include "structs.h"
#include "TaskNpcTables.h"
#include <cstdarg>
#include <functional>
#include <limits>
#include <regex>
#include <vector>

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
    static constexpr DWORD kPacketIntervalMs = 300;
    enum class PacketAction { Other, Move, Skill };
    bool IsActionAllowed(PacketAction kind);
    bool DispatchPacketWithInterval(const CString& action,
                                    const std::function<long()>& writePacket,
                                    bool traceSend = false,
                                    PacketAction kind = PacketAction::Other);
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
    using MamsArray = std::array<DWORD, TaskMemoryState::MamsSlotCount>;
    using BagArray = std::array<BagItem, TaskMemoryState::BagSlotCount>;
    static constexpr TaskMemoryState::AddressSlot ROLEB = TaskMemoryState::ROLEB;
    static constexpr TaskMemoryState::MamsSlot Menpaid = TaskMemoryState::Menpaid;
    static constexpr TaskMemoryState::MamsSlot BagMax1 = TaskMemoryState::BagMax1;
    static constexpr TaskMemoryState::MamsSlot BagMax2 = TaskMemoryState::BagMax2;
    static constexpr TaskMemoryState::MamsSlot BagMax3 = TaskMemoryState::BagMax3;
    static constexpr TaskMemoryState::BagSlot baxue = TaskMemoryState::baxue;
    static constexpr TaskMemoryState::BagSlot balan = TaskMemoryState::balan;
    static constexpr TaskMemoryState::BagSlot bacow = TaskMemoryState::bacow;
    static constexpr TaskMemoryState::BagSlot balsy = TaskMemoryState::balsy;
    static constexpr TaskMemoryState::MamsSlot JiGj = TaskMemoryState::JiGj;
    static constexpr TaskMemoryState::MamsSlot CoXuHa = TaskMemoryState::CoXuHa;
    static constexpr TaskMemoryState::MamsSlot Cox1 = TaskMemoryState::Cox1;
    static constexpr TaskMemoryState::MamsSlot Cox2 = TaskMemoryState::Cox2;
    static constexpr TaskMemoryState::MamsSlot duibxh = TaskMemoryState::duibxh;
    static constexpr TaskMemoryState::MamsSlot benbxh = TaskMemoryState::benbxh;
    static constexpr TaskMemoryState::MamsSlot Jijl = TaskMemoryState::Jijl;
    static constexpr TaskMemoryState::MamsSlot dxuem = TaskMemoryState::dxuem;
    static constexpr TaskMemoryState::MamsSlot ddenj = TaskMemoryState::ddenj;
    static constexpr TaskMemoryState::MamsSlot dztsj = TaskMemoryState::dztsj;
    static constexpr TaskMemoryState::MamsSlot dzbsj = TaskMemoryState::dzbsj;
    static constexpr TaskMemoryState::MamsSlot drwzt = TaskMemoryState::drwzt;
    static constexpr TaskMemoryState::MamsSlot fbys = TaskMemoryState::fbys;
    static constexpr TaskMemoryState::MamsSlot dxnjc = TaskMemoryState::dxnjc;
    TaskMemoryStatePtr m_memory;
    AddressArray& jz;
    ExpressionArray& tsg;
    // Compatibility alias for the reference project's window table.
    WindowArray& ck;
    MamsArray& Mams;
    BagArray& bag;
    DWORD dwls = 0;
    // 血祭（宠物技能）状态：由 tCheckXj 填充，供副本任务判断
    bool m_hasXueJi = false;
    DWORD m_xjSkillId = 0;
    // HkWorld 后续实现需设置这两个远程地址；0 表示 Hook 尚未安装。
    // 写入方先写 ANSI 文本，再置 DWORD 标志为 1；读取方消费后清零。
    DWORD m_worldMessageFlag = 0;
    DWORD m_worldMessageText = 0;
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
    bool m_actionStatusesConfigured = false;
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
    std::vector<DWORD> vecrun;
    std::vector<DWORD> vecskill;
    // 在副本函数入口创建；return/异常退出时恢复原配置。ID 为自身状态 ID。
    class ScopedActionStatuses {
    public:
        ScopedActionStatuses(CTaskThread& owner, std::vector<DWORD> moveIds,
                             std::vector<DWORD> skillIds, bool onlyIfUnset = false);
        ~ScopedActionStatuses();
        ScopedActionStatuses(const ScopedActionStatuses&) = delete;
        ScopedActionStatuses& operator=(const ScopedActionStatuses&) = delete;
    private:
        CTaskThread& m_owner;
        std::vector<DWORD> m_moveIds, m_skillIds;
        bool m_configured;
    };
    // Working copies of the reference task/NPC tables (BianL.h VRENW / VHUOD).
    std::vector<TaskNpcRewardEntry> VRenw;
    std::vector<TaskNpcActivityEntry> VHuod;
    // Active skill lists built by CNearby::mChusrw (reference Vkill/VYid/VZtai).
    std::vector<TaskSkillEntry> Vkill;
    std::vector<TaskSkillEntry> VYid;
    std::vector<TaskSkillZtEntry> VZtai;
    std::vector<Monst> vMonst;
    Monst aMonst{};

    //---------------------封包+操作----------------------
    int wBenTuXuL2(float xx, float yy);
    int wBenTuXuL2_Fast(float xx, float yy);
    int wOpenMan(DWORD did);//打开NPC名字
    int wOpenMan(CString name);//打开NPC名字
    void wXuanZg(DWORD moid);//选择怪物
    void wXuanZg2(DWORD moid);//选择怪物id + 判断距离
    bool wUseSk6(DWORD jiid, DWORD moid, bool basicAttack = false);//已发送返回true；被状态拦截返回false
    bool wUseSk6(DWORD jiid, Point2f ptx, bool basicAttack = false);//已发送返回true；被状态拦截返回false
    bool wUseSk6cw(DWORD jiid);//宠物技能：血祭等，沿用当前封包调度器
    void wClickNpc(DWORD djg, DWORD gjg = 1);//点击NPC--序号从1开始  改时间间隔
	void wClickNpc(CString cname);//点击NPC--序号从1开始  改时间间隔
	void wClickNpcDJ(DWORD djg, DWORD gjg = 1);//点击大劫大难专用

	int  wPet_Chu(int xuh, int cz);//0出站 1休息
    bool wUseBag2();//人物
	void wUseBag2(int zl);
	bool wUseBag2x(int xuh);//人物
    int  wDui_Gens(int xuh);//队员跟随--1跟随 0休息
    void wSendTeamChat(const CString& message);//队伍频道发言，预留实现；message为完整消息。

    void mLua_Dostr(CString csc);//执行lua脚本--限100字节
    void Hk_Paos(int kg);//Hook 消息  跑商
    void HkWorld(int kg);//Hook 消息开关：1 打开，0 关闭（实现待补）
    bool ReadWorldMessage(CString& text);//消费 HkWorld 消息，未安装时返回 false
    CString qHksxwz(CString csc);//取杀星文本末尾的 _编号}
    void wPsGou(CString cname);//跑商购买
    void wPsChu(DWORD djg);//跑商购买
	void wLyGou();//跑商洛阳购买
	void wLyChu(int xuh);//跑商洛阳出售
	int  WP_CS(int xuh);//普通商店出售包裹物品

	int wRenw_jie(DWORD npcid, DWORD rwid);//接任务
	int wRenw_fan(DWORD rwid);//放弃任务
	int wRenw_jix(DWORD npcid, DWORD rwid);//继续任务
	int wRenw_wan(DWORD npcid, DWORD rwid);//完成任务--无奖励选择的
    DWORD wXunL_Guih(float xx,float yy);//寻路规划
    void wBagJiaoH(DWORD xh1, DWORD xh2);//背包交换


    //---------------------遍历技能----------------------
	void  BianAr(DWORD Erc);//遍历技能
	int  BianSK(DWORD Erc);//遍历技能--轻功


    //---------------------遍历----------------------
	void NerbAr(DWORD Erc);//遍历周围所有
	int  NerbMonst2(DWORD Erc, regex txt_reg, bool logSummary = true);//返回vector
	// Scan nearby entities and return only targets the current player can attack.
	std::vector<Monst> FindAttackableMonsters(DWORD Erc, regex txt_reg,
	                                           float distance = 40.0f,
	                                           bool logSummary = false);
	int  NerbMonst3(DWORD Erc, int leix);//返回vector  0怪物 
	int  NerbMonst3(DWORD Erc, int leix, CString txt_reg);//返回vector
	int  NerbXian(DWORD Erc, CString txt_reg);//陷阱
	int  NerbMonst222(DWORD Erc, regex txt_reg, bool logSummary = true);//定点返回vector + 不要血量
	void NearbyMonst5(DWORD Erc);//打图用
	bool NerExist();
	bool NerExist(DWORD Mid);//是否在附近
	DWORD NerExist(CString cname);//返回地址
    // Resolve a nearby object by its display name through the object-manager
    // tree.  The return value is the object pointer, not the tree field slot.
    DWORD FindNearbyObjectByName(const CString& name);
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
    // Read the active-state count directly from characterObject + 0x67410.
    DWORD mRead_BufferCount(DWORD characterObject);
    // Read the same state tree from an arbitrary character/object pointer.
    // The caller supplies the object; this method only follows +0x6740C/+0x67410
    // and the shared state-definition table, without invoking game code.
    DWORD mRead_BufferFromObject(DWORD characterObject, DWORD xuh,
                                 CString* name = nullptr,
                                 CString* description = nullptr,
                                 DWORD* stateObject = nullptr);
    // Target/pet objects use the live +0x130 status tree consumed by
    // Target:GetAllBuffIconName (count is stored at object +0x134).
    DWORD mRead_TargetBufferCount(DWORD targetObject);
    DWORD mRead_TargetBufferFromObject(
        DWORD targetObject, DWORD xuh, CString* name = nullptr,
        CString* description = nullptr, DWORD* stateObject = nullptr);
    // Resolve the selected CObject_PlayerOther through the live object-manager
    // slot, then read its state tree with the same direct-memory path.
    DWORD mRead_TargetObject();
    DWORD mRead_TargetBuffer(DWORD xuh, CString* name = nullptr,
                             CString* description = nullptr,
                             DWORD* stateObject = nullptr);
    int Read_RWZT(bool qzjc = false);//读取状态
    int Read_DTID();//读地图ID
    void wRideOn(int sx);//坐骑 1上 0下
	DWORD Read_RWZTC2(DWORD erc, std::vector<DWORD> vzt);//读人物状态
	DWORD Read_RWZTC2(DWORD erc, DWORD zt);//读人物状态
	DWORD Read_RWZTgs();//读人物状态个数
	DWORD Read_RWJY();//读取经验
	int Read_GENS();//读取跟随 
	int Read_MoZTT(DWORD gwdz);//读怪物状态
	int Read_ATID();//读攻击ID
	int Read_ATID(DWORD gwdz);//读攻击ID

    // 人物朝向：优先项目 jz[jYcdiz] 的 Actor，后备 [[Game.exe+0x7C5AE8]+0x64]；Yaw = Float[Actor+0x4C]。
    bool Read_PlayerYaw(float& yawRadians);
    // 读取朝向和自身 X/Z 坐标，返回朝向前方 distance 距离的点。
    Point2f Read_PlayerFacingPoint(float distance = 19.6f);


    //---------------------窗口----------------------
    // 在 CEGUI WindowManager 的红黑树中按注册名查找窗口对象。
    // 返回值为节点 + 0x28 中保存的 Window*，未找到时返回 0。
    DWORD FindCeguiWindowByName(const CString& name);
    int IsWndGuot();//过图窗口是否打开
    int IsWndOpen(int xuh);//window open
    int IsWndOpen2(DWORD ckdx);//window open
    DWORD cGetTime();//获取当前时间
    DWORD cGetNetIc();//获取当前网络图标序号
    // xuh is the zero-based item index in BagJ->[*+0x22C00].
    // Identify follows LifeAbility:IsIndentify: 0 = identified/not applicable,
    // 1 = unidentified. Bind returns the raw GetItemBindStatus result:
    // 0 = no bind, 1 = already bound, 2 = the category-dependent bind branch,
    // 3 = bind when equipped.
    int cRead_Identify(DWORD xuh);//读取物品鉴定
    int cRead_Bind(DWORD xuh);//读取物品绑定
    CString cRead_CeguiText(DWORD xuh);
    // 读取聊天面板「队伍」频道最后一条消息；outMessage=冒号后的内容。
    // outSender 可选，返回发送者名字。返回 true=读到队伍消息。
    // 内部自带节流轮询+缓存：1.5 秒内的重复调用零开销直接返回缓存，
    // 打怪等高频循环里可放心调用；长时间没收到新队伍消息时返回缓存中的旧值。
    bool ReadLastTeamChat(CString& outMessage, CString* outSender = nullptr);
    // 缓存最后更新时间（GetTickCount），可用于判断消息新旧。
    DWORD GetLastTeamChatTick() const;
    // 换角色/重新登录后清空缓存。
    void ResetLastTeamChat();


    //---------------------周末活动----------------------
    DWORD mRead_Benj();//读取本金
    DWORD mRead_TodayActive();//通过周活跃窗口读取今日活跃值
    //DWORD mRead_Tuixh();//读取推荐序号
    PaoShangRecommendData mRead_TuiJg();//读取推荐价格、物品ID和目的地
    CString mGet_Kjda(CString cTim);//获取科举答案
    // 读取科举题目：题干只保留到第一个问号（U+FF1F），再用 mGet_Kjda 查 CSV 答案。
    // 返回 true=读到题目；outAnswer=匹配到的答案（CSV 没匹配到则为空）。
    bool ReadKejuQuestion(CString& outQuestion, CString& outAnswer);


    //---------------------跑商--------------------
    DWORD mRead_BangZw();//读取帮派职位
    int mYanzm(bool& cxjs, bool zjcsj = false);//验证码；zjcsj=true仅检查手机验证


    //---------------------任务--------------------
    CString Read_NpcT(int xuh = 0);//读npc对话
	int mRe_Renx(DWORD rwid);//取任务序号
	int mRe_Renx(CString cname);//取任务序号
	int mRenw_wc(DWORD rwxh, int xuh = 0);//判断任务是否完成 0是任务总体完成 1开始是怪物
	int mRenw_jie(size_t xuh);//按 VRenw 表：寻路->开NPC->接任务
	int mRenw_jiao(size_t xuh);//按 VRenw 表：开NPC->交任务


    //---------------------宠物+物品----------------------
	int Read_PetMx();//读最大宠物
	bool Read_CWCZ(int xuh);//读取宠物是否出站
	int Read_CWHapp(int xuh);//宠物快乐
	int Read_CWID(int xuh);//宠物ID
	int Read_CWSkillID(int xuh, int skillIndex);//读取宠物技能ID
	bool Read_CWHaveSkill(int xuh, int skillId);//判断宠物是否拥有指定技能
	int Read_CWXueJi(int xuh);//0=无，1=血祭，2=高级血祭
	void tCheckXj();//检查血祭：确保宠物出战并读取宠物技能，结果存入 m_hasXueJi/m_xjSkillId
	BagItem Read_BagX(int zl);//取加血物品位置
	BagItem Read_BagN3(CString Nam, int djg, int zl);//取物品位置银票
    int PAND_WPLX2(int xuh);//普通商店整理：-100空，1存，2留，3卖，4使用
    CString Read_BagCr(int zl);//读装备制造者名字

	//---------------------寻路----------------------
	void wBenTuXuL3z(float xx, float yy);//带方向副本用 监控停止 准确位置
	void wBenTuXuL3f(float xx, float yy);//带方向副本用 监控停止
	void wBenTuXuL3fy(float xx, float yy);//带方向副本用 监控停止--验证码
	int wBenTuXuL33(float xx, float yy, int fx);//带方向副本用距离近--8个方向
	void wBenTuXuL34(float xx, float yy, int jl, int fx);//带方向副本用杀星--16个方向
	void wBenTuXuL35(float xx, float yy);//反贼

	int wKuaTuXL(float xx, float yy, int dtid);//
	int wKuaTuXL33(float xx, float yy, int dtid);//
	void wBenTuXuL7(int mos, float xx, float yy, int szq = 1);//半圆寻路--0 1-4象限-- 1  2-1象限


	//---------------------队伍----------------------
	bool Read_Sfem();//判断队伍是否有峨眉
	int Read_TeamC();//读队伍数量
	int Read_TeamDtId(int xuh);//读队员地图
	CString Read_DuiwMz(int xuh);//读名字
	DWORD   Read_DuiwId(int xuh);//读ID
	DWORD Read_Ziji();//读自己位置序号
	Point2f Read_DuiwZb(int xuh);//读坐标
	DWORD  Read_Dmax(int xuh);//读最大血
	int Read_NeedJx2(DWORD diz, float bfb, DWORD rid);//读需要加血
	int Read_MoBelo(DWORD diz, std::vector<DWORD>& vddwid);//读怪物归属是否自己队伍
	bool ReadServerTime(std::int64_t& unixSeconds);	// 读取已同步到客户端的服务器 Unix 时间（秒），不依赖时间窗口。

    //---------------------队伍----------------------
	ZuoBi* Read_Datu();//读打图及跑
	void UpdateBag();//更新背包


private:
    DWORD ReadCeguiNumericText(DWORD window);
    CString ReadCeguiText(DWORD window);
    bool ReadTeamChatFromWindow(CString& outMessage, CString* outSender);
    bool ReadLastTeamFromPacketRing(CString& outMessage, CString* outSender);
    HANDLE EnsureGameProcess();
    bool ReadGameMemory(DWORD address, void* buffer, size_t size);
    DWORD FindNetPacketObject();
    static CString ExtractPaoShangDestination(const CString& text);
    bool ReadNearbyPlayerPosition(Point2f& position);
    DWORD ResolvePlayerActor();
    DWORD CopyNearbyDescriptor(DWORD node, CString& expression);
    static float NearbyDistance(const Point2f& from, const Point2f& to);

    CString m_lastTeamChatMessage;
    CString m_lastTeamChatSender;
    DWORD m_lastTeamChatTick = 0;
    DWORD m_lastTeamChatPollTick = 0;
    HANDLE m_gameProcess = nullptr;
    DWORD m_gameProcessPid = 0;
    DWORD m_netObject = 0;
    DWORD m_netObjectTick = 0;

    float m_nearbyDistanceLimit = (std::numeric_limits<float>::max)();
    Point2f m_nearbyReferencePosition{};
    bool m_nearbyReferencePositionValid = false;
};
