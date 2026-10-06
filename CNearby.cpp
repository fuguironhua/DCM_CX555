#include "pch.h"
#include "CNearby.h"
#include <cmath>
#include <mutex>
bool CNearby::ScanNearbyOnce() { return true; }


namespace {

// The reference project used global Split()/ReadIni() helpers; the new
// framework keeps the same behaviour with the two helpers below.
void SplitText(const CString& text, std::vector<CString>& tokens, LPCTSTR delimiter) {
    tokens.clear();
    int start = 0;
    CString token = text.Tokenize(delimiter, start);
    while (!token.IsEmpty()) {
        tokens.push_back(token);
        token = text.Tokenize(delimiter, start);
    }
}

enum class RouteMoveSkillKind {
    SelfBuff,
    FacingTeleport
};

struct RouteMoveSkill {
    DWORD id = 0;
    RouteMoveSkillKind kind = RouteMoveSkillKind::SelfBuff;
    DWORD cooldownMs = 0;
    DWORD recoverMinMs = 5000;
    DWORD recoverMaxMs = 5000;
};

using RouteMoveSkillList = std::vector<RouteMoveSkill>;

constexpr bool kEnableRouteMoveSkills = true;

const std::vector<RouteMoveSkillList>& RouteMoveSkillsByMenPai() {
    static const std::vector<RouteMoveSkillList> table = {
        {}, {}, {}, {}, {}, {}, {},
        { {0x1EE, RouteMoveSkillKind::SelfBuff, 30000, 2000, 2000} }, // 白驹过隙：固定 30+2 秒间隔
        { {0x20C, RouteMoveSkillKind::FacingTeleport, 14000, 0, 0} }, // 逍遥：凌波微步（固定 14 秒）
        {}, {}, {}, {}
    };
    return table;
}

// Mirrors the reference behaviour of reading "<workdir>/<file>"; the packaged
// build keeps the same files under "<workdir>/config".
// Color probes used by the reference wBenTuXuLPP map-transition dialogs.
constexpr LPCTSTR kXunLu1First = _T("00ff00");
constexpr LPCTSTR kXunLu1Offset = _T("-38|22|00ff00,-26|26|fff263,-52|44|fff263,76|97|fdeb78,96|97|fdeb78,128|96|fdeb78,136|103|fdeb78,23|37|fff263");
constexpr LPCTSTR kXunLu2First = _T("fff263");
constexpr LPCTSTR kXunLu2Offset = _T("17|4|fff263,30|18|fff263,68|25|fff263,167|76|fdeb78,190|85|fdeb78,207|75|fdeb78,224|76|fdeb78,69|24|fff263");
constexpr LPCTSTR kXunLu3First = _T("00ff00");
constexpr LPCTSTR kXunLu3Offset = _T("45|10|00ff00,-17|38|fff263,26|43|fff263,93|69|fdeb78,110|79|fdeb78,135|70|fdeb78,148|76|fdeb78,27|42|fff263");
constexpr LPCTSTR kXunLu4First = _T("fff263");
constexpr LPCTSTR kXunLu4Offset = _T("18|3|fff263,67|10|00ff00,91|10|00ff00,108|7|00ff00,128|0|fff263,146|93|fdeb78,157|102|fdeb78,194|97|fdeb78");

CString ResolveWorkIni(const CString& fileName, const CString& workDir) {
    CString base = workDir;
    if (base.IsEmpty()) {
        base = CString(std::filesystem::current_path().c_str());
    }

    CString path = base + _T("/") + fileName;
    if (!std::filesystem::exists(std::filesystem::path(path.GetString()))) {
        const CString nested = base + _T("/config/") + fileName;
        if (std::filesystem::exists(std::filesystem::path(nested.GetString()))) {
            return nested;
        }
    }
    return path;
}

}  // namespace

// 初始化人物: 先按门派装默认攻击技能, 再用 人物配置.ini 的 RWJN 列表覆盖。
// Ported from the reference project (DCM_CX5 CNearby.cpp:24 CNearby::mChusrw()).
void CNearby::mChusrw()
{
    const std::vector<TaskSkillPair> attackSkills = VSKILL2;
    const std::vector<TaskSkillMenu> defaultSkills = VSKILL;
    const DWORD menPai = Mams[Menpaid];

    if (menPai < attackSkills.size() && menPai < defaultSkills.size()) {
        Mams[Jijl] = std::get<0>(attackSkills[menPai]);
        Mams[JiGj] = std::get<1>(attackSkills[menPai]);
        DebugLog(_T("mChusrw: menpai=%lu Jijl=%lu JiGj=%lu"),
                 static_cast<unsigned long>(menPai),
                 static_cast<unsigned long>(Mams[Jijl]),
                 static_cast<unsigned long>(Mams[JiGj]));

        // 默认按门派技能表
        Vkill.clear();
        Vkill = defaultSkills[menPai];
    } else {
        DebugLog(_T("mChusrw: menpai=%lu out of VSKILL range (%lu)"),
                 static_cast<unsigned long>(menPai),
                 static_cast<unsigned long>(defaultSkills.size()));
    }

    // 人物配置.ini 里配置了 RWJN 时, 按 Jineng.ini 的定义重建技能列表
    const CString characterIni =
        ResolveWorkIni(_T("人物配置.ini"), tsg[TaskMemoryState::workdir]);
    const CString skillIni =
        ResolveWorkIni(_T("Jineng.ini"), tsg[TaskMemoryState::workdir]);

    const CString skillList = Dm()->ReadIni(
        _T("RWJN"), tsg[TaskMemoryState::m_Name].GetString(), characterIni.GetString());
    if (!skillList.IsEmpty()) {
        std::vector<CString> skillNames;
        SplitText(skillList, skillNames, _T("|"));

        Vkill.clear();
        VYid.clear();
        VZtai.clear();

        for (const CString& skillName : skillNames) {
            const CString definition = Dm()->ReadIni(
                _T("BASE"), skillName.GetString(), skillIni.GetString());
            if (definition.IsEmpty()) continue;

            std::vector<CString> fields;
            SplitText(definition, fields, _T("|"));
            if (fields.size() < 6) {
                DebugLog(_T("mChusrw: skill '%s' definition has %lu fields"),
                         skillName.GetString(),
                         static_cast<unsigned long>(fields.size()));
                continue;
            }

            const int jid = static_cast<int>(wcstoul(fields[0].GetString(), nullptr, 16));
            const int jlen = static_cast<int>(wcstoul(fields[3].GetString(), nullptr, 10));
            const int jleix = static_cast<int>(wcstoul(fields[5].GetString(), nullptr, 10));

            // 判断状态/移动类技能
            if (jleix == L_yd || jleix == L_js) {
                VYid.push_back({ jid, jlen, 0, static_cast<DWORD>(jleix), skillName });
                continue;
            }
            if (jleix == L_zt || jleix == L_zt1) {
                const int ijcid = fields.size() > 6
                    ? static_cast<int>(wcstoul(fields[6].GetString(), nullptr, 10))
                    : 0;
                VZtai.push_back({ jid, jlen, 0, static_cast<DWORD>(jleix), skillName,
                                  static_cast<DWORD>(ijcid) });
                continue;
            }

            Vkill.push_back({ jid, jlen, 0, static_cast<DWORD>(jleix), skillName });// 攻击类技能
        }
    }

    BianSK(0);
}

// Ported from DCM_CX5 CNearby::Pet_Xueup.  The CX555 framework has no global
// gf/isKill or poif cache, so this version uses CThreadControl and reads the
// live player copy through the current base-class helpers.
bool CNearby::UseRouteMoveSkills(const Point2f& current, const Point2f& target) {
    if (!m_control || m_control->IsStopping()) return false;

    const auto& table = RouteMoveSkillsByMenPai();
    const DWORD menPai = Mams[Menpaid];
    if (menPai >= table.size() || table[menPai].empty()) return false;

    if (!m_routeMoveSkillsLogged) {
        LogD(_T("route move skills active: menpai=%lu count=%lu"),
             static_cast<unsigned long>(menPai),
             static_cast<unsigned long>(table[menPai].size()));
        m_routeMoveSkillsLogged = true;
    }

    DWORD now = GetTickCount();
    const DWORD currentMap = static_cast<DWORD>(Read_DTID());
    if (!m_routeLastMapIdValid) {
        m_routeLastMapId = currentMap;
        m_routeLastMapIdValid = true;
        m_routeLastMapTransitionTick = now - 3000;
    } else if (currentMap != m_routeLastMapId) {
        m_routeLastMapId = currentMap;
        m_routeLastMapTransitionTick = now;
        LogD(_T("route move skills: map=%lu changed, wait 3s"),
             static_cast<unsigned long>(currentMap));
    }

    if (IsWndGuot() == 1) return false;

    const float dx = target.x - current.x;
    const float dy = target.y - current.y;
    const float remaining = std::sqrt(dx * dx + dy * dy);
    if (!std::isfinite(remaining) || remaining <= 0.0f) return false;
    if (remaining <= 20.0f) return false;

    // 蓝量读取：属性块 Actor -> +148 -> +10（与医生加血同源），mp 为 0~1 比例。
    const auto Read_Mana = [this]() -> float {
        if (tsg[TaskMemoryState::Shuxtb].IsEmpty()) return -1.0f;
        if (!UpdateMP(tsg[TaskMemoryState::Shuxtb], 0x10)) return -1.0f;
        const DWORD copy = dwls;
        if (copy < 0x10000) return -1.0f;
        const float mp = *reinterpret_cast<float*>(static_cast<ULONG_PTR>(copy) + 0x0C);
        return std::isfinite(mp) ? mp : -1.0f;
    };

    // 结算上一次瞬移的蓝量校验：非阻塞（到点才读一次），避免卡住寻路循环。
    if (m_pendingCastSkillId != 0 &&
        static_cast<LONG>(now - m_pendingCastCheckAt) >= 0) {
        const DWORD pendingId = m_pendingCastSkillId;
        const float manaBefore = m_pendingCastMana;
        const float manaAfter = Read_Mana();
        m_pendingCastSkillId = 0;
        m_pendingCastMana = -1.0f;
        const bool bothValid = manaBefore >= 0.0f && manaAfter >= 0.0f;
        const bool decreased = bothValid && manaAfter < manaBefore - 0.0005f;
        if (bothValid && !decreased) {
            LogD(_T("route move skill: id=0x%lX cast failed (mp %.4f -> %.4f), retry in 2s"),
                 static_cast<unsigned long>(pendingId), manaBefore, manaAfter);
            m_routeMoveSkillReadyTick[pendingId] = GetTickCount() + 2000;
        } else if (bothValid) {
            LogD(_T("route move skill: id=0x%lX cast ok (mp %.4f -> %.4f)"),
                 static_cast<unsigned long>(pendingId), manaBefore, manaAfter);
        }
    }

    // 技能被跳过时的原因日志（节流 5 秒一条），排查"长时间不放技能"用。
    const auto logSkillSkip = [this, now](DWORD skillId, LPCTSTR reason) {
        if (static_cast<LONG>(now - m_lastSkillSkipLogTick) < 5000) return;
        m_lastSkillSkipLogTick = now;
        LogD(_T("route move skill: id=0x%lX skipped (%s)"),
             static_cast<unsigned long>(skillId), reason);
    };

    const LONG transitionElapsed =
        static_cast<LONG>(now - m_routeLastMapTransitionTick);
    if (transitionElapsed >= 0 && transitionElapsed < 3000) return false;

    bool anySent = false;
    for (const RouteMoveSkill& skill : table[menPai]) {
        const auto ready = m_routeMoveSkillReadyTick.find(skill.id);
        if (ready != m_routeMoveSkillReadyTick.end() &&
            static_cast<LONG>(now - ready->second) < 0) {
            CString skipReason;
            skipReason.Format(_T("cooldown %lu ms left"),
                              static_cast<unsigned long>(ready->second - now));
            logSkillSkip(skill.id, skipReason.GetString());
            continue;
        }

        bool sent = false;
        float manaBefore = -1.0f;
        Point2f castPoint{};
        if (skill.kind == RouteMoveSkillKind::SelfBuff) {
            sent = wUseSk6(skill.id, static_cast<DWORD>(-1));
        } else if (remaining > 20.0f) {
            // Do not teleport while the acceleration buff is active.
            if (static_cast<LONG>(now - m_routeAccelActiveUntil) < 0) {
                logSkillSkip(skill.id, _T("accel buff active"));
                continue;
            }

            // 只有瞬移技能需要终点计算：维护路线缓存（阶段目标变化后 2 秒读一次），
            // 加速技能（SelfBuff）不走这里。
            EnsureGamePath(target);

            // 技能最大距离 20，留 0.2 米余量避免“超范围”被拒（客户端/服务器按 20 判定）。
            constexpr float kTeleportDistance = 19.6f;

            // ① 优先：人物朝向方向的点，且必须可到达（wXunL_Guih == 1）。
            const Point2f facingPoint = Read_PlayerFacingPoint(kTeleportDistance);
            bool pointReady = false;
            if (wXunL_Guih(facingPoint.x, facingPoint.y) == 1) {
                castPoint = facingPoint;
                pointReady = true;
            } else {
                // ② 其次：路线上的点（圆∩折线，kTeleportDistance 米），同样要求可到达。
                Point2f pathPoint{};
                if (GetTeleportPointOnPath(current, kTeleportDistance, pathPoint) &&
                    wXunL_Guih(pathPoint.x, pathPoint.y) == 1) {
                    castPoint = pathPoint;
                    pointReady = true;
                }
            }
            // ③ 两者都不可到达：等 2 秒再判断。
            if (!pointReady) {
                logSkillSkip(skill.id, _T("no reachable teleport point, wait 2s"));
                m_routeMoveSkillReadyTick[skill.id] = now + 2000;
                continue;
            }

            manaBefore = Read_Mana(); // 记录施法前蓝量，用于判定瞬移是否成功
            sent = wUseSk6(skill.id, castPoint);
        }

        if (sent) {
            anySent = true;
            if (skill.kind == RouteMoveSkillKind::FacingTeleport) {
                LogD(_T("route move skill: id=0x%lX point=(%.1f, %.1f) dist=%.1f"),
                     static_cast<unsigned long>(skill.id), castPoint.x, castPoint.y, remaining);
            } else {
                LogD(_T("route move skill: id=0x%lX"),
                     static_cast<unsigned long>(skill.id));
            }

            // 瞬移技能登记异步蓝量校验（1 秒后检查），不阻塞寻路循环。
            if (skill.kind == RouteMoveSkillKind::FacingTeleport) {
                m_pendingCastSkillId = skill.id;
                m_pendingCastMana = manaBefore;
                m_pendingCastCheckAt = GetTickCount() + 1000;
            }

            const DWORD recoverBufferMs = skill.recoverMinMs +
                (skill.recoverMaxMs > skill.recoverMinMs
                    ? static_cast<DWORD>(rand()) % (skill.recoverMaxMs - skill.recoverMinMs + 1)
                    : 0);
            m_routeMoveSkillReadyTick[skill.id] =
                GetTickCount() + skill.cooldownMs + recoverBufferMs;

            if (skill.kind == RouteMoveSkillKind::SelfBuff) {
                // 白驹过隙 buff lasts about 15s. Teleport is delayed
                // another random 5-10s after that buff ends.
                m_routeAccelActiveUntil = now + 15000;
                const DWORD teleportBufferMs =
                    7000 + static_cast<DWORD>(rand() % 5001);
                for (const RouteMoveSkill& other : table[menPai]) {
                    if (other.kind == RouteMoveSkillKind::FacingTeleport) {
                        m_routeMoveSkillReadyTick[other.id] =
                            m_routeAccelActiveUntil + teleportBufferMs;
                    }
                }
            }

            // Only one route-move skill per check.
            break;
        }
    }
    return anySent;
}
int CNearby::Pet_Xueup(float xll)
{
    constexpr DWORD kPetRecordSize = 524;
    if (Mams[CoXuHa] == static_cast<DWORD>(-1)) return 0;
    if (!m_control || m_control->IsStopping()) return 0;

    const auto readPlayerPosition = [this](Point2f& position) -> bool {
        if (!UpdateMP(static_cast<DWORD>(jz[TaskMemoryState::jYcdiz]), 0x100)) return false;
        const DWORD copy = dwls;
        if (copy < 0x10000) return false;
        position.x = *reinterpret_cast<float*>(static_cast<ULONG_PTR>(copy) + 0x54);
        position.y = *reinterpret_cast<float*>(static_cast<ULONG_PTR>(copy) + 0x5C);
        return true;
    };

    while (!Read_CWCZ(static_cast<int>(Mams[CoXuHa])) &&
           !m_control->IsStopping()) {
        while (Read_RWZT() == 9 && !m_control->IsStopping()) SleepEx(1000);
        if (m_control->IsStopping()) return 0;

        wRideOn(0);
        if (Read_RWZT() != 0) {
            Point2f position{};
            if (readPlayerPosition(position)) {
                wBenTuXuL2(position.x + 0.1f, position.y);
            }
            SleepEx(1500);
        }

        if (Read_RWZT() == 0) {
            if (wPet_Chu(static_cast<DWORD>(Mams[CoXuHa]), 0) != 0) return -1;
            SleepEx(1000);
            while (Read_RWZT() == 5 && !m_control->IsStopping()) SleepEx(1000);
            SleepEx(1500);
        }
        LogD(_T("cw111"));
    }

    DWORD usedCount = 0;
    const DWORD petIndex = Mams[CoXuHa];
    while (!m_control->IsStopping() &&
           UpdateMP(tsg[TaskMemoryState::shuxcw], 0x1000)) {
        while (Read_RWZT() == 9 && !m_control->IsStopping()) SleepEx(1000);
        if (m_control->IsStopping()) break;

        const DWORD petRecord = static_cast<DWORD>(
            static_cast<ULONG_PTR>(dwls) + petIndex * kPetRecordSize);
        const float currentHp = static_cast<float>(
            *reinterpret_cast<DWORD*>(static_cast<ULONG_PTR>(petRecord) + 68));
        const float maxHp = static_cast<float>(
            *reinterpret_cast<DWORD*>(static_cast<ULONG_PTR>(petRecord) + 72));
        if (maxHp <= 0.0f) break;
        if ((currentHp / maxHp) >= xll) break;

        wUseBag2(3);
        if (bag[bacow].bjxcz) return -1;
        SleepEx(1000);
        ++usedCount;
        LogD(_T("cw222"));
    }

    return usedCount == 0 ? 1 : 0;
}


