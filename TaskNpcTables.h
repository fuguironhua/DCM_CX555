#pragma once

// Data tables ported from the reference project (DCM_CX5 BianL.h).
//   VRENW slot : { task name, task id, type/map id, position, npc name }
//   VHUOD slot : { activity name, type, position, npc name }
//
// The working vectors live on CTaskThread so every window keeps its own copy
// (the reference project used process-wide globals).  Chinese literals use
// \uXXXX escapes so they do not depend on the source file encoding.

#include <tuple>
#include <vector>

#include "structs.h"

using TaskNpcRewardEntry = std::tuple<CString, DWORD, DWORD, Point2f, CString>;
using TaskNpcActivityEntry = std::tuple<CString, DWORD, Point2f, CString>;

#define VRENW { \
    {_T("\u4e00\u4e2a\u90fd\u4e0d\u80fd\u8dd1"), 0xC3B4, 1, Point2f{62.7f, 162.6f}, _T("\u94b1\u5b8f\u5b87")}, \
    {_T("\u9664\u5bb3"), 0xC3B5, 1, Point2f{251.1f, 109.0f}, _T("\u82b1\u5251\u96e8")}, \
    {_T("\u527f\u532a"), 0xC3B6, 1, Point2f{62.7f, 162.6f}, _T("\u94b1\u5b8f\u5b87")}, \
    {_T("\u4e00\u5343\u96f6\u4e00\u4e2a\u613f\u671b"), 0xC54C3, 1, Point2f{186.0f, 182.0f}, _T("\u6881\u9053\u58eb")}, \
    {_T("\u9ec4\u91d1\u4e4b\u94fe"), 0xC42C, 0xF6, Point2f{295.0f, 68.0f}, _T("\u4f55\u60a6")}, \
    {_T("\u7384\u4f5b\u73e0"), 0xC42D, 0xF6, Point2f{122.0f, 56.0f}, _T("\u5218\u76fe")}, \
    {_T("\u7194\u5ca9\u4e4b\u5730"), 0xC42E, 0xF6, Point2f{295.0f, 68.0f}, _T("\u4f55\u60a6")}, \
    {_T("\u6740\u661f"), 0x62280, 2, Point2f{130.0f, 76.0f}, _T("\u67af\u8363\u5927\u5e08")}, \
    {_T("\u60e9\u51f6\u6253\u56fe"), 0x37EA2, 1, Point2f{127.0f, 133.0f}, _T("\u5434\u73a0")} \
}

#define VHUOD { \
    {_T("\u767e\u53d8\u8138\u8c31"), 1, Point2f{168.6f, 168.5f}, _T("\u6587\u5f66\u535a")}, \
    {_T("\u8fdb\u5165\u68cb\u5c40"), 1, Point2f{174.0f, 147.0f}, _T("\u5f20\u5f08\u56fd")}, \
    {_T("\u5e78\u8fd0\u5feb\u6d3b\u4e09"), 1, Point2f{130.5f, 230.0f}, _T("\u67f3\u6708\u8679")}, \
    {_T("\u9886\u53d6\u5e78\u8fd0\u5feb\u6d3b\u4e09\u5956\u52b1"), 1, Point2f{130.5f, 230.0f}, _T("\u67f3\u6708\u8679")}, \
    {_T("\u524d\u5f80\u9752\u4e18"), 1, Point2f{135.0f, 81.0f}, _T("\u4e91\u5170\u821f")}, \
    {_T("\u521d\u6218\u7f25\u7f08\u5cf0"), 0xF6, Point2f{192.6f, 224.0f}, _T("\u7a0b\u9752\u971c")}, \
    {_T("\u8ba8\u4f10\u71d5\u5b50\u575e"), 4, Point2f{70.5f, 119.0f}, _T("\u674e\u7eb2")} \
}
// ---------------------------------------------------------------------------
// VRENW 槽位：参考项目 BianL.h 的 F_* 枚举下标，副本任务用它取任务表项。
// ---------------------------------------------------------------------------
enum TaskRewardSlot : size_t {
    F_LY = 0,  // 一个都不能跑（副本老一, 任务 0xC3B4, 地图 0x4F）
    F_LE = 1,  // 除害（副本老二, 任务 0xC3B5, 地图 0x51）
    F_LS = 2,  // 剿匪（副本老三, 任务 0xC3B6, 地图 0x50）
};

