#include "pch.h"
#include "CTaskScript.h"
#include "TeamServerClient.h"

#include <mutex>
#include <algorithm>
#include <vector>
#include <utility>
#include <cmath>

namespace {

// These are the CX5 offsets used by the reference constructor. They are
// deliberately kept as data in the new per-window state instead of being
// process-wide globals.
constexpr LONGLONG kRolePointerOffset = 3216635;       // ROLEP
constexpr LONGLONG kBagPointerOffset = 3209849;        // BAGBP
constexpr LONGLONG kMapPointerOffset = 3333305;        // MYDITUP
constexpr LONGLONG kRoleWorkOffset = 2089596;          // RENWP
constexpr LONGLONG kRoleErcOffset = 1611047;           // RENERCP
constexpr LONGLONG kHookCodeOffset = 3420342;          // HHKK3P
constexpr LONGLONG kLuaOffset = 3735957;               // LUAP
constexpr LONGLONG kBagSplitOffset = 89674;             // FENGBP
constexpr LONGLONG kMonsterFenOffset = 1653545;        // MONSTFENP
constexpr LONGLONG kSleepOffset = 3152901;             // KESLEEPP
constexpr LONGLONG kGetTickOffset = 3475609;           // KEGETTIP
constexpr LONGLONG kLuaDoStringOffset = 2754090;       // GALUADOSP
constexpr LONGLONG kGameThreadOffset = 3148495;        // GAZHANTP
constexpr LONGLONG kSendOffset = 3897970;              // GAWSENDP
constexpr LONGLONG kTerminateThreadOffset = 3149794;   // KETERMTP
constexpr LONGLONG kMapTransitionOffset = 1889684;     // KUATUXLP
constexpr LONGLONG kTeamOffset = 541893;               // GADUIWP
constexpr LONGLONG kDebugOffset = 3387509;             // GADEBUGP
constexpr LONGLONG kStringOffset = 2754342;            // GSTRSTRP
constexpr LONGLONG kTaskHookOffset = 2541031;           // HKRWZTP
constexpr LONGLONG kShopOffset = 3536556;              // SHANGHP
constexpr LONGLONG kActivityOffset = 673018;           // HUOYP
constexpr LONGLONG kSweepOffset = 645746;              // SAODP
constexpr LONGLONG kKeyInputOffset = 3149144;          // SHIJEP
constexpr LONGLONG kMemoryCopyOffset = 5874958;        // MEMCP
constexpr LONGLONG kRoleNameOffset = 2096072;          // RENMSP
constexpr LONGLONG kAttackNumberOffset = -3604480;     // ATTNUMP
constexpr LONGLONG kBufferClearOffset = 2543479;       // BUFXP
constexpr LONGLONG kWindowBaseOffset = 1125552;        // WPBDP
constexpr LONGLONG kSweepDetailOffset = 148033;        // SAODDP
constexpr LONGLONG kWindowOpenOffset = 312859;         // WNDOPP
constexpr LONGLONG kSceneHeightOffset = 625840;         // SHIHP
constexpr LONGLONG kBagHelperOffset = 518688;           // BAGJHP
constexpr LONGLONG kMapCheckOffset = 807808;           // GAXUNLUGP

bool ShouldUseRoute3(const CString& route) {
    const int separator = route.ReverseFind(_T('-'));
    CString endPoint = separator >= 0 ? route.Mid(separator + 1) : route;
    endPoint.Trim();
    return endPoint == _T("\u6d1b\u9633") ||
           endPoint == _T("\u82cf\u5dde") ||
           endPoint == _T("\u5927\u7406");
}

// Retained offsets for task paths that are not wired into this framework yet.
constexpr LONGLONG kF1Offset = -3604478;               // F1FP
constexpr LONGLONG kRoleEcxOffset = 1098465;            // RENWECXP
constexpr LONGLONG kLocalMoveOffset = 1076091;          // BENTUXLP
constexpr LONGLONG kSelectTargetOffset = 6093535;       // XZZTP
constexpr LONGLONG kLocalMove2Offset = -3604478;        // BENTUXL2P
constexpr LONGLONG kRunOpenOffset = -3604470;           // RUNOPENP
constexpr LONGLONG kSelectOffset = -3604478;            // XUANP
constexpr LONGLONG kSkillOffset = 1015826;              // SKILLP
constexpr LONGLONG kUseBagOffset = -3604466;            // USEBAGP
constexpr LONGLONG kMapCheckCallOffset = -3604468;      // GAXUNLUCP
constexpr LONGLONG kLocalMove3Offset = 1849082;         // BENTUXL3P
constexpr LONGLONG kWindowDetailOffset = 3650288;       // WPJDP
constexpr LONGLONG kCeguiFAOffset = 250155;             // CEFAP
constexpr LONGLONG MANAGEP = 0x24BD3C;           // MANAGEP（CEGUIBase.dll 偏移，2026-09-17 更新后实测仍有效）
constexpr LONGLONG WORLDMANAP = 0x7C6A7C;
// 2026-09-17 客户端更新后这两个跑商全局读取无效（读到 0/1），但当前代码只赋值、无使用方；
// 待跑商功能真正用到时，再按旧版 IDA 库(Game.exe.i64) + 新版对比重新定位。
constexpr LONGLONG PaoShangSceneManagerGlobal = 0x7CE46C;
constexpr LONGLONG PaoShangSceneManagerCache = 0x7CBD98;

constexpr LONGLONG kCharacterNameOffset = 0x2C;        // namz
constexpr LONGLONG kMenPaiOffset = 0xD0;               // MenPai
constexpr LONGLONG kLevelOffset = 0x60;                // dengj
constexpr unsigned long kLuqiOffset = 0x64;

// The cache is intentionally keyed by the game window handle.  A handle is
// only valid for the current process lifetime, but keeping the record for the
// day lets several task windows reuse the same remote block information.
constexpr LPCTSTR kGlobalConfigSection = _T("QBJB");
constexpr LPCTSTR kGlobalConfigDateKey = _T("Date");
std::mutex g_globalConfigMutex;

bool IsAddressInsideModule(HMODULE module, std::uintptr_t address) {
    if (!module) return false;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const BYTE*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const auto moduleBase = reinterpret_cast<std::uintptr_t>(module);
    const auto moduleEnd = moduleBase + nt->OptionalHeader.SizeOfImage;
    return address >= moduleBase && address < moduleEnd;
}

// Keep the original two-argument resolver for the existing CreateThread and
// OutputDebugStringA address setup.
LONGLONG ResolveRemoteExport(LONGLONG remoteModule, LPCSTR name) {
    if (remoteModule == 0) return 0;

    const HMODULE localModule = ::GetModuleHandle(_T("KERNEL32.dll"));
    const FARPROC localAddress = localModule
        ? ::GetProcAddress(localModule, name) : nullptr;
    if (!localModule || !localAddress) return 0;

    const auto localBase = reinterpret_cast<std::uintptr_t>(localModule);
    const auto localProc = reinterpret_cast<std::uintptr_t>(localAddress);
    return remoteModule + static_cast<LONGLONG>(localProc - localBase);
}

LONGLONG ResolveRemoteExport(LONGLONG remoteModule,
                              LONGLONG remoteNtdll,
                              LPCSTR name) {
    if (remoteModule == 0) return 0;

    const HMODULE localKernel32 = ::GetModuleHandle(_T("KERNEL32.dll"));
    const FARPROC localAddress = localKernel32
        ? ::GetProcAddress(localKernel32, name) : nullptr;
    if (!localKernel32 || !localAddress) return 0;

    const auto localProc = reinterpret_cast<std::uintptr_t>(localAddress);
    if (IsAddressInsideModule(localKernel32, localProc)) {
        const auto localBase = reinterpret_cast<std::uintptr_t>(localKernel32);
        return remoteModule + static_cast<LONGLONG>(localProc - localBase);
    }

    // Some KERNEL32 exports are forwarders.  On this target,
    // AddVectoredExceptionHandler resolves into NTDLL, so use the target
    // process's NTDLL base rather than copying the controller's VA.
    const HMODULE localNtdll = ::GetModuleHandle(_T("ntdll.dll"));
    if (remoteNtdll != 0 && IsAddressInsideModule(localNtdll, localProc)) {
        const auto localBase = reinterpret_cast<std::uintptr_t>(localNtdll);
        return remoteNtdll + static_cast<LONGLONG>(localProc - localBase);
    }

    return 0;
}

CString ConvertAnsiText(const char* text) {
    if (!text) return {};

    int sourceLength = 0;
    while (sourceLength < 256 && text[sourceLength] != '\0') ++sourceLength;
    if (sourceLength == 0) return {};

    const int targetLength = ::MultiByteToWideChar(
        CP_ACP, 0, text, sourceLength, nullptr, 0);
    if (targetLength <= 0) return {};

    CString result;
    wchar_t* buffer = result.GetBuffer(targetLength);
    ::MultiByteToWideChar(CP_ACP, 0, text, sourceLength,
                          buffer, targetLength);
    result.ReleaseBuffer(targetLength);
    return result;
}

bool EnableDebugPrivilege()
{
	HANDLE hToken;
	LUID sedebugnameValue;
	TOKEN_PRIVILEGES tkp;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
	{
		return  false;
	}
	if (!LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &sedebugnameValue))
	{
		CloseHandle(hToken);
		return false;
	}
	tkp.PrivilegeCount = 1;
	tkp.Privileges[0].Luid = sedebugnameValue;
	tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	if (!AdjustTokenPrivileges(hToken, FALSE, &tkp, sizeof(tkp), NULL, NULL))
	{
		CloseHandle(hToken);
		return false;
	}

	return true;
}

DWORD GetMainThreadId(DWORD dwPid)
{
	HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (hSnap == INVALID_HANDLE_VALUE) return 0;
	THREADENTRY32 te = { sizeof(te) };
	DWORD tid = 0;
	if (Thread32First(hSnap, &te)) {
		do {
			if (te.th32OwnerProcessID == dwPid) {
				tid = te.th32ThreadID;
				break;
			}
		} while (Thread32Next(hSnap, &te));
	}
	CloseHandle(hSnap);
	return tid;
}

} // namespace

BOOL CTaskScript::HijackMainThread32(DWORD dwPid)
{
	EnableDebugPrivilege();

	if (dwPid == 0) dwPid = mmpid;
	if (dwPid == 0 || mmainthread == 0 || jz[TaskMemoryState::Nbsq] == 0) {
		DebugLog(_T("hijack skipped: pid/thread/Nbsq is invalid"));
		return FALSE;
	}

	HANDLE hThread = OpenThread(
		THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
		FALSE, mmainthread);
	if (!hThread) {
		CString message;
		message.Format(_T("hijack failed: OpenThread tid=%lu error=%lu"),
			static_cast<unsigned long>(mmainthread),
			static_cast<unsigned long>(GetLastError()));
		DebugLog(message);
		return FALSE;
	}

	if (SuspendThread(hThread) == static_cast<DWORD>(-1)) {
		CString message;
		message.Format(_T("hijack failed: SuspendThread tid=%lu error=%lu"),
			static_cast<unsigned long>(mmainthread),
			static_cast<unsigned long>(GetLastError()));
		DebugLog(message);
		CloseHandle(hThread);
		return FALSE;
	}

	CONTEXT ctx = { 0 };
	ctx.ContextFlags = CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS;
	if (!GetThreadContext(hThread, &ctx)) {
		CString message;
		message.Format(_T("hijack failed: GetThreadContext tid=%lu error=%lu"),
			static_cast<unsigned long>(mmainthread),
			static_cast<unsigned long>(GetLastError()));
		DebugLog(message);
		ResumeThread(hThread);
		CloseHandle(hThread);
		return FALSE;
	}

	// The shellcode runs inside the target process.  A function pointer from
	// this process is not necessarily valid there because KERNEL32 can have a
	// different ASLR base in each process; resolve the export against the
	// target's KERNEL32 module instead.
    const LONGLONG remoteNtdll = Dm()->GetModuleBaseAddr(mmhwnd, _T("ntdll.dll"));
    const LONGLONG remoteVehApi = ResolveRemoteExport(
        Memory().kernel32Module, remoteNtdll, "AddVectoredExceptionHandler");
    if (remoteVehApi == 0 || remoteVehApi > 0xFFFFFFFFLL) {
        CString message;
        message.Format(_T("hijack failed: remote AddVectoredExceptionHandler is invalid (KERNEL32=0x%08I64X NTDLL=0x%08I64X)"),
            static_cast<unsigned __int64>(Memory().kernel32Module),
            static_cast<unsigned __int64>(remoteNtdll));
		DebugLog(message);
		ResumeThread(hThread);
		CloseHandle(hThread);
		return FALSE;
	}

	const DWORD pVehApi = static_cast<DWORD>(remoteVehApi);
	const DWORD_PTR hardwareBp[4] = {
		static_cast<DWORD_PTR>(jz[TaskMemoryState::HHKK3PM]),
		static_cast<DWORD_PTR>(jz[TaskMemoryState::FFzhant]),
		static_cast<DWORD_PTR>(jz[TaskMemoryState::Hksendd]),
		static_cast<DWORD_PTR>(jz[TaskMemoryState::Hkwordd])
	};
	for (const DWORD_PTR bpAddress : hardwareBp) {
		if (bpAddress == 0 || bpAddress > 0xFFFFFFFFULL) {
			DebugLog(_T("hijack failed: one hardware breakpoint address is invalid"));
			ResumeThread(hThread);
			CloseHandle(hThread);
			return FALSE;
		}
	}

	// The VEH dispatch compares the exception EIP with these four original
	// game addresses.  Nbsq is the trampoline/handler area; it is not one of
	// the four hardware-breakpoint targets.
	ctx.Dr0 = hardwareBp[0];
	ctx.Dr1 = hardwareBp[1];
	ctx.Dr2 = hardwareBp[2];
	ctx.Dr3 = hardwareBp[3];
	ctx.Dr6 = 0;
	ctx.Dr7 &= ~static_cast<DWORD_PTR>(0xFF); // clear old DR0..DR3 enables
	for (DWORD index = 0; index < 4; ++index) {
		ctx.Dr7 &= ~(static_cast<DWORD_PTR>(0xF) << (16 + index * 4));
		ctx.Dr7 |= static_cast<DWORD_PTR>(1) << (index * 2); // local execute, 1 byte
	}

	*(DWORD*)(install_veh_shellcode2 + 2) = static_cast<DWORD>(jz[TaskMemoryState::Nbsq] + 30);
	*(DWORD*)(install_veh_shellcode2 + 9) = pVehApi;
	*(DWORD*)(install_veh_shellcode2 + 0x11) = ctx.Eip;
	const long writeResult = Dm()->WriteDataAddrFromBin(mmhwnd,
		jz[TaskMemoryState::Nbsq],
		static_cast<long>(reinterpret_cast<LONG_PTR>(install_veh_shellcode2)),
		22);
	if (writeResult == 0) {
		DebugLog(_T("hijack failed: writing Nbsq shellcode returned 0"));
		ResumeThread(hThread);
		CloseHandle(hThread);
		return FALSE;
	}

	SleepEx(100);
	ctx.Eip = static_cast<DWORD>(jz[TaskMemoryState::Nbsq]);
	if (!SetThreadContext(hThread, &ctx)) {
		CString message;
		message.Format(_T("hijack failed: SetThreadContext tid=%lu error=%lu"),
			static_cast<unsigned long>(mmainthread),
			static_cast<unsigned long>(GetLastError()));
		DebugLog(message);
		ResumeThread(hThread);
		CloseHandle(hThread);
		return FALSE;
	}

	ResumeThread(hThread);
	CloseHandle(hThread);
	CString message;
	message.Format(_T("hijack ok: pid=%lu tid=%lu Nbsq=0x%08lX VEH=0x%08lX DR0=0x%08lX DR1=0x%08lX DR2=0x%08lX DR3=0x%08lX"),
		static_cast<unsigned long>(dwPid),
		static_cast<unsigned long>(mmainthread),
		static_cast<unsigned long>(jz[TaskMemoryState::Nbsq]),
		static_cast<unsigned long>(pVehApi),
		static_cast<unsigned long>(hardwareBp[0]),
		static_cast<unsigned long>(hardwareBp[1]),
		static_cast<unsigned long>(hardwareBp[2]),
		static_cast<unsigned long>(hardwareBp[3]));
	DebugLog(message);
	return TRUE;
}

CTaskScript::CTaskScript(std::shared_ptr<tagWndInfo> wnd,
                         std::shared_ptr<CThreadControl> control,
                         const tagWndIni& ini,
                         HWND logWindow,
                         TaskSchedulePtr schedule,
                         TaskMemoryStatePtr memory)
    : CNearby(std::move(wnd), std::move(control), ini, logWindow, std::move(memory)),
      m_schedule(std::move(schedule)) {}

void CTaskScript::RequestTest() {
    m_testRequested.store(true, std::memory_order_release);
}

void CTaskScript::RequestRoute(bool reverse) {
    m_routeRequest.store(reverse ? 1 : 0, std::memory_order_release);
}

bool CTaskScript::OnEventThreadStart() {
    // The reference implementation performed these reads in its constructor.
    // In this framework dmsoft is created by CTaskThread::ThreadMain, so the
    // equivalent must run here, on the same worker thread as the dm object.
    (void)InitializeGameState();
    // Print the shared role base once after initialization for verification.
    havetest();
    return true;
}

void CTaskScript::OnEventThreadConclude() {
    // If COM/dmsoft initialization failed before OnEventThreadStart could
    // run, release helper threads waiting on the shared state as well.
    if (m_memory && !m_memory->IsInitialized()) {
        m_memory->FinishInitialization(false);
    }
}