int CNearby::wBenTuXuLPP(float xx, float yy, CString nam, bool useMoveSkills)
{
    if (!m_control || m_control->IsStopping()) return -1;

    const CString mapIni = ResolveWorkIni(
        _T("\u5730\u56fe\u914d\u7f6e.txt"), tsg[TaskMemoryState::workdir]);
    const CString mapIdText = Dm()->ReadIni(
        _T("\u914d\u7f6e"), nam.GetString(), mapIni.GetString());
    if (mapIdText.IsEmpty()) {
        LogD(_T("map id missing: %s"), nam.GetString());
        return -1;
    }
    const int targetMap = _wtoi(mapIdText.GetString());

    const auto readPlayerPosition = [this](Point2f& position) -> bool {
        if (!UpdateMP(static_cast<DWORD>(jz[TaskMemoryState::jYcdiz]), 0x100)) return false;
        const DWORD copy = dwls;
        if (copy < 0x10000) return false;
        position.x = *reinterpret_cast<float*>(static_cast<ULONG_PTR>(copy) + 0x54);
        position.y = *reinterpret_cast<float*>(static_cast<ULONG_PTR>(copy) + 0x5C);
        return true;
    };
    const auto findMultiColorClient = [this](long x1, long y1, long x2, long y2,
                                             const TCHAR* firstColor,
                                             const TCHAR* offsetColor,
                                             double sim) -> CString {
        POINT topLeft{ x1, y1 };
        POINT bottomRight{ x2, y2 };
        if (m_wnd && m_wnd->hWnd) {
            ::ClientToScreen(m_wnd->hWnd, &topLeft);
            ::ClientToScreen(m_wnd->hWnd, &bottomRight);
        }
        return Dm()->FindMultiColorE(
            topLeft.x, topLeft.y, bottomRight.x, bottomRight.y,
            firstColor, offsetColor, sim, 0);
    };

    const auto distance = [](const Point2f& a, const Point2f& b) -> float {
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        return std::sqrt(dx * dx + dy * dy);
    };

    const auto healAtCityIfNeeded = [this, &mapIni, &readPlayerPosition, &distance]() {
        if (!m_control || m_control->IsStopping() || IsWndGuot()) return;
        const int map = Read_DTID();
        const CString suzhouId = Dm()->ReadIni(_T("配置"), _T("苏州"), mapIni.GetString());
        const CString luoyangId = Dm()->ReadIni(_T("配置"), _T("洛阳"), mapIni.GetString());
        const CString daliId = Dm()->ReadIni(_T("配置"), _T("大理"), mapIni.GetString());
        struct CityDoctor { Point2f position; LPCTSTR name; };
        std::vector<CityDoctor> doctors;
        if (!suzhouId.IsEmpty() && map == _wtoi(suzhouId)) {
            doctors = {{{183, 263}, _T("程远志")}, {{181, 74}, _T("朱防风")}};
        } else if (!luoyangId.IsEmpty() && map == _wtoi(luoyangId)) {
            doctors = {{{66, 134}, _T("柯伯仁")}, {{255, 134}, _T("窦梧桐")},
                       {{161, 245}, _T("韩银叶")}};
        } else if (!daliId.IsEmpty() && map == _wtoi(daliId)) {
            doctors = {{{40, 152}, _T("陶先秋")}, {{278, 152}, _T("白展元")},
                       {{159, 252}, _T("谭景天")}};
        }
        if (doctors.empty()) return;

        // 血蓝位于属性块（Actor -> +148 -> +10），一次复制同时读取。
        if (tsg[TaskMemoryState::Shuxtb].IsEmpty() ||
            !UpdateMP(tsg[TaskMemoryState::Shuxtb], 0x10)) return;
        const DWORD copy = dwls;
        if (copy < 0x10000) return;
        const float hp = *reinterpret_cast<float*>(static_cast<ULONG_PTR>(copy) + 0x08);
        const float mp = *reinterpret_cast<float*>(static_cast<ULONG_PTR>(copy) + 0x0C);
        if (!std::isfinite(hp) || !std::isfinite(mp) || mp < 0.0f || mp >= 0.5f) return;
        Point2f position{};
        if (!readPlayerPosition(position) ||
            !std::isfinite(position.x) || !std::isfinite(position.y)) return;
        const auto nearest = std::min_element(doctors.begin(), doctors.end(), [&position, &distance](const CityDoctor& a, const CityDoctor& b) {
            return distance(position, a.position) < distance(position, b.position);
        });
        const CString doctorName = nearest->name;
        const Point2f destination = nearest->position;
        const auto active = [this, map]() {
            return !m_control->IsStopping() && !IsWndGuot() &&
                Read_DTID() == map && Read_RWZT(true) != 9;
        };
        LogD(_T("city healer: mp=%.3f seek %s (%.0f, %.0f)"),
             mp, doctorName.GetString(), destination.x, destination.y);
        const ULONGLONG started = GetTickCount64();
        ULONGLONG lastMove = 0;
        ULONGLONG lastProgress = started;
        Point2f progressPosition = position;
        bool arrived = false;
        // 寻路包只负责发起移动；必须等到医生坐标附近才开始遍历。
        while (active() && GetTickCount64() - started < 120000) {
            if (readPlayerPosition(position) &&
                std::isfinite(position.x) && std::isfinite(position.y)) {
                if (distance(position, destination) < 3.0f) { arrived = true; break; }
                const ULONGLONG now = GetTickCount64();
                if (distance(position, progressPosition) >= 1.0f) {
                    progressPosition = position;
                    lastProgress = now;
                }
                if (!lastMove || (now - lastMove >= 3000 &&
                    (Read_RWZT(true) != 2 || now - lastProgress >= 5000))) {
                    wBenTuXuL2(destination.x, destination.y);
                    lastMove = GetTickCount64();
                }
            }
            SleepEx(300);
        }
        if (!active() || !arrived) {
            LogD(_T("city healer: movement interrupted or timed out: %s"), doctorName.GetString());
            return;
        }
        if (!doctorName.IsEmpty()) {
            // 与"寻路到NPC进帮派"同款流程：wOpenMan(名字) 内部完成
            // NerbMonst2 查找 + wXuanZg2 选择 + 打开，然后再等窗口、点选项。
            wOpenMan(doctorName);
            if (!active()) return;
            bool npcWindowOpen = false;
            const ULONGLONG windowWaitStarted = GetTickCount64();
            while (active() && GetTickCount64() - windowWaitStarted < 10000) {
                if (IsWndOpen2(Quest_Frame) == 1) {
                    npcWindowOpen = true;
                    break;
                }
                SleepEx(200 + rand() % 100);
            }
            if (!active()) return;
            if (!npcWindowOpen) {
                LogD(_T("city healer: npc window not open: %s"), doctorName.GetString());
                return;
            }
            wClickNpc(1); // 点击治疗选项
            SleepEx(300);
            if (!active()) return;
            wClickNpc(1); // 点击确认
            SleepEx(300);
        }
    };
    wBenTuXuL2(xx, yy);
    SleepEx(400);

    int stalled = 0;
    DWORD lastMoveTick = GetTickCount();
    bool moveIssued = true; // 初始寻路包已发送，后续只在停止/卡住时重发。
    DWORD arrivedAt = 0;
    DWORD skillRetryAt = 0;
    bool skillRetryPending = false;
    const auto readMoveSpeed = [this]() -> float {
        if (tsg[TaskMemoryState::Shuxtb].IsEmpty()) return 0.0f;
        const DWORD characterData = Read_Dw(tsg[TaskMemoryState::Shuxtb]);
        if (characterData < 0x10000) return 0.0f;
        return Dm()->ReadFloatAddr(
            mmhwnd, static_cast<LONGLONG>(characterData) + 0x4C);
    };
    while (!m_control->IsStopping()) {
        if (Read_DTID() == targetMap) {
            while (IsWndGuot() == 1 && !m_control->IsStopping()) SleepEx(1000);
            healAtCityIfNeeded();
            return 1;
        }
        if (Read_RWZT(true) == 9) return -3;

        Point2f position{};
        if (!readPlayerPosition(position)) {
            SleepEx(500);
            continue;
        }

        if (skillRetryPending && static_cast<LONG>(GetTickCount() - skillRetryAt) >= 0) {
            const float moveSpeed = readMoveSpeed();
            LogD(_T("route accel check: speed=%.3f"), moveSpeed);
            if (std::isfinite(moveSpeed) && moveSpeed > 0.0f && moveSpeed <= 4.0f) {
                LogD(_T("route accel retry: speed=%.3f"), moveSpeed);
                // 重试也遵守技能冷却 + 5 秒的限制。
                if (useMoveSkills && UseRouteMoveSkills(position, Point2f{ xx, yy })) {
                    SleepEx(300);
                    if (wBenTuXuL2(xx, yy) != 0) {
                        LogD(_T("route accel retry: endpoint move command failed"));
                    }
                    lastMoveTick = GetTickCount();
                    stalled = 0;
                    moveIssued = true;
                }
            }
            skillRetryPending = false;
            continue;
        }

        if (useMoveSkills && UseRouteMoveSkills(position, Point2f{ xx, yy })) {
            if (m_control->IsStopping()) return -1;
            SleepEx(Mams[Menpaid] == 8 ? 1000 : 300);

            // The skill packet can clear the previous move command. Reissue
            // the current waypoint and reset the stall detector.
            if (wBenTuXuL2(xx, yy) != 0) {
                LogD(_T("route skill: endpoint move command failed"));
            }
            lastMoveTick = GetTickCount();
            stalled = 0;
            moveIssued = true;
            // 仅天山白驹过隙检查加速效果，凌波微步不按移速判断。
            skillRetryAt = GetTickCount() + 3000;
            skillRetryPending = (Mams[Menpaid] == 7);

            if (!readPlayerPosition(position)) {
                SleepEx(300);
                continue;
            }
        }

        const float remaining = distance(position, Point2f{ xx, yy });
        if (remaining < 5.0f) {
            if (arrivedAt == 0) {
                arrivedAt = GetTickCount();
                LogD(_T("waiting map transition: %s target=%d"), nam.GetString(), targetMap);
            }

            bool normalDialogHandled = false;
            if (IsWndOpen2(MessageBox_Self_Frame) == 1) {
                mLua_Dostr(XUNLQR);
                normalDialogHandled = true;
                SleepEx(1000 + rand() % 500);
            }
            if (IsWndOpen2(AcceptBox_Frame) == 1) {
                mLua_Dostr(XUNLQR2);
                SleepEx(1000 + rand() % 500);
            }

            // The client shows a pathfinding confirmation dialog on several
            // map transitions.  These are the same probes used by the
            // reference wBenTuXuLPP implementation.
            const bool normalDialog =
                findMultiColorClient(390, 302, 639, 409,
                                    kXunLu2First, kXunLu2Offset, 0.98) != _T("-1|-1") ||
                findMultiColorClient(406, 348, 643, 478,
                                    kXunLu1First, kXunLu1Offset, 0.98) != _T("-1|-1") ||
                findMultiColorClient(386, 347, 638, 473,
                                    kXunLu4First, kXunLu4Offset, 0.98) != _T("-1|-1");
            if (normalDialog && !normalDialogHandled) {
                mLua_Dostr(XUNLQR);
                SleepEx(1000 + rand() % 500);
            }

            const bool acceptDialog =
                findMultiColorClient(355, 232, 621, 335,
                                    kXunLu3First, kXunLu3Offset, 0.98) != _T("-1|-1");
            if (acceptDialog) {
                mLua_Dostr(XUNLQR2);
                SleepEx(1000 + rand() % 500);
            }

            while (IsWndGuot() == 1 && !m_control->IsStopping()) {
                SleepEx(1000);
            }

            if (Read_DTID() == targetMap) {
                while (IsWndGuot() == 1 && !m_control->IsStopping()) {
                    SleepEx(1000);
                }
                healAtCityIfNeeded();
                return 1;
            }

            if (GetTickCount() - arrivedAt > 45000) {
                LogD(_T("route transition timeout: %s target=(%.1f, %.1f) current=(%.1f, %.1f) map=%d"),
                     nam.GetString(), xx, yy, position.x, position.y, Read_DTID());
                return -2;
            }

            SleepEx(1000);
            continue;
        }

        arrivedAt = 0;

        if (Read_RWZT() == 0) {
            ++stalled;
            if (stalled >= 2) {
                bool moved = false;
                for (int direction = 0; direction < 8 && !m_control->IsStopping(); ++direction) {
                    const Point2f before = position;
                    wBenTuXuL33(position.x, position.y, direction);
                    SleepEx(1200);
                    Point2f after{};
                    if (readPlayerPosition(after) && distance(before, after) > 0.25f) {
                        moved = true;
                        break;
                    }
                }
                if (!moved) {
                    LogD(_T("route stuck: %s target=(%.1f, %.1f) current=(%.1f, %.1f) map=%d"),
                     nam.GetString(), xx, yy, position.x, position.y, Read_DTID());
                    return -2;
                }
                stalled = 0;
                moveIssued = false; // 八方调整成功后重新发送主目标。
            }
        } else {
            stalled = 0;
        }

        if (!moveIssued) {
            wBenTuXuL2(xx, yy);
            lastMoveTick = GetTickCount();
            moveIssued = true;
        }
        SleepEx(1000);
    }
    return -1;
}