// ---------------------------------------------------------------------------
// Skill tables, ported from the reference project (DCM_CX5 BianL.h).
//   L_*      : reference Fruit13 skill kinds
//   VSKILL   : one row per menpai -> default attack skills (id, level, 0, kind, name)
//   VSKILL2  : one row per menpai -> (Jijl, JiGj)
// ---------------------------------------------------------------------------
enum TaskSkillKind : DWORD {
    L_dt = 0x101, L_fw = 0x102, L_dd = 0x103, L_zt = 0x104, L_cx = 0x105,
    L_nq = 0x106, L_fd = 0x107, L_zt1 = 0x108, L_yd = 0x109, L_js = 0x10A
};

using TaskSkillEntry = std::tuple<int, int, DWORD, DWORD, CString>;
using TaskSkillZtEntry = std::tuple<int, int, DWORD, DWORD, CString, DWORD>;
using TaskSkillMenu = std::vector<TaskSkillEntry>;
using TaskSkillPair = std::tuple<DWORD, DWORD>;

#define VSKILL { \
    { \
        {0x127,20,0,L_dt,_T("\u4e00\u62cd\u4e24\u6563")},{0x123,20,0,L_dt,_T("\u6469\u8bc3\u65e0\u91cf")} \
    }, \
    { \
        {0x141,12,0,L_dt,_T("\u6c34\u6df9\u4e03\u519b")},{0x145,12,0,L_dt,_T("\u706b\u70e7\u8d64\u58c1")} \
    }, \
    { \
        {0x163,16,0,L_dt,_T("\u9707\u60ca\u767e\u91cc")},{0x15f,20,0,L_dt,_T("\u9752\u9f99\u51fa\u6c34")} \
    }, \
    { \
        {0x17d,20,0,L_dt,_T("\u4ed9\u4eba\u6307\u8def")},{0x181,20,0,L_dt,_T("\u7389\u5973\u7a7f\u68ad")} \
    }, \
    { \
        {0x19f,20,0,L_dt,_T("\u969c\u773c\u6cd5")} \
    }, \
    { \
        {0x1b9,24,0,L_dt,_T("\u9752\u86c7\u6bd2\u638c")},{0x1bd,24,0,L_dt,_T("\u51b0\u8695\u6bd2\u638c")} \
    }, \
    { \
        {0x1d7,20,0,L_dt,_T("\u6b32\u64d2\u6545\u7eb5")},{0x1db,20,0,L_dt,_T("\u96ea\u62e5\u84dd\u5173")} \
    }, \
    { \
        {0x1f9,15,0,L_dt,_T("\u6885\u82b1\u4e09\u5f04")},{0x1f5,15,0,L_dt,_T("\u96ea\u82b1\u516d\u51fa")} \
    }, \
    { \
        {0x217,10,0,L_dt,_T("\u6eaa\u5c71\u884c\u65c5")} \
    }, \
    { \
        {117,15,0,L_dt,_T("\u6eaa\u5c71\u884c\u65c5")},{118,15,0,L_dt,_T("\u6eaa\u5c71\u884c\u65c5")} \
    }, \
    { \
        {0x303,15,0,L_dt,_T("\u957f\u76f8\u601d")},{0x307,15,0,L_dt,_T("\u5982\u68a6\u4ee4")} \
    }, \
    { \
        {0x31F,15,0,L_dt,_T("\u5243\u9b42")},{0x323,15,0,L_dt,_T("\u7089\u4e0a")} \
    }, \
    { \
        {0x337,15,0,L_dt,_T("\u91cd\u4e91")},{0x33B,15,0,L_dt,_T("\u5251\u9501")} \
    } \
}

#define VSKILL2 { \
    {3,281},{3,311},{3,341},{15,371},{15,401},{15,431},{15,461}, \
    {3,491},{15,521},{3,0},{15,760},{15,788},{3,812} \
}