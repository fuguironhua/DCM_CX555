#include "pch.h"
#include "CTaskScript.h"

#include <mutex>
#include <vector>

namespace {

// These are the CX5 offsets used by the reference constructor. They are
// deliberately kept as data in the new per-window state instead of being
// process-wide globals.
constexpr LONGLONG kRolePointerOffset = 3214459;       // ROLEP
constexpr LONGLONG kBagPointerOffset = 3207721;        // BAGBP
constexpr LONGLONG kMapPointerOffset = 3331049;        // MYDITUP
constexpr LONGLONG kRoleWorkOffset = 2087836;          // RENWP
constexpr LONGLONG kRoleErcOffset = 1612087;           // RENERCP
constexpr LONGLONG kHookCodeOffset = 3418454;          // HHKK3P
constexpr LONGLONG kLuaOffset = 3734117;               // LUAP
constexpr LONGLONG kBagSplitOffset = 89050;
constexpr LONGLONG kMonsterFenOffset = 1654025;        // MONSTFENP
constexpr LONGLONG kSleepOffset = 3151301;             // KESLEEPP
constexpr LONGLONG kGetTickOffset = 3474329;           // KEGETTIP
constexpr LONGLONG kLuaDoStringOffset = 2752698;       // GALUADOSP
constexpr LONGLONG kGameThreadOffset = 3146895;        // GAZHANTP
constexpr LONGLONG kSendOffset = 3894690;              // GAWSENDP
constexpr LONGLONG kTerminateThreadOffset = 3148194;   // KETERMTP
constexpr LONGLONG kMapTransitionOffset = 1888884;     // KUATUXLP
constexpr LONGLONG kTeamOffset = 541221;               // GADUIWP
constexpr LONGLONG kDebugOffset = 3385445;             // GADEBUGP
constexpr LONGLONG kStringOffset = 2752950;            // GSTRSTRP
constexpr LONGLONG kTaskHookOffset = 2539575;           // HKRWZTP
constexpr LONGLONG kShopOffset = 3535740;              // SHANGHP
constexpr LONGLONG kActivityOffset = 672122;           // HUOYP
constexpr LONGLONG kSweepOffset = 644834;              // SAODP
constexpr LONGLONG kKeyInputOffset = 3147544;          // SHIJEP
constexpr LONGLONG kMemoryCopyOffset = 5871678;        // MEMCP
constexpr LONGLONG kRoleNameOffset = 2094312;          // RENMSP
constexpr LONGLONG kAttackNumberOffset = -7274496;     // ATTNUMP
constexpr LONGLONG kBufferClearOffset = 2542023;       // BUFXP
constexpr LONGLONG kWindowBaseOffset = 1124160;        // WPBDP
constexpr LONGLONG kSweepDetailOffset = 147281;        // SAODDP
constexpr LONGLONG kWindowOpenOffset = 312859;         // WNDOPP
constexpr LONGLONG kSceneHeightOffset = 625840;
constexpr LONGLONG kBagHelperOffset = 518688;
constexpr LONGLONG kMapCheckOffset = 806864;           // GAXUNLUGP
constexpr LONGLONG MANAGEP = 0x24BD3C;           // MANAGEP
// Updated 32-bit build: mov ecx, dword ptr ds:[00B46034].
// Current Game.exe base is 0x00380000, so the relocatable module RVA is
// 0x00B46034 - 0x00380000 = 0x007C6034.
constexpr LONGLONG RIDEP = 0x007C6034;
constexpr LONGLONG PaoShangSceneManagerGlobal = 0x7CE46C;
constexpr LONGLONG PaoShangSceneManagerCache = 0x7CBD98;

constexpr LONGLONG kCharacterNameOffset = 0x2C;        // namz
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
        address[TaskMemoryState::zqpd] = readInt(game + RIDEP, 4);
        address[TaskMemoryState::jYcdiz] = readInt(address[TaskMemoryState::ROLEB], 4) + 0x64;
        address[TaskMemoryState::BagJ] = readInt(game + kBagPointerOffset, 4);
        address[TaskMemoryState::jcYcdi] = readInt(address[TaskMemoryState::BagJ], 4) + 0x65EC0;
        address[TaskMemoryState::BagEcx] = readInt(game + kBagSplitOffset, 4);
        address[TaskMemoryState::HHKK3PM] = game + kHookCodeOffset;
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
    expression[TaskMemoryState::shuxcw].Format(_T("[%08lX]+65EC0"), hex32(memory.addresses[TaskMemoryState::BagJ]));
    expression[TaskMemoryState::shuxbg].Format(_T("[%08lX]+22A9C"), hex32(memory.addresses[TaskMemoryState::BagJ]));
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
        const CString iniPath = CString((root / L"全局配置.ini").c_str());

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