bool CTaskScript::InitializeGameState() {
    if (!m_memory) return false;
    auto& memory = *m_memory;
    memory.BeginInitialization();

    if (!Dm() || !m_wnd || !m_wnd->hWnd) {
        memory.FinishInitialization(false);
        DebugLog(_T("task memory init skipped: dmsoft or target window is unavailable"));
        return false;
    }

    const long targetHwnd = static_cast<long>(reinterpret_cast<INT_PTR>(m_wnd->hWnd));
    mmhwnd = targetHwnd;
    memory.gameModule = Dm()->GetModuleBaseAddr(targetHwnd, _T("Game.exe"));
    memory.ceguiModule = Dm()->GetModuleBaseAddr(targetHwnd, _T("UI_CEGUI.dll"));
    memory.kernel32Module = Dm()->GetModuleBaseAddr(targetHwnd, _T("KERNEL32.dll"));
    memory.ceguiBaseModule = Dm()->GetModuleBaseAddr(targetHwnd, _T("CEGUIBase.dll"));

    CString modules;
    modules.Format(_T("task memory modules: Game=0x%08lX, UI_CEGUI=0x%08lX, KERNEL32=0x%08lX, CEGUIBase=0x%08lX"),
                   static_cast<unsigned long>(memory.gameModule),
                   static_cast<unsigned long>(memory.ceguiModule),
                   static_cast<unsigned long>(memory.kernel32Module),
                   static_cast<unsigned long>(memory.ceguiBaseModule));
    DebugLog(modules);

    const auto readInt = [this, targetHwnd](LONGLONG address, long type) -> LONGLONG {
        return Dm()->ReadIntAddr(targetHwnd, address, type);
    };

    if (memory.gameModule != 0) {
        const LONGLONG game = memory.gameModule;
        auto& address = memory.addresses;

        address[TaskMemoryState::ROLEB] = readInt(game + kRolePointerOffset, 4);
        address[TaskMemoryState::jYcdiz] = readInt(address[TaskMemoryState::ROLEB], 4) + 0x64;
        address[TaskMemoryState::BagJ] = readInt(game + kBagPointerOffset, 4);
        // BAGBP resolves the global slot; zqpd holds its object, not the slot.
        address[TaskMemoryState::zqpd] = address[TaskMemoryState::BagJ] != 0
            ? readInt(address[TaskMemoryState::BagJ], 4) : 0;
        address[TaskMemoryState::jcYcdi] = readInt(address[TaskMemoryState::BagJ], 4) + 0x65EC0;
        address[TaskMemoryState::BagEcx] = readInt(game + kBagSplitOffset, 4);
        address[TaskMemoryState::HHKK3PM] = game + kHookCodeOffset + 0x158;
        address[TaskMemoryState::HHKK3P2] = readInt(address[TaskMemoryState::HHKK3PM] + 2, 4);
        address[TaskMemoryState::Sleepd] = readInt(game + kSleepOffset, 4);
        address[TaskMemoryState::Gettick] = readInt(game + kGetTickOffset, 4);
        address[TaskMemoryState::FFzhant] = game + kGameThreadOffset;
        memory.expressions[TaskMemoryState::tls1].Format(_T("[[%08lX]]"),
                                                         static_cast<unsigned long>(game + kLuaOffset));
        address[TaskMemoryState::luaState] =
            Dm()->ReadInt(targetHwnd, memory.expressions[TaskMemoryState::tls1].GetString(), 4);
        address[TaskMemoryState::Luadost] = readInt(game + kLuaDoStringOffset, 4);
        address[TaskMemoryState::Wsasend] = readInt(game + kSendOffset, 4);
        address[TaskMemoryState::Hksendd] = game + kSendOffset + 4;
        address[TaskMemoryState::TermThM] = readInt(game + kTerminateThreadOffset, 4);
        address[TaskMemoryState::KuaTuX] = readInt(game + kMapTransitionOffset, 4);
        address[TaskMemoryState::RenW] = readInt(game + kRoleWorkOffset, 4);
        address[TaskMemoryState::RenWERC] = readInt(game + kRoleErcOffset, 4);
        address[TaskMemoryState::MyDiTu] = readInt(game + kMapPointerOffset, 4);
        address[TaskMemoryState::Paos] = readInt(game + kShopOffset, 4);
        address[TaskMemoryState::Duiwu] = readInt(game + kTeamOffset, 4);
        address[TaskMemoryState::SaoD] = readInt(game + kSweepOffset, 4);
        address[TaskMemoryState::HuoYe] = readInt(game + kActivityOffset, 4);
        address[TaskMemoryState::Hzt] = game + kTaskHookOffset + readInt(game + kTaskHookOffset, 0) + 4;
        address[TaskMemoryState::Hzt2] = readInt(game + kTaskHookOffset - 9, 4);
        address[TaskMemoryState::Strs] = readInt(game + kStringOffset, 4);
        address[TaskMemoryState::Gdebug] = game + kDebugOffset + 5;
        address[TaskMemoryState::Hkwordd] = game + kKeyInputOffset;
        address[TaskMemoryState::Memccp] = readInt(game + kMemoryCopyOffset, 4);
        address[TaskMemoryState::Mfen] = readInt(game + kMonsterFenOffset, 4);
        address[TaskMemoryState::Xguih] = game + kMapCheckOffset;
        address[TaskMemoryState::Renms] = game + kRoleNameOffset;
        address[TaskMemoryState::Renmcall] = game + kRoleNameOffset + 13 + readInt(game + kRoleNameOffset + 13, 0) + 4;
        address[TaskMemoryState::Hkattjc] = game + kAttackNumberOffset;
        address[TaskMemoryState::Bufqx] = game + kBufferClearOffset + readInt(game + kBufferClearOffset, 0) + 4;
        address[TaskMemoryState::sddd] = readInt(game + kSweepDetailOffset, 4);
        address[TaskMemoryState::bagbd] = game + kWindowBaseOffset;
        address[TaskMemoryState::SceneManagerGlobal] = readInt(game + PaoShangSceneManagerGlobal, 4);
        address[TaskMemoryState::SceneManagerCache] = readInt(game + PaoShangSceneManagerCache, 4);
        address[TaskMemoryState::CWorldManager] = game + WORLDMANAP;
    }
    
    if (memory.ceguiModule != 0) {
        memory.addresses[TaskMemoryState::WndOpen] =
            readInt(memory.ceguiModule + kWindowOpenOffset, 4);
    }
    if (memory.ceguiBaseModule != 0) {
        memory.addresses[TaskMemoryState::ShicH] =
            memory.ceguiBaseModule + kSceneHeightOffset;
        memory.addresses[TaskMemoryState::BagJH] =
            readInt(memory.ceguiBaseModule + kBagHelperOffset, 4);
    }
    memory.addresses[TaskMemoryState::BagJH2] = 0;
    if (memory.kernel32Module != 0) {
        memory.addresses[TaskMemoryState::Createh] =
            ResolveRemoteExport(memory.kernel32Module, "CreateThread");
        memory.addresses[TaskMemoryState::Kdebug] =
            ResolveRemoteExport(memory.kernel32Module, "OutputDebugStringA");
    }

    auto& expression = memory.expressions;
    const auto hex32 = [](LONGLONG value) -> unsigned long {
        return static_cast<unsigned long>(value);
    };

    wchar_t modulePath[MAX_PATH]{};
    const DWORD moduleLength = ::GetModuleFileNameW(nullptr, modulePath, _countof(modulePath));
    std::filesystem::path root = moduleLength > 0 && moduleLength < _countof(modulePath)
        ? std::filesystem::path(modulePath).parent_path()
        : std::filesystem::current_path();
    expression[TaskMemoryState::workdir] = CString(root.c_str());
    memory.resourcePath = CString((root / L"res").c_str());
    CString dmResourcePath = memory.resourcePath + _T("\\");
    const long setPathResult = Dm()->SetPath(dmResourcePath.GetString());
    CString pathMessage;
    pathMessage.Format(_T("task memory resource path: %s (SetPath=%ld)"),
                       dmResourcePath.GetString(), setPathResult);
    DebugLog(pathMessage);

    expression[TaskMemoryState::ERC].Format(_T("[[%08lX]+58]+4"), hex32(memory.addresses[TaskMemoryState::ROLEB]));
    expression[TaskMemoryState::ZJID].Format(_T("[[%08lX]+64]+2C"), hex32(memory.addresses[TaskMemoryState::ROLEB]));
    expression[TaskMemoryState::m_Dqzt].Format(_T("[[%08lX]+64]+14c"), hex32(memory.addresses[TaskMemoryState::ROLEB]));
    expression[TaskMemoryState::Shuxtb].Format(_T("[[[%08lX]+64]+148]+10"), hex32(memory.addresses[TaskMemoryState::ROLEB]));
    expression[TaskMemoryState::shuxcw].Format(_T("[%08lX]+6711C"), hex32(memory.addresses[TaskMemoryState::BagJ]));
    expression[TaskMemoryState::shuxbg].Format(_T("[%08lX]+22C00"), hex32(memory.addresses[TaskMemoryState::BagJ]));
    expression[TaskMemoryState::Rwerc].Format(_T("[%08lX]+4"), hex32(memory.addresses[TaskMemoryState::RenWERC]));
    expression[TaskMemoryState::GJID].Format(_T("[[%08lX]+64]+214"), hex32(memory.addresses[TaskMemoryState::ROLEB]));
    expression[TaskMemoryState::rwxl].Format(_T("[[[[%08lX]+64]+148]+10]+8"), hex32(memory.addresses[TaskMemoryState::ROLEB]));
    expression[TaskMemoryState::rwll].Format(_T("[[[[%08lX]+64]+148]+10]+0C"), hex32(memory.addresses[TaskMemoryState::ROLEB]));
    expression[TaskMemoryState::rwlq].Format(_T("[[[[%08lX]+64]+148]+10]+%02lX"),
                                             hex32(memory.addresses[TaskMemoryState::ROLEB]), kLuqiOffset);

    if (!::GetClientRect(m_wnd->hWnd, &memory.clientRect)) {
        DebugLog(_T("task memory init: GetClientRect failed"));
    }
    const LONG clientWidth = memory.clientRect.right - memory.clientRect.left;
    const LONG clientHeight = memory.clientRect.bottom - memory.clientRect.top;
    memory.referenceWindowSize = clientWidth == 1024 && clientHeight == 768;
    CString rectMessage;
    rectMessage.Format(_T("task memory client rect: %ldx%ld"), clientWidth, clientHeight);
    DebugLog(rectMessage);
    if (!memory.referenceWindowSize) {
        DebugLog(_T("reference window size is 1024x768; task bodies should check this state"));
    }

    const bool modulesReady = memory.gameModule != 0 &&
                               memory.ceguiModule != 0 &&
                               memory.kernel32Module != 0 &&
                               memory.ceguiBaseModule != 0;
    CString myNamls;
    if (UpdateMP(expression[TaskMemoryState::Shuxtb], 0x100) && dwls != 0) {
        const char* nameBuffer = reinterpret_cast<const char*>(
            static_cast<ULONG_PTR>(dwls) +
            static_cast<ULONG_PTR>(kCharacterNameOffset));
        myNamls = ConvertAnsiText(nameBuffer);
    }

    memory.expressions[TaskMemoryState::m_Name] =
        myNamls.IsEmpty() ? _T("NULL") : myNamls;

    CString nameMessage;
    nameMessage.Format(_T("character name: %s"),
                       memory.expressions[TaskMemoryState::m_Name].GetString());
    DebugLog(nameMessage);

    const auto resolveConfigPath = [&root](const wchar_t* fileName) {
        std::filesystem::path path = root / fileName;
        if (!std::filesystem::exists(path)) {
            path = root / L"config" / fileName;
        }
        return CString(path.c_str());
    };
    const CString globalConfigPath =
        resolveConfigPath(L"全局配置.ini");
    const CString characterConfigPath =
        resolveConfigPath(L"人物配置.ini");

    const CString machineConfigPath = resolveConfigPath(L"全局配配.ini");
    tsg[TaskMemoryState::m_Djxh] = Dm()->ReadIni(
        _T("窗口"), _T("本机序号"), machineConfigPath.GetString());
    tsg[TaskMemoryState::m_Djxh].Trim();
    if (tsg[TaskMemoryState::m_Djxh].IsEmpty()) {
        tsg[TaskMemoryState::m_Djxh] = _T("NULL");
    }

    if (dwls != 0) {
        const char* menPai = reinterpret_cast<const char*>(
            static_cast<ULONG_PTR>(dwls) +
            static_cast<ULONG_PTR>(kMenPaiOffset));
        Mams[Menpaid] = static_cast<DWORD>(
            static_cast<unsigned char>(*menPai));
    }

    CString levelExpression;
    levelExpression.Format(_T("[[[[%08x]+64]+148]+10]+%08x"),
                           static_cast<DWORD>(jz[TaskMemoryState::ROLEB]),
                           static_cast<DWORD>(kLevelOffset));
    Mams[dxuem] = static_cast<DWORD>(
        Dm()->ReadInt(mmhwnd, levelExpression, 4));
    Mams[ddenj] = Mams[dxuem];
    Mams[dxuem] = (Mams[dxuem] < 50) ? 15 : 25;

    if (tsg[TaskMemoryState::m_Name] != _T("NULL")) {
        Dm()->WriteIni(_T("CQ"), _T("zhmm"),
                       tsg[TaskMemoryState::m_Name].GetString(),
                       globalConfigPath.GetString());
    }

    tsg[TaskMemoryState::luxi] = Dm()->ReadIni(
        myNamls.GetString(), _T("luxi"),
        characterConfigPath.GetString());
    Mams[benbxh] = static_cast<DWORD>(_ttoi(
        Dm()->ReadIni(myNamls.GetString(), _T("benbxh"),
                      characterConfigPath.GetString()).GetString()));
    Mams[duibxh] = static_cast<DWORD>(_ttoi(
        Dm()->ReadIni(myNamls.GetString(), _T("duibxh"),
                      characterConfigPath.GetString()).GetString()));
    tsg[TaskMemoryState::mmsp] = Dm()->ReadIni(
        myNamls.GetString(), _T("mmwp"),
        characterConfigPath.GetString());

    // The reference mutex protocol identifies the owner by character name.
    CString serverUrl = m_ini.serverUrl;
    serverUrl.Trim();
    m_wnd->teamClient.reset();
    m_wnd->wndIp = tsg[TaskMemoryState::m_Name];
    m_wnd->wndIp.Trim();
    if (!serverUrl.IsEmpty()) {
        m_wnd->teamClient = std::make_shared<CTeamServerClient>(serverUrl, 2000);
        DebugLog(_T("mutex client ready: server=%s owner=%s"),
                 serverUrl.GetString(), m_wnd->wndIp.GetString());
    } else {
        DebugLog(_T("mutex client skipped: server address is empty"));
    }

    const bool canReportOnline = tsg[TaskMemoryState::m_Djxh] != _T("NULL") &&
                                 tsg[TaskMemoryState::m_Name] != _T("NULL");
    if (canReportOnline && tsg[TaskMemoryState::luxi].GetLength() < 6) {
        CTeamServerClient server(m_ini.serverUrl, 2000);
        const CString response = server.SendOnlineCommand3(
            _T("luxi"),
            tsg[TaskMemoryState::m_Djxh],
            tsg[TaskMemoryState::m_Name]);

        OnlineLuxiResult serverResult;
        if (CTeamServerClient::ParseOnlineLuxiResponse(response, serverResult)) {
            tsg[TaskMemoryState::luxi] = serverResult.luxi;
            Mams[benbxh] = serverResult.benbxh;
            Mams[duibxh] = serverResult.duibxh;

            Dm()->WriteIni(
                tsg[TaskMemoryState::m_Name].GetString(),
                _T("luxi"),
                tsg[TaskMemoryState::luxi].GetString(),
                characterConfigPath.GetString());
            Dm()->WriteIni(
                tsg[TaskMemoryState::m_Name].GetString(),
                _T("benbxh"),
                serverResult.benbxhText.GetString(),
                characterConfigPath.GetString());
            Dm()->WriteIni(
                tsg[TaskMemoryState::m_Name].GetString(),
                _T("duibxh"),
                serverResult.duibxhText.GetString(),
                characterConfigPath.GetString());
        } else {
            DebugLog(_T("server luxi query failed: %s"), response.GetString());
        }
    }

    if (canReportOnline) {
        CTeamServerClient server(m_ini.serverUrl, 2000);
        const CString response = server.SendOnlineCommand2(
            _T("CSWB"), tsg[TaskMemoryState::m_Djxh], tsg[TaskMemoryState::m_Name]);
        DebugLog(_T("server online: index=%s name=%s server=%s response=%s"),
                 tsg[TaskMemoryState::m_Djxh].GetString(),
                 tsg[TaskMemoryState::m_Name].GetString(),
                 m_ini.serverUrl.GetString(), response.GetString());
    } else {
        DebugLog(_T("server online skipped: missing [窗口] 本机序号 or character name"));
    }

    CString initMessage;
    initMessage.Format(
        _T("character init: index=%s name=%s menpai=%lu level=%lu "
           "threshold=%lu luxi=%s benbxh=%lu duibxh=%lu mmwp=%s"),
        tsg[TaskMemoryState::m_Djxh].GetString(),
        tsg[TaskMemoryState::m_Name].GetString(),
        static_cast<unsigned long>(Mams[Menpaid]),
        static_cast<unsigned long>(Mams[ddenj]),
        static_cast<unsigned long>(Mams[dxuem]),
        tsg[TaskMemoryState::luxi].GetString(),
        static_cast<unsigned long>(Mams[benbxh]),
        static_cast<unsigned long>(Mams[duibxh]),
        tsg[TaskMemoryState::mmsp].GetString());
    DebugLog(initMessage);

    // Bag counts + deployed pet: migrated from the reference CTaskScript.cpp
    // block at line 539. Notes for this port:
    //  - the reference used ReadDataToBin() (raw bytes read at the resolved
    //    expression). UpdateMP() would add one extra dereference, so the same
    //    dmsoft call is kept here.
    //  - this client build keeps the per-page bag capacities 0x164 bytes
    //    higher than the reference client: +22C14 instead of +22AB0
    //    (live: 68 / 68 / 20 for page 1 / 2 / 3).
    {
        CString bagCountExpression;
        bagCountExpression.Format(_T("[%08lX]+22C14"),
                                  static_cast<unsigned long>(jz[TaskMemoryState::BagJ]));
        dwls = static_cast<DWORD>(Dm()->ReadDataToBin(
            targetHwnd, bagCountExpression.GetString(), 2));
        if (dwls != 0) {
            const auto* bagCounts = reinterpret_cast<const BYTE*>(
                static_cast<ULONG_PTR>(dwls));
            Mams[BagMax1] = bagCounts[0];
            Mams[BagMax2] = bagCounts[1];
            Mams[CoXuHa] = static_cast<DWORD>(Read_PetMx());// deployed pet check
            DebugLog(_T("\u6570\u91cf\uff1a%lu   %lu"),
                     static_cast<unsigned long>(Mams[BagMax1]),
                     static_cast<unsigned long>(Mams[BagMax2]));

            bag[baxue] = Read_BagX(1);
            bag[balan] = Read_BagX(2);
            bag[bacow] = Read_BagX(3);
            DebugLog(_T("\u5ba0\u7269\uff1a%lu  jxuewp:%lu"),
                     static_cast<unsigned long>(Mams[CoXuHa]),
                     static_cast<unsigned long>(bag[bacow].xuh));
        } else {
            DebugLog(_T("bag count read failed: expr=%s BagJ=0x%08lX"),
                     bagCountExpression.GetString(),
                     static_cast<unsigned long>(jz[TaskMemoryState::BagJ]));
        }
    }
    // Reference CTaskScript.cpp:570 - reset the task/NPC working tables.
    VRenw = VRENW;  VHuod = VHUOD;
    mChusrw();

    // Keep the per-window remote memory information in the global cache.
    // Layout:
    //   [QBJB]
    //   Date=yyyy-mm-dd
    //   0x00123456=1
    //
    //   [0x00123456]
    //   Nbsq=...
    //   HHKK3P=...
    //   ...
    //
    // Only the script thread touches this cache for a window.  The process
    // mutex prevents two windows initialized at the same time from losing
    // each other's handle entry.

    bool handleCached = false;
    if (memory.gameModule != 0) {
        std::scoped_lock cacheLock(g_globalConfigMutex);

        auto& address = memory.addresses;
        const CString iniPath = globalConfigPath;

        SYSTEMTIME now{};
        ::GetLocalTime(&now);
        CString today;
        today.Format(_T("%04u-%02u-%02u"),
                     static_cast<unsigned>(now.wYear),
                     static_cast<unsigned>(now.wMonth),
                     static_cast<unsigned>(now.wDay));

        CString handleKey;
        handleKey.Format(_T("0x%08lX"),
                         static_cast<unsigned long>(targetHwnd));

        CString cachedDate = Dm()->ReadIni(
            kGlobalConfigSection, kGlobalConfigDateKey, iniPath.GetString());
        if (cachedDate.IsEmpty()) {
            // Accept the lower-case spelling if an older manually-created
            // configuration used it.
            cachedDate = Dm()->ReadIni(
                kGlobalConfigSection, _T("date"), iniPath.GetString());
        }
        cachedDate.Trim();

        const CString enumResult = Dm()->EnumIniKey(
            kGlobalConfigSection, iniPath.GetString());
        std::vector<CString> handleKeys;
        int tokenPosition = 0;
        while (tokenPosition != -1) {
            CString key = enumResult.Tokenize(_T("|"), tokenPosition);
            key.Trim();
            if (!key.IsEmpty() && key.CompareNoCase(kGlobalConfigDateKey) != 0 &&
                key.CompareNoCase(_T("date")) != 0) {
                handleKeys.push_back(key);
            }
        }

        auto deleteCacheSection = [&](const CString& section) {
            // A null key deletes the entire section, including its header.
            if (!::WritePrivateProfileString(section.GetString(), nullptr,
                                              nullptr, iniPath.GetString())) {
                DebugLog(_T("global cache: failed to delete section %s (error=%lu)"),
                         section.GetString(), ::GetLastError());
            }
        };

        // Older versions removed keys but left empty handle section headers.
        // Scan sections as well as QBJB: these orphans are no longer indexed.
        const CString sections = Dm()->EnumIniSection(iniPath.GetString());
        int sectionPosition = 0;
        while (sectionPosition != -1) {
            CString section = sections.Tokenize(_T("|"), sectionPosition);
            section.Trim();
            if (section.GetLength() != 10 ||
                section.Left(2).CompareNoCase(_T("0x")) != 0 ||
                section.Mid(2).SpanIncluding(_T("0123456789abcdefABCDEF")).GetLength() != 8) {
                continue;
            }
            TCHAR values[4]{};
            if (::GetPrivateProfileSection(section.GetString(), values,
                                            _countof(values), iniPath.GetString()) == 0 &&
                values[0] == _T('\0')) {
                deleteCacheSection(section);
            }
        }

        if (cachedDate.Compare(today) != 0) {
            for (const CString& oldHandle : handleKeys) {
                deleteCacheSection(oldHandle);
                Dm()->DeleteIni(kGlobalConfigSection, oldHandle.GetString(),
                                iniPath.GetString());
            }
            Dm()->WriteIni(kGlobalConfigSection, kGlobalConfigDateKey,
                           today.GetString(), iniPath.GetString());

            CString resetMessage;
            resetMessage.Format(_T("global cache date reset: %s -> %s"),
                                cachedDate.IsEmpty() ? _T("<empty>") : cachedDate.GetString(),
                                today.GetString());
            DebugLog(resetMessage);
            handleKeys.clear();
        } else {
            // Normalize the key spelling even when the date is already
            // current; this also creates the file/section on first run.
            Dm()->WriteIni(kGlobalConfigSection, kGlobalConfigDateKey,
                           today.GetString(), iniPath.GetString());
        }

        //bool handleCached = false;
        for (const CString& savedHandle : handleKeys) {
            if (savedHandle.CompareNoCase(handleKey) == 0) {
                handleCached = true;
                break;
            }
        }

        if (handleCached) {
            const CString savedNbsq = Dm()->ReadIni(
                handleKey.GetString(), _T("Nbsq"), iniPath.GetString());
            CString savedNbsqText = savedNbsq;
            savedNbsqText.Trim();
            TCHAR* parseEnd = nullptr;
            const unsigned __int64 parsedNbsq = _tcstoui64(
                savedNbsqText.GetString(), &parseEnd, 0);
            if (parseEnd != savedNbsqText.GetString() &&
                parseEnd != nullptr && *parseEnd == _T('\0') &&
                parsedNbsq != 0) {
                address[TaskMemoryState::Nbsq] =
                    static_cast<LONGLONG>(parsedNbsq);

                CString cacheMessage;
                cacheMessage.Format(_T("global cache hit: hwnd=%s, Nbsq=%08x"),
                                    handleKey.GetString(), parsedNbsq);
                DebugLog(cacheMessage);
            } else {
                // A partially written record is treated as a cache miss so
                // the current window can repair it.
                handleCached = false;
                deleteCacheSection(handleKey);
                Dm()->DeleteIni(kGlobalConfigSection, handleKey.GetString(),
                                iniPath.GetString());
                DebugLog(_T("global cache record is invalid; rebuilding it"));
            }
        }

        if (!handleCached) {
            //const CString hookOpcode = Dm()->ReadDataAddr(
            //    targetHwnd, address[TaskMemoryState::HHKK3PM], 1);
            //if (hookOpcode.CompareNoCase(_T("E9")) == 0) {
            //    const LONGLONG displacement = Dm()->ReadIntAddr(
            //        targetHwnd, address[TaskMemoryState::HHKK3PM] + 1, 0);
            //    address[TaskMemoryState::Nbsq] =
            //        displacement + address[TaskMemoryState::HHKK3PM] + 5;
            //}

            if (address[TaskMemoryState::Nbsq] == 0) {
                address[TaskMemoryState::Nbsq] = Dm()->VirtualAllocEx(
                    targetHwnd, 0, 0x2000, 0);
            }

            CString nbsqValue;
            nbsqValue.Format(_T("%I64u"),
                             static_cast<unsigned __int64>(
                                 address[TaskMemoryState::Nbsq]));
            Dm()->WriteIni(kGlobalConfigSection, handleKey.GetString(),
                           _T("1"), iniPath.GetString());
            Dm()->WriteIni(handleKey.GetString(), _T("Nbsq"),
                           nbsqValue.GetString(), iniPath.GetString());

            const auto saveBytes = [&](LPCTSTR field, LONGLONG fieldAddress) {
                const CString bytes = Dm()->ReadDataAddr(
                    targetHwnd, fieldAddress, 8);
                Dm()->WriteIni(handleKey.GetString(), field,
                               bytes.GetString(), iniPath.GetString());
            };
            saveBytes(_T("HHKK3P"), address[TaskMemoryState::HHKK3PM]);
            saveBytes(_T("FFzhant"), address[TaskMemoryState::FFzhant]);
            saveBytes(_T("Gdebug"), address[TaskMemoryState::Gdebug]);
            saveBytes(_T("Hkwordd"), address[TaskMemoryState::Hkwordd]);
            saveBytes(_T("Hksendd"), address[TaskMemoryState::Hksendd]);

            // Ncdz is kept as the legacy alias for the allocated remote
            // block.  Older versions wrote this value beside Nbsq.
            Dm()->WriteIni(handleKey.GetString(), _T("Ncdz"),
                           nbsqValue.GetString(), iniPath.GetString());

            CString cacheMessage;
            cacheMessage.Format(_T("global cache miss: hwnd=%s, allocated Nbsq=%08x"),
                                handleKey.GetString(),
                                static_cast<unsigned __int64>(
                                    address[TaskMemoryState::Nbsq]));
            DebugLog(cacheMessage);
        }
    } else {
        DebugLog(_T("global cache skipped: Game.exe module is unavailable"));
    }

    memory.FinishInitialization(modulesReady);
    DebugLog(modulesReady
        ? _T("task memory initialization completed")
        : _T("task memory initialization completed with missing module(s)"));


    if (!handleCached) {
        //准备硬件断点
        *(DWORD*)(veh_shellcode + 0x1B) = jz[TaskMemoryState::HHKK3PM];
        *(DWORD*)(veh_shellcode + 0x27) = jz[TaskMemoryState::Nbsq] + 160;
        *(DWORD*)(veh_shellcode + 0x2F) = jz[TaskMemoryState::FFzhant];
        *(DWORD*)(veh_shellcode + 0x3B) = jz[TaskMemoryState::Nbsq] + 340;
        *(DWORD*)(veh_shellcode + 0x43) = jz[TaskMemoryState::Hksendd];
        *(DWORD*)(veh_shellcode + 0x4F) = jz[TaskMemoryState::Nbsq] + 370;
        *(DWORD*)(veh_shellcode + 0x57) = jz[TaskMemoryState::Hkwordd];
        *(DWORD*)(veh_shellcode + 0x63) = jz[TaskMemoryState::Nbsq] + 400;
        Dm()->WriteDataAddrFromBin(mmhwnd, jz[TaskMemoryState::Nbsq] + 30, (DWORD)veh_shellcode, 0x7D);


        *(DWORD*)(Tiaoz1 + 0x3) = jz[TaskMemoryState::Nbsq] + 340 + 0x15 + 0;//关键地方
        *(DWORD*)(Tiaoz1 + 0x10) = jz[TaskMemoryState::Nbsq] + 160 + 0x9B + 4;
        *(DWORD*)(Tiaoz1 + 0x20) = jz[TaskMemoryState::Nbsq] + 160 + 0x9B + 4;
        *(DWORD*)(Tiaoz1 + 0x2C) = jz[TaskMemoryState::Nbsq] + 160 + 0x9B + 8;
        *(DWORD*)(Tiaoz1 + 0x35) = jz[TaskMemoryState::BagEcx];
        *(DWORD*)(Tiaoz1 + 0x46) = jz[TaskMemoryState::Nbsq] + 160 + 0x9B + 12;
        *(DWORD*)(Tiaoz1 + 0x4B) = jz[TaskMemoryState::Nbsq] + 0x1000;//封内
        *(DWORD*)(Tiaoz1 + 0x6F) = jz[TaskMemoryState::Wsasend];
        *(DWORD*)(Tiaoz1 + 0x7A) = jz[TaskMemoryState::Nbsq] + 160 + 0x9B + 12;
        *(DWORD*)(Tiaoz1 + 0x86) = jz[TaskMemoryState::Nbsq] + 160 + 0x9B + 8;
        *(DWORD*)(Tiaoz1 + 0x90) = jz[TaskMemoryState::HHKK3P2];
        *(DWORD*)(Tiaoz1 + 0x97) = jz[TaskMemoryState::HHKK3PM] + 7 - jz[TaskMemoryState::Nbsq] - 160 - 0x9B;
        Dm()->WriteDataAddrFromBin(mmhwnd, jz[TaskMemoryState::Nbsq] + 160, (DWORD)Tiaoz1, 0x9B);//0x1000-0x1100

        *(DWORD*)(Tiaoz2 + 0x2) = jz[TaskMemoryState::Nbsq] + 340 + 0x15 + 0;
        *(DWORD*)(Tiaoz2 + 0x11) = jz[TaskMemoryState::FFzhant] + 6 - jz[TaskMemoryState::Nbsq] - 340 - 0x15;
        Dm()->WriteDataAddrFromBin(mmhwnd, jz[TaskMemoryState::Nbsq] + 340, (DWORD)Tiaoz2, 0x15);

        *(DWORD*)(SendBz + 0x2) = jz[TaskMemoryState::Nbsq] + 340 + 0x15 + 0;
        *(DWORD*)(SendBz + 0x10) = jz[TaskMemoryState::Hksendd] + 5 - jz[TaskMemoryState::Nbsq] - 370 - 0x14;
        Dm()->WriteDataAddrFromBin(mmhwnd, jz[TaskMemoryState::Nbsq] + 370, (DWORD)SendBz, 0x14);

        *(DWORD*)(PS_hhk + 0x2) = jz[TaskMemoryState::Nbsq] + 400 + 0x3B + 0;
        *(DWORD*)(PS_hhk + 0x26) = jz[TaskMemoryState::Nbsq] + 400 + 0x3B + 4;
        dwls = Dm()->ReadIntAddr(mmhwnd, jz[TaskMemoryState::Hkwordd] + 2, 4);
        *(DWORD*)(PS_hhk + 0x31) = dwls;
        *(DWORD*)(PS_hhk + 0x37) = jz[TaskMemoryState::Hkwordd] + 7 - jz[TaskMemoryState::Nbsq] - 400 - 0x3B;
        Dm()->WriteDataAddrFromBin(mmhwnd, jz[TaskMemoryState::Nbsq] + 400, (DWORD)PS_hhk, 0x3B);

        //添加硬件断点
        mmpid = Dm()->GetWindowProcessId(targetHwnd);
        mmainthread = GetMainThreadId(mmpid);
        HijackMainThread32(mmpid);

        *(DWORD*)(BenTuXuL3 + 1) = jz[TaskMemoryState::ROLEB];
        Dm()->WriteDataAddrFromBin(mmhwnd, jz[TaskMemoryState::Nbsq] + 480, (DWORD)BenTuXuL3, 0x2A);//需要检查

		*(DWORD*)(Opennp + 23) = FENGT - 530 - 27;
        Dm()->WriteDataAddrFromBin(mmhwnd, jz[TaskMemoryState::Nbsq] + 530, (DWORD)Opennp, 27);//需要检查

		*(DWORD*)(Xuanz + 20) = FENGT - 560 - 0x18;
		Dm()->WriteDataAddrFromBin(mmhwnd, jz[TaskMemoryState::Nbsq] + 560, (DWORD)Xuanz, 0x18);//需要检查

        *(DWORD*)(WsUseSk2 + 0x1D) = jz[TaskMemoryState::ROLEB];
		*(DWORD*)(WsUseSk2 + 66) = FENGT - 590 - 0x46;
		Dm()->WriteDataAddrFromBin(mmhwnd, jz[TaskMemoryState::Nbsq] + 590, (DWORD)WsUseSk2, 0x46);//需要检查

        *(DWORD*)(LuaDostring + 1) = jz[TaskMemoryState::Nbsq] + 0x1500;
        *(DWORD*)(LuaDostring + 6) = jz[TaskMemoryState::luaState];
        *(DWORD*)(LuaDostring + 0xC) = jz[TaskMemoryState::Luadost];
        Dm()->WriteDataAddrFromBin(mmhwnd, jz[TaskMemoryState::Nbsq] + 670, (DWORD)LuaDostring, 0x14);//

		*(DWORD*)(ClickNpc + 37) = FENGT - 700 - 0x29;
		Dm()->WriteDataAddrFromBin(mmhwnd, jz[TaskMemoryState::Nbsq] + 700, (DWORD)ClickNpc, 0x29);//需要检查

		*(DWORD*)(CwCz + 37) = FENGT - 750 - 0x29;
		Dm()->WriteDataAddrFromBin(mmhwnd, jz[TaskMemoryState::Nbsq] + 750, (DWORD)CwCz, 0x29);//需要检查

		*(DWORD*)(UseBag + 80) = FENGT - 800 - 0x54;
		Dm()->WriteDataAddrFromBin(mmhwnd, jz[TaskMemoryState::Nbsq] + 800, (DWORD)UseBag, 0x54);//需要检查

		*(DWORD*)(KuaTuXuL + 26) = jz[TaskMemoryState::KuaTuX];
        Dm()->WriteDataAddrFromBin(mmhwnd, jz[TaskMemoryState::Nbsq] + 900, (DWORD)KuaTuXuL, 0x31);

		*(DWORD*)(LY_gou + 77 - 4) = FENGT - 960 - 77;
        Dm()->WriteDataAddrFromBin(mmhwnd, jz[TaskMemoryState::Nbsq] + 960, (DWORD)LY_gou, 77);//需要检查 -- +0字节

		*(DWORD*)(LY_chu + 28 - 4) = FENGT - 1050 - 28;
        Dm()->WriteDataAddrFromBin(mmhwnd, jz[TaskMemoryState::Nbsq] + 1050, (DWORD)LY_chu, 28);//需要检查 -- +0字节

		*(DWORD*)(PS_gou + 115 - 4) = FENGT - 1090 - 115;
        Dm()->WriteDataAddrFromBin(mmhwnd, jz[TaskMemoryState::Nbsq] + 1090, (DWORD)PS_gou, 115);//需要检查

		*(DWORD*)(PS_chu + 115 - 4) = FENGT - 1210 - 115;
        Dm()->WriteDataAddrFromBin(mmhwnd, jz[TaskMemoryState::Nbsq] + 1210, (DWORD)PS_chu, 115);//需要检查

		*(DWORD*)(Dui_Gens + 13 - 4) = FENGT - 1330 - 13;
		Dm()->WriteDataAddrFromBin(mmhwnd, jz[TaskMemoryState::Nbsq] + 1330, (DWORD)Dui_Gens, 13);//需要检查

		*(DWORD*)(Renw_Jie + 34 - 4) = FENGT - 1350 - 34;
        Dm()->WriteDataAddrFromBin(mmhwnd, jz[TaskMemoryState::Nbsq] + 1350, (DWORD)Renw_Jie, 34);//需要检查 -- +0字节

		*(DWORD*)(Renw_Fan + 20 - 4) = FENGT - 1390 - 20;
        Dm()->WriteDataAddrFromBin(mmhwnd, jz[TaskMemoryState::Nbsq] + 1390, (DWORD)Renw_Fan, 20);//需要检查 -- +0字节

		*(DWORD*)(Renw_Jix + 34 - 4) = FENGT - 1420 - 34;
        Dm()->WriteDataAddrFromBin(mmhwnd, jz[TaskMemoryState::Nbsq] + 1420, (DWORD)Renw_Jix, 34);//需要检查 -- +0字节

		*(DWORD*)(Renw_Wan + 41 - 4) = FENGT - 1460 - 41;
        Dm()->WriteDataAddrFromBin(mmhwnd, jz[TaskMemoryState::Nbsq] + 1460, (DWORD)Renw_Wan, 41);//需要检查 -- +0字节


		*(DWORD*)(Renw_XunL + 0x19) = jz[TaskMemoryState::ROLEB];
        *(DWORD*)(Renw_XunL + 0x3B) = jz[TaskMemoryState::Xguih];
        *(DWORD*)(Renw_XunL + 0x5B) = jz[TaskMemoryState::Nbsq] + 1510 + 108;
		Dm()->WriteDataAddrFromBin(mmhwnd, jz[TaskMemoryState::Nbsq] + 1510, (DWORD)Renw_XunL, 106);

		*(DWORD*)(BG_jiaoh + 0x19 - 4) = FENGT - 1630 - 0x19;
		Dm()->WriteDataAddrFromBin(mmhwnd, jz[TaskMemoryState::Nbsq] + 1630, (DWORD)BG_jiaoh, 0x19);//需要检查 -- +0字节

		*(DWORD*)(HkWord2 + 0x2) = jz[TaskMemoryState::Nbsq] + 1660 + 0x42 + 0;//开关
		*(DWORD*)(HkWord2 + 0x1E) = jz[TaskMemoryState::Nbsq] + 0x1500;//内容
        *(DWORD*)(HkWord2 + 0x24) = jz[TaskMemoryState::Memccp];//拷贝函数
        *(DWORD*)(HkWord2 + 0x2D) = jz[TaskMemoryState::Nbsq] + 1660 + 0x42 + 4;//结果标志
		dwls = Dm()->ReadIntAddr(mmhwnd, jz[TaskMemoryState::Hkwordd] + 2, 4);
		*(DWORD*)(HkWord2 + 0x38) = dwls;
		*(DWORD*)(HkWord2 + 0x42-4) = jz[TaskMemoryState::Hkwordd] + 7 - jz[TaskMemoryState::Nbsq] - 1660 - 0x42;
		Dm()->WriteDataAddrFromBin(mmhwnd, jz[TaskMemoryState::Nbsq] + 1660, (DWORD)HkWord2, 0x42);
    }


    {
        ck[Manager] = memory.ceguiBaseModule + MANAGEP;
        ck[Frame_Zhy] = FindCeguiWindowByName(_T("ZhouHuoYue_FrameFull"));
        ck[ZhouHuoYue_Condition_Text] = FindCeguiWindowByName( _T("ZhouHuoYue_Condition_Text"));
        ck[PlayerFrame_Icon] = FindCeguiWindowByName(_T("PlayerFrame_Icon"));
        ck[SceneTrans_Frame] = FindCeguiWindowByName(_T("SceneTrans_Frame"));
        ck[Packet_Frame] = FindCeguiWindowByName(_T("Packet_Frame"));
        ck[Frame_Time] = FindCeguiWindowByName(_T("MiniMap_ChineseTime"));
        ck[MiniMap_MiniMap_NetStatus] = FindCeguiWindowByName(_T("MiniMap_MiniMap_NetStatus"));
		ck[PaoShang_Frame] = FindCeguiWindowByName(_T("PaoShang_Frame"));//周末活动那个跑
        ck[Shop_Frame] = FindCeguiWindowByName(_T("Shop_Frame"));
        ck[PaoShang_Goumai_Tuijian1] = FindCeguiWindowByName(_T("PaoShang_Goumai_Tuijian1"));
        ck[PaoShang_Goumai_Tuijian2] = FindCeguiWindowByName(_T("PaoShang_Goumai_Tuijian2"));
        ck[PaoShang_Goumai_Tuijian3] = FindCeguiWindowByName(_T("PaoShang_Goumai_Tuijian3"));
		ck[PaoShang_ItemInfo1_Price] = FindCeguiWindowByName(_T("PaoShang_ItemInfo1_Price"));
		ck[PaoShang_ItemInfo2_Price] = FindCeguiWindowByName(_T("PaoShang_ItemInfo2_Price"));
		ck[PaoShang_ItemInfo3_Price] = FindCeguiWindowByName(_T("PaoShang_ItemInfo3_Price"));
		ck[MessageBox_Self_Frame] = FindCeguiWindowByName(_T("MessageBox_Self_Frame"));
		ck[MessageBox_Self_Text] = FindCeguiWindowByName(_T("MessageBox_Self_Text"));
		ck[AcceptBox_Frame] = FindCeguiWindowByName(_T("AcceptBox_Frame"));
		ck[AcceptBox_Text] = FindCeguiWindowByName(_T("AcceptBox_Text"));
        ck[MiniMap_Placename] = FindCeguiWindowByName(_T("MiniMap_Placename"));
        ck[Quest_Frame] = FindCeguiWindowByName(_T("Quest_Frame"));
        ck[QuitRelative_Frame_Back] = FindCeguiWindowByName(_T("QuitRelative_Frame_Back"));
        ck[Chat_Frame] = FindCeguiWindowByName(_T("Chat_Frame_History__auto_parent__"));

    }

    return modulesReady;
}