int CNearby::wBenTuXuLPoint(float xx, float yy, bool useMoveSkills)
{
    if (!m_control || m_control->IsStopping() ||
        !std::isfinite(xx) || !std::isfinite(yy)) return -1;
    const Point2f target{ xx, yy };
    const auto readPosition = [this](Point2f& position) {
        if (!UpdateMP(static_cast<DWORD>(jz[TaskMemoryState::jYcdiz]), 0x100) ||
            dwls < 0x10000) return false;
        const ULONG_PTR copy = static_cast<ULONG_PTR>(dwls);
        position.x = *reinterpret_cast<const float*>(copy + 0x54);
        position.y = *reinterpret_cast<const float*>(copy + 0x5C);
        return std::isfinite(position.x) && std::isfinite(position.y);
    };
    const auto distance = [](const Point2f& a, const Point2f& b) {
        const float dx = a.x - b.x, dy = a.y - b.y;
        return std::sqrt(dx * dx + dy * dy);
    };

    LogD(_T("route point start: (%.1f, %.1f) skills=%d"), xx, yy, useMoveSkills);
    ULONGLONG lastProgress = GetTickCount64();
    ULONGLONG lastMove = 0;
    Point2f progressPosition{};
    bool haveProgress = false;
    bool movePending = true;
    while (!m_control->IsStopping()) {
        // 过图期间暂停读取，过图结束后继续；地图改变本身不代表到达。
        if (IsWndGuot()) {
            haveProgress = false;
            movePending = true;
            SleepEx(300);
            lastProgress = GetTickCount64();
            continue;
        }
        if (Read_RWZT(true) == 9) return -3;
        Point2f position{};
        if (!readPosition(position)) {
            if (GetTickCount64() - lastProgress >= 15000) {
                LogD(_T("route point: position unavailable"));
                return -2;
            }
            SleepEx(300);
            continue;
        }
        if (distance(position, target) < 2.0f) {
            LogD(_T("route point arrived: (%.1f, %.1f)"), xx, yy);
            return 1;
        }
        const ULONGLONG now = GetTickCount64();
        if (!haveProgress || distance(position, progressPosition) > 0.25f) {
            progressPosition = position;
            haveProgress = true;
            lastProgress = now;
        }

        if (now - lastProgress >= 5000) {
            LogD(_T("route point stuck: try eight directions"));
            bool adjusted = false;
            for (int direction = 0; direction < 8; ++direction) {
                if (m_control->IsStopping()) return -1;
                if (IsWndGuot()) break;
                if (Read_RWZT(true) == 9) return -3;
                Point2f before{};
                if (!readPosition(before)) continue;
                wBenTuXuL33(before.x, before.y, direction);
                SleepEx(1200);
                if (m_control->IsStopping()) return -1;
                if (IsWndGuot()) break;
                if (readPosition(position) && distance(before, position) > 0.25f) {
                    adjusted = true;
                    break;
                }
            }
            if (IsWndGuot()) continue;
            if (!adjusted) {
                LogD(_T("route point stuck: (%.1f, %.1f)"), xx, yy);
                return -2;
            }
            progressPosition = position;
            lastProgress = GetTickCount64();
            movePending = true;
            // 下轮先判断八方调整是否已抵达终点。
            continue;
        }

        if (useMoveSkills && UseRouteMoveSkills(position, target)) {
            SleepEx(Mams[Menpaid] == 8 ? 1000 : 300);
            if (m_control->IsStopping()) return -1;
            if (IsWndGuot()) continue;
            if (Read_RWZT(true) == 9) return -3;
            // 瞬移/加速会打断寻路，等待后重新发送同一个终点。
            movePending = true;
        }
        if (movePending || (Read_RWZT(true) == 0 && GetTickCount64() - lastMove >= 1500)) {
            const int result = wBenTuXuL2(xx, yy);
            lastMove = GetTickCount64();
            movePending = (result != 0);
            if (movePending) LogD(_T("route point: move command failed"));
        }
        SleepEx(300);
    }
    return -1;
}

int CNearby::RunRouteInternal(const CString& lux, int fx, int lxdjg, int mode)
{
    if (!m_control || m_control->IsStopping()) return -1;

    std::vector<CString> route;
    SplitText(lux, route, _T("-"));
    if (route.size() < 3) {
        LogD(_T("route is too short: %s"), lux.GetString());
        return -1;
    }
    if (fx == 1) std::reverse(route.begin(), route.end());

    const CString mapIni = ResolveWorkIni(
        _T("\u5730\u56fe\u914d\u7f6e.txt"), tsg[TaskMemoryState::workdir]);
    const CString characterIni = ResolveWorkIni(
        _T("\u4eba\u7269\u914d\u7f6e.ini"), tsg[TaskMemoryState::workdir]);

    // lxdjg == -1 is requested by the forward/reverse route buttons.
    const bool locateCurrentMap = (lxdjg == -1);
    size_t currentRouteIndex = route.size();
    if (locateCurrentMap) {
        while (!m_control->IsStopping() && IsWndGuot()) SleepEx(300);
        if (m_control->IsStopping()) return -1;
        const int currentMapId = Read_DTID();
        CString mapKey;
        mapKey.Format(_T("%03d"), currentMapId);
        CString mapName = Dm()->ReadIni(
            _T("配置"), mapKey.GetString(), mapIni.GetString());
        mapName.Trim();
        for (size_t i = 0; !mapName.IsEmpty() && i < route.size(); ++i) {
            route[i].Trim();
            if (route[i] == mapName) {
                currentRouteIndex = i;
                break;
            }
        }
        if (currentRouteIndex < route.size()) {
            LogD(_T("route resume: map=%d name=%s direction=%s position=%u/%u"),
                 currentMapId, mapName.GetString(), fx == 1 ? _T("reverse") : _T("forward"),
                 static_cast<unsigned>(currentRouteIndex + 1), static_cast<unsigned>(route.size()));
        } else {
            LogD(_T("route resume: map=%d name=%s not in route, start from guild"),
                 currentMapId, mapName.GetString());
        }
    }

    if (!tsg[TaskMemoryState::m_Name].IsEmpty()) {
        Dm()->WriteIni(
            _T("PS"), tsg[TaskMemoryState::m_Name].GetString(),
            (fx == 1) ? _T("1") : _T("0"), characterIni.GetString());
    }

    const bool useRouteMoveSkills = kEnableRouteMoveSkills && (mode == 1 || mode == 3);
    if (useRouteMoveSkills) {
        LogD(_T("route move skills enabled: mode=%d menpai=%lu"),
             mode, static_cast<unsigned long>(Mams[Menpaid]));
    }
    const bool useInitialMove = locateCurrentMap
        ? (currentRouteIndex == route.size())
        : ((mode == 3) ? (fx == 0 && lxdjg == 0) : (lxdjg == 0));
    if (useInitialMove && route.size() >= 2) {
        const float startX = static_cast<float>((9850 + rand() % 250) / 100.0);
        const float startY = static_cast<float>((15600 + rand() % 300) / 100.0);
        const int result = wBenTuXuLPP(startX, startY, route[1], useRouteMoveSkills);
        if (mode == 3) {
            // The original wLianxXuLPP3 ignores the initial move result.
            if (result == -1 || result == -2) {
                LogD(_T("wLianxXuLPP3: ignored initial move result=%d destination=%s"),
                     result, route[1].GetString());
            } else if (result == -3) {
                return result;
            }
        } else {
            if (result == -2 || result == -3) return result;
            if (result == -1) return -1;
        }
    } else if (!locateCurrentMap && lxdjg != 0) {
        lxdjg -= 100;
        if (lxdjg < 0) lxdjg = 0;
        if (mode == 3 && fx == 1) ++lxdjg;
    }

    const auto readCoordinates = [&](const CString& key,
                                     std::vector<CString>& coords) -> bool {
        const CString value = Dm()->ReadIni(
            _T("\u914d\u7f6e2"), key.GetString(), mapIni.GetString());
        if (value.IsEmpty()) {
            LogD(_T("route coordinate missing: %s"), key.GetString());
            return false;
        }
        SplitText(value, coords, _T("|"));
        return coords.size() >= 2;
    };

    const auto moveCoordinates = [&](const std::vector<CString>& coords,
                                     const CString& destination,
                                     bool finalPoint) -> int {
        if (coords.size() < 2) return -1;
        const float x = static_cast<float>(_wtof(coords[0].GetString()));
        const float y = static_cast<float>(_wtof(coords[1].GetString()));
        if (finalPoint) {
            const int result = wBenTuXuLPoint(x, y, useRouteMoveSkills);
            return result == 1 ? 0 : result;
        }
        if (mode == 2 && coords.size() >= 4) {
            const float nearX = static_cast<float>(_wtof(coords[2].GetString()));
            const float nearY = static_cast<float>(_wtof(coords[3].GetString()));
            const int result = wBenTuXuLPP(nearX, nearY, destination, useRouteMoveSkills);
            if (result == -2 || result == -3) return result;
            if (result == -1) return -1;
            wBenTuXuL3f(
                static_cast<float>(_wtof(coords[0].GetString())),
                static_cast<float>(_wtof(coords[1].GetString())));
            return 0;
        }

        return wBenTuXuLPP(x, y, destination, useRouteMoveSkills);
    };

    const bool shiftPairing = (mode == 3 && fx == 1);
    // After leaving the guild we are at route[1]. Reverse city routes pair
    // route[i] with route[i+1]; the other modes pair route[i+1] with route[i+2].
    const size_t resumeNode = currentRouteIndex < route.size() ? currentRouteIndex : 1;
    const size_t startIndex = locateCurrentMap
        ? (shiftPairing ? resumeNode : (resumeNode > 0 ? resumeNode - 1 : 0))
        : (lxdjg > 0 ? static_cast<size_t>(lxdjg) : 0);

    const auto handleMoveResult = [&](int result, const CString& key) -> bool {
        if (result == -3) return false;
        if (mode == 3) {
            // The original wLianxXuLPP3 ignores -1/-2 and continues.
            if (result == -1 || result == -2) {
                LogD(_T("wLianxXuLPP3: ignored move result=%d key=%s"),
                     result, key.GetString());
            }
            return true;
        }
        return result != -1 && result != -2;
    };

    if (shiftPairing) {
        for (size_t i = startIndex; i + 2 < route.size(); ++i) {
            if (m_control->IsStopping()) return -1;
            const CString key = route[i] + route[i + 1];
            std::vector<CString> coords;
            if (!readCoordinates(key, coords)) return -1;
            const int result = moveCoordinates(coords, route[i + 1], false);
            if (!handleMoveResult(result, key)) return result;
        }
    } else if (mode == 3) {
        for (size_t i = startIndex; i + 2 < route.size(); ++i) {
            if (m_control->IsStopping()) return -1;
            const CString key = route[i + 1] + route[i + 2];
            std::vector<CString> coords;
            if (!readCoordinates(key, coords)) return -1;
            const int result = moveCoordinates(coords, route[i + 2], false);
            if (!handleMoveResult(result, key)) return result;
        }
    } else {
        for (size_t i = startIndex; i + 3 < route.size(); ++i) {
            if (m_control->IsStopping()) return -1;
            const CString key = route[i + 1] + route[i + 2];
            std::vector<CString> coords;
            if (!readCoordinates(key, coords)) return -1;
            const int result = moveCoordinates(coords, route[i + 2], false);
            if (!handleMoveResult(result, key)) return result;
        }
    }

    if (mode == 3 && fx == 0) {
        const int mapNow = Read_DTID();
        Point2f city{};
        if (mapNow == 0) city = { 175.0f, 115.0f };
        else if (mapNow == 1) city = { 194.0f, 128.0f };
        else if (mapNow == 2) city = { 87.0f, 163.0f };
        else return 1;
        return wBenTuXuLPoint(city.x, city.y, useRouteMoveSkills);
    }

    const CString finalKey = route[route.size() - 2] + route[route.size() - 1];
    std::vector<CString> finalCoords;
    if (!readCoordinates(finalKey, finalCoords)) return -1;

    const int finalMove = moveCoordinates(finalCoords, route.back(), true);
    if (finalMove == -2 || finalMove == -3) return finalMove;
    if (finalMove == -1) return -1;

    const CString mapIdText = Dm()->ReadIni(
        _T("\u914d\u7f6e"), route[route.size() - 2].GetString(), mapIni.GetString());
    const int targetMap = mapIdText.IsEmpty() ? Read_DTID() : _wtoi(mapIdText.GetString());
    const DWORD npcIndex = (fx == 0) ? Mams[duibxh] : Mams[benbxh];

    bool npcWindowOpen = false;
    for (int attempt = 0; attempt < 8 && !m_control->IsStopping(); ++attempt) {
        wOpenMan(_T(".*\u6821\u5c09")); SleepEx(1000 + rand() % 500);
        if (IsWndOpen2(Quest_Frame) == 1) {
            npcWindowOpen = true;
            break;
        }
        //SleepEx(2000 + rand() % 1000);
    }
    if (!npcWindowOpen) {
        LogD(_T("npc window not open"));
        return -1;
    }

    wClickNpc(npcIndex);
    for (int index = 0; index < 15 && !m_control->IsStopping(); ++index) {
        SleepEx(1000);
        if (Read_DTID() != targetMap) break;
    }
    if (Read_DTID() == targetMap) {
        LogD(_T("map transition failed: %d"), targetMap);
        return -1;
    }

    const int pointResult = wBenTuXuLPoint(149.0f, 56.0f, useRouteMoveSkills);
    return pointResult == 1 ? 0 : pointResult;
}

int CNearby::wLianxXuLPP(CString lux, int fx, int lxdjg)
{
    return RunRouteInternal(lux, fx, lxdjg, 1);
}

int CNearby::wLianxXuLPP2(CString lux, int fx, int lxdjg)
{
    return RunRouteInternal(lux, fx, lxdjg, 2);
}

int CNearby::wLianxXuLPP3(CString lux, int fx, int lxdjg)
{
    return RunRouteInternal(lux, fx, lxdjg, 3);
}

int CNearby::FUN_PS6()
{
    return RunMerchantInternal(true);
}

int CNearby::FUN_PS7()
{
    return RunMerchantInternal(false);
}

int CNearby::FUN_CS2()
{
    if (!m_control || m_control->IsStopping() || !m_dm ||
        !m_ready.load() || !m_wnd || !m_wnd->hWnd) return -1;

    const int mapId = Read_DTID();
    Point2f npcPoint{};
    CString npcName;
    if (mapId == 0) { npcPoint = { 95.0f, 152.0f }; npcName = _T("何生财"); }
    else if (mapId == 1) { npcPoint = { 90.0f, 177.5f }; npcName = _T("张进宝"); }
    else if (mapId == 2) { npcPoint = { 207.0f, 172.5f }; npcName = _T("何生金"); }
    else if (mapId == 246) { npcPoint = { 219.0f, 130.0f }; npcName = _T("祝三思"); }
    else {
        LogD(_T("FUN_CS2 当前地图无出售NPC: %d"), mapId);
        return -1;
    }

    // 原版 mOpen_Npc2：跑到 NPC，直接打开普通商店，等待 268 号商店窗口。
    bool shopOpen = false;
    while (!m_control->IsStopping() && !shopOpen) {
        wBenTuXuL3f(npcPoint.x, npcPoint.y);
        if (m_control->IsStopping()) return -1;
        wOpenMan(npcName);
        for (int attempt = 0; attempt < 10 && !m_control->IsStopping(); ++attempt) {
            if (IsWndOpen2(Shop_Frame) == 1) {
                shopOpen = true;
                break;
            }
            SleepEx(1000);
        }
    }
    if (!shopOpen) return -1;
    SleepEx(500 + rand() % 500);

    tsg[TaskMemoryState::wpcs] =
        _T("宝石雕琢符1级宝石雕琢符3级|功力丹|寒冰星屑|精铁碎片|缥缈玄符|玄天寒玉|丝线1级丝线2级丝线3级");

    int sold = 0;
    for (int slot = 0; slot < 200 && !m_control->IsStopping(); ++slot) {
        const int action = PAND_WPLX2(slot);
        if (action == -100) continue;
        CString nameExpression;
        nameExpression.Format(_T("[[[[[%08x]+22C00]+%08x]+2C]+18]"),
                              static_cast<DWORD>(jz[TaskMemoryState::BagJ]),
                              static_cast<DWORD>(slot) * 4);
        const CString itemName = Dm()->ReadString(mmhwnd, nameExpression, 0, 0);
        LogD(_T("判断 名称%s  序号%d  类型%d"),
             itemName.GetString(), slot, action);
        if (action == 3 ) {
            wLyChu(slot);
            ++sold;
            SleepEx(500 + rand() % 500);
        }
    }

    LogD(_T("FUN_CS2 出售数量=%d"), sold);
    return m_control->IsStopping() ? -1 : 0;
}