        auto deleteSectionValues = [&](const CString& section) {
            // Enumerate instead of deleting only the known names, so a
            // future field is also removed when the date rolls over.
            const CString keys = Dm()->EnumIniKey(
                section.GetString(), iniPath.GetString());
            int fieldPosition = 0;
            while (fieldPosition != -1) {
                CString field = keys.Tokenize(_T("|"), fieldPosition);
                field.Trim();
                if (!field.IsEmpty()) {
                    Dm()->DeleteIni(section.GetString(), field.GetString(),
                                    iniPath.GetString());
                }
            }
        };

        
        if (cachedDate.Compare(today) != 0) {
            for (const CString& oldHandle : handleKeys) {
                deleteSectionValues(oldHandle);
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
                cacheMessage.Format(_T("global cache hit: hwnd=%s, Nbsq=%I64u"),
                                    handleKey.GetString(), parsedNbsq);
                DebugLog(cacheMessage);
            } else {
                // A partially written record is treated as a cache miss so
                // the current window can repair it.
                handleCached = false;
                deleteSectionValues(handleKey);
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

        *(DWORD*)(Tiaoz1 + 0x3) = jz[TaskMemoryState::Nbsq] + 160 + 0x9B + 0;
        *(DWORD*)(Tiaoz1 + 0x10) = jz[TaskMemoryState::Nbsq] + 160 + 0x9B + 4;
        *(DWORD*)(Tiaoz1 + 0x20) = jz[TaskMemoryState::Nbsq] + 160 + 0x9B + 4;
        *(DWORD*)(Tiaoz1 + 0x2C) = jz[TaskMemoryState::Nbsq] + 160 + 0x9B + 8;
        *(DWORD*)(Tiaoz1 + 0x35) = jz[TaskMemoryState::BagEcx];
        *(DWORD*)(Tiaoz1 + 0x46) = jz[TaskMemoryState::Nbsq] + 160 + 0x9B + 12;
        *(DWORD*)(Tiaoz1 + 0x4B) = jz[TaskMemoryState::Nbsq] + 0x1000;
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

    }


    {
        ck[Manager] = memory.ceguiBaseModule + MANAGEP;
        ck[Frame_Zhy] = FindCeguiWindowByName(_T("ZhouHuoYue_FrameFull"));
        ck[ZhouHuoYue_Condition_Text] = FindCeguiWindowByName(
            _T("ZhouHuoYue_Condition_Text"));
        ck[PlayerFrame_Icon] = FindCeguiWindowByName(_T("PlayerFrame_Icon"));
        ck[SceneTrans_Frame] = FindCeguiWindowByName(_T("SceneTrans_Frame"));
        ck[Packet_Frame] = FindCeguiWindowByName(_T("Packet_Frame"));
        ck[Frame_Time] = FindCeguiWindowByName(_T("MiniMap_ChineseTime"));
        ck[MiniMap_MiniMap_NetStatus] = FindCeguiWindowByName(_T("MiniMap_MiniMap_NetStatus"));
		ck[PaoShang_Frame] = FindCeguiWindowByName(_T("PaoShang_Frame"));
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


    }

    return modulesReady;
}

void CTaskScript::RunTaskStub(LPCTSTR functionName, LPCTSTR taskName) {
    CString message;
    message.Format(_T("task dispatch: %s (%s)"), functionName, taskName);
    DebugLog(message);
}

bool CTaskScript::OnLoop() {
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
        }
        // Keep entering OnLoop, just as the reference framework does. A new
        // start/stop generation will reset the per-window sequence position.
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

void CTaskScript::Fub_Shuaqi()      { RunTaskStub(_T("Fub_Shuaqi"),      _T("\u68cb\u5c40")); }
void CTaskScript::Fun_DFanz()       { RunTaskStub(_T("Fun_DFanz"),       _T("\u53cd\u8d3c")); }
void CTaskScript::Fub_DLaoY()       { RunTaskStub(_T("Fub_DLaoY"),       _T("\u8001\u4e00")); }
void CTaskScript::Fub_DLaoE()       { RunTaskStub(_T("Fub_DLaoE"),       _T("\u8001\u4e8c")); }
void CTaskScript::Fub_DLaoS()       { RunTaskStub(_T("Fub_DLaoS"),       _T("\u8001\u4e09")); }
void CTaskScript::Fub_Attalon()     { RunTaskStub(_T("Fub_Attalon"),     _T("\u6253\u602a")); }
void CTaskScript::Fub_Jihe()        { RunTaskStub(_T("Fub_Jihe"),        _T("\u96c6\u5408")); }
void CTaskScript::Fub_DXinY()       { RunTaskStub(_T("Fub_DXinY"),       _T("\u65b0\u4e00")); }
void CTaskScript::Fub_DXinE()       { RunTaskStub(_T("Fub_DXinE"),       _T("\u65b0\u4e8c")); }
void CTaskScript::Fub_DXinS()       { RunTaskStub(_T("Fub_DXinS"),       _T("\u65b0\u4e09")); }
void CTaskScript::Fub_DHuoD()       { RunTaskStub(_T("Fub_DHuoD"),       _T("\u65b0\u6d3b\u52a8")); }
void CTaskScript::Fub_Shaxg()       { RunTaskStub(_T("Fub_Shaxg"),       _T("\u6740\u661f")); }
void CTaskScript::Fub_DYanz()       { RunTaskStub(_T("Fub_DYanz"),       _T("\u71d5\u5b50")); }
void CTaskScript::Fub_DPiaom()      { RunTaskStub(_T("Fub_DPiaom"),      _T("\u7f25\u7f08")); }
void CTaskScript::Fun_Luojx()       { RunTaskStub(_T("Fun_Luojx"),       _T("\u6d1b\u52a0\u8840")); }
void CTaskScript::Fun_Shengj()      { RunTaskStub(_T("Fun_Shengj"),      _T("\u5347\u7ea7")); }
void CTaskScript::Fun_ZhonD()       { RunTaskStub(_T("Fun_ZhonD"),       _T("\u79cd\u5730")); }
void CTaskScript::Fun_Paosc()       { RunTaskStub(_T("Fun_Paosc"),       _T("\u8dd1\u5546")); }
void CTaskScript::Fun_Shanj()       { RunTaskStub(_T("Fun_Shanj"),       _T("\u4e0a\u67b6")); }
void CTaskScript::Fun_Datuu()       { RunTaskStub(_T("Fun_Datuu"),       _T("\u6253\u56fe")); }
void CTaskScript::Fun_Kuozh()       { RunTaskStub(_T("Fun_Kuozh"),       _T("\u6269\u5f20")); }
void CTaskScript::Fun_Cujuu()       { RunTaskStub(_T("Fun_Cujuu"),       _T("\u8e74\u97a0")); }
void CTaskScript::Fub_SaoDg()       { RunTaskStub(_T("Fub_SaoDg"),       _T("\u626b\u8361")); }
void CTaskScript::Fun_Chushou()     { RunTaskStub(_T("Fun_Chushou"),     _T("\u51fa\u552e")); }
void CTaskScript::Fun_DFanzSingle() { RunTaskStub(_T("Fun_DFanzSingle"), _T("\u5355\u53cd\u8d3c")); }
void CTaskScript::Fun_Zhengl()      { RunTaskStub(_T("Fun_Zhengl"),      _T("\u6574\u7406")); }
void CTaskScript::Fub_DYanz2()      { RunTaskStub(_T("Fub_DYanz2"),      _T("\u5355\u71d52")); }

void CTaskScript::Fub_Test() {
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


    DebugLog(_T("mRead_BangZw result: %lu"),
             static_cast<unsigned long>(mRead_BangZw()));


    //wBenTuXuL2(155, 155);
    //wOpenMan(0xEA);
    //wXuanZg(0xCE);
    //const bool hasRide = wHaveRide();
    //CString message;
    //message.Format(_T("是否有坐骑：%d（%s）"),
    //               hasRide ? 1 : 0,
    //               hasRide ? _T("有") : _T("没有"));
    //DebugLog(message);


}