void CTaskScript::RunTaskStub(LPCTSTR functionName, LPCTSTR taskName) {
    CString message;
    message.Format(_T("task dispatch: %s (%s)"), functionName, taskName);
    DebugLog(message);
}

bool CTaskScript::OnLoop() {
    const int routeDirection = m_routeRequest.exchange(-1, std::memory_order_acq_rel);
    if (routeDirection != -1) {
        const CString route = tsg[TaskMemoryState::luxi];
        const bool useRoute3 = ShouldUseRoute3(route);
        CString message = _T("route run requested: direction=");
        message += routeDirection == 1 ? _T("reverse") : _T("forward");
        message += _T(" mode=");
        message += useRoute3 ? _T("wLianxXuLPP3") : _T("wLianxXuLPP");
        DebugLog(message);

        // -1: locate the current map before starting either route direction.
        const int result = useRoute3
            ? wLianxXuLPP3(route, routeDirection, -1)
            : wLianxXuLPP(route, routeDirection, -1);
        message = _T("route run finished: direction=");
        message += routeDirection == 1 ? _T("reverse") : _T("forward");
        message += _T(" result=");
        message += CString(std::to_wstring(result).c_str());
        DebugLog(message);
        SleepEx(10);
        return !m_control->IsStopping();
    }

    // The concrete task layer owns the loop. The two layers above it only
    // provide the thread entry point and the virtual dispatch path.
    if (m_testRequested.exchange(false, std::memory_order_acq_rel)) {
        DebugLog(_T("test dispatch: Fub_Test"));
        Fub_Test();
        SleepEx(10);
        return !m_control->IsStopping();
    }

    if (!m_schedule) {
        SleepEx(100);
        return !m_control->IsStopping();
    }

    const auto snapshot = m_schedule->Read();
    if (snapshot.generation != m_seenGeneration) {
        m_seenGeneration = snapshot.generation;
        m_taskIndex = 0;
        m_sequenceActive = false;
        m_sequenceComplete = false;
    }

    if (!snapshot.running || snapshot.taskIds.empty()) {
        if (m_sequenceActive) DebugLog(_T("task sequence stopped"));
        m_sequenceActive = false;
        m_sequenceComplete = false;
        m_taskIndex = 0;
        SleepEx(100);
        return !m_control->IsStopping();
    }

    if (!m_sequenceActive) {
        m_sequenceActive = true;
        DebugLog(_T("task sequence started for this window"));
    }

    if (m_taskIndex >= snapshot.taskIds.size()) {
        if (!m_sequenceComplete) {
            m_sequenceComplete = true;
            DebugLog(_T("task sequence completed for this window"));
            // This runtime has finished its copy of the queue. The manager
            // keeps other selected windows running until they finish too.
            m_schedule->Stop();
        }
        SleepEx(200);
        return !m_control->IsStopping();
    }

    const int taskId = snapshot.taskIds[m_taskIndex];
    switch (taskId) {
    case 0:  Fub_Shuaqi();      break; // \u68cb\u5c40
    case 1:  Fun_DFanz();       break; // \u53cd\u8d3c
    case 2:  Fub_DLaoY();       break; // \u8001\u4e00
    case 3:  Fub_DLaoE();       break; // \u8001\u4e8c
    case 4:  Fub_DLaoS();       break; // \u8001\u4e09
    case 5:  Fub_Attalon();     break; // \u6253\u602a
    case 6:  Fub_Jihe();        break; // \u96c6\u5408
    case 7:  Fub_DXinY();       break; // \u65b0\u4e00
    case 8:  Fub_DXinE();       break; // \u65b0\u4e8c
    case 9:  Fub_DXinS();       break; // \u65b0\u4e09
    case 10: Fub_DHuoD();       break; // \u65b0\u6d3b\u52a8
    case 11: Fub_Shaxg();       break; // \u6740\u661f
    case 12: Fub_DYanz();       break; // \u71d5\u5b50
    case 13: Fub_DPiaom();      break; // \u7f25\u7f08
    case 14: Fun_Luojx();       break; // \u6d1b\u52a0\u8840
    case 15: Fun_Shengj();      break; // \u5347\u7ea7
    case 16: Fun_ZhonD();       break; // \u79cd\u5730
    case 17: Fun_Paosc();       break; // \u8dd1\u5546
    case 18: Fun_Shanj();       break; // \u4e0a\u67b6
    case 19: Fun_Datuu();       break; // \u6253\u56fe
    case 20: Fun_Kuozh();       break; // \u6269\u5f20
    case 21: Fun_Cujuu();       break; // \u8e74\u97a0
    case 22: Fub_SaoDg();       break; // \u626b\u8361
    case 23: Fun_Chushou();     break; // \u51fa\u552e
    case 24: Fun_DFanzSingle(); break; // \u5355\u53cd\u8d3c
    case 25: Fun_Zhengl();      break; // \u6574\u7406
    case 26: Fub_DYanz2();      break; // \u5355\u71d52
    default:
        DebugLog(_T("task dispatch: unknown task id"));
        break;
    }

    ++m_taskIndex;
    SleepEx(10);
    return !m_control->IsStopping();
}

CString CTaskScript::CurrentTaskName() const
{
    if (!m_schedule) return CString();
    const auto snapshot = m_schedule->Read();
    if (!snapshot.running || snapshot.taskIds.empty() ||
        m_taskIndex >= snapshot.taskIds.size()) return CString();

    switch (snapshot.taskIds[m_taskIndex]) {
    case 0:  return _T("Fub_Shuaqi");
    case 1:  return _T("Fun_DFanz");
    case 2:  return _T("Fub_DLaoY");
    case 3:  return _T("Fub_DLaoE");
    case 4:  return _T("Fub_DLaoS");
    case 5:  return _T("Fub_Attalon");
    case 6:  return _T("Fub_Jihe");
    case 7:  return _T("Fub_DXinY");
    case 8:  return _T("Fub_DXinE");
    case 9:  return _T("Fub_DXinS");
    case 10: return _T("Fub_DHuoD");
    case 11: return _T("Fub_Shaxg");
    case 12: return _T("Fub_DYanz");
    case 13: return _T("Fub_DPiaom");
    case 22: return _T("Fub_SaoDg");
    case 26: return _T("Fub_DYanz2");
    case 14: return _T("Fun_Luojx");
    case 15: return _T("Fun_Shengj");
    case 16: return _T("Fun_ZhonD");
    case 17: return _T("Fun_Paosc");
    case 18: return _T("Fun_Shanj");
    case 19: return _T("Fun_Datuu");
    case 20: return _T("Fun_Kuozh");
    case 21: return _T("Fun_Cujuu");
    case 23: return _T("Fun_Chushou");
    case 24: return _T("Fun_DFanzSingle");
    case 25: return _T("Fun_Zhengl");
    default: return _T("UnknownTask");
    }
}

void CTaskScript::Fub_Shuaqi()
{
    if (!m_control || m_control->IsStopping() || !m_ready.load() || !Dm() ||
        !WaitIfPaused()) return;

    const ScopedActionStatuses actionStatuses(*this,
        { 213, 14, 18, 22, 12 },   // vecrun：禁止移动的自身状态
        { 213, 14, 18, 22, 12 }); // vecskill：禁止技能（含基础攻击）的自身状态

    const auto readChessKillCount = [this]() -> int {
        constexpr DWORD kMessageOwnerRva = 0x7CF478;
        const LONGLONG game = Dm()->GetModuleBaseAddr(mmhwnd, _T("Game.exe"));
        if (game < 0x10000 || game > 0xFFFFFFFFLL - kMessageOwnerRva) return -1;

        const DWORD owner = Read_Dw(static_cast<DWORD>(game) + kMessageOwnerRva);
        if (owner < 0x10000) return -1;
        const DWORD buffer = Read_Dw(owner + 0x3C);
        if (buffer < 0x10000 || buffer > 0xFFFFFF00) return -1;

        const WORD byteLength = static_cast<WORD>(Read_Dw(buffer));
        if (byteLength == 0 || byteLength > 255) return -1;
        CString text = Dm()->ReadStringAddr(mmhwnd, buffer + 2, 0, byteLength);
        text.Trim();

        const CString prefix = _T("\u4f60\u5df2\u6740\u6b7b\u68cb\u5b50");
        if (text.Left(prefix.GetLength()) != prefix) return -1;
        const int slash = text.Find(_T('/'), prefix.GetLength());
        if (slash <= prefix.GetLength() || text.Mid(slash + 1) != _T("200")) return -1;

        const CString digits = text.Mid(prefix.GetLength(), slash - prefix.GetLength());
        if (digits.GetLength() > 3) return -1;
        for (int index = 0; index < digits.GetLength(); ++index) {
            if (digits[index] < _T('0') || digits[index] > _T('9')) return -1;
        }
        const int count = _ttoi(digits);
        return count <= 200 ? count : -1;
    };

    const std::regex chessNames = NerbCs(40, { 0, 0 }, _T(".*\u68cb.*"));
    const CString bossName = _T("\u8fdc\u53e4\u68cb\u9b42");
    int killCount = readChessKillCount();
    int lastMessageCount = killCount;
    bool movedAt38 = false;
    bool movedAt108 = false;
    DWORD bossNode = 0;
    CString bossHealth;
    int bossMissingScans = 0;

    while (!m_control->IsStopping()) {
        if (!WaitIfPaused()) return;

        // This cache is the most recent script message; unrelated prompts are ignored.
        const int latestCount = readChessKillCount();
        if (latestCount >= 0) {
            if (lastMessageCount >= 0 && latestCount < lastMessageCount) {
                movedAt38 = false;
                movedAt108 = false;
            }
            if (latestCount != lastMessageCount)
                DebugLog(_T("chess kills=%d/200"), latestCount);
            lastMessageCount = latestCount;
            killCount = latestCount;
        }
        if (!movedAt108 && killCount >= 108) {
            movedAt38 = true;
            movedAt108 = true;
            wBenTuXuL3f(46.0f, 67.0f);
        } else if (!movedAt38 && killCount >= 38) {
            movedAt38 = true;
            wBenTuXuL3f(42.0f, 86.0f);
        }
        if (m_control->IsStopping()) return;

        const std::vector<Monst> attackable = FindAttackableMonsters(
            Read_Dw(tsg[TaskMemoryState::ERC]), chessNames, 40.0f, false);
        for (const Monst& monster : attackable) {
            if (monster.mname == bossName) {
                bossNode = monster.Dibu;
                bossHealth = monster.cxue;
                break;
            }
        }

        if (!attackable.empty()) KillMonsterBs({ attackable.front() });
        if (m_control->IsStopping()) return;

        if (bossNode != 0) {
            if (!bossHealth.IsEmpty() && UpdateMP(bossHealth, 0x100)) {
                const float health = *reinterpret_cast<const float*>(
                    static_cast<ULONG_PTR>(dwls) + 0x08);
                if (health <= 0.0f) break;
            }
            bossMissingScans = NerExist(bossNode) ? 0 : bossMissingScans + 1;
            if (bossMissingScans >= 2) break;
        }

        SleepEx(attackable.empty() ? 1000 : 200);
    }

    if (!m_control->IsStopping()) DebugLog(_T("chess boss defeated"));
}
void CTaskScript::Fun_DFanz()
{
    if (!m_control || !m_ready.load() || !Dm()) return;
    constexpr int kInstanceMap = 0xAA;
    const auto initialSchedule = m_schedule ? m_schedule->Read() : CTaskSchedule::Snapshot{};
    const auto active = [this, initialSchedule]() {
        if (m_control->IsStopping() || !WaitIfPaused()) return false;
        // Direct Fub_Test calls also work when no sequence is running.
        if (!initialSchedule.running || !m_schedule) return true;
        const auto current = m_schedule->Read();
        return current.running && current.generation == initialSchedule.generation;
    };
    if (!active()) return;

    const DWORD selfIndex = Read_Ziji();
    if (selfIndex == MAXDWORD) {
        LogD(_T("fanz: team/self not ready"));
        return;
    }
    const bool leader = selfIndex == 0;
    const auto readPosition = [this](Point2f& p) {
        if (!UpdateMP(static_cast<DWORD>(jz[TaskMemoryState::jYcdiz]), 0x100) || !dwls)
            return false;
        p.x = *reinterpret_cast<const float*>(static_cast<ULONG_PTR>(dwls) + 0x54);
        p.y = *reinterpret_cast<const float*>(static_cast<ULONG_PTR>(dwls) + 0x5C);
        return std::isfinite(p.x) && std::isfinite(p.y) && p.x > 0 && p.y > 0;
    };
    const auto distance = [](Point2f a, Point2f b) {
        const float dx = a.x - b.x, dy = a.y - b.y;
        return std::sqrt(dx * dx + dy * dy);
    };
    // DCM_CX5 BianL.h: 反贼N. Working route data belongs to this invocation.
    std::vector<std::vector<Point2f>> instanceRoutes = {
        {{76.4f,101.9f},{86.2f,86.7f},{99.2f,82.3f}},
        {{103.7f,57.3f},{109.5f,45.3f}},
        {{100.4f,22.6f},{83.2f,29.1f}}, {{58.6f,23.3f}},
        {{54.3f,24.0f},{31.3f,25.9f}}, {{22.9f,29.7f},{41,39}},
        {{39.4f,76.2f}}, {{30,85},{22.7f,97.3f}}
    };
    const auto runInstance = [&]() -> bool {
        LogD(_T("fanz: waiting for local instance load"));
        int readySamples = 0;
        while (active()) {
            if (Read_DTID() != kInstanceMap && !IsWndGuot()) return true;
            Point2f p{};
            if (Read_DTID() == kInstanceMap && !IsWndGuot() && readPosition(p)) {
                if (++readySamples >= 4) break;
            } else readySamples = 0;
            SleepEx(250);
        }
        if (!active()) return false;

        // 本机过图完成后再等待两秒，给场景初始化留出时间。
        SleepEx(2000);
        if (!active()) return false;

        // Each client cancels its own follow AFTER its local load completes.
        // A slow client may receive the follow state later: require stable zero.
        bool cancelSent = false;
        ULONGLONG lastCancel = 0;
        int clearSamples = 0;
        while (active()) {
            if (Read_DTID() != kInstanceMap) return true;
            Point2f p{};
            if (IsWndGuot() || !readPosition(p)) {
                clearSamples = 0;
                SleepEx(250);
                continue;
            }
            const ULONGLONG now = GetTickCount64();
            const int following = Read_GENS();
            if ((!cancelSent || following != 0) && (!lastCancel || now - lastCancel >= 1000)) {
                cancelSent = wDui_Gens(0) == 0;
                lastCancel = GetTickCount64();
                clearSamples = 0;
            } else if (cancelSent && following == 0) {
                if (++clearSamples >= 6) break;
            } else clearSamples = 0;
            SleepEx(250);
        }
        if (!active()) return false;
        LogD(_T("fanz: local follow cleared"));
        const int result = KillMonst_Fz(_T("贼兵喽啰|贼兵头目"), instanceRoutes, active);
        if (!active() || result == -1) return false;

        // Preserve the reference leader's regroup command after the fight.
        if (leader && Read_DTID() == kInstanceMap && !IsWndGuot()) {
            wRideOn(1);
            SleepEx(1000);
            if (!active()) return false;
            wDui_Gens(1);
        }
        while (active() && (Read_DTID() == kInstanceMap || IsWndGuot())) SleepEx(500);
        return active();
    };

    if (!leader) {
        LogD(_T("fanz: member waiting for instance"));
        while (active()) {
            if (Read_DTID() == kInstanceMap && !runInstance()) return;
            SleepEx(250);
        }
        return;
    }

    const bool startedInside = Read_DTID() == kInstanceMap;
    if (startedInside && !runInstance()) return;
    while (active() && IsWndGuot()) SleepEx(250);
    if (!active()) return;
    const int outdoorMap = Read_DTID();

    // Existing per-map configuration contains the reference outdoor patrols.
    CString routeFile = tsg[TaskMemoryState::workdir] + _T("/FanXiao.ini");
    if (!std::filesystem::exists(std::filesystem::path(routeFile.GetString())))
        routeFile = tsg[TaskMemoryState::workdir] + _T("/config/FanXiao.ini");
    CString mapKey;
    mapKey.Format(_T("%d"), outdoorMap);
    const CString routeText = Dm()->ReadIni(_T("mapFanzei"), mapKey, routeFile);
    std::vector<Point2f> patrol;
    int tokenPos = 0;
    for (CString token = routeText.Tokenize(_T("|"), tokenPos); !token.IsEmpty();
         token = routeText.Tokenize(_T("|"), tokenPos)) {
        Point2f point{};
        if (_stscanf_s(token, _T("%f,%f"), &point.x, &point.y) != 2 ||
            !std::isfinite(point.x) || !std::isfinite(point.y) || point.x <= 0 || point.y <= 0) {
            LogD(_T("fanz: invalid patrol coordinate"));
            return;
        }
        patrol.push_back(point);
    }

    if (!startedInside) {
        std::int64_t serverTime = 0;
        if (!ReadServerTime(serverTime)) { LogD(_T("fanz: server clock unavailable")); return; }
        // Same late-start rule as CX5, but use the server clock instead of Hk_Fanz.
        if ((serverTime % 3600) / 60 > 25) {
            const std::int64_t triggerTime = (serverTime / 3600 + 1) * 3600 - 1;
            LogD(_T("fanz: wait %lld s to xx:59:59"), triggerTime - serverTime);
            while (active()) {
                if (Read_DTID() != outdoorMap) return;
                if (ReadServerTime(serverTime) && serverTime >= triggerTime) break;
                SleepEx(100);
            }
        }
    }
    if (!active()) return;
    LogD(_T("fanz: nearby scan 10 s"));
    const ULONGLONG scanStarted = GetTickCount64();
    ULONGLONG retryAfter = 0;
    DWORD failedNpc = 0;
    const auto scanNpc = [&](DWORD wantedId, Monst& found) -> bool {
        // This is an entrance NPC; do not filter it by HP or attackability.
        const regex name = NerbCs(40, {0, 0}, _T("造反恶贼"));
        NerbMonst222(Read_Dw(tsg[TaskMemoryState::ERC]), name, false);
        bool matched = false;
        for (const Monst& npc : vMonst) {
            if (npc.ID <= 0 || (wantedId && static_cast<DWORD>(npc.ID) != wantedId)) continue;
            if (!wantedId && static_cast<DWORD>(npc.ID) == failedNpc && GetTickCount64() < retryAfter) continue;
            if (!matched || npc.Juli < found.Juli) { found = npc; matched = true; }
        }
        return matched;
    };
    const auto enterNpc = [&](Monst npc) -> bool {
        const ULONGLONG approachStarted = GetTickCount64();
        ULONGLONG lastMove = 0;
        bool arrived = false;
        while (active() && Read_DTID() == outdoorMap && !IsWndGuot() &&
               GetTickCount64() - approachStarted < 20000) {
            Monst fresh{};
            if (!scanNpc(static_cast<DWORD>(npc.ID), fresh) || fresh.Dibu != npc.Dibu) return false;
            npc = fresh;
            Point2f p{};
            if (readPosition(p)) {
                if (distance(p, npc.pos) < 3.0f) { arrived = true; break; }
                if (!lastMove || (GetTickCount64() - lastMove >= 1500 && Read_RWZT(true) != 2)) {
                    wBenTuXuL2(npc.pos.x, npc.pos.y);
                    lastMove = GetTickCount64();
                }
            }
            SleepEx(200);
        }
        if (!active() || !arrived) return false;
        // Discard any previous NPC dialog before waiting for this NPC's reply.
        if (IsWndOpen2(Quest_Frame)) {
            mLua_Dostr(CLOSE_NPC);
            const ULONGLONG closeStarted = GetTickCount64();
            while (active() && IsWndOpen2(Quest_Frame) && GetTickCount64() - closeStarted < 3000)
                SleepEx(100);
            if (!active() || IsWndOpen2(Quest_Frame)) return false;
        }
        if (Read_DTID() != outdoorMap || IsWndGuot()) return false;
        aMonst = npc;
        wXuanZg(static_cast<DWORD>(npc.ID));
        if (!active()) return false;
        if (wOpenMan(static_cast<DWORD>(npc.ID)) != 0) return false;
        const ULONGLONG dialogStarted = GetTickCount64();
        bool dialogReady = false;
        while (active() && GetTickCount64() - dialogStarted < 5000) {
            if (Read_DTID() == kInstanceMap) return true;
            if (Read_DTID() != outdoorMap || IsWndGuot()) break;
            if (IsWndOpen2(Quest_Frame) && !Read_NpcT(1).IsEmpty()) { dialogReady = true; break; }
            SleepEx(100);
        }
        if (!active() || !dialogReady) return false;
        wClickNpc(1); // The reference entrance uses the first dialog option.
        const ULONGLONG entryStarted = GetTickCount64();
        bool transitionSeen = false;
        while (active() && GetTickCount64() - entryStarted < 90000) {
            if (Read_DTID() == kInstanceMap) return true;
            if (IsWndGuot()) {
                transitionSeen = true;
            } else if (Read_DTID() != outdoorMap) {
                return false;
            }
            // Poll every 100 ms for up to two seconds, regardless of NPC presence.
            if (!transitionSeen && GetTickCount64() - entryStarted >= 2000) {
                return false; // The caller resumes the entrance-NPC search.
            }
            SleepEx(transitionSeen ? 250 : 100);
        }
        return false;
    };

    size_t patrolIndex = 0;
    bool patrolLogged = false, moveSent = false;
    ULONGLONG pointStarted = 0, lastMove = 0;
    while (active()) {
        if (Read_DTID() == kInstanceMap) {
            if (!runInstance()) return;
            moveSent = false;
            continue;
        }
        if (IsWndGuot()) { SleepEx(250); continue; }
        if (Read_DTID() != outdoorMap) { LogD(_T("fanz: outdoor map changed")); return; }
        Monst npc{};
        if (scanNpc(0, npc)) {
            LogD(_T("fanz: found id=%d"), npc.ID);
            if (enterNpc(npc)) {
                if (!runInstance()) return;
                failedNpc = 0;
            } else {
                if (!active()) return;
                LogD(_T("fanz: entry failed id=%d"), npc.ID);
                failedNpc = static_cast<DWORD>(npc.ID);
                retryAfter = GetTickCount64() + 5000;
            }
            moveSent = false;
            continue;
        }
        if (GetTickCount64() - scanStarted >= 10000) {
            if (!patrolLogged) { LogD(_T("fanz: patrol map=%d points=%u"), outdoorMap, static_cast<unsigned>(patrol.size())); patrolLogged = true; }
            if (patrolIndex >= patrol.size()) { LogD(_T("fanz: patrol finished")); return; }
            const Point2f point = patrol[patrolIndex];
            Point2f current{};
            if (readPosition(current) && distance(current, point) < 5.0f) {
                ++patrolIndex;
                moveSent = false;
            } else if (moveSent && GetTickCount64() - pointStarted >= 45000) {
                LogD(_T("fanz: patrol point timeout %.1f %.1f"), point.x, point.y);
                ++patrolIndex;
                moveSent = false;
            } else if (!moveSent || (GetTickCount64() - lastMove >= 2000 && Read_RWZT(true) != 2)) {
                if (!moveSent) pointStarted = GetTickCount64();
                wBenTuXuL35(point.x, point.y);
                lastMove = GetTickCount64();
                moveSent = true;
            }
        }
        SleepEx(100);
    }
}
void CTaskScript::Fub_DLaoY()
{
    if (!m_control || m_control->IsStopping() || !m_ready.load() || !Dm() || !WaitIfPaused()) return;

    constexpr int kMapId = 0x4F;
    constexpr DWORD kQuestId = 0xC3B4;
    // 参考项目 vvD2 的两个队长站位坐标被抹掉了（原文只剩“老三三”），
    // 这里先留 0 表示未配置；未配置时只记日志不乱跑，实测后再填。
    const std::vector<Point2f> leaderSpots = { {0.0f, 0.0f}, {0.0f, 0.0f} };

    const ScopedActionStatuses actionStatuses(*this,
        { 213, 14, 18, 22, 12 },   // vecrun
        { 213, 14, 18, 22, 12 });  // vecskill

    regex targetRegex = NerbCs(40, { 0, 0 }, _T(".*宋兵|.*都统|逃窜匪类|.*余毒"));
    tCheckXj();// 检查血祭（宠物技能）

    // 任务已接（或已在副本地图）就直接跳到 labelks 进图等待，不再走接任务流程
    if (mRe_Renx(kQuestId) == -1 && Read_DTID() != kMapId) {
        mRenw_jie(F_LY);
    } else {
        LogD(_T("Fub_DLaoY: quest accepted or already inside, skip mRenw_jie"));
    }
    while (!m_control->IsStopping() && Read_DTID() != kMapId) SleepEx(1000);
    if (m_control->IsStopping()) return;
    SleepEx(3000);

    const int taskIndex = mRe_Renx(kQuestId);
    const int groupIndex = (Read_Ziji() % 2 == 0) ? 0 : 1;
    bool spotDone = false, wentToSpot = false, phaseBack = false, bossStage = false;

    while (!m_control->IsStopping()) {
        if (!WaitIfPaused()) return;
        if (Read_GENS() || IsWndGuot() || Read_DTID() != kMapId) {
            SleepEx(1000); continue;
        }

        if (!spotDone && mRenw_wc(taskIndex, 4)) {
            LogD(_T("4 wc")); spotDone = true;
            if (groupIndex >= 0 && groupIndex < static_cast<int>(leaderSpots.size()) &&
                (leaderSpots[groupIndex].x != 0.0f || leaderSpots[groupIndex].y != 0.0f)) {
                wBenTuXuL3f(leaderSpots[groupIndex].x, leaderSpots[groupIndex].y);
            } else {
                LogD(_T("Fub_DLaoY: leader spot not configured, skip move"));
            }
            wentToSpot = true;
            targetRegex = NerbCs(40, { 0, 0 }, _T("逃窜匪类"));
        }
        if (!phaseBack && mRenw_wc(taskIndex, 2) == 10) {
            phaseBack = true; wentToSpot = false;
            targetRegex = NerbCs(40, { 0, 0 }, _T(".*宋兵|.*都统|.*余毒"));
            LogD(_T("2 wc"));
        }
        if (mRenw_wc(taskIndex, 0)) { LogD(_T("0 wc")); break; }
        if (!bossStage && mRenw_wc(taskIndex, 1) == 50) { bossStage = true; LogD(_T("1 wc")); }

        const int found = NerbMonst2(Read_Dw(tsg[TaskMemoryState::ERC]), targetRegex);
        if (found == 0) {
            if (!wentToSpot) RunTeam(2);
            SleepEx(500); continue;
        }

        for (const Monst& mols : vMonst) {
            const int monsterState = Read_MoZTT(mols.Dibu);
            if (mols.mname == _T("伪宋军都统") && monsterState == 0) continue;
            LogD(_T("当前：%s"), mols.mname.GetString());
            if (mols.mname == _T("逃窜匪类")) { //引怪模式
                LogD(_T("feifeifei----------"));
                if (monsterState == 2 || Read_RWZT() == 0) {
                    aMonst = mols;
                    wXuanZg2(mols.ID);
                    RunAndAttk(Mams[JiGj], mols.ID);
                    SleepEx(500);
                }
                continue;
            }
            const int killResult = KillMonster(mols);
            if (mols.mname == _T("伪宋军都统")) {
                LogD(_T("伪宋结果：%08x"), static_cast<unsigned>(killResult));
                break;
            }
        }
        SleepEx(200);
    }
    if (m_control->IsStopping()) return;

    LogD(_T("11111")); SleepEx(3000);
    wBenTuXuLPP(31, 13, _T("苏州"));
    while (!m_control->IsStopping() && IsWndGuot()) SleepEx(1000);
    wBenTuXuL3f(62.7f, 162.6f);
    mRenw_jiao(F_LY);
}

void CTaskScript::Fub_DLaoE()
{
    if (!m_control || m_control->IsStopping() || !m_ready.load() || !Dm() || !WaitIfPaused()) return;

    constexpr int kMapId = 0x51;
    constexpr DWORD kQuestId = 0xC3B5;
    const std::vector<Point2f> patrol = {
        {83.0f, 66.0f}, {48.0f, 50.0f}, {84.0f, 23.0f}, {39.0f, 34.0f}
    };// 狂暴后的路线

    const ScopedActionStatuses actionStatuses(*this,
        { 213, 14, 17, 18, 22, 12 },
        { 213, 14, 17, 18, 22, 12 });

    regex targetRegex = NerbCs(12, { 0, 0 }, _T("野熊|.*红熊王"));
    tCheckXj();// 检查血祭（宠物技能）

    // 任务已接（或已在副本地图）就直接跳到 labelks 进图等待，不再走接任务流程
    if (mRe_Renx(kQuestId) == -1 && Read_DTID() != kMapId) {
        mRenw_jie(F_LE);
    } else {
        LogD(_T("Fub_DLaoE: quest accepted or already inside, skip mRenw_jie"));
    }
    while (!m_control->IsStopping() && Read_DTID() != kMapId) SleepEx(1000);
    if (m_control->IsStopping()) return;
    SleepEx(3000);

    const int taskIndex = mRe_Renx(kQuestId);
    const auto readSelf = [this](Point2f& p) {
        if (!UpdateMP(static_cast<DWORD>(jz[TaskMemoryState::jYcdiz]), 0x100) ||
            dwls < 0x10000) {
            return false;
        }
        p.x = *reinterpret_cast<const float*>(static_cast<ULONG_PTR>(dwls) + 0x54);
        p.y = *reinterpret_cast<const float*>(static_cast<ULONG_PTR>(dwls) + 0x5C);
        return std::isfinite(p.x) && std::isfinite(p.y) && p.x > 0.0f && p.y > 0.0f;
    };

    bool patrolling = true, patrolDone = false;
    int patrolIndex = 0;

    while (!m_control->IsStopping()) {
        if (!WaitIfPaused()) return;
        if (Read_GENS() || IsWndGuot() || Read_DTID() != kMapId) { SleepEx(1000); continue; }

        if (mRenw_wc(taskIndex, 0)) break;
        if (!patrolDone && mRenw_wc(taskIndex, 1) == 80) {
            for (int attempt = 0; attempt < 5 && !m_control->IsStopping(); ++attempt) {
                const int scan = NerbMonst2(Read_Dw(tsg[TaskMemoryState::ERC]), targetRegex);
                if (Read_GENS() == 1 ||
                    (scan > 0 && !vMonst.empty() && vMonst[0].mname == _T("红熊王"))) {
                    break;
                }
                SleepEx(1000);// 等待队长跟随
            }
            patrolDone = true; patrolling = false;
        }
        const int found = NerbMonst2(Read_Dw(tsg[TaskMemoryState::ERC]), targetRegex);

        if (found == 0) {
            if (patrolling) RunTeam(2);
            if (!patrolling && Read_RWZT() == 0) {
                Point2f self{};
                if (readSelf(self)) {
                    const Point2f spot = patrol[patrolIndex];
                    const float dx = spot.x - self.x, dy = spot.y - self.y;
                    if (std::sqrt(dx * dx + dy * dy) < 2.0f) {
                        patrolIndex = (patrolIndex + 1) % static_cast<int>(patrol.size());
                    }
                    wBenTuXuL2(patrol[patrolIndex].x, patrol[patrolIndex].y);
                }
                SleepEx(500); continue;
            }
            SleepEx(1000); continue;
        }

        // 判断有无红熊王攻击自己或宠物：收宠物/隐身
        for (const Monst& mols : vMonst) {
            if (mols.mname == _T("红熊王") && patrolling && Read_MoZTT(mols.Dibu) != 0) {
                const DWORD attacker = static_cast<DWORD>(Read_ATID(mols.Dibu));
                if (Read_Dw(tsg[TaskMemoryState::ZJID]) == attacker) {
                    wUseSk6(0xF8, static_cast<DWORD>(-1));
                    SleepEx(1500 + rand() % 500);
                }
                if (static_cast<DWORD>(Read_CWID(static_cast<int>(Mams[CoXuHa]))) == attacker) {
                    wPet_Chu(static_cast<int>(Mams[CoXuHa]), 1);
                    SleepEx(500);
                }
                continue;
            }
        }
        for (const Monst& mols : vMonst) {
            if (mols.mname == _T("红熊王") && patrolling) continue;
            KillMonster(mols);
        }
        SleepEx(200);
    }
    if (m_control->IsStopping()) return;

    LogD(_T("11111")); SleepEx(3000);
    wBenTuXuLPP(108, 113, _T("苏州"));
    while (!m_control->IsStopping() && IsWndGuot()) SleepEx(1000);
    wBenTuXuL3f(251.1f, 109.0f);
    mRenw_jiao(F_LE);
}

void CTaskScript::Fub_DLaoS()
{
    if (!m_control || m_control->IsStopping() || !m_ready.load() || !Dm() || !WaitIfPaused()) return;

    constexpr int kMapId = 0x50;
    constexpr DWORD kQuestId = 0xC3B6;

    const ScopedActionStatuses actionStatuses(*this,
        { 213, 14, 18, 22, 12, 21 },
        { 213, 14, 18, 22, 12, 21 });

    regex targetRegex = NerbCs(40, { 0, 0 }, _T(".*葛荣|山寨大王"));
    tCheckXj();// 检查血祭（宠物技能）

    // 任务已接（或已在副本地图）就直接跳到 labelks 进图等待，不再走接任务流程
    if (mRe_Renx(kQuestId) == -1 && Read_DTID() != kMapId) {
        mRenw_jie(F_LS);
    } else {
        LogD(_T("Fub_DLaoS: quest accepted or already inside, skip mRenw_jie"));
    }
    while (!m_control->IsStopping() && Read_DTID() != kMapId) SleepEx(1000);
    if (m_control->IsStopping()) return;
    SleepEx(3000);

    const int taskIndex = mRe_Renx(kQuestId);
    const auto readSelf = [this](Point2f& p) {
        if (!UpdateMP(static_cast<DWORD>(jz[TaskMemoryState::jYcdiz]), 0x100) ||
            dwls < 0x10000) {
            return false;
        }
        p.x = *reinterpret_cast<const float*>(static_cast<ULONG_PTR>(dwls) + 0x54);
        p.y = *reinterpret_cast<const float*>(static_cast<ULONG_PTR>(dwls) + 0x5C);
        return std::isfinite(p.x) && std::isfinite(p.y) && p.x > 0.0f && p.y > 0.0f;
    };

    bool exitTask = false;
    while (!m_control->IsStopping() && !exitTask) {
        if (!WaitIfPaused()) return;
        if (Read_GENS() || IsWndGuot() || Read_DTID() != kMapId) { SleepEx(1000); continue; }

        const int found = NerbMonst2(Read_Dw(tsg[TaskMemoryState::ERC]), targetRegex);
        if (mRenw_wc(taskIndex, 0)) break;
        if (found == 0) {
            LogD(_T("MMMMMMMMM"));
            Point2f self{};
            if (readSelf(self)) {
                const float dx = 15.0f - self.x, dy = 30.0f - self.y;
                if (std::sqrt(dx * dx + dy * dy) > 8.0f) wBenTuXuL3f(15.0f, 30.0f);
            }
            SleepEx(1000); continue;
        }

        for (const Monst& mols : vMonst) {
            if (mols.mname == _T("山寨大王") ||
                mols.mname.Find(_T("葛荣")) != -1) {
                const int killResult = KillMonster(mols);
                if (killResult == 0x1001) { exitTask = true; break; }
            }
        }
        SleepEx(200);
    }
    if (m_control->IsStopping()) return;

    LogD(_T("11111")); SleepEx(3000);
    wBenTuXuLPP(10.6f, 21.0f, _T("苏州"));
    while (!m_control->IsStopping() && IsWndGuot()) SleepEx(1000);
    wBenTuXuL3f(62.7f, 162.6f);
    mRenw_jiao(F_LS);
}

namespace {
using CoordsPair = std::pair<std::vector<Point2f>, std::vector<Point2f>>;

// DCM_CX5 faction keys and all outer/inner coordinates, preserved in order.
CoordsPair getFactionPoints(int faction_id) {
    switch (faction_id) {
    case 0: // 少林
        return {
            { {95, 84}, {70, 60}, {67, 109}, {95, 139}, {128, 76}, {133, 143}, {154, 144}, {149, 61}, {48, 40}, {46, 141}, {66, 136} },
            { {95, 130}, {105, 106}, {109, 97}, {114, 91}, {86, 105}, {78, 93}, {70, 86}, {69, 70}, {86, 82}, {95, 84}, {109, 81}, {103, 73}, {95, 72}, {88, 72} }
        };
    case 1: // 明教
        return {
            { {98, 108}, {72, 139}, {83, 154}, {61, 117}, {48, 97}, {39, 37}, {61, 36}, {59, 79}, {86, 56}, {83, 39}, {112, 38}, {110, 58}, {133, 51}, {153, 37}, {154, 59}, {116, 139}, {138, 158}, {154, 78} },
            { {98, 149}, {98, 111}, {88, 98}, {93, 88}, {106, 98}, {102, 87}, {113, 71}, {104, 66}, {83, 76}, {84, 66}, {89, 58}, {105, 54}, {97, 46} }
        };
    case 2: // 丐帮
        return {
            { {92, 135}, {72, 118}, {70, 92}, {92, 79}, {76, 60}, {92, 46}, {125, 63}, {106, 80}, {112, 117}, {130, 155}, {152, 149}, {155, 92}, {134, 83}, {155, 64}, {152, 37}, {61, 39}, {45, 36}, {43, 147} },
            { {48, 115}, {43, 103}, {39, 88}, {59, 90}, {48, 78}, {42, 74}, {40, 60}, {57, 62}, {60, 50}, {75, 52}, {71, 66}, {80, 64}, {83, 80}, {76, 89}, {68, 91}, {74, 105}, {87, 109} }
        };
    case 3: // 武当
        return {
            { {78, 113}, {96, 112}, {105, 127}, {78, 86}, {47, 55}, {81, 49}, {97, 62}, {80, 135}, {90, 180}, {50, 177}, {46, 139} },
            { {85, 169}, {92, 172}, {104, 166}, {110, 167}, {88, 137}, {79, 137}, {77, 118}, {76, 112}, {77, 105}, {65, 109}, {49, 94}, {45, 60}, {40, 55}, {50, 56}, {84, 57}, {88, 52}, {94, 50}, {92, 62}, {79, 46}, {70, 99}, {84, 99}, {91, 112}, {88, 96}, {69, 83}, {79, 83}, {88, 83} }
        };
    case 4: // 峨眉
        return {
            { {65, 107}, {39, 152}, {37, 108}, {40, 46}, {98, 39}, {145, 106}, {146, 153}, {144, 43} },
            { {92, 110}, {84, 89}, {77, 94}, {65, 107}, {45, 110}, {37, 104}, {60, 98}, {52, 89}, {52, 73}, {52, 58}, {60, 57}, {55, 49}, {35, 58}, {75, 77}, {76, 69}, {84, 65} }
        };
    case 5: // 星宿
        return {
            { {96, 108}, {96, 84}, {124, 73}, {144, 54}, {136, 36}, {71, 43}, {71, 80}, {48, 65}, {61, 145}, {91, 129}, {144, 140}, {147, 87}, {96, 125} },
            { {96, 52}, {73, 56}, {67, 66}, {72, 73}, {85, 72}, {96, 77}, {106, 73}, {96, 84}, {91, 97}, {101, 98}, {95, 102}, {96, 107}, {96, 118}, {90, 127}, {91, 138}, {101, 135}, {96, 147} }
        };
    case 6: // 天龙
        return {
            { {96, 109}, {66, 122}, {64, 146}, {42, 135}, {24, 126}, {40, 80}, {53, 39}, {153, 36}, {96, 79}, {154, 90}, {154, 124}, {118, 124}, {96, 129} },
            { {95, 115}, {113, 110}, {77, 108}, {85, 93}, {89, 87}, {107, 93}, {103, 87}, {120, 71}, {137, 58}, {153, 38}, {115, 35}, {102, 35}, {95, 41}, {89, 36}, {78, 35}, {42, 56}, {65, 69}, {78, 55} }
        };
    case 7: // 天山
        return {
            { {95, 80}, {67, 100}, {40, 147}, {33, 95}, {38, 44}, {66, 80}, {95, 100}, {128, 98}, {146, 140}, {155, 45}, {124, 69}, {117, 51}, {61, 42}, {61, 65} },
            { {75, 122}, {112, 122}, {94, 116}, {99, 97}, {119, 96}, {87, 97}, {76, 98}, {66, 97}, {95, 83}, {88, 71}, {101, 70}, {96, 58}, {107, 49}, {126, 49}, {125, 35}, {105, 42}, {86, 54}, {81, 47}, {61, 35}, {59, 58}, {72, 65}, {70, 48} }
        };
    case 8: // 逍遥
        return {
            { {69, 151}, {48, 147}, {49, 55}, {115, 88}, {57, 111}, {89, 124}, {144, 154}, {148, 123}, {159, 54}, {126, 43} },
            { {64, 136}, {72, 135}, {79, 151}, {45, 148}, {49, 155}, {107, 147}, {126, 133}, {143, 123}, {126, 93}, {122, 73}, {104, 89}, {152, 59}, {70, 56}, {58, 64}, {54, 77} }
        };
    case 9: // 曼陀
        return {
            { {136,137},{125,105},{80,100},{84,66},{77,45},{91,21},{167,38},{184,76},{210,94},{176,118},{217,158},{88,170},{33,196} },
            { {136,137},{125,105},{80,100},{84,66},{77,45},{91,21},{167,38},{184,76},{210,94},{176,118},{217,158},{88,170},{33,196} }
        };
    case 10: // 慕容
        return {
            { {30,166},{84,133},{68,105},{47,112},{55,65},{47,32},{73,25},{132,30},{169,53},{134,117},{142,163},{165,138} },
            { {30,166},{84,133},{68,105},{47,112},{55,65},{47,32},{73,25},{132,30},{169,53},{134,117},{142,163},{165,138} }
        };
    case 11: // 恶人谷
        return {
            { {115,121},{127,102},{167,113},{182,137},{175,178},{53,201},{24,178},{39,114},{41,58},{88,37},{154,61},{195,47} },
            { {115,121},{127,102},{167,113},{182,137},{175,178},{53,201},{24,178},{39,114},{41,58},{88,37},{154,61},{195,47} }
        };
    default: return {};
    }
}
} // namespace