int CNearby::RunMerchantInternal(bool cityRoute)
{
    if (!m_control || m_control->IsStopping() || !Dm() || !m_wnd || !m_wnd->hWnd) return -1;

    const DWORD merchantRank = mRead_BangZw();
    if (merchantRank <= 3) {
        LogD(_T("商人职位不足: %lu"), merchantRank);
        return 0;
    }
    const DWORD initialTime = cGetTime();
    if (initialTime == MAXDWORD) {
        LogD(_T("获取当前时辰失败"));
        return -1;
    }
    LogD(_T("跑商职位=%lu 时辰=%lu"), merchantRank, initialTime);

    std::vector<CString> route;
    std::vector<CString> goods;
    SplitText(tsg[TaskMemoryState::luxi], route, _T("-"));
    SplitText(tsg[TaskMemoryState::mmsp], goods, _T("|"));
    if (route.size() < 3 || goods.size() < 2 || Mams[BagMax2] == 0) {
        LogD(_T("跑商配置缺少路线、商品或背包容量"));
        return -1;
    }

    const CString characterIni = ResolveWorkIni(_T("人物配置.ini"), tsg[TaskMemoryState::workdir]);
    const CString mapIni = ResolveWorkIni(_T("地图配置.txt"), tsg[TaskMemoryState::workdir]);
    int farMap = -1;
    for (auto it = route.rbegin(); it != route.rend(); ++it) {
        const CString mapId = Dm()->ReadIni(_T("配置"), it->GetString(), mapIni.GetString());
        if (mapId.IsEmpty()) continue;
        const int id = _ttoi(mapId.GetString());
        if (id >= 0 && id <= 2) { farMap = id; break; }
    }
    if (cityRoute && farMap == -1) {
        LogD(_T("跑商路线缺少终点城市"));
        return -1;
    }

    const auto stopped = [this] { return m_control->IsStopping(); };
    const auto ticket = [this] {
        return Read_BagN3(_T("银票"), 2, 1).xuh != static_cast<DWORD>(-1);
    };
    const auto ticketMoney = [this]() -> DWORD {
        const BagItem item = Read_BagN3(_T("银票"), 2, 1);
        if (item.xuh == static_cast<DWORD>(-1)) return 0;
        CString expression;
        expression.Format(_T("[[[[%08lX]+22C00]+%08lX]+14]+4C"),
                          static_cast<DWORD>(jz[TaskMemoryState::BagJ]), item.xuh * 4);
        return static_cast<DWORD>(Dm()->ReadInt(mmhwnd, expression, 4));
    };
    const auto hasGoods = [this]() -> bool {
        for (DWORD slot = 0; slot < Mams[BagMax2]; ++slot) {
            CString expression;
            expression.Format(_T("[[[[[%08lX]+22C00]+%08lX]+2C]+50]"),
                              static_cast<DWORD>(jz[TaskMemoryState::BagJ]), (slot + 100) * 4);
            if (Dm()->ReadString(mmhwnd, expression, 0, 0) == _T("商品")) return true;
        }
        return false;
    };
    const auto waitForNpc = [this, &stopped](bool cityMerchant = false) -> bool {
        for (int attempt = 0; attempt < 10 && !stopped(); ++attempt) {
            if (IsWndOpen2(Quest_Frame) == 1 ) return true;
            if (cityMerchant) {
                const int map = Read_DTID();
                Point2f position{};
                CString name;
                if (map == 0) { position = { 175.0f, 115.0f }; name = _T("乔致广"); }
                else if (map == 1) { position = { 194.0f, 128.0f }; name = _T("苏双"); }
                else if (map == 2) { position = { 87.0f, 163.0f }; name = _T("张世平"); }
                else return false;
                wBenTuXuL3f(position.x, position.y);
                wOpenMan(name);
            } else {
                wBenTuXuL3f(149.0f, 56.0f);
                wOpenMan(_T("钱为一"));
            }
            SleepEx(1000);
        }
        return false;
    };
    const auto waitForShop = [this, &stopped, &waitForNpc](int npcOption,
                                                           bool cityMerchant) -> bool {
        if (!waitForNpc(cityMerchant)) return false;
        for (int attempt = 0; attempt < 12 && !stopped(); ++attempt) {
            if (IsWndOpen2(CityShop_Frame) == 1) return true;
            wClickNpc(npcOption);
            SleepEx(500);
        }
        return false;
    };
    const auto tradePhase = [this]() -> int {
        const DWORD time = cGetTime();
        return time == MAXDWORD ? 0 : static_cast<int>(time % 3) + 1;
    };
    const auto waitForSaleTime = [this, &stopped, &tradePhase]() -> bool {
        const DWORD started = GetTickCount();
        while (!stopped() && GetTickCount() - started <= 350000) {
            if (tradePhase() == 2) return true;
            SleepEx(2000);
        }
        LogD(_T("等待跑商出售时辰超时"));
        return false;
    };
    const auto shopItem = [this](const CString& name, DWORD& price, DWORD& stock) -> bool {
        const DWORD shop = static_cast<DWORD>(jz[TaskMemoryState::Paos]);
        if (shop < 0x10000) return false;
        for (DWORD slot = 0; slot < 24; ++slot) {
            CString expression;
            expression.Format(_T("[%08lX]+%08lX"), shop + 304, slot * 4);
            if (Dm()->ReadInt(mmhwnd, expression, 4) == 0) break;
            expression.Format(_T("[[[[[%08lX]+%08lX]+C]+2C]+18]"), shop + 304, slot * 4);
            const CString currentName = Dm()->ReadString(mmhwnd, expression, 0, 0);
            if (currentName != name) continue;
            expression.Format(_T("[[%08lX]+%08lX]+4"), shop + 304, slot * 4);
            price = static_cast<DWORD>(Dm()->ReadInt(mmhwnd, expression, 4));
            expression.Format(_T("[[[%08lX]+%08lX]+C]+20"), shop + 304, slot * 4);
            stock = static_cast<DWORD>(Dm()->ReadInt(mmhwnd, expression, 4));
            return price != 0;
        }
        return false;
    };

    // The old service-side mutex is intentionally absent. Only local trade
    // operations share this lock; travel and waiting can run independently.
    static std::mutex tradeMutex;
    const auto buy = [this, cityRoute, &stopped, &waitForShop, &shopItem,
                      &ticketMoney, &characterIni](const CString& name, int direction) -> bool {
        std::lock_guard<std::mutex> lock(tradeMutex);
        const bool cityMerchant = cityRoute && direction == 1;
        if (!waitForShop(cityMerchant ? 2 : (direction == 0 ? 4 : 1),
                         cityMerchant)) return false;
        Hk_Paos(1);
        bool purchased = false;
        int failures = 0;
        for (int attempt = 0; attempt < 200 && !stopped(); ++attempt) {
            DWORD price = 0, stock = 0;
            if (cityMerchant) {
                price = 3800;
                stock = 1;
            } else if (!shopItem(name, price, stock)) {
                break;
            }
            const DWORD money = ticketMoney();
            if (price > money || stock == 0) break;
            if (cityMerchant) wLyGou();
            else wPsGou(name);
            SleepEx(400);
            if (ticketMoney() < money) {
                purchased = true;
                failures = 0;
            } else if (++failures >= 8) {
                break;
            }
        }
        Hk_Paos(0);
        if (IsWndOpen2(CityShop_Frame) == 1) mLua_Dostr(SHANFH);
        if (!purchased || stopped()) return false;
        Dm()->WriteIni(_T("PS"), tsg[TaskMemoryState::m_Name].GetString(),
                       direction == 0 ? _T("0") : _T("1"), characterIni.GetString());
        return true;
    };
    const auto sell = [this, cityRoute, &stopped, &waitForShop, &hasGoods,
                       &ticketMoney, &waitForSaleTime, &goods](int direction) -> bool {
        if ((!cityRoute || direction == 0) && !waitForSaleTime()) return false;
        std::lock_guard<std::mutex> lock(tradeMutex);
        const bool cityMerchant = cityRoute && direction == 0;
        if (!waitForShop(cityMerchant ? 2 : (direction == 0 ? 1 : 4),
                         cityMerchant)) return false;
        if (!hasGoods()) return false;
        const BagItem merchandise = Read_BagN3(goods[direction], 1, 1);
        if (merchandise.xuh == static_cast<DWORD>(-1)) return false;
        if (!cityRoute || direction == 0) {
            CString expression;
            expression.Format(_T("[[[[%08lX]+22C00]+%08lX]+14]+50"),
                              static_cast<DWORD>(jz[TaskMemoryState::BagJ]), merchandise.xuh * 4);
            const DWORD salePrice = static_cast<DWORD>(Dm()->ReadInt(mmhwnd, expression, 4));
            expression.Format(_T("[[[[%08lX]+22C00]+%08lX]+14]+4C"),
                              static_cast<DWORD>(jz[TaskMemoryState::BagJ]), merchandise.xuh * 4);
            const DWORD purchasePrice = static_cast<DWORD>(Dm()->ReadInt(mmhwnd, expression, 4));
            const double minRatio = cityMerchant ? 2.7 : 3.5;
            if (purchasePrice == 0 ||
                static_cast<double>(salePrice) / purchasePrice < minRatio) {
                LogD(_T("商品出售价格过低: %lu/%lu"), salePrice, purchasePrice);
                return false;
            }
        }
        const DWORD started = GetTickCount();
        while (!stopped() && hasGoods()) {
            bool sold = false;
            for (DWORD slot = 0; slot < Mams[BagMax2] && !stopped(); ++slot) {
                CString expression;
                expression.Format(_T("[[[[[%08lX]+22C00]+%08lX]+2C]+50]"),
                                  static_cast<DWORD>(jz[TaskMemoryState::BagJ]), (slot + 100) * 4);
                if (Dm()->ReadString(mmhwnd, expression, 0, 0) != _T("商品")) continue;
                const DWORD money = ticketMoney();
                if (cityMerchant) wLyChu(slot + 100);
                else wPsChu(slot + 100);
                SleepEx(cityMerchant ? 50 : 350);
                sold = ticketMoney() > money || sold;
            }
            if (!sold && GetTickCount() - started > 10000) {
                LogD(_T("出售商品后银票金额未变化"));
                return false;
            }
        }
        if (IsWndOpen2(CityShop_Frame) == 1) mLua_Dostr(SHANFH);
        return !stopped();
    };
    const auto travel = [this, cityRoute](int direction, int resume) -> int {
        return cityRoute
            ? wLianxXuLPP3(tsg[TaskMemoryState::luxi], direction, resume)
            : wLianxXuLPP(tsg[TaskMemoryState::luxi], direction, resume);
    };

    bool claimedTicket = false;
    if (!ticket()) {
        const int map = Read_DTID();
        if (map == 0 || map == 1 || map == 2 || map == 246) {
            CString key, coordinates;
            key.Format(_T("%d"), map);
            coordinates = Dm()->ReadIni(_T("JRBP"), key.GetString(), characterIni.GetString());
            std::vector<CString> parts;
            SplitText(coordinates, parts, _T(","));
            if (parts.size() != 2) return -1;
            wBenTuXuL3f(static_cast<float>(_ttof(parts[0])),
                           static_cast<float>(_ttof(parts[1])));
            if (stopped()) return -1;
            wOpenMan(_T("范纯.*"));
            wClickNpc(map == 0 ? 7 : 4);
            for (int attempt = 0; attempt < 20 && IsWndGuot() == 1 && !stopped(); ++attempt) SleepEx(1000);
        }
        if (!waitForNpc()) return -1;
        DWORD emptySlots = 0;
        for (DWORD slot = 0; slot < Mams[BagMax2]; ++slot) {
            CString expression;
            expression.Format(_T("[[%08lX]+22C00]+%08lX"),
                              static_cast<DWORD>(jz[TaskMemoryState::BagJ]), 400 + slot * 4);
            if (Dm()->ReadInt(mmhwnd, expression, 4) == 0) ++emptySlots;
        }
        if (emptySlots < (cityRoute ? 52u : 20u)) {
            LogD(_T("背包空格不足: %lu"), emptySlots);
            return -1;
        }
        for (int attempt = 0; attempt < 10 && !stopped() && !ticket(); ++attempt) {
            wClickNpc(1);
            SleepEx(1000);
            const CString npcText = Read_NpcT();
            if (npcText.Find(_T("#{HYZ")) >= 0 && npcText.Right(2).Left(1) == _T("0")) {
                mLua_Dostr(CLOSE_NPC);
                LogD(_T("今日跑商次数已用完"));
                return 0x1001;
            }
        }
        if (!ticket()) return -1;
        claimedTicket = true;
    }

    const auto finish = [this, cityRoute, &stopped, &ticket, &ticketMoney,
                         &waitForNpc]() -> int {
        if (ticketMoney() < 50000) return -1;
        if (!waitForNpc() || stopped()) return -1;
        wClickNpc(2);
        SleepEx(1000);
        if (ticket()) {
            LogD(_T("交还银票后仍在背包中"));
            return -1;
        }
        return cityRoute ? 1 : 0;
    };

    const int map = Read_DTID();
    CString mapKey;
    mapKey.Format(_T("%03d"), map);
    const CString mapName = Dm()->ReadIni(_T("配置"), mapKey.GetString(), mapIni.GetString());
    size_t routeIndex = route.size();
    for (size_t index = 0; index < route.size(); ++index) {
        if (route[index] == mapName) { routeIndex = index; break; }
    }
    const CString directionText = Dm()->ReadIni(
        _T("PS"), tsg[TaskMemoryState::m_Name].GetString(), characterIni.GetString());
    const int direction = directionText == _T("0") ? 0 :
                          directionText == _T("1") ? 1 : -1;
    const bool atGuild = routeIndex == route.size() && map != 0 && map != 1 && map != 2;
    bool homeGuild = false;
    if (atGuild) {
        if (!waitForNpc()) return -1;
        homeGuild = Read_NpcT().Find(_T("在下负责")) >= 0;
    }
    bool atFarEnd = cityRoute ? map == farMap : atGuild && !homeGuild;
    bool atHome = claimedTicket || (atGuild && homeGuild);
    if (!claimedTicket && !atFarEnd && !atHome) {
        if (direction == -1) return -1;
        if (routeIndex == route.size()) {
            LogD(_T("当前位置不在跑商路线上: %s"), mapName.GetString());
            return -1;
        }
        const int resume = direction == 0
            ? static_cast<int>(routeIndex) + 99
            : static_cast<int>(route.size() - routeIndex) + 98;
        const int result = travel(direction, resume);
        if (result == -3) return 0x1002;
        if (result < 0 || stopped()) return -1;
        atFarEnd = direction == 0;
        atHome = direction == 1;
    }

    if (!claimedTicket && direction == 1 && atHome) {
        if (hasGoods() && !sell(1)) return -1;
        return finish();
    }

    bool firstLegDone = false;
    const bool secondBought = !claimedTicket && direction == 1 && atFarEnd;
    if (!claimedTicket && direction == 0 && atFarEnd) {
        if (hasGoods() && !sell(0)) return -1;
        firstLegDone = true;
    } else if (!claimedTicket && direction == 0 && atHome && hasGoods()) {
        const int result = travel(0, 0);
        if (result == -3) return 0x1002;
        if (result < 0 || stopped() || !sell(0)) return -1;
        firstLegDone = true;
    } else if (secondBought) {
        firstLegDone = true;
    }

    if (!firstLegDone) {
        if (!atHome) return -1;
        if (!buy(goods[0], 0)) return -1;
        const int result = travel(0, 0);
        if (result == -3) return 0x1002;
        if (result < 0 || stopped() || !sell(0)) return -1;
    }

    if (!secondBought && !buy(goods[1], 1)) return -1;
    const int returnResult = travel(1, 0);
    if (returnResult == -3) return 0x1002;
    if (returnResult < 0 || stopped() || !sell(1)) return -1;
    return finish();
}

int CNearby::RunAndAttk(DWORD jiid, DWORD moid)
{
    if (!m_control || m_control->IsStopping() || !m_ready.load() || !Dm() ||
        jiid == 0 || moid == 0 ||
        aMonst.Dibu == 0 || aMonst.cxue.IsEmpty() || Mams[Jijl] == 0) return -1;

    if (!WaitIfPaused()) return -1;
    for (int attempt = 0; attempt < 80 && !m_control->IsStopping(); ++attempt) {
        if (!WaitIfPaused() || !NerExist(aMonst.Dibu)) return -1;

        if (!UpdateMP(static_cast<DWORD>(jz[TaskMemoryState::jYcdiz]), 0x100)) return -1;
        const DWORD playerCopy = dwls;
        const Point2f player{
            *reinterpret_cast<float*>(static_cast<ULONG_PTR>(playerCopy) + 0x54),
            *reinterpret_cast<float*>(static_cast<ULONG_PTR>(playerCopy) + 0x5C)
        };
        if (!UpdateMP(aMonst.Dibu, 0x100)) return -1;
        const DWORD monsterCopy = dwls;
        const Point2f monster{
            *reinterpret_cast<float*>(static_cast<ULONG_PTR>(monsterCopy) + 0x54),
            *reinterpret_cast<float*>(static_cast<ULONG_PTR>(monsterCopy) + 0x5C)
        };
        if (!std::isfinite(player.x) || !std::isfinite(player.y) ||
            !std::isfinite(monster.x) || !std::isfinite(monster.y) ||
            monster.x <= 0.0f || monster.y <= 0.0f) return -1;

        const float distance = std::hypot(monster.x - player.x, monster.y - player.y);
        if (distance > 41.0f) return -1;
        if (distance < static_cast<float>(Mams[Jijl])) {
            if (!UpdateMP(aMonst.cxue, 0x100)) return -1;
            const float hp = *reinterpret_cast<float*>(static_cast<ULONG_PTR>(dwls) + 0x08);
            if (hp > 0.0f && !m_control->IsStopping() && WaitIfPaused()) {
                return wUseSk6(jiid, moid, true) ? 1 : 0;
            }
            return -1;
        }
        if (Read_RWZT() != 2) {
            if (wBenTuXuL2(monster.x, monster.y) != 0) return -1;
        }
        SleepEx(100 + rand() % 100);
    }
    return -1;
}

bool CNearby::ShaxActive()
{
    return m_control && !m_control->IsStopping() && m_ready.load() && Dm() && WaitIfPaused();
}

bool CNearby::ReadShaxPosition(DWORD objectSlot, Point2f& point)
{
    if (!objectSlot || !UpdateMP(objectSlot, 0x100) || !dwls) return false;
    point.x = *reinterpret_cast<const float*>(static_cast<ULONG_PTR>(dwls) + 0x54);
    point.y = *reinterpret_cast<const float*>(static_cast<ULONG_PTR>(dwls) + 0x5C);
    return std::isfinite(point.x) && std::isfinite(point.y);
}

bool CNearby::ReadShaxVitals(float& hp, float& mp)
{
    if (!UpdateMP(tsg[TaskMemoryState::Shuxtb], 0x100) || !dwls) return false;
    hp = *reinterpret_cast<const float*>(static_cast<ULONG_PTR>(dwls) + 0x08);
    mp = *reinterpret_cast<const float*>(static_cast<ULONG_PTR>(dwls) + 0x0C);
    return std::isfinite(hp) && std::isfinite(mp) && hp >= 0 && hp <= 1 && mp >= 0 && mp <= 1;
}

bool CNearby::ShaxReadHealth(DWORD objectSlot, float& hp)
{
    const DWORD object = Read_Dw(objectSlot);
    const DWORD component = object ? Read_Dw(object + 0x148) : 0;
    const DWORD attributes = component ? Read_Dw(component + 0x10) : 0;
    if (!attributes) return false;
    hp = Dm()->ReadFloatAddr(mmhwnd, attributes + 0x08);
    return std::isfinite(hp) && hp >= 0.0f && hp <= 1.0f;
}

void CNearby::ShaxMaintain()
{
    float hp = 0, mp = 0;
    if (!ShaxActive() || !ReadShaxVitals(hp, mp) || hp <= 0) return;
    const DWORD now = GetTickCount();
    if (m_shaxUseHealthItem && hp < 0.5f && now - m_shaxItemTick >= 1000) {
        bag[baxue] = Read_BagX(1);
        wUseBag2(1);
        m_shaxItemTick = now;
    }
    if (m_shaxPetRest || Mams[CoXuHa] == MAXDWORD) return;
    if (mp < 0.5f && m_hasXueJi && m_xjSkillId &&
        (!m_shaxXuejiTick || now - m_shaxXuejiTick >= 126000)) {
        if (Pet_Xueup(0.9f) < 0 || !ShaxActive()) return;
        if (wUseSk6cw(m_xjSkillId)) {
            m_shaxXuejiTick = GetTickCount();
            SleepEx(500);
            Pet_Xueup(0.9f);
        }
    } else {
        Pet_Xueup(0.7f);
    }
}

int CNearby::EmeiJia()
{
    if (!ShaxActive() || Mams[Menpaid] != 4) return 0;
    float selfHp = 0, mana = 0;
    Point2f self{};
    if (!ReadShaxVitals(selfHp, mana) || selfHp <= 0 ||
        !ReadShaxPosition(static_cast<DWORD>(jz[TaskMemoryState::jYcdiz]), self)) return 0;
    if (mana < 0.09f) { ShaxMaintain(); return 0; }

    struct HealTarget { DWORD slot, id, maxHp; float hp; };
    std::vector<HealTarget> targets;
    std::vector<std::pair<DWORD, DWORD>> team;
    const int count = Read_TeamC();
    if (count < 1 || count > 6) return 0;
    for (int i = 0; i < count; ++i) team.emplace_back(Read_DuiwId(i), Read_Dmax(i));
    // NerbAr 只改 varrou，不覆盖 Fub_Shaxg 已选出的 vMonst。
    NerbAr(Read_Dw(tsg[TaskMemoryState::ERC]));
    const auto nearby = varrou;
    for (DWORD slot : nearby) {
        if (!ShaxActive()) return 0;
        if (!UpdateMP(slot, 0x200)) continue;
        const DWORD id = *reinterpret_cast<const DWORD*>(static_cast<ULONG_PTR>(dwls) + 0x2C);
        const auto member = std::find_if(team.begin(), team.end(), [id](const auto& item) {
            return item.first != 0 && item.first != MAXDWORD && item.first == id;
        });
        if (member == team.end()) continue;
        Point2f point{};
        if (!ReadShaxPosition(slot, point) || std::hypot(point.x - self.x, point.y - self.y) >= 15) continue;
        float hp = 0;
        if (ShaxReadHealth(slot, hp) && hp > 0 && hp < 0.75f)
            targets.push_back({ slot, id, member->second, hp });
    }
    std::stable_sort(targets.begin(), targets.end(), [](const auto& a, const auto& b) { return a.hp < b.hp; });
    int healed = 0;
    for (const auto& target : targets) {
        if (!ShaxActive()) break;
        m_emeiHealSkill = m_emeiQingxin && target.maxHp >= 30000 ? 0x1A8 : 0x197;
        const DWORD times = target.hp < 0.4f ? 3 + rand() % 2 : 1 + rand() % 2;
        healed += EmeiJiaxd(target.slot, target.id, times);
    }
    ShaxMaintain();
    return healed;
}

int CNearby::EmeiJiaxd(DWORD diz, DWORD did, DWORD jxcs)
{
    if (!ShaxActive() || !diz || !did || did == MAXDWORD) return 0;
    float hp = 0, mp = 0;
    if (ReadShaxVitals(hp, mp) && hp > 0 && hp < 0.4f) {
        if (!m_shaxInvisibleTick || GetTickCount() - m_shaxInvisibleTick > 32000) {
            if (wUseSk6(0xF8, MAXDWORD)) {
                m_shaxInvisibleTick = GetTickCount();
                SleepEx(2500 + rand() % 500);
            }
        } else if (hp < 0.2f && aMonst.mname != _T("花容")) RunCircle(3);
    }
    int healed = 0;
    for (DWORD i = 0; i < jxcs && ShaxActive(); ++i) {
        if (Read_RWZT(true) == 9 || IsWndGuot() || !NerExist(diz) ||
            !ShaxReadHealth(diz, hp) || hp <= 0 || hp >= 0.8f) break;
        if (m_worldMessageFlag && Dm()->ReadIntAddr(mmhwnd, m_worldMessageFlag, 0) == 1) break;
        if (!wUseSk6(m_emeiHealSkill, did)) { SleepEx(300); continue; }
        ++healed;
        if (m_emeiHealSkill == 0x1A8) SleepEx(2700 + rand() % 500);
        else {
            int waits = 0;
            for (; waits < 8 && ShaxActive(); ++waits) {
                SleepEx(1000);
                if (Read_RWZT() != 5) break;
            }
            if (waits == 8 && ShaxActive()) RunCircle(3);
            SleepEx(1200);
        }
    }
    return healed;
}

Point2f CNearby::Get_Minp(std::vector<Point2f> traps, Point2f monster, Point2f player, bool evadeOnly)
{
    const Point2f invalid{ -1.0f, -1.0f };
    constexpr float radiusSquared = 5.0f * 5.0f;
    const float range = static_cast<float>(Mams[Jijl]);
    const auto valid = [](Point2f p) {
        return std::isfinite(p.x) && std::isfinite(p.y) &&
               p.x >= 0.0f && p.y >= 0.0f && p.x < 80.0f && p.y < 80.0f;
    };
    const auto distanceSquared = [](Point2f a, Point2f b) {
        const float x = a.x - b.x, y = a.y - b.y;
        return x * x + y * y;
    };
    if (!ShaxActive() || !valid(monster) || !valid(player) || range <= 0 || range > 80)
        return invalid;
    for (const auto& trap : traps) {
        if (!valid(trap)) return invalid;
        // 近战 BOSS 在阵内时不找攻击点；调用方另用 evadeOnly 找安全待机点。
        if (!evadeOnly && range < 5.0f && distanceSquared(monster, trap) <= radiusSquared) return invalid;
    }
    const auto pointSafe = [&](Point2f p) {
        return std::none_of(traps.begin(), traps.end(), [&](Point2f trap) {
            return distanceSquared(p, trap) <= radiusSquared;
        });
    };
    const auto straightSafe = [&](Point2f start, Point2f end, bool allowExit) {
        const float dx = end.x - start.x, dy = end.y - start.y;
        const float lengthSquared = dx * dx + dy * dy;
        for (const auto& trap : traps) {
            if (distanceSquared(end, trap) <= radiusSquared) return false;
            const float sx = start.x - trap.x, sy = start.y - trap.y;
            const float outward = sx * dx + sy * dy;
            if (distanceSquared(start, trap) <= radiusSquared) {
                // 已踩阵时允许沿远离圆心的直线退出，不能先深入阵内再出去。
                if (!allowExit || outward < 0.0f) return false;
            } else {
                const float t = lengthSquared > 0.0f
                    ? (std::max)(0.0f, (std::min)(1.0f, -outward / lengthSquared)) : 0.0f;
                const Point2f nearest{ start.x + t * dx, start.y + t * dy };
                if (distanceSquared(nearest, trap) <= radiusSquared) return false;
            }
        }
        return true;
    };

    // 近战即使继续贴身，站位到 BOSS 的整段也不能穿阵；远程在射程内直接攻击，
    // 不把弹道当行走路线，否则 BOSS 在阵内时远程也会被错误禁攻。
    const auto attackApproachSafe = [&](Point2f p) {
        return evadeOnly || range >= 5.0f || straightSafe(p, monster, false);
    };
    const float attackRange = range - 0.2f;
    if (pointSafe(player) &&
        (evadeOnly || distanceSquared(player, monster) <= attackRange * attackRange) &&
        attackApproachSafe(player)) return player;

    Point2f result = invalid;
    float best = (std::numeric_limits<float>::max)();
    // 攻击点留 0.2 射程余量；仅避险时取消射程限制，找离自己最近的安全待机点。
    // 两种模式都校验接近站位的路线；近战额外检查贴身路线，按 0..80、0.1 网格搜索。
    const int minX = evadeOnly ? 0 : (std::max)(0, static_cast<int>(std::ceil((monster.x - attackRange) * 10)));
    const int maxX = evadeOnly ? 799 : (std::min)(799, static_cast<int>(std::floor((monster.x + attackRange) * 10)));
    const int minY = evadeOnly ? 0 : (std::max)(0, static_cast<int>(std::ceil((monster.y - attackRange) * 10)));
    const int maxY = evadeOnly ? 799 : (std::min)(799, static_cast<int>(std::floor((monster.y + attackRange) * 10)));
    for (int y = minY; y <= maxY; ++y) {
        if (!ShaxActive()) return invalid;
        for (int x = minX; x <= maxX; ++x) {
            const Point2f p{ x / 10.0f, y / 10.0f };
            if (!evadeOnly && distanceSquared(p, monster) > attackRange * attackRange) continue;
            const float distance = distanceSquared(p, player);
            if (distance >= best || !straightSafe(player, p, true) || !attackApproachSafe(p)) continue;
            best = distance;
            result = p;
        }
    }
    return result;
}

void CNearby::ShaxUseSkills()
{
    float hp = 0, mp = 0, targetHp = 0;
    const DWORD now = GetTickCount();
    if (Vkill.empty() || now - m_shaxSkillTick < 1500 ||
        !ReadShaxVitals(hp, mp) || mp < 0.1f ||
        !ShaxReadHealth(aMonst.Dibu, targetHp) || targetHp <= 0.05f) return;
    for (auto& skill : Vkill) {
        const DWORD id = static_cast<DWORD>(std::get<0>(skill));
        const DWORD kind = std::get<3>(skill);
        const int cooldown = std::get<1>(skill);
        const DWORD delay = cooldown > 0 ? static_cast<DWORD>(cooldown) * 1000 : 0;
        if (!id || (std::get<2>(skill) && now - std::get<2>(skill) < delay)) continue;
        // 加血单独由 EmeiJia 调度，避免把治疗技能放到首领身上。
        if (id == 0x197 || id == 0x1A8) continue;
        bool sent = false;
        if (kind == L_dt || kind == L_nq) sent = wUseSk6(id, static_cast<DWORD>(aMonst.ID));
        else if (kind == L_fw || kind == L_cx) sent = wUseSk6(id, MAXDWORD);
        else if (kind == L_dd) {
            Point2f p{};
            if (ReadShaxPosition(aMonst.Dibu, p)) sent = wUseSk6(id, p);
        }
        if (!sent) continue;
        std::get<2>(skill) = m_shaxSkillTick = GetTickCount();
        SleepEx(300 + rand() % 200);
        if (kind == L_cx) {
            const DWORD started = GetTickCount();
            while (ShaxActive() && Read_RWZT() == 6 && GetTickCount() - started < 10000) SleepEx(300);
        }
        break;
    }
}

int CNearby::KillMonstShax()
{
    return KillMonstShaxInternal(0, 0, 0, {}, false);
}

int CNearby::KillMonstShax(DWORD dglx, DWORD dgsj, DWORD xjsj, Point2f ptdd)
{
    return KillMonstShaxInternal(dglx, dgsj, xjsj, ptdd, true);
}

