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
    int  Pet_Xueup(float xll);//宠物加血
    int EmeiJia();//峨眉加血：优先救急，再补普通队员
    int EmeiJiaxd(DWORD diz, DWORD did, DWORD jxcs);//峨眉单目标加血


	int wLianxXuLPP(CString lux, int fx, int lxdjg = 0);//连续跑路
	int wLianxXuLPP2(CString lux, int fx, int lxdjg = 0);//连续跑路
	int wLianxXuLPP3(CString lux, int fx, int lxdjg = 0);//连续跑路t
	int wBenTuXuLPP(float xx, float yy, CString nam, bool useMoveSkills = false);//商店跑
	int wBenTuXuLPoint(float xx, float yy, bool useMoveSkills = true);//到坐标：1到达，-1停止/无效，-2卡住/超时，-3死亡

	int FUN_PS6();//跑商ts4--跑城市--重新试试互斥体
	int FUN_PS7();//跑商封包--重新试试互斥体
	int FUN_CS2();//出售物品



	int RunAndAttk(DWORD jiid, DWORD moid);//走过去攻击：1已发，0跳过，-1目标/路径无效
	void KillMonsterBs(std::vector<Monst> vvm);
	bool KillMonsterBs();
    int KillMonstShax();//妖星战斗
    int KillMonstShax(DWORD dglx, DWORD dgsj, DWORD xjsj, Point2f ptdd);//杀星首领
    Point2f Get_Minp(std::vector<Point2f> traps, Point2f monster, Point2f player,
                     bool evadeOnly = false);//半径5，接近路线/近战贴身路线安全；远程射程内停稳；evadeOnly仅避险；无解(-1,-1)
	int KillMonst(CString nam, DWORD dtid, int rwxh = -1, bool sfdd = false);//通用打怪
	int KillMonst_Fz(CString nam, std::vector<std::vector<Point2f>>& vvp,
	                 const std::function<bool()>& continueTask = {});//副本里面的打怪

	int KillMonster(Monst  vvm1, int xdz = 1);
	int KillMonsterMz(Monst  vvm1, std::vector<DWORD>& vddwid);

	void RunCircle(int leix, float xuel = 0.45);//跑圆圈

	int Yins();//隐身 + 停止
	void RunTeam(int ilj = 1);//集合：向队友平均位置移动（参考 RunTeam 简化版）
	// 游戏路线缓存：每个阶段（路线目标变化）首次寻路后 2 秒读取一次，本阶段内不再重复读取。
	bool EnsureGamePath(const Point2f& target);
	bool ReadGamePath(std::vector<Point2f>& path);
	// 取缓存路线上“距当前位置沿路前进 distance 米”的点（瞬移施放点）。
	bool GetTeleportPointOnPath(const Point2f& current, float distance, Point2f& out) const;



protected:
    enum ShaxResult { M_hmsg = 0x101, M_nomt = 0x102, M_wkil = 0x103,
                      M_dead = 0x105 };
    // 原 gf/blp 中杀星使用的状态改为当前脚本实例独享，避免串窗口。
    bool m_shaxPetRest = false;
    bool m_shaxUseHealthItem = false;
    bool m_shaxOnlyHeal = false;//当前项目没有旧版 m_zjx 控件，默认兼顾攻击
    bool m_emeiQingxin = false;
    DWORD m_emeiHealSkill = 0x197;
    DWORD m_shaxInvisibleTick = 0;
    DWORD m_shaxXuejiTick = 0;
    DWORD m_shaxItemTick = 0;
    DWORD m_shaxSkillTick = 0;
    DWORD m_shaxGongSunId = 0;//本轮首次发现的可攻击公孙圣，分身不覆盖此 ID
    bool m_shaxGongSunPetRest = false;
    static constexpr DWORD kHuaRongTrapWindowMs = 25000;//花容喊话后25秒内，攻击前检查陷阱
    bool ShaxActive();
    bool ShaxHandleGongSunClones();//返回 true 时暂停攻击，等待分身转移目标
    bool ReadShaxPosition(DWORD objectSlot, Point2f& point);
    bool ReadShaxVitals(float& hp, float& mp);

private:
    int KillMonstShaxInternal(DWORD dglx, DWORD dgsj, DWORD xjsj,
                             Point2f ptdd, bool boss);
    void ShaxUseSkills();
    void ShaxMaintain();
    bool ShaxReadHealth(DWORD objectSlot, float& hp);
    int RunRouteInternal(const CString& lux, int fx, int lxdjg, int mode);
    int RunMerchantInternal(bool cityRoute);
    bool UseRouteMoveSkills(const Point2f& current, const Point2f& target);
    std::unordered_map<DWORD, DWORD> m_routeMoveSkillReadyTick;
    bool m_routeMoveSkillsLogged = false;
    bool m_routeLastMapIdValid = false;
    DWORD m_routeLastMapId = 0;
    DWORD m_routeLastMapTransitionTick = 0;
    DWORD m_routeAccelActiveUntil = 0;
    DWORD m_lastSkillCastTick = 0;
    // 瞬移施放后的异步蓝量校验（不阻塞寻路循环）
    DWORD m_pendingCastSkillId = 0;
    DWORD m_pendingCastCheckAt = 0;
    float m_pendingCastMana = -1.0f;
    // 技能被跳过的原因日志节流（5 秒最多一条）
    DWORD m_lastSkillSkipLogTick = 0;
    DWORD m_teamGatherTick = 0;
    // 游戏路线缓存：人物移动中(状态2)时每次调用都重新读取更新。
    std::vector<Point2f> m_gamePath;
    bool m_pathReady = false;
    DWORD m_pathLogTick = 0;
};