void CTaskScript::Fub_Xiaox()
{
    if (!m_control || m_control->IsStopping() || !m_ready.load() || !Dm() ||
        !WaitIfPaused()) return;

    // 宵小没有固定副本任务号：队长巡逻外圈，队员保持跟随。
    // Select the original outer/inner coordinate pair using the current map ID.
    const DWORD selfIndex = Read_Ziji();
    if (selfIndex == MAXDWORD) {
        LogD(_T("xiaox: team/self not ready"));
        return;
    }
    const bool leader = selfIndex == 0;
    const auto active = [this]() {
        return m_control && !m_control->IsStopping() && WaitIfPaused();
    };
    const auto readSelf = [this](Point2f& p) {
        if (!UpdateMP(static_cast<DWORD>(jz[TaskMemoryState::jYcdiz]), 0x100) ||
            dwls < 0x10000) return false;
        p.x = *reinterpret_cast<const float*>(static_cast<ULONG_PTR>(dwls) + 0x54);
        p.y = *reinterpret_cast<const float*>(static_cast<ULONG_PTR>(dwls) + 0x5C);
        return std::isfinite(p.x) && std::isfinite(p.y) && p.x > 0.0f && p.y > 0.0f;
    };
    const int dqdtid = Read_DTID();
    const CoordsPair my_coords = getFactionPoints(dqdtid);
    const std::vector<Point2f>& outer = my_coords.first;
    const std::vector<Point2f>& inner = my_coords.second;
    if (outer.empty()) {
        LogD(_T("xiaox: no faction coordinates for map=%d"), dqdtid);
        return;
    }
    const regex targetRegex = NerbCs(40, { 0, 0 }, _T(".*宵小.*"));
    size_t patrolIndex = 0;
    ULONGLONG lastMove = 0;
    int lastMap = dqdtid;

    while (active()) {
        if (IsWndGuot()) { SleepEx(500); continue; }
        const int currentMap = Read_DTID();
        // 外圈是门派地图，进入 0xAA 宵小副本后切换到原版内圈坐标。
        const std::vector<Point2f>& patrol =
            (currentMap == 0xAA && !inner.empty()) ? inner : outer;
        if (currentMap != lastMap) {
            patrolIndex = 0;
            lastMove = 0;
            lastMap = currentMap;
            LogD(_T("xiaox: map=%d, route=%s"), currentMap,
                 currentMap == 0xAA && !inner.empty() ? _T("inner") : _T("outer"));
        }
        const int found = NerbMonst2(Read_Dw(tsg[TaskMemoryState::ERC]), targetRegex);
        if (found > 0) {
            const std::vector<Monst> monsters = FindAttackableMonsters(
                Read_Dw(tsg[TaskMemoryState::ERC]), targetRegex, 40.0f, false);
            if (!monsters.empty()) {
                KillMonsterBs({ monsters.front() });
                SleepEx(300);
                continue;
            }
        }
        if (!leader) {
            RunTeam(2);
            SleepEx(700);
            continue;
        }
        Point2f self{};
        if (readSelf(self)) {
            const Point2f point = patrol[patrolIndex % patrol.size()];
            const float dx = point.x - self.x, dy = point.y - self.y;
            if (std::sqrt(dx * dx + dy * dy) < 2.5f) {
                patrolIndex = (patrolIndex + 1) % patrol.size();
                lastMove = 0;
            }
            if (!lastMove || GetTickCount64() - lastMove >= 2000) {
                const Point2f next = patrol[patrolIndex];
                wBenTuXuL35(next.x, next.y);
                lastMove = GetTickCount64();
            }
        }
        SleepEx(300);
    }
}
void CTaskScript::Fub_Attalon()     { RunTaskStub(_T("Fub_Attalon"),     _T("\u6253\u602a")); }
void CTaskScript::Fub_Jihe()        { RunTaskStub(_T("Fub_Jihe"),        _T("\u96c6\u5408")); }
void CTaskScript::Fub_DXinY()       { RunTaskStub(_T("Fub_DXinY"),       _T("\u65b0\u4e00")); }
void CTaskScript::Fub_DXinE()       { RunTaskStub(_T("Fub_DXinE"),       _T("\u65b0\u4e8c")); }
void CTaskScript::Fub_DXinS()       { RunTaskStub(_T("Fub_DXinS"),       _T("\u65b0\u4e09")); }
void CTaskScript::Fub_DHuoD()       { RunTaskStub(_T("Fub_DHuoD"),       _T("\u65b0\u6d3b\u52a8")); }
void CTaskScript::Fub_Shaxg()
{
    if (!ShaxActive()) return;
    const ScopedActionStatuses actionStatuses(*this,
        { 213, 14, 18, 22, 12 }, { 213, 14, 18, 22, 12 });
    tCheckXj();
    wRideOn(0);
    const CString bossNames = _T("关盛|吴永|宋姜|鲁志生|花容|李魁|卢君逸|林充|秦冥|呼延啄|武颂|公孙圣");
    const regex bossRegex = NerbCs(25, { 30, 33 }, bossNames);
    DWORD dlzsjs = 0, dxjjs = 0;
    DWORD huaRongId = 0;
    Point2f liKuiPosition{};
    DWORD positionTeamIndex = MAXDWORD;
    CString positionLeader;
    const bool oldPetRest = m_shaxPetRest, oldUseItem = m_shaxUseHealthItem;
    m_shaxPetRest = false;
    m_shaxUseHealthItem = !Read_Sfem();
    CString iniPath = tsg[TaskMemoryState::workdir];
    if (!iniPath.IsEmpty() && iniPath.Right(1) != _T("\\") && iniPath.Right(1) != _T("/")) iniPath += _T("\\");
    iniPath += _T("人物配置.ini");
    m_emeiQingxin = Dm()->ReadIni(_T("RWQX"), tsg[TaskMemoryState::m_Name], iniPath) == _T("1");
    // 旧版 m_zjx 尚无对应控件：沿用人物配置，未配置则兼顾攻击。
    m_shaxOnlyHeal = Dm()->ReadIni(_T("RWZJX"), tsg[TaskMemoryState::m_Name], iniPath) == _T("1");
    HkWorld(1);//打开hook（实现由用户后续补充）
    try {
        while (ShaxActive()) {
            if ((Mams[Menpaid] != 4 && Read_GENS()) || IsWndGuot() ||
                Read_DTID() != 0x222 || Read_RWZT(true) == 9) {
                SleepEx(1000);
                continue;
            }
            const DWORD teamIndex = Read_Ziji();
            if (teamIndex == MAXDWORD || teamIndex >= 6) { SleepEx(1000); continue; }
            const bool isduiz = teamIndex == 0;
            // 参考版遍历的是“可攻击”的怪（经 Lua 判定）；这里同样只保留可攻击
            // 的首领，避免选中打不了的首领导致“当前”怪判断错误。
            std::vector<Monst> attackable = FindAttackableMonsters(
                Read_Dw(tsg[TaskMemoryState::ERC]), bossRegex, 25.0f, false);
            vMonst.swap(attackable);
            const int count = static_cast<int>(vMonst.size());
            if (count > 0) m_shaxPetRest = vMonst.front().mname == _T("李魁");
            if (m_shaxPetRest && Mams[CoXuHa] != MAXDWORD)
                wPet_Chu(static_cast<int>(Mams[CoXuHa]), 1);
            if (Mams[Menpaid] == 4) {
                if (Read_GENS()) wDui_Gens(0);
                if (count > 0 && vMonst.front().mname != _T("花容")) wBenTuXuL3f(30, 33);
                // 花容先检查陷阱再治疗，防止在这里反复加血而延迟消费喊话/避阵。
                if ((count <= 0 || vMonst.front().mname != _T("花容")) && EmeiJia() > 0) {
                    SleepEx(1000); continue;
                }
            }
            if (count <= 0 || vMonst.empty()) {
                if (aMonst.mname != _T("花容")) RunTeam();
                SleepEx(1000);
                continue;
            }
            aMonst = vMonst.front();
            const DWORD currentHuaRongId = aMonst.mname == _T("花容")
                ? static_cast<DWORD>(aMonst.ID) : 0;
            if (huaRongId != currentHuaRongId) {
                dxjjs = 0;//切换首领后，上一轮花容喊话计时不再生效。
                huaRongId = currentHuaRongId;
            }
            LogD(_T("当前：%s"), aMonst.mname.GetString());
            DWORD ddgzl = 0;
            Point2f fixedPoint{};
            if (aMonst.mname == _T("李魁")) {
                const CString leader = Read_DuiwMz(0);
                if (liKuiPosition.x == 0 || positionTeamIndex != teamIndex || positionLeader != leader) {
                    if (leader.IsEmpty()) { SleepEx(1000); continue; }
                    tsg[TaskMemoryState::dz_Name] = leader;
                    int angle = 0;
                    if (isduiz) {
                        angle = rand() % 360;
                        CString message;
                        message.Format(_T("jd+%d"), angle);//队伍频道协议，例如 jd+120。
                        wSendTeamChat(message);//发送实现由用户后续补充。
                    } else {
                        SleepEx(1500);
                        if (!ShaxActive()) break;
                        CString message, sender;
                        const bool received = ReadLastTeamChat(message, &sender);
                        message.Trim();
                        const CString angleText = message.Mid(3);
                        // 只接受当前队长的 jd+整数，避免普通聊天/无效内容被转成角度0。
                        if (!received || sender != leader || message.Left(3) != _T("jd+") ||
                            angleText.IsEmpty() || angleText.GetLength() > 3 ||
                            angleText.SpanIncluding(_T("0123456789")) != angleText ||
                            _ttoi(angleText) >= 360) {
                            LogD(_T("杀星：等待队长队伍消息 jd+角度（0-359）"));
                            SleepEx(1000);
                            continue;
                        }
                        angle = _ttoi(angleText);
                    }
                    const double radians = (teamIndex * 60 + angle) * 3.141592653589793 / 180.0;
                    liKuiPosition = { 30.0f + static_cast<float>(std::cos(radians) * 6.5),
                                      33.0f + static_cast<float>(std::sin(radians) * 6.5) };
                    if (Mams[Menpaid] == 4) liKuiPosition = { 30, 33 };
                    positionTeamIndex = teamIndex;
                    positionLeader = leader;
                    wBenTuXuL3f(liKuiPosition.x, liKuiPosition.y);//修正参考版 x,x 笔误
                }
                ddgzl = 0x101;
                fixedPoint = liKuiPosition;
            }
            if (aMonst.mname == _T("鲁志生")) ddgzl = Mams[ddenj] < 85 ? 0x102 : 0x103;
            if (aMonst.mname == _T("花容")) ddgzl = 0x104;
            if (aMonst.mname == _T("关盛")) { fixedPoint = { 30, 33 }; ddgzl = 0x105; }

            int result = KillMonstShax(ddgzl, dlzsjs, dxjjs, fixedPoint);
            while (result == M_hmsg && ShaxActive()) {
                result = 0;
                const CString chkword = qHksxwz(tsg[TaskMemoryState::hkwords]);
                LogD(_T("杀星消息：%s"), chkword.GetString());
                if (chkword == _T("119") || chkword == _T("167")) {
                    const DWORD wait = chkword == _T("119") ? 5000 : 12000;
                    if (isduiz) {
                        wDui_Gens(1); SleepEx(wait); wDui_Gens(0);
                    } else {
                        wBenTuXuL34(30, 33, 1000, rand() % 16);
                        SleepEx(wait);
                    }
                    if (chkword == _T("167")) SleepEx(8000);
                    continue;
                }
                if (aMonst.mname == _T("关盛") && chkword == _T("ERR")) {
                    SleepEx(3000); wBenTuXuL3f(30, 33); continue;
                }
                if (chkword == _T("111") || chkword == _T("112") || chkword == _T("113")) {
                    const CString name = chkword == _T("111") ? _T("紫色妖星") :
                        (chkword == _T("112") ? _T("黄色妖星") : _T("红色妖星"));
                    const regex starRegex = NerbCs(25, { 30, 33 }, name);
                    SleepEx(5000);
                    bool found = false;
                    for (int i = 0; i < 10 && ShaxActive(); ++i) {
                        if (NerbMonst222(Read_Dw(tsg[TaskMemoryState::ERC]), starRegex, false) > 0 && !vMonst.empty()) {
                            aMonst = vMonst.front(); found = true; break;
                        }
                        SleepEx(1000);
                    }
                    if (!found) { LogD(_T("杀星：未找到%s"), name.GetString()); continue; }
                    wXuanZg2(static_cast<DWORD>(aMonst.ID));
                    RunAndAttk(Mams[JiGj], static_cast<DWORD>(aMonst.ID));
                    result = KillMonstShax();//连续消息继续分派，不丢弃已消费的消息
                    continue;
                }
                if (chkword == _T("114")) {
                    Point2f self{}, boss{};
                    if (ReadShaxPosition(static_cast<DWORD>(jz[TaskMemoryState::jYcdiz]), self) &&
                        ReadShaxPosition(aMonst.Dibu, boss)) {
                        if (std::hypot(self.x - boss.x, self.y - boss.y) < 6.5f) {
                            const double angle = mGetAngle2(boss, self);
                            const float distance = static_cast<float>(10 + rand() % 5);
                            wBenTuXuL3z(boss.x + distance * static_cast<float>(std::cos(angle)),
                                        boss.y + distance * static_cast<float>(std::sin(angle)));
                        }
                        if (Mams[Jijl] < 5) {
                            while (ShaxActive() && !IsWndGuot() && Read_DTID() == 0x222 &&
                                NerExist(aMonst.Dibu) && Read_RWZTC2(aMonst.Dibu, 0x13E3) == 0x13E3)
                                SleepEx(1000);
                        }
                    }
                    if (ReadShaxPosition(static_cast<DWORD>(jz[TaskMemoryState::jYcdiz]), self) &&
                        std::hypot(self.x - 30, self.y - 33) > 10) wBenTuXuL34(30, 33, 1000, rand() % 16);
                    continue;
                }
                if (chkword == _T("118")) { dlzsjs = GetTickCount(); continue; }
                if (chkword == _T("122")) {
                    SleepEx(10000);
                    if (!ShaxActive()) break;
                    NerbMonst2(Read_Dw(tsg[TaskMemoryState::ERC]), bossRegex, false);
                    for (const auto& monster : vMonst) {
                        if (monster.mname == _T("公孙圣") && monster.ID != aMonst.ID) {
                            if (Read_Dw(tsg[TaskMemoryState::ZJID]) == static_cast<DWORD>(Read_ATID(monster.Dibu))) {
                                wUseSk6(0xF8, MAXDWORD); SleepEx(1500 + rand() % 500);
                            }
                            break;
                        }
                    }
                }
                if (aMonst.mname == _T("花容")) {
                    dxjjs = GetTickCount();
                    LogD(_T("花容：收到喊话，立即启用陷阱判断，持续 %lu 秒（半径5）"), kHuaRongTrapWindowMs / 1000);
                    result = KillMonstShax(ddgzl, dlzsjs, dxjjs, fixedPoint);
                }
            }
        }
    } catch (...) {
        HkWorld(0);
        m_shaxPetRest = oldPetRest;
        m_shaxUseHealthItem = oldUseItem;
        throw;
    }
    HkWorld(0);//关闭hook
    m_shaxPetRest = oldPetRest;
    m_shaxUseHealthItem = oldUseItem;
}
void CTaskScript::Fub_DYanz()       { RunTaskStub(_T("Fub_DYanz"),       _T("\u71d5\u5b50")); }
void CTaskScript::Fub_DPiaom()      { RunTaskStub(_T("Fub_DPiaom"),      _T("\u7f25\u7f08")); }
void CTaskScript::Fun_Luojx()       { RunTaskStub(_T("Fun_Luojx"),       _T("\u6d1b\u52a0\u8840")); }
void CTaskScript::Fun_Shengj()      { RunTaskStub(_T("Fun_Shengj"),      _T("\u5347\u7ea7")); }
void CTaskScript::Fun_ZhonD()       { RunTaskStub(_T("Fun_ZhonD"),       _T("\u79cd\u5730")); }
void CTaskScript::Fun_Paosc()       { 
	const CString routeText = tsg[TaskMemoryState::luxi];
	if (routeText.IsEmpty()) {
		DebugLog(_T("route coord check: luxi is empty"));
		return;
	}

	CString workDir = tsg[TaskMemoryState::workdir];
	if (workDir.IsEmpty()) {
		workDir = CString(std::filesystem::current_path().c_str());
	}
	CString mapIni = workDir + _T("/") + _T("\u5730\u56fe\u914d\u7f6e.txt");
	if (!std::filesystem::exists(std::filesystem::path(mapIni.GetString()))) {
		const CString nestedIni =
			workDir + _T("/config/") + _T("\u5730\u56fe\u914d\u7f6e.txt");
		if (std::filesystem::exists(std::filesystem::path(nestedIni.GetString()))) {
			mapIni = nestedIni;
		}
	}

	const bool useRoute3 = ShouldUseRoute3(routeText);

	const auto checkRouteDirection = [&](bool reverse) {
		std::vector<CString> route;
		int routeOffset = 0;
		CString routePoint = routeText.Tokenize(_T("-"), routeOffset);
		while (!routePoint.IsEmpty()) {
			route.push_back(routePoint);
			routePoint = routeText.Tokenize(_T("-"), routeOffset);
		}
		if (reverse) std::reverse(route.begin(), route.end());

		const LPCTSTR directionName = reverse ? _T("reverse") : _T("forward");
		if (route.size() < 3) {
			DebugLog(_T("route coord check [%s]: route is too short, points=%u"),
				directionName, static_cast<unsigned>(route.size()));
			return;
		}

		CString routePath;
		for (size_t i = 0; i < route.size(); ++i) {
			if (i != 0) routePath += _T("-");
			routePath += route[i];
		}
		DebugLog(_T("route coord check [%s]: route=%s mode=%s"),
			directionName, routePath.GetString(),
			useRoute3 ? _T("wLianxXuLPP3") : _T("wLianxXuLPP"));

		std::vector<CString> coordinateKeys;
		coordinateKeys.reserve(route.size());
		// Mirror the original RunRouteInternal/wLianxXuLPP3 loops exactly.
		if (useRoute3 && reverse) {
			for (size_t i = 0; i + 2 < route.size(); ++i) {
				coordinateKeys.push_back(route[i] + route[i + 1]);
			}
			coordinateKeys.push_back(route[route.size() - 2] + route[route.size() - 1]);
		}
		else if (useRoute3) {
			for (size_t i = 0; i + 2 < route.size(); ++i) {
				coordinateKeys.push_back(route[i + 1] + route[i + 2]);
			}
		}
		else {
			for (size_t i = 0; i + 3 < route.size(); ++i) {
				coordinateKeys.push_back(route[i + 1] + route[i + 2]);
			}
			coordinateKeys.push_back(route[route.size() - 2] + route[route.size() - 1]);
		}

		size_t readable = 0;
		size_t missing = 0;
		DebugLog(_T("route coord check [%s]: start, points=%u keys=%u"),
			directionName, static_cast<unsigned>(route.size()),
			static_cast<unsigned>(coordinateKeys.size()));

		for (size_t i = 0; i < coordinateKeys.size(); ++i) {
			const CString& key = coordinateKeys[i];
			const CString value = Dm()->ReadIni(
				_T("\u914d\u7f6e2"), key.GetString(), mapIni.GetString());
			if (value.IsEmpty()) {
				++missing;
				DebugLog(_T("route coord check [%s][%u/%u] key=%s value=<empty> MISSING"),
					directionName, static_cast<unsigned>(i + 1),
					static_cast<unsigned>(coordinateKeys.size()), key.GetString());
				continue;
			}

			std::vector<CString> coordinates;
			int coordinateOffset = 0;
			CString coordinate = value.Tokenize(_T("|"), coordinateOffset);
			while (!coordinate.IsEmpty()) {
				coordinates.push_back(coordinate);
				coordinate = value.Tokenize(_T("|"), coordinateOffset);
			}

			if (coordinates.size() < 2) {
				++missing;
				DebugLog(_T("route coord check [%s][%u/%u] key=%s value=%s INVALID_COORDS"),
					directionName, static_cast<unsigned>(i + 1),
					static_cast<unsigned>(coordinateKeys.size()), key.GetString(),
					value.GetString());
				continue;
			}

			++readable;
			DebugLog(_T("route coord check [%s][%u/%u] key=%s value=%s x=%s y=%s parts=%u OK"),
				directionName, static_cast<unsigned>(i + 1),
				static_cast<unsigned>(coordinateKeys.size()), key.GetString(),
				value.GetString(), coordinates[0].GetString(),
				coordinates[1].GetString(), static_cast<unsigned>(coordinates.size()));
		}

		DebugLog(_T("route coord check [%s]: done readable=%u missing=%u total=%u"),
			directionName, static_cast<unsigned>(readable),
			static_cast<unsigned>(missing), static_cast<unsigned>(coordinateKeys.size()));
	};

	checkRouteDirection(false);
	checkRouteDirection(true);

}
void CTaskScript::Fun_Shanj()       { RunTaskStub(_T("Fun_Shanj"),       _T("\u4e0a\u67b6")); }
void CTaskScript::Fun_Datuu()
{
    if (!m_control || !m_ready.load() || !Dm()) return;
    constexpr DWORD taskId = 0x37EA2;
    const auto initialSchedule = m_schedule ? m_schedule->Read() : CTaskSchedule::Snapshot{};
    const auto active = [this, initialSchedule]() {
        if (m_control->IsStopping() || !WaitIfPaused()) return false;
        if (!initialSchedule.running || !m_schedule) return true;
        const auto current = m_schedule->Read();
        return current.running && current.generation == initialSchedule.generation;
    };
    const auto wait = [&](DWORD milliseconds) {
        while (milliseconds > 0) {
            if (!active()) return false;
            const DWORD slice = (std::min)(milliseconds, DWORD{100});
            SleepEx(slice); // 暂停保留当前栈和剩余等待时间，继续后从原步骤恢复。
            milliseconds -= slice;
        }
        return active();
    };
    if (!active()) return;
    const ScopedActionStatuses actionStatuses(*this, {5, 2}, vecskill);
    const auto taskComplete = [&]() {
        const int index = mRe_Renx(taskId);
        return index >= 0 && mRenw_wc(static_cast<DWORD>(index)) > 0;
    };
    const auto hasToken = [&]() {
        return Read_BagN3(_T("惩恶铁令"), 2, 1).xuh != MAXDWORD;
    };
    const auto readPosition = [&](Point2f& p) {
        if (!active() || !UpdateMP(static_cast<DWORD>(jz[TaskMemoryState::jYcdiz]), 0x100) || !dwls)
            return false;
        p.x = *reinterpret_cast<const float*>(static_cast<ULONG_PTR>(dwls) + 0x54);
        p.y = *reinterpret_cast<const float*>(static_cast<ULONG_PTR>(dwls) + 0x5C);
        return std::isfinite(p.x) && std::isfinite(p.y) && p.x > 0 && p.y > 0;
    };
    // 不依赖旧 wOpenMan(CString) 未定义的成功返回值；仍走原来的选中/打开封包。
    const auto openNpc = [&](LPCTSTR name) {
        if (!active()) return false;
        const auto names = NerbCs(20, {0, 0}, name);
        if (NerbMonst2(Read_Dw(tsg[TaskMemoryState::ERC]), names) <= 0 || vMonst.empty()) return false;
        const Monst npc = vMonst.front();
        if (npc.ID <= 0 || !active()) return false;
        aMonst = npc;
        wXuanZg2(static_cast<DWORD>(npc.ID));
        if (!wait(100)) return false;
        wOpenMan(static_cast<DWORD>(npc.ID));
        return active();
    };
    const auto openTask = [&]() {
        if (!active()) return false;
        wBenTuXuL7(0, 127, 133);
        if (!wait(1000) || !openNpc(_T("吴玠")) || !wait(500 + rand() % 500)) return false;
        wClickNpc(Read_NpcT(2) == _T("#{CXGB_240621_9}") ? DWORD{2} : DWORD{3});
        return wait(500 + rand() % 500);
    };
    // 已迁入的任务封包函数没有公共发送锁，在调用处接入本窗口的封包调度。
    const auto sendQuest = [&](int step, DWORD npcId) {
        if (!active()) return false;
        return DispatchPacketWithInterval(_T("datuu quest packet"), [&, step, npcId]() -> long {
            if (!active()) return 0;
            if (step == 0) wRenw_jie(npcId, taskId);
            else if (step == 1) wRenw_jix(npcId, taskId);
            else wRenw_wan(npcId, taskId);
            return m_control->IsStopping() ? 0L : 1L;
        });
    };

    if (!active()) return;
    if (hasToken()) {
        LogD(_T("datuu: token already present, skip acquisition"));
    } else {
        LogD(_T("datuu: acquire token"));
        while (active() && !hasToken()) {
            if (!openTask()) {
                if (!wait(1000)) return;
                continue;
            }
            if (!sendQuest(0, static_cast<DWORD>(aMonst.ID))) return;
            for (int attempt = 0; attempt < 6 && active(); ++attempt) {
                if (!wait(1000 + rand() % 1000)) return;
                if (hasToken()) break;
            }
        }
    }
    if (!active()) return;
    LogD(_T("datuu: use token and read destination"));
    if (!active()) return;
    wUseBag2();
    // 令牌窗口异步打开，沿用原版 116 窗口判定，增加可中断的等待上限。
    bool destinationReady = false;
    ZuoBi destination{};
    for (int attempt = 0; attempt < 60 && active(); ++attempt) {
        if (!wait(250)) return;
        if (IsWndOpen2(Quest_Frame) != 1) continue;
        const ZuoBi* parsed = Read_Datu();
        if (parsed && parsed->ID >= 0 && std::isfinite(parsed->x) && std::isfinite(parsed->y) &&
            parsed->x > 0 && parsed->y > 0) {
            destination = *parsed;
            destinationReady = true;
            break;
        }
    }
    if (!destinationReady) { LogD(_T("datuu: token destination unavailable")); return; }

    const int mapId = destination.ID;
    CString transferName;
    if (mapId == 34 || mapId == 35 || mapId == 32 || mapId == 33) transferName = _T("南海");
    if (mapId == 28 || mapId == 29 || mapId == 26 || mapId == 27) transferName = _T("南诏");
    if (mapId == 22 || mapId == 23 || mapId == 20 || mapId == 21) transferName = _T("长白山");
    const auto waitNpcText = [&](LPCTSTR prefix) {
        for (int attempt = 0; attempt < 10 && active(); ++attempt) {
            if (Read_NpcT().Find(prefix) != -1) return true;
            if (!wait(1000)) return false;
        }
        return false;
    };
    if (!transferName.IsEmpty()) {
        LogD(_T("datuu: transfer via %s"), transferName.GetString());
        if (!active()) return;
        wBenTuXuL7(2, 240.8f, 158);
        if (!wait(1000)) return;
        while (active() && Read_DTID() == 1) {
            if (!openNpc(_T("车传安"))) {
                if (!wait(1500 + rand() % 500)) return;
                continue;
            }
            if (!waitNpcText(_T("#{SFCS"))) continue;
            if (!active()) return;
            wClickNpc(transferName);
            if (!wait(500 + rand() % 500)) return;
            if (!waitNpcText(_T("#{FFCS"))) continue;
            if (!active()) return;
            wClickNpc(DWORD{1});
            if (!wait(500 + rand() % 500)) return;
            for (int attempt = 0; attempt < 10 && active(); ++attempt) {
                if (IsWndGuot()) break;
                if (IsWndOpen2(MessageBox_Self_Frame) == 1) mLua_Dostr(XUNLQR);
                if (!active()) return;
                if (IsWndOpen2(AcceptBox_Frame) == 1) mLua_Dostr(XUNLQR2);
                if (!wait(1000)) return;
            }
            while (active() && IsWndGuot()) if (!wait(1000)) return;
        }
    }
    while (active() && IsWndGuot()) if (!wait(1000)) return;
    if (!active()) return;
    LogD(_T("datuu: travel map=%d x=%.2f y=%.2f"), mapId, destination.x, destination.y);
    if (!active()) return;
    wKuaTuXL33(destination.x, destination.y, mapId);
    if (!active() || Read_DTID() != static_cast<DWORD>(mapId)) return;

    const auto approach = [&]() {
        Point2f p{};
        if (!readPosition(p)) return false;
        if (std::hypot(p.x - destination.x, p.y - destination.y) > 2.5f) {
            wBenTuXuL3z(destination.x, destination.y);
            if (!readPosition(p)) return false;
        }
        return std::hypot(p.x - destination.x, p.y - destination.y) <= 2.5f;
    };
    const auto scanMonster = [&]() {
        if (!active()) return false;
        NearbyMonst5(Read_Dw(tsg[TaskMemoryState::ERC]));
        // 新版扫描未命中时清空为 Monst{}，ID 是 0，不是旧版的 -1。
        return aMonst.ID > 0 && aMonst.Dibu != 0 && !aMonst.cxue.IsEmpty();
    };
    LogD(_T("datuu: summon and fight"));
    while (active() && !taskComplete()) {
        if (!approach()) {
            if (!wait(1000)) return;
            continue;
        }
        // 已有怪物且令牌还在，按原流程等待别人完成。
        while (active() && !taskComplete() && scanMonster()) {
            if (UpdateMP(aMonst.cxue, 0x100) && dwls) {
                const float hp = *reinterpret_cast<const float*>(static_cast<ULONG_PTR>(dwls) + 0x08);
                if (std::isfinite(hp) && hp <= 0) break;
            }
            if (!hasToken()) break;
            LogD(_T("datuu: waiting for existing monster"));
            if (!wait(1000)) return;
        }
        if (!active()) return;
        if (taskComplete()) break;
        wRideOn(0);
        // 原 UpdateZB 的位置缓存改为 approach 实时读取；bdatucz 仅有已注释消费者。
        bool found = false;
        for (int attempt = 0; attempt < 15 && active(); ++attempt) {
            if (taskComplete()) break;
            if (hasToken()) {
                if (!approach()) { if (!wait(1000)) return; continue; }
                if (!active()) return;
                wUseBag2();
            }
            if (!wait(1000)) return;
            if (scanMonster()) { found = true; break; }
        }
        if (!active()) return;
        if (taskComplete()) break;
        if (!found) continue;
        while (active() && !taskComplete()) {
            if (!scanMonster()) break; // 消失/跑远后重新到坐标搜索。
            const Monst target = aMonst;
            const int result = KillMonster(aMonst);
            if (!active()) return;
            LogD(_T("datuu: combat result=%08x"), result);
            if (taskComplete()) break;
            // 现有 KillMonster 不返回旧 0x1001；任务状态才是结束依据。
            if (!wait(1000)) return;
            if (!NerExist(target.Dibu)) break;
            if (UpdateMP(target.cxue, 0x100) && dwls) {
                const float hp = *reinterpret_cast<const float*>(static_cast<ULONG_PTR>(dwls) + 0x08);
                if (std::isfinite(hp) && hp <= 0) break;
            }
        }
    }

    LogD(_T("datuu: return with locator"));
    DWORD lastInvisibleTick = 0;
    bool usedInvisible = false;
    while (active()) {
        while (active() && Read_RWZTC2(0x1111, vecrun) != 0) if (!wait(1000)) return;
        if (!wait(1000 + rand() % 1000)) return;
        bool attacked = false;
        if (NerbMonst3(Read_Dw(tsg[TaskMemoryState::ERC]), 0) > 0) {
            const DWORD selfId = Read_Dw(tsg[TaskMemoryState::ZJID]);
            const auto monsters = vMonst;
            for (const Monst& monster : monsters) {
                if (!active()) return;
                if (selfId != 0 && Read_ATID(monster.Dibu) == selfId) { attacked = true; break; }
            }
        }
        if (attacked && (!usedInvisible || GetTickCount() - lastInvisibleTick >= 32000)) {
            if (!active()) return;
            if (wUseSk6(0xF8, MAXDWORD)) { usedInvisible = true; lastInvisibleTick = GetTickCount(); }
            if (!wait(1500 + rand() % 500)) return;
        }
        if (!active()) return;
        if (Read_BagX(4).xuh == MAXDWORD) { LogD(_T("datuu: no locator item")); return; }
        if (!active()) return;
        wUseBag2();
        if (!wait(1000 + rand() % 1000)) return;
        // 原版这里无限等地图变化，定位符失败后永远走不到重试；改为限次等待。
        for (int attempt = 0; attempt < 15 && active(); ++attempt) {
            if (!IsWndGuot() && Read_DTID() != static_cast<DWORD>(mapId)) break;
            if (!wait(1000)) return;
        }
        while (active() && IsWndGuot()) if (!wait(1000)) return;
        if (!active()) return;
        if (Read_DTID() != static_cast<DWORD>(mapId)) break;
        LogD(_T("datuu: locator did not change map, retry after cooldown"));
        if (!wait(32000 + rand() % 8000)) return;
    }
    if (!wait(1000)) return;
    if (!taskComplete()) { LogD(_T("datuu: task not complete")); return; }
    LogD(_T("datuu: hand in task"));
    while (active() && !openTask()) if (!wait(1000)) return;
    if (!active() || !sendQuest(1, 0x73) || !wait(500 + rand() % 500)) return;
    if (!sendQuest(2, 0x73) || !wait(500 + rand() % 500)) return;
    if (!wait(5000)) return;
    // 原 mYanzm 的本窗口 CEF 等待；不迁入旧机器的 HTTP 通知和进程终止代码。
    bool reportedVerification = false;
    while (active() && !Dm()->EnumWindow(mmhwnd, _T("CEF_Control_Example"), _T("CEF_Control_Example"), 1).IsEmpty()) {
        if (!reportedVerification) { LogD(_T("datuu: waiting for verification window to close")); reportedVerification = true; }
        if (!wait(1000)) return;
    }
    if (active()) LogD(_T("datuu: flow finished"));
}
void CTaskScript::Fun_Kuozh()       { RunTaskStub(_T("Fun_Kuozh"),       _T("\u6269\u5f20")); }
void CTaskScript::Fun_Cujuu()       { RunTaskStub(_T("Fun_Cujuu"),       _T("\u8e74\u97a0")); }
void CTaskScript::Fub_SaoDg()       { RunTaskStub(_T("Fub_SaoDg"),       _T("\u626b\u8361")); }
void CTaskScript::Fun_Chushou()     { 
    FUN_CS2();
}
void CTaskScript::Fun_DFanzSingle() { RunTaskStub(_T("Fun_DFanzSingle"), _T("\u5355\u53cd\u8d3c")); }
void CTaskScript::Fun_Zhengl()
{
    const auto active = [this]() {
        return m_control && !m_control->IsStopping() && m_ready.load() &&
               Dm() && WaitIfPaused();
    };
    if (!active()) return;

    const DWORD bagCount = Mams[BagMax1];
    if (bagCount < 2 || bagCount > 200) {
        LogD(_T("整理探宝手记：包裹格子数无效 %lu"),
             static_cast<unsigned long>(bagCount));
        return;
    }
    const DWORD scratchSlot = bagCount - 1;
    const DWORD bagBase = static_cast<DWORD>(jz[TaskMemoryState::BagJ]);
    if (!bagBase) {
        LogD(_T("整理探宝手记：包裹基址未就绪"));
        return;
    }

    // 三组地图按用户指定的顺序连续排列；同地图的手记保持原有先后顺序。
    static const LPCTSTR mapOrder[] = {
        _T("镜湖"), _T("太湖"), _T("西湖"), _T("龙泉"), _T("武夷"),_T("梅岭"), _T("南海"), _T("琼州"),
        _T("剑阁"), _T("无量山"), _T("洱海"), _T("苍山"), _T("石林"), _T("玉溪"),_T("南诏"), _T("苗疆"),
        _T("嵩山"), _T("敦煌"), _T("雁南"), _T("雁北"), _T("草原"), _T("辽西"), _T("长白山"), _T("黄龙府")
    };
    constexpr size_t mapCount = sizeof(mapOrder) / sizeof(mapOrder[0]);

    struct TreasureNote {
        DWORD slot = 0;
        size_t mapIndex = 0;
        CString map;
        int x = 0;
        int y = 0;
        size_t identity = 0;
    };

    const auto readItemName = [&](DWORD slot) {
        CString expression;
        expression.Format(_T("[[[[[%08lX]+22C00]+%08lX]+2C]+18]"),
                          static_cast<unsigned long>(bagBase),
                          static_cast<unsigned long>(slot * sizeof(DWORD)));
        CString name = Dm()->ReadString(mmhwnd, expression, 0, 0);
        name.Trim();
        return name;
    };
    const auto parsePositiveInteger = [](const CString& field, int& value) {
        int begin = -1;
        int end = -1;
        for (int i = 0; i < field.GetLength(); ++i) {
            const TCHAR ch = field[i];
            if (ch >= _T('0') && ch <= _T('9')) {
                if (begin < 0) begin = i;
                end = i + 1;
            } else if (begin >= 0) {
                break;
            }
        }
        if (begin < 0 || end <= begin) return false;
        const CString number = field.Mid(begin, end - begin);
        TCHAR* parseEnd = nullptr;
        const long parsed = _tcstol(number.GetString(), &parseEnd, 10);
        if (!parseEnd || *parseEnd != 0 || parsed <= 0 || parsed > 10000) return false;
        value = static_cast<int>(parsed);
        return true;
    };
    const auto parseNote = [&](const CString& text, TreasureNote& note) {
        if (text.Find(_T("#{XCBT_")) < 0) return false;
        for (size_t mapIndex = 0; mapIndex < mapCount; ++mapIndex) {
            const CString map(mapOrder[mapIndex]);
            const int mapAt = text.Find(map);
            if (mapAt < 0) continue;
            const int xSeparator = text.Find(_T('*'), mapAt + map.GetLength());
            const int ySeparator = xSeparator < 0 ? -1 : text.Find(_T('*'), xSeparator + 1);
            const int endSeparator = ySeparator < 0 ? -1 : text.Find(_T('*'), ySeparator + 1);
            if (xSeparator < 0 || ySeparator < 0 || endSeparator < 0) return false;
            const CString xField = text.Mid(xSeparator + 1, ySeparator - xSeparator - 1);
            const CString yField = text.Mid(ySeparator + 1, endSeparator - ySeparator - 1);
            int x = 0, y = 0;
            if (!parsePositiveInteger(xField, x) || !parsePositiveInteger(yField, y)) return false;
            note.mapIndex = mapIndex;
            note.map = map;
            note.x = x;
            note.y = y;
            return true;
        }
        return false;
    };

    std::vector<DWORD> noteSlots;
    noteSlots.reserve(bagCount);
    bool scratchContainsNote = false;
    for (DWORD slot = 0; slot < bagCount && active(); ++slot) {
        if (readItemName(slot) != _T("探宝手记")) continue;
        if (slot == scratchSlot) scratchContainsNote = true;
        else noteSlots.push_back(slot);
    }
    if (!active()) return;
    if (scratchContainsNote) {
        LogD(_T("整理探宝手记：末格 %lu 是探宝手记，无法作为临时交换位"),
             static_cast<unsigned long>(scratchSlot));
        return;
    }
    if (noteSlots.empty()) {
        LogD(_T("整理探宝手记：未找到探宝手记"));
        return;
    }

    std::vector<TreasureNote> notes;
    notes.reserve(noteSlots.size());
    for (DWORD slot : noteSlots) {
        if (!active()) return;
        const CString previousText = Read_NpcT();
        if (!wUseBag2x(static_cast<int>(slot))) {
            LogD(_T("整理探宝手记：使用格子 %lu 失败"), static_cast<unsigned long>(slot));
            continue;
        }

        TreasureNote note;
        bool parsed = false;
        // 使用物品后文本是异步更新的；先等待它与使用前不同，再解析完整 XCBT 内容。
        for (int attempt = 0; attempt < 40 && active(); ++attempt) {
            SleepEx(250);
            const CString currentText = Read_NpcT();
            if (currentText == previousText || currentText.IsEmpty()) continue;
            if (parseNote(currentText, note)) {
                parsed = true;
                break;
            }
        }
        if (!active()) return;
        if (!parsed) {
            LogD(_T("整理探宝手记：格子 %lu 的文本未在限时内更新为有效内容"),
                 static_cast<unsigned long>(slot));
            continue;
        }
        note.slot = slot;
        note.identity = notes.size();
        notes.push_back(note);
        LogD(_T("探宝手记：格子=%lu 地图=%s 坐标=%d,%d"),
             static_cast<unsigned long>(slot), note.map.GetString(), note.x, note.y);
    }
    if (!active() || notes.size() < 2) {
        LogD(_T("整理探宝手记：有效手记数=%lu，无需交换"),
             static_cast<unsigned long>(notes.size()));
        return;
    }

    std::vector<TreasureNote> desired = notes;
    std::stable_sort(desired.begin(), desired.end(), [](const TreasureNote& left,
                                                        const TreasureNote& right) {
        return left.mapIndex < right.mapIndex;
    });

    size_t exchangeCount = 0;
    for (size_t destination = 0; destination < notes.size() && active(); ++destination) {
        if (notes[destination].identity == desired[destination].identity) continue;
        size_t source = destination + 1;
        while (source < notes.size() &&
               notes[source].identity != desired[destination].identity) ++source;
        if (source >= notes.size()) {
            LogD(_T("整理探宝手记：排序状态不一致"));
            return;
        }

        const DWORD firstSlot = notes[destination].slot;
        const DWORD secondSlot = notes[source].slot;
        // 指定交换流程：第一件 -> 末格，第二件 -> 第一格，末格 -> 第二格。
        wBagJiaoH(firstSlot, scratchSlot);
        if (!active()) return;
        wBagJiaoH(secondSlot, firstSlot);
        if (!active()) return;
        wBagJiaoH(scratchSlot, secondSlot);
        if (!active()) return;

        TreasureNote first = notes[destination];
        TreasureNote second = notes[source];
        second.slot = firstSlot;
        first.slot = secondSlot;
        notes[destination] = second;
        notes[source] = first;
        ++exchangeCount;
        LogD(_T("整理探宝手记：交换 %lu(%s) <-> %lu(%s)"),
             static_cast<unsigned long>(firstSlot), first.map.GetString(),
             static_cast<unsigned long>(secondSlot), second.map.GetString());
    }

    if (active()) {
        LogD(_T("整理探宝手记完成：手记=%lu 交换=%lu 临时格=%lu"),
             static_cast<unsigned long>(notes.size()),
             static_cast<unsigned long>(exchangeCount),
             static_cast<unsigned long>(scratchSlot));
    }
}
void CTaskScript::Fub_DYanz2()      { RunTaskStub(_T("Fub_DYanz2"),      _T("\u5355\u71d52")); }