int CNearby::KillMonstShaxInternal(DWORD dglx, DWORD dgsj, DWORD xjsj, Point2f ptdd, bool boss)
{
    DWORD attempts = 0, sampleTick = GetTickCount(), maintainTick = 0, healTick = 0;
    float lastHp = -1, sampleHp = -1;
    const bool huaRong = boss && dglx == 0x104;
    DWORD trapScanTick = 0, trapMoveTick = 0, trapAttackTick = 0;
    bool trapScanValid = false, trapHolding = false, trapMoving = false;
    bool wasBossInTrap = false;
    Point2f trapDestination{};
    std::vector<Point2f> traps;
    const auto holdForTrap = [&](Point2f self) {
        const int state = Read_RWZT();
        if (!trapHolding || ((state == 2 || state == 5 || state == 6 || state == 7) &&
                             GetTickCount() - trapMoveTick >= 500)) {
            wXuanZg(MAXDWORD);//取消已选目标，阻止旧的普攻继续追怪。
            wBenTuXuL2(self.x, self.y);//只发自身坐标，绝不把 (-1,-1) 当移动点。
            trapMoveTick = GetTickCount();
        }
        trapHolding = true;
        trapMoving = false;
    };
    if (!aMonst.Dibu || !aMonst.ID || !Mams[JiGj]) return M_nomt;
    while (ShaxActive()) {
        if (Read_RWZT(true) == 9) return M_dead;
        if (IsWndGuot() || Read_DTID() != 0x222) return M_nomt;
        if (ReadWorldMessage(tsg[TaskMemoryState::hkwords])) return M_hmsg;
        if (Read_GENS()) { SleepEx(1000); continue; }
        if (!NerExist(aMonst.Dibu)) return M_nomt;
        float monsterHp = 0;
        if (!ShaxReadHealth(aMonst.Dibu, monsterHp)) { SleepEx(300); continue; }
        if (monsterHp <= 0) return M_wkil;
        const bool trapGuard = huaRong && xjsj && GetTickCount() - xjsj < kHuaRongTrapWindowMs;

        float hp = 0, mp = 0;
        if (ReadShaxVitals(hp, mp)) {
            bool falling = lastHp >= 0 && lastHp - hp > 0.3f;
            lastHp = hp;
            if (!boss && GetTickCount() - sampleTick > 2000) {
                falling = falling || (sampleHp >= 0 && sampleHp - hp > 0.3f);
                sampleHp = hp; sampleTick = GetTickCount();
            }
            if (falling && !trapGuard) { Yins(); SleepEx(2000); continue; }
            // 花容陷阱判断期间统一走下面的安全点逻辑，禁止随机跑圈穿阵。
            if (boss && !trapGuard && hp < 0.3f && hp > 0 && Read_Ziji() != 0 && monsterHp >= 0.25f &&
                Read_Dw(tsg[TaskMemoryState::ZJID]) == static_cast<DWORD>(Read_ATID(aMonst.Dibu))) {
                RunCircle(4);
                continue;
            }
        }
        if (!boss && Read_RWZTC2(0x1111, 18) == 18) { SleepEx(3000); continue; }

        if (dglx == 0x101 || dglx == 0x105) {
            Point2f self{}, target{};
            if (!ReadShaxPosition(static_cast<DWORD>(jz[TaskMemoryState::jYcdiz]), self)) { SleepEx(300); continue; }
            if (std::hypot(self.x - ptdd.x, self.y - ptdd.y) > 1) wBenTuXuL3z(ptdd.x, ptdd.y);
            if (!ReadShaxPosition(aMonst.Dibu, target)) { SleepEx(300); continue; }
            ReadShaxPosition(static_cast<DWORD>(jz[TaskMemoryState::jYcdiz]), self);
            if (std::hypot(self.x - target.x, self.y - target.y) > Mams[Jijl]) { SleepEx(1000); continue; }
        }
        bool trapAttackReady = !trapGuard;
        if (trapGuard) {
            Point2f self{}, target{};
            if (!ReadShaxPosition(static_cast<DWORD>(jz[TaskMemoryState::jYcdiz]), self)) {
                wXuanZg(MAXDWORD);
                SleepEx(100);
                continue;
            }
            // 喊话后立即判断，持续25秒；当前安全就攻击，不等待倒计时结束。
            if (!trapScanValid || GetTickCount() - trapScanTick >= 500) {
                const int count = NerbXian(Read_Dw(tsg[TaskMemoryState::ERC]),
                                           _T("天罡阵|地煞阵|人亡阵"));
                trapScanTick = GetTickCount();
                trapScanValid = count >= 0;
                traps.clear();
                if (trapScanValid) for (const auto& trap : vMonst) traps.push_back(trap.pos);
            }
            if (!trapScanValid || !ReadShaxPosition(aMonst.Dibu, target)) {
                holdForTrap(self);
            } else {
                // 枚举对象期间人物可能仍在移动，必须用最新起点校验整条线段。
                if (!ReadShaxPosition(static_cast<DWORD>(jz[TaskMemoryState::jYcdiz]), self)) {
                    wXuanZg(MAXDWORD);
                    SleepEx(100);
                    continue;
                }
                // target 每轮重新读取；BOSS 一旦离开所有陷阱就恢复寻找攻击点，不锁死停攻状态。
                const bool bossInTrap = Mams[Jijl] < 5 && std::any_of(traps.begin(), traps.end(), [&](Point2f trap) {
                    return std::hypot(target.x - trap.x, target.y - trap.y) <= 5.0f;
                });
                if (wasBossInTrap && !bossInTrap)
                    LogD(_T("花容：BOSS已走出陷阱，近战重新寻找安全攻击点"));
                wasBossInTrap = bossInTrap;
                bool retreatOnly = bossInTrap;
                Point2f safe = Get_Minp(traps, target, self, retreatOnly);
                if (!retreatOnly && Mams[Jijl] < 5 && (safe.x < 0.0f || safe.y < 0.0f)) {
                    // 没有近战攻击点也不能站在阵里：先退出所有陷阱，安全后继续停攻。
                    retreatOnly = true;
                    safe = Get_Minp(traps, target, self, true);
                }
                if (!ShaxActive()) break;
                if (safe.x < 0.0f || safe.y < 0.0f) {
                    if (!trapHolding) LogD(_T("花容：暂未找到直达安全点，停止攻击并重试"));
                    holdForTrap(self);
                } else if (safe.x != self.x || safe.y != self.y) {
                    // 只发送验证过的直线目标，不调用会绕路/追到 BOSS 的阻塞寻路。
                    if (!trapMoving || std::hypot(safe.x - trapDestination.x, safe.y - trapDestination.y) > 0.1f ||
                        GetTickCount() - trapMoveTick >= 500) {
                        wXuanZg(MAXDWORD);
                        if (wBenTuXuL2(safe.x, safe.y) == 0) {
                            if (!trapMoving) LogD(_T("花容：移动至%s (%.1f, %.1f)"),
                                retreatOnly ? _T("安全待机点（停攻）") : _T("安全攻击点"), safe.x, safe.y);
                            trapMoving = true;
                            trapDestination = safe;
                        }
                        trapMoveTick = GetTickCount();
                    }
                    trapHolding = false;
                } else if (retreatOnly) {
                    if (!trapHolding) LogD(_T("花容：近战已在安全位置，保持停攻"));
                    holdForTrap(self);
                } else if (trapMoving || Read_RWZT() == 2) {
                    holdForTrap(self);//近战/远程均先取消旧追怪、停稳，下一轮确认射程后再攻击。
                } else {
                    trapHolding = trapMoving = false;
                    trapAttackReady = true;
                }
            }
        }
        if (!ShaxActive()) break;
        if (trapAttackReady && Mams[Menpaid] == 4 && GetTickCount() - healTick >= 1000) {
            healTick = GetTickCount();
            if (EmeiJia() > 0) continue;
        }
        // 李魁近战、低等级鲁志生只监测血量，不发攻击；技能也受同一开关约束。
        const bool noAttack = (dglx == 0x101 && Mams[Jijl] < 5) || dglx == 0x102 ||
                              (Mams[Menpaid] == 4 && m_shaxOnlyHeal) || !trapAttackReady;
        if (trapGuard && !noAttack) {
            // 站位计算/治疗后 BOSS 仍可能移动。每次发普攻或技能前重读坐标，
            // 远程也必须已经在安全射程内，不能让攻击动作代替受控的接近过程。
            Point2f self{}, target{};
            if (!ReadShaxPosition(static_cast<DWORD>(jz[TaskMemoryState::jYcdiz]), self)) {
                wXuanZg(MAXDWORD); SleepEx(100); continue;
            }
            if (!ReadShaxPosition(aMonst.Dibu, target)) {
                holdForTrap(self); SleepEx(100); continue;
            }
            const Point2f ready = Get_Minp(traps, target, self);
            if (!ShaxActive()) break;
            if (ready.x != self.x || ready.y != self.y || Read_RWZT() == 2) {
                holdForTrap(self); SleepEx(100); continue;
            }
        }
        const int state = Read_RWZT();
        if (!noAttack && state != 7 && state != 5) {
            if (trapGuard) {
                // 已验证当前位置安全且在射程内，只发攻击，禁止 RunAndAttk 自动追入阵中。
                if (!trapAttackTick || GetTickCount() - trapAttackTick >= 1000) {
                    wXuanZg(static_cast<DWORD>(aMonst.ID));
                    wUseSk6(Mams[JiGj], static_cast<DWORD>(aMonst.ID), true);
                    trapAttackTick = GetTickCount();
                }
            } else {
                if (attempts >= 2) return M_nomt;
                wXuanZg2(static_cast<DWORD>(aMonst.ID));
                if (RunAndAttk(Mams[JiGj], static_cast<DWORD>(aMonst.ID)) != 0) ++attempts;
            }
            SleepEx(boss ? 100 : 1000);
        } else if (!noAttack && boss &&
                   (dglx != 0x103 || (dgsj && GetTickCount() - dgsj < 20000))) {
            ShaxUseSkills();
        }
        if (GetTickCount() - maintainTick >= 1000) {
            maintainTick = GetTickCount();
            ShaxMaintain();
        }
        SleepEx(boss ? 100 : 1000);
    }
    return 0;
}

void CNearby::KillMonsterBs(std::vector<Monst> vvm)
{
    for (const Monst& monster : vvm) {
        if (!m_control || m_control->IsStopping()) return;
        aMonst = monster;
        if (!KillMonsterBs()) return;
    }
}

bool CNearby::KillMonsterBs()
{
    if (!m_control || m_control->IsStopping() || !m_ready.load() || !Dm() ||
        aMonst.Dibu == 0 ||
        aMonst.ID == 0 || aMonst.cxue.IsEmpty() || Mams[JiGj] == 0) return false;

    const auto useConfiguredSkill = [this]() {
        if (aMonst.ID <= 0 || Vkill.empty()) return;

        const DWORD now = GetTickCount();
        for (TaskSkillEntry& skill : Vkill) {
            const DWORD skillId = static_cast<DWORD>(std::get<0>(skill));
            const int cooldown = std::get<1>(skill);
            const DWORD cooldownSeconds = cooldown > 0
                ? static_cast<DWORD>(cooldown) : 0;
            const DWORD lastCast = std::get<2>(skill);
            const DWORD skillKind = std::get<3>(skill);
            if (skillId == 0 ||
                (skillKind != L_dt && skillKind != L_nq) ||
                now - lastCast < cooldownSeconds * 1000) {
                continue;
            }

            if (!UpdateMP(aMonst.cxue, 0x100)) return;
            const float hp = *reinterpret_cast<float*>(
                static_cast<ULONG_PTR>(dwls) + 0x08);
            if (!std::isfinite(hp) || hp <= 0.0f) return;

            if (wUseSk6(skillId, static_cast<DWORD>(aMonst.ID))) {
                std::get<2>(skill) = GetTickCount();
            }
            return;
        }
    };

    DWORD attempts = 0;
    while (!m_control->IsStopping()) {
        if (!WaitIfPaused()) return false;
        if (Read_RWZT(true) == 9) return false;
        const int following = Read_GENS();
        const int transitioning = following == 1 ? 0 : IsWndGuot();
        if (following == 1 || transitioning == 1) {
            SleepEx(1000);
            continue;
        }
        if (!NerExist(aMonst.Dibu)) return true;
        if (!UpdateMP(aMonst.cxue, 0x100)) return false;
        const float hp = *reinterpret_cast<float*>(static_cast<ULONG_PTR>(dwls) + 0x08);
        if (!std::isfinite(hp)) return false;
        if (hp <= 0.0f) return true;

        if (Read_RWZT() == 0) {
            if (attempts >= 2) return true;
            wXuanZg2(static_cast<DWORD>(aMonst.ID));
            // 被控制跳过不消耗尝试次数；无效目标仍按原有次数退出。
            if (RunAndAttk(Mams[JiGj], static_cast<DWORD>(aMonst.ID)) != 0) ++attempts;
            SleepEx(1000);
            continue;
        }

        if (Mams[CoXuHa] != static_cast<DWORD>(-1)) {
            const int petResult = Pet_Xueup(0.7f);
            if (petResult < 0) return false;
        }
        useConfiguredSkill();
        SleepEx(kPacketIntervalMs);
    }
    return false;
}

int CNearby::KillMonst(CString nam, DWORD dtid, int rwxh, bool sfdd)
{
    if (!m_control || m_control->IsStopping() || nam.IsEmpty()) return -1;

    const ScopedActionStatuses actionStatuses(*this,
        { 213, 14, 18, 22, 12, 21 }, { 213, 14, 18, 22, 12, 21 }, true);
    const regex targetRegex = NerbCs(40, { 0, 0 }, nam);
    while (!m_control->IsStopping()) {
        if (!WaitIfPaused()) return -1;
        if (Read_GENS() == 1 || IsWndGuot() == 1 ||
            Read_DTID() != dtid || Read_RWZT(true) == 9) {
            SleepEx(1000);
            continue;
        }
        if (rwxh != -1 && mRenw_wc(static_cast<DWORD>(rwxh), 0) != 0) break;

        const int nearbyCount = NerbMonst2(
            Read_Dw(tsg[TaskMemoryState::ERC]), targetRegex, false);
        if (nearbyCount <= 0) {
            SleepEx(500);
            continue;
        }

        // sfdd is retained for source compatibility. The current movement
        // layer already validates the live player position before moving.
        (void)sfdd;
        for (const Monst& monster : vMonst) {
            if (m_control->IsStopping()) return -1;
            if (KillMonster(monster) == 0x1003) break;
        }
        SleepEx(200);
    }
    return m_control->IsStopping() ? -1 : 0;
}

int CNearby::KillMonster(Monst vvm1, int xdz)
{
    (void)xdz;
    if (!m_control || m_control->IsStopping()) return -1;
    aMonst = vvm1;
    if (!NerExist(aMonst.Dibu)) return 0x1003;
    return KillMonsterBs() ? 0 : 0x1004;
}

int CNearby::KillMonsterMz(Monst vvm1, std::vector<DWORD>& vddwid)
{
    // The current object model does not expose the old owner-field reader;
    // target ownership is still revalidated by KillMonsterBs/NerExist.
    (void)vddwid;
    return KillMonster(vvm1, 0);
}

void CNearby::RunCircle(int leix, float xuel)
{
    (void)xuel;
    if (!m_control || m_control->IsStopping()) return;

    Point2f center{};
    if (!UpdateMP(static_cast<DWORD>(jz[TaskMemoryState::jYcdiz]), 0x100)) return;
    center.x = *reinterpret_cast<float*>(static_cast<ULONG_PTR>(dwls) + 0x54);
    center.y = *reinterpret_cast<float*>(static_cast<ULONG_PTR>(dwls) + 0x5C);
    if (leix == 3) wUseSk6(0xF8, static_cast<DWORD>(-1));

    constexpr float kRadius = 5.0f;
    for (int index = 0; index < 8 && !m_control->IsStopping(); ++index) {
        const float angle = static_cast<float>(index) * 3.1415926535f / 4.0f;
        wBenTuXuL3z(center.x + kRadius * std::cos(angle),
                    center.y + kRadius * std::sin(angle));
        SleepEx(200);
    }
}

