#include "pch.h"
#include "CNearby.h"
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

// Mirrors the reference behaviour of reading "<workdir>/<file>"; the packaged
// build keeps the same files under "<workdir>/config".
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