// \u83b7\u53d6\u670d\u52a1\u5668\u4e92\u65a5\u4f53\uff08\u53d1\u9001 HHBP \u5165\u961f\uff0c\u8f6e\u8be2 WWBP \u76f4\u5230\u8fd4\u56de 1111\uff09
bool CTaskScript::mHzkhct(const CString& bangpaiName) {
    if (!m_wnd || !m_wnd->teamClient) {
        LogD(_T("mHzkhct: teamClient \u672a\u521d\u59cb\u5316"));
        return false;
    }

    const CString localAddr = m_wnd->wndIp;
    if (localAddr.IsEmpty() || localAddr == _T("NULL") || localAddr == _T("FFFF") ||
        localAddr.Find(_T('-')) >= 0 || bangpaiName.IsEmpty()) {
        LogD(_T("mHzkhct: invalid owner or guild key: owner=%s guild=%s"),
             localAddr.GetString(), bangpaiName.GetString());
        return false;
    }
    if (m_control->IsStopping()) return false;

    // \u53d1\u9001 HHBP \u5165\u961f\u8bf7\u6c42
    CString response = m_wnd->teamClient->SendOnlineCommand3(_T("HHBP"), localAddr, bangpaiName);
    LogD(_T("HHBP \u5165\u961f: addr=%s bangpai=%s response=%s"),
         localAddr.GetString(), bangpaiName.GetString(), response.GetString());

    CString enqueueAck;
    enqueueAck.Format(_T("0-HHBP-%s-%s"), localAddr.GetString(), bangpaiName.GetString());
    if (response != enqueueAck) {
        LogD(_T("HHBP failed: response=%s"), response.GetString());
        return false;
    }

    // \u8f6e\u8be2 WWBP \u76f4\u5230\u8f6e\u5230\u81ea\u5df1\uff08\u8fd4\u56de 1111\uff09
    const ULONGLONG waitStarted = ::GetTickCount64();
    ULONGLONG nextWaitLogAt = waitStarted;
    while (::GetTickCount64() - waitStarted < 60000) {
        if (m_control->IsStopping()) {
            LogD(_T("WWBP stopped: owner=%s guild=%s"), localAddr.GetString(), bangpaiName.GetString());
            mFzkhct(bangpaiName);
            return false;
        }

        response = m_wnd->teamClient->SendOnlineCommand3(_T("WWBP"), localAddr, bangpaiName);

        if (response == _T("1111")) {
            LogD(_T("WWBP \u6210\u529f\u83b7\u53d6\u4e92\u65a5\u4f53: addr=%s bangpai=%s"),
                 localAddr.GetString(), bangpaiName.GetString());
            return true;
        } else if (response != _T("2222")) {
            LogD(_T("WWBP failed: response=%s"), response.GetString());
            mFzkhct(bangpaiName);
            return false;
        }

        // 2222 \u8868\u793a\u8fd8\u5728\u6392\u961f\uff0c\u7ee7\u7eed\u7b49\u5f85
        const ULONGLONG now = ::GetTickCount64();
        if (now >= nextWaitLogAt) {
            LogD(_T("WWBP \u6392\u961f\u4e2d: addr=%s bangpai=%s response=2222 elapsed=%llu/60000ms"),
                 localAddr.GetString(), bangpaiName.GetString(), now - waitStarted);
            nextWaitLogAt = now + 5000;
        }
        SleepEx(100);
    }

    LogD(_T("WWBP \u8d85\u65f6\uff1aaddr=%s bangpai=%s"),
         localAddr.GetString(), bangpaiName.GetString());
    mFzkhct(bangpaiName);
    return false;
}

// \u91ca\u653e\u670d\u52a1\u5668\u4e92\u65a5\u4f53\uff08\u53d1\u9001 LLBP\uff09
void CTaskScript::mFzkhct(const CString& bangpaiName) {
    if (!m_wnd || !m_wnd->teamClient) {
        LogD(_T("mFzkhct: teamClient \u672a\u521d\u59cb\u5316"));
        return;
    }

    const CString localAddr = m_wnd->wndIp;
    if (localAddr.IsEmpty() || localAddr == _T("NULL") || localAddr == _T("FFFF") ||
        localAddr.Find(_T('-')) >= 0 || bangpaiName.IsEmpty()) {
        LogD(_T("mFzkhct: invalid owner or guild key: owner=%s guild=%s"),
             localAddr.GetString(), bangpaiName.GetString());
        return;
    }
    const CString response = m_wnd->teamClient->SendOnlineCommand3(_T("LLBP"), localAddr, bangpaiName);

    LogD(_T("LLBP \u91ca\u653e\u4e92\u65a5\u4f53: addr=%s bangpai=%s response=%s"),
         localAddr.GetString(), bangpaiName.GetString(), response.GetString());
}

void CTaskScript::Fub_Test() {
    // 初始化帮派名称
    CString bangmu1, bangmu2;
    CStringArray luxjhh;
    int routeOffset = 0;
    CString routePoint = tsg[TaskMemoryState::luxi].Tokenize(_T("-"), routeOffset);
    while (!routePoint.Trim().IsEmpty()) {
        luxjhh.Add(routePoint);
        routePoint = tsg[TaskMemoryState::luxi].Tokenize(_T("-"), routeOffset);
    }
    int sll = luxjhh.GetCount();
    if (sll < 2) {
        LogD(_T("Fub_Test: route requires at least 2 points, got %d"), sll);
        return;
    }
    bangmu1.Format(_T("%s%s%d"), luxjhh[1], luxjhh[0], Mams[benbxh]);
    bangmu2.Format(_T("%s%s%d"), luxjhh[sll - 2], luxjhh[sll - 1], Mams[duibxh]);
    mFzkhct(bangmu1);
    mFzkhct(bangmu2);

    LogD(_T("tttt"));
    {
        CSingleLock mm_lock(&m_mutex2, TRUE);  // 获取本地互斥体
        LogD(_T("tttt1"));

        if (!mHzkhct(bangmu1)) {  // 获取局域网互斥体
            LogD(_T("Fub_Test: acquire failed; skipping notification test"));
            return;
        }

        // 获取互斥体后立即请求置前，省去测试前置等待。
        if (m_control->IsStopping()) {
            mFzkhct(bangmu1);
            return;
        }

        CString testParam;
        testParam.Format(_T("0-SZCC-%s-%s"), tsg[TaskMemoryState::m_Djxh].GetString(), m_wnd->wndIp.GetString());
        CString response = m_wnd->teamClient->Send(_T("test"), testParam);
        LogD(_T("Fub_Test: cmd=test param=%s response=%s"), testParam.GetString(), response.GetString());

        SleepEx(15000);     // 等待5秒；停止时也发送 WBCC 收尾

        testParam.Format(_T("0-WBCC-%s-%s"), tsg[TaskMemoryState::m_Djxh].GetString(), m_wnd->wndIp.GetString());
        response = m_wnd->teamClient->Send(_T("test"), testParam);
        LogD(_T("Fub_Test: cmd=test param=%s response=%s"), testParam.GetString(), response.GetString());

        mFzkhct(bangmu1);  // 释放局域网互斥体

    }  // 释放本地互斥体（mm_lock析构）

    //wBagJiaoH(0x1F, 0x15);
    //CString cctt;
    //ReadLastTeamChat(cctt);

 //   HkWorld(1);
 //   CString ccctt;
 //   ReadWorldMessage(ccctt);
 //   DebugLog(_T("科举答案：%s"), ccctt);
	//const CString chkword = qHksxwz(ccctt);
	//LogD(_T("杀星消息：%s"), chkword.GetString());


    //// 科举测试：轮询监视题目，题目变化就自动打印题目+答案（点停止退出）。
    //CString lastQuestion;
    //while (!m_control->IsStopping() && WaitIfPaused()) {
    //    CString kejuQuestion, kejuAnswer;
    //    if (ReadKejuQuestion(kejuQuestion, kejuAnswer)) {
    //        if (kejuQuestion != lastQuestion) {
    //            lastQuestion = kejuQuestion;
    //            DebugLog(_T("科举题目：%s"), kejuQuestion.GetString());
    //            DebugLog(_T("科举答案：%s"),
    //                     kejuAnswer.IsEmpty() ? _T("(CSV未匹配)") : kejuAnswer.GetString());
    //        }
    //    } else {
    //        lastQuestion.Empty();   // 窗口关闭后重置，下次打开会重新打印
    //    }
    //    SleepEx(300);
    //}


    //LogD(_T("xunl:%d"), wXunL_Guih(98, 18));
    //LogD(_T("xunl:%d"), wXunL_Guih(156, 102));

	//if (IsWndOpen2(MessageBox_Self_Frame) == 1) {//还没有判断名字
	//	mLua_Dostr(XUNLQR);
	//	DebugLog(_T("route11111111111"));
	//}
	//if (IsWndOpen2(AcceptBox_Frame) == 1) {
	//	//mLua_Dostr(XUNLQR2);
 //       DebugLog(_T("route22222222222"));
	//}
    //mLua_Dostr(XUNLQR);

    //mLua_Dostr(XUNLQR2);
    //wOpenMan(0x70);
    //wUseSk6(0x1EE, -1);

    //FUN_CS2();
  //  while (!m_control->IsStopping()) {
		//NerbMonst2(Read_Dw(tsg[TaskMemoryState::ERC]), std::regex(".*"), false);
		//std::vector<Monst> attackable;
		//attackable.reserve(vMonst.size());
		//for (const Monst& monster : vMonst) {
		//	if (m_control->IsStopping() || !WaitIfPaused()) return;
		//	if (monster.ID <= 0 || monster.Dibu == 0 || monster.cxue.IsEmpty()) continue;
		//	const DWORD targetObject = Read_Dw(monster.Dibu);
		//	if (targetObject != 0 && mCanAttack(playerObject, targetObject)) {
		//		attackable.push_back(monster);
		//	}
		//}
		//if (!attackable.empty()) KillMonsterBs(attackable);
  //      SleepEx(500);
  //  }



    //wPsGou(_T("粮食"));
    //wPsChu(130);
    //wLyGou();
    //wLyChu(127);
    //Hk_Paos(1);
    //DebugLog(_T("BianSK test: nodes=%d"), Read_DTID());
    //wUseBag2x(1);
    //mLua_Dostr(CLOSE_NPC);
    //wUseSk6(0x15, -1);
    //for (int Index = 0; Index < 2 ; Index++) {
    //    CString cccc = Read_NpcT(Index);
    //    DebugLog(_T("NPC[%d] len=%d text=%s"),
    //             Index, cccc.GetLength(), cccc.GetString());
    //}

	//const int skillNodeCount = BianSK(0);
	//DebugLog(_T("BianSK test: nodes=%d"), skillNodeCount);
	//for (size_t i = 0; i < varrou.size(); ++i) {
	//	const DWORD node = varrou[i];
	//	const DWORD skillId = Read_Dw(node + 0x10);
	//	DebugLog(_T("BianSK[%lu]: node=0x%08lX id=0x%08lX"),
	//		static_cast<unsigned long>(i),
	//		static_cast<unsigned long>(node),
	//		static_cast<unsigned long>(skillId));
	//}

    //wClickNpc(1);
    //wPet_Chu(0,0);

    //DebugLog(_T("人物状态测试：读取前 5 个状态"));
    //for (DWORD xuh = 0; xuh < 5; ++xuh) {
    //    CString name;
    //    CString description;
    //    const DWORD stateId = mRead_Buffer(xuh, &name, &description);

    //    CString message;
    //    if (stateId == 0) {
    //        message.Format(_T("状态[%lu] 读取失败"),
    //                       static_cast<unsigned long>(xuh));
    //    } else {
    //        message.Format(_T("状态[%lu] ID=%lu 名称=%s 描述=%s"),
    //                       static_cast<unsigned long>(xuh),
    //                       static_cast<unsigned long>(stateId),
    //                       name.GetString(), description.GetString());
    //    }
    //    DebugLog(message);
    //}


	//DebugLog(_T("mRead_BangZw result: %lu"), static_cast<unsigned long>(mRead_BangZw()));

	//const CString blueBirdName = _T("\u5C0F\u84DD\u9E1F");
	//const DWORD blueBird = FindNearbyObjectByName(blueBirdName);

	//if (blueBird == 0) {
	//	DebugLog(_T("\u5C0F\u84DD\u9E1F not found"));
	//}
	//else {
	//	const DWORD stateCount = mRead_TargetBufferCount(blueBird);

	//	for (DWORD xuh = 0; xuh < stateCount; ++xuh) {
	//		CString name;
	//		CString description;
	//		DWORD stateObject = 0;
	//		const DWORD stateId = mRead_TargetBufferFromObject(blueBird, xuh, &name, &description, &stateObject);
	//	}
	//}


    //wBenTuXuL2(155, 155);
    //wOpenMan(0xEA);
    //wXuanZg(0xCE);
    //const bool hasRide = wHaveRide();
    //CString message;
    //message.Format(_T("是否有坐骑：%d（%s）"),
    //               hasRide ? 1 : 0,
    //               hasRide ? _T("有") : _T("没有"));
    //DebugLog(message);


    //wLianxXuLPP(tsg[TaskMemoryState::luxi], 0, 0);
}