int CNearby::Yins()
{
    if (!m_control || m_control->IsStopping()) return -1;
    Point2f position{};
    if (UpdateMP(static_cast<DWORD>(jz[TaskMemoryState::jYcdiz]), 0x100)) {
        position.x = *reinterpret_cast<float*>(static_cast<ULONG_PTR>(dwls) + 0x54);
        position.y = *reinterpret_cast<float*>(static_cast<ULONG_PTR>(dwls) + 0x5C);
        wBenTuXuL2(position.x + 0.1f, position.y);
    }
    wUseSk6(0xF8, static_cast<DWORD>(-1));
    SleepEx(500);
    return m_control->IsStopping() ? -1 : 0;
}

void CNearby::RunTeam(int ilj)
{
    if (!m_control || m_control->IsStopping()) return;
    // ilj==2 是副本内的集合调用，不做 10 秒节流
    if (ilj != 2 && GetTickCount() - m_teamGatherTick < 10000) return;
    m_teamGatherTick = GetTickCount();

    const auto readSelf = [this](Point2f& p) {
        if (!UpdateMP(static_cast<DWORD>(jz[TaskMemoryState::jYcdiz]), 0x100) ||
            dwls < 0x10000) {
            return false;
        }
        p.x = *reinterpret_cast<const float*>(static_cast<ULONG_PTR>(dwls) + 0x54);
        p.y = *reinterpret_cast<const float*>(static_cast<ULONG_PTR>(dwls) + 0x5C);
        return std::isfinite(p.x) && std::isfinite(p.y) && p.x > 0.0f && p.y > 0.0f;
    };

    std::vector<Point2f> points;
    while (!m_control->IsStopping()) {
        if (!WaitIfPaused()) return;
        if (Read_GENS()) { SleepEx(1000); continue; }

        const int members = Read_TeamC();
        if (members <= 0) return;
        points.clear();
        for (int index = 0; index < members; ++index) {
            const Point2f mate = Read_DuiwZb(index);
            if (!std::isfinite(mate.x) || !std::isfinite(mate.y)) continue;
            if (mate.x == 0.0f && mate.y == 0.0f) continue;
            points.push_back(mate);
        }
        if (points.empty()) return;

        Point2f center{0.0f, 0.0f};
        for (const Point2f& pt : points) {
            center.x += pt.x;
            center.y += pt.y;
        }
        center.x /= static_cast<float>(points.size());
        center.y /= static_cast<float>(points.size());

        Point2f self{};
        float distance = -1.0f;
        if (readSelf(self)) {
            const float dx = center.x - self.x, dy = center.y - self.y;
            distance = std::sqrt(dx * dx + dy * dy);
            if (distance > 3.0f) wBenTuXuL3f(center.x, center.y);
        }
        LogD(_T("RunTeam: members=%d center=%.1f,%.1f dist=%.1f"),
             members, center.x, center.y, distance);

        // 队友基本都在附近（最多缺 2 个）就结束集合，参考 shul-bljs<3
        if (members - static_cast<int>(points.size()) < 3) break;
        SleepEx(200);
    }
}

bool CNearby::ReadGamePath(std::vector<Point2f>& path)
{
    path.clear();
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd) return false;

    const long targetHwnd = static_cast<long>(reinterpret_cast<INT_PTR>(m_wnd->hWnd));
    const LONGLONG game = m_dm->GetModuleBaseAddr(targetHwnd, _T("Game.exe"));
    if (game < 0x10000) return false;

    // 2026-09 客户端寻路链（PathLib.dll 的路径对象）：
    //   [Game+0x7C6A7C] → +0xD95C → +0xA4 → +4 = inner
    //   路径点数组在 inner+0x1C(begin)/inner+0x20(end)，每点 8 字节 float(x,z)。
    constexpr DWORD kPathManagerRva = 0x007C6A7C;
    const DWORD manager = Read_Dw(static_cast<DWORD>(game) + kPathManagerRva);
    if (manager < 0x10000) return false;
    const DWORD scene = Read_Dw(manager + 0xD95C);
    if (scene < 0x10000) return false;
    const DWORD map = Read_Dw(scene + 0xA4);
    if (map < 0x10000) return false;
    const DWORD inner = Read_Dw(map + 4);
    if (inner < 0x10000) return false;

    for (int attempt = 0; attempt < 3; ++attempt) {
        const DWORD begin = Read_Dw(inner + 0x1C);
        const DWORD end = Read_Dw(inner + 0x20);
        // 读的时候游戏可能正在改写数组：双读不一致就重试。
        if (begin != Read_Dw(inner + 0x1C) || end != Read_Dw(inner + 0x20)) continue;
        if (begin < 0x10000 || end < begin) return false;

        const DWORD count = (end - begin) / 8;
        if (count < 2 || count > 4096) return false;

        path.reserve(count);
        for (DWORD i = 0; i < count; ++i) {
            Point2f p{};
            p.x = m_dm->ReadFloatAddr(targetHwnd, static_cast<LONGLONG>(begin) + i * 8);
            p.y = m_dm->ReadFloatAddr(targetHwnd, static_cast<LONGLONG>(begin) + i * 8 + 4);
            if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
                path.clear();
                return false;
            }
            path.push_back(p);
        }
        return true;
    }
    return false;
}

bool CNearby::EnsureGamePath(const Point2f& target)
{
    (void)target;
    // 人物处于移动状态(2)时游戏才有活动路线；每次调用都重新读取并更新缓存。
    if (Read_RWZT() != 2) return m_pathReady;

    std::vector<Point2f> path;
    if (!ReadGamePath(path) || path.size() < 2) return m_pathReady;

    // 路线终点或点数变化时才打日志（读取很频繁，避免刷屏）。
    const DWORD now = GetTickCount();
    bool changed = path.size() != m_gamePath.size();
    if (!changed && !m_gamePath.empty()) {
        changed = path.back().x != m_gamePath.back().x ||
                  path.back().y != m_gamePath.back().y;
    }
    m_gamePath = std::move(path);
    m_pathReady = true;
    if (changed && now - m_pathLogTick >= 3000) {
        m_pathLogTick = now;
        LogD(_T("game path updated: %lu points, end=(%.1f, %.1f)"),
             static_cast<unsigned long>(m_gamePath.size()),
             m_gamePath.back().x, m_gamePath.back().y);
    }
    return m_pathReady;
}

bool CNearby::GetTeleportPointOnPath(const Point2f& current, float distance, Point2f& out) const
{
    if (!m_pathReady || m_gamePath.size() < 2 ||
        !std::isfinite(distance) || distance <= 0.0f) {
        return false;
    }

    // 路线点连成折线，以人物当前位置为圆心画半径 distance 的圆；
    // 从人物在折线上的最近投影位置沿路向前，取圆与折线的第一个交点。
    const float radius2 = distance * distance;

    // 1) 求人物到折线的最近投影（段索引 + 段内参数）。
    size_t bestSegment = 0;
    float bestT = 0.0f;
    float bestDistance2 = (std::numeric_limits<float>::max)();
    for (size_t i = 0; i + 1 < m_gamePath.size(); ++i) {
        const Point2f& a = m_gamePath[i];
        const Point2f& b = m_gamePath[i + 1];
        const float abx = b.x - a.x;
        const float aby = b.y - a.y;
        const float length2 = abx * abx + aby * aby;
        float t = 0.0f;
        if (length2 > 1e-6f) {
            t = ((current.x - a.x) * abx + (current.y - a.y) * aby) / length2;
            t = (std::max)(0.0f, (std::min)(1.0f, t));
        }
        const float px = a.x + abx * t;
        const float py = a.y + aby * t;
        const float dx = current.x - px;
        const float dy = current.y - py;
        const float d2 = dx * dx + dy * dy;
        if (d2 < bestDistance2) {
            bestDistance2 = d2;
            bestSegment = i;
            bestT = t;
        }
    }

    // 2) 在段上解 |a + t*(b-a) - current| = distance，取 t >= tStart 的最小根。
    const auto segmentHit = [&](size_t index, float tStart, Point2f& hit) -> bool {
        const Point2f& a = m_gamePath[index];
        const Point2f& b = m_gamePath[index + 1];
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float fx = a.x - current.x;
        const float fy = a.y - current.y;
        const float qa = dx * dx + dy * dy;
        if (qa < 1e-9f) return false;
        const float qb = 2.0f * (fx * dx + fy * dy);
        const float qc = fx * fx + fy * fy - radius2;
        const float disc = qb * qb - 4.0f * qa * qc;
        if (disc < 0.0f) return false;
        const float root = std::sqrt(disc);
        const float t1 = (-qb - root) / (2.0f * qa);
        const float t2 = (-qb + root) / (2.0f * qa);
        float tHit = -1.0f;
        if (t1 >= tStart && t1 <= 1.0f) tHit = t1;
        else if (t2 >= tStart && t2 <= 1.0f) tHit = t2;
        if (tHit < 0.0f) return false;
        hit.x = a.x + dx * tHit;
        hit.y = a.y + dy * tHit;
        return true;
    };

    Point2f hit{};
    if (segmentHit(bestSegment, bestT, hit)) {
        out = hit;
        return true;
    }
    for (size_t i = bestSegment + 1; i + 1 < m_gamePath.size(); ++i) {
        if (segmentHit(i, 0.0f, hit)) {
            out = hit;
            return true;
        }
    }

    // 3) 圆与路线无交点（离路线超过 distance / 剩余路线不足）：
    //    不猜点，交给调用方用"朝目标方向 20 米"兜底。
    return false;
}

int CNearby::KillMonst_Fz(CString nam, std::vector<std::vector<Point2f>>& vvp,
                         const std::function<bool()>& continueTask)
{
    if (!m_control || m_control->IsStopping() || nam.IsEmpty()) return -1;
    const auto active = [&]() {
        return !m_control->IsStopping() && WaitIfPaused() && (!continueTask || continueTask());
    };

    // The reference fills these route/skill lists before entering the
    // instance loop.  They are per-thread members in CX555.
    const ScopedActionStatuses actionStatuses(*this,
        { 213, 14, 18, 22, 12, 21 }, { 213, 14, 18, 22, 12, 21 }, true);
    regex targetRegex = NerbCs(40, { 0, 0 }, nam);

    // A delayed team-follow update can arrive after entry. Clear it locally
    // before each route/combat step instead of waiting forever for the leader.
    const auto readyForCombat = [&]() {
        while (active()) {
            if (Read_DTID() != 0xAA) return false;
            if (!IsWndGuot()) {
                if (Read_GENS() == 0) return true;
                wDui_Gens(0);
            }
            SleepEx(1000);
        }
        return false;
    };
    if (!readyForCombat()) return active() ? 0 : -1;

    bool dismountPending = true; // Reset for every instance invocation.
    for (const std::vector<Point2f>& route : vvp) {
        if (!active()) return -1;
        bool bossSeen = false;

        for (const Point2f& point : route) {
            if (!readyForCombat()) return active() ? 0 : -1;
            wBenTuXuL3f(point.x, point.y);

            // 远程门派到每个点后点怪引怪；组末仍按原流程集中清怪。
            if (Mams[Jijl] <= 5 || Mams[JiGj] == 0) continue;
            if (!readyForCombat()) return active() ? 0 : -1;
            // XuL3f 也可能因寻路失败提前返回，先确认确实到达坐标。
            if (!UpdateMP(static_cast<DWORD>(jz[TaskMemoryState::jYcdiz]), 0x100) || !dwls) continue;
            const Point2f position{
                *reinterpret_cast<const float*>(static_cast<ULONG_PTR>(dwls) + 0x54),
                *reinterpret_cast<const float*>(static_cast<ULONG_PTR>(dwls) + 0x5C)
            };
            if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
                std::hypot(position.x - point.x, position.y - point.y) >= 2.0f) continue;

            const std::vector<Monst> monsters = FindAttackableMonsters(
                Read_Dw(tsg[TaskMemoryState::ERC]), targetRegex, 40.0f, false);
            for (const Monst& monster : monsters) {
                if (!readyForCombat()) return active() ? 0 : -1;
                if (monster.Juli >= static_cast<float>(Mams[Jijl]) || !NerExist(monster.Dibu)) continue;
                if (Read_MoZTT(monster.Dibu) != 0) continue;
                if (dismountPending) {
                    wRideOn(0);
                    dismountPending = false;
                    if (!readyForCombat()) return active() ? 0 : -1;
                }
                // 前一只怪的 500ms 间隔或下马后，复查目标血量和状态。
                if (!UpdateMP(monster.cxue, 0x100) || !dwls) continue;
                const float hp = *reinterpret_cast<const float*>(static_cast<ULONG_PTR>(dwls) + 0x08);
                if (!std::isfinite(hp) || hp <= 0.0f || Read_MoZTT(monster.Dibu) != 0) continue;
                if (!active()) return -1;
                if (wUseSk6(Mams[JiGj], static_cast<DWORD>(monster.ID), true)) {
                    if (monster.mname == _T("\u8d3c\u5175\u5934\u76ee")) bossSeen = true;
                    SleepEx(500);
                }
            }
        }

        int nearbyCount = 0;
        while (active()) {
            if (!readyForCombat()) return active() ? 0 : -1;
            nearbyCount = NerbMonst2(Read_Dw(tsg[TaskMemoryState::ERC]),
                                     targetRegex, false);
            if (nearbyCount <= 0) break;

            if (dismountPending) {
                wRideOn(0);
                dismountPending = false;
                // Recheck the scene and targets after the mount transition.
                continue;
            }

            for (const Monst& monster : vMonst) {
                if (monster.mname == _T("\u8d3c\u5175\u5934\u76ee")) {
                    bossSeen = true;
                    break;
                }
            }

            KillMonsterBs(vMonst);
            if (!active()) return -1;
            SleepEx(1000);
        }

        if (bossSeen) {
            for (int wait = 0; wait < 12 && active(); ++wait) SleepEx(500);
            return active() ? 0x1005 : -1;
        }
    }

    return active() ? 0 : -1;
}
