#include "pch.h"
#include "TlbbLauncher.h"

#include <TlHelp32.h>
#include <vector>

namespace
{
	constexpr DWORD kPatchInstructionRva = 0x89C9D;
	constexpr DWORD kPatchByteRva = 0x89C9F;
	constexpr DWORD kMutexHandleRva = 0x7B85CC;
	constexpr ULONG kProcessBasicInformation = 0;
	constexpr ULONG kProcessWow64Information = 26;

	using NtQueryInformationProcessFn = LONG(NTAPI*)(
		HANDLE, ULONG, PVOID, ULONG, PULONG);

	struct ProcessBasicInformationLocal
	{
		PVOID reserved1;
		PVOID pebBaseAddress;
		PVOID reserved2[2];
		ULONG_PTR uniqueProcessId;
		PVOID reserved3;
	};

	class ScopedHandle
	{
	public:
		explicit ScopedHandle(HANDLE handle = nullptr) : m_handle(handle) {}
		~ScopedHandle()
		{
			if (m_handle && m_handle != INVALID_HANDLE_VALUE)
				CloseHandle(m_handle);
		}

		ScopedHandle(const ScopedHandle&) = delete;
		ScopedHandle& operator=(const ScopedHandle&) = delete;

		HANDLE Get() const { return m_handle; }
		HANDLE Release()
		{
			HANDLE handle = m_handle;
			m_handle = nullptr;
			return handle;
		}

	private:
		HANDLE m_handle;
	};

	CString NormalizeRoot(CString path)
	{
		path.Trim();
		path.TrimRight(L"\\/");
		return path;
	}

	CString JoinPath(const CString& left, const wchar_t* right)
	{
		CString path = NormalizeRoot(left);
		path += L"\\";
		path += right;
		return path;
	}

	CString GetWin32ErrorText(DWORD error)
	{
		wchar_t* buffer = nullptr;
		const DWORD count = FormatMessageW(
			FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
			FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);

		CString text;
		if (count && buffer)
		{
			text = buffer;
			text.Trim();
			LocalFree(buffer);
		}
		else
		{
			text.Format(L"Win32 error %lu", error);
		}
		return text;
	}

	bool SamePath(const CString& left, const CString& right)
	{
		wchar_t leftFull[MAX_PATH * 2] = {};
		wchar_t rightFull[MAX_PATH * 2] = {};
		if (!GetFullPathNameW(left, _countof(leftFull), leftFull, nullptr) ||
			!GetFullPathNameW(right, _countof(rightFull), rightFull, nullptr))
		{
			return left.CompareNoCase(right) == 0;
		}
		return _wcsicmp(leftFull, rightFull) == 0;
	}

	bool QueryProcessPath(DWORD processId, CString& path)
	{
		ScopedHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
			FALSE, processId));
		if (!process.Get())
			return false;

		wchar_t buffer[MAX_PATH * 2] = {};
		DWORD size = _countof(buffer);
		if (!QueryFullProcessImageNameW(process.Get(), 0, buffer, &size))
			return false;

		path.SetString(buffer, static_cast<int>(size));
		return true;
	}

	DWORD FindProcessByPath(const CString& expectedPath)
	{
		ScopedHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
		if (snapshot.Get() == INVALID_HANDLE_VALUE)
			return 0;

		PROCESSENTRY32W entry = {};
		entry.dwSize = sizeof(entry);
		if (!Process32FirstW(snapshot.Get(), &entry))
			return 0;

		do
		{
			CString processPath;
			if (QueryProcessPath(entry.th32ProcessID, processPath) &&
				SamePath(processPath, expectedPath))
			{
				return entry.th32ProcessID;
			}
		} while (Process32NextW(snapshot.Get(), &entry));

		return 0;
	}

	bool StartOfficialLauncher(const CString& rootPath, DWORD& launchBinPid,
		bool& launcherOwned, CString& error)
	{
		const CString launchBinPath = JoinPath(rootPath, L"Launch.bin");
		launchBinPid = FindProcessByPath(launchBinPath);
		launcherOwned = false;
		if (launchBinPid)
			return true;

		const CString launchExePath = JoinPath(rootPath, L"Launch.exe");
		CString commandLine;
		commandLine.Format(L"\"%s\"", static_cast<LPCWSTR>(launchExePath));
		std::vector<wchar_t> command(commandLine.GetLength() + 1);
		wcscpy_s(command.data(), command.size(), commandLine);

		STARTUPINFOW startup = {};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION processInfo = {};
		if (!CreateProcessW(launchExePath, command.data(), nullptr, nullptr, FALSE,
			0, nullptr, NormalizeRoot(rootPath), &startup, &processInfo))
		{
			error.Format(L"启动 Launch.exe 失败：%s",
				static_cast<LPCWSTR>(GetWin32ErrorText(GetLastError())));
			return false;
		}

		ScopedHandle launchProcess(processInfo.hProcess);
		ScopedHandle launchThread(processInfo.hThread);
		launcherOwned = true;

		for (int elapsed = 0; elapsed < 15000; elapsed += 100)
		{
			launchBinPid = FindProcessByPath(launchBinPath);
			if (launchBinPid)
				return true;
			Sleep(100);
		}

		error = L"等待 Launch.bin 超时，请检查官方启动器是否正常打开。";
		return false;
	}

	bool ReadRemoteImageBase(HANDLE process, DWORD& imageBase, CString& error)
	{
		HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
		auto queryProcess = reinterpret_cast<NtQueryInformationProcessFn>(
			GetProcAddress(ntdll, "NtQueryInformationProcess"));
		if (!queryProcess)
		{
			error = L"找不到 NtQueryInformationProcess。";
			return false;
		}

		SIZE_T bytesRead = 0;
#ifdef _WIN64
		ULONG_PTR peb32 = 0;
		const LONG status = queryProcess(process, kProcessWow64Information,
			&peb32, sizeof(peb32), nullptr);
		if (status < 0 || peb32 == 0 ||
			!ReadProcessMemory(process, reinterpret_cast<BYTE*>(peb32) + 8,
				&imageBase, sizeof(imageBase), &bytesRead))
		{
			error = L"读取 32 位游戏 PEB 失败。";
			return false;
		}
#else
		ProcessBasicInformationLocal info = {};
		const LONG status = queryProcess(process, kProcessBasicInformation,
			&info, sizeof(info), nullptr);
		if (status < 0 || !info.pebBaseAddress ||
			!ReadProcessMemory(process,
				reinterpret_cast<BYTE*>(info.pebBaseAddress) + 8,
				&imageBase, sizeof(imageBase), &bytesRead))
		{
			error = L"读取游戏 PEB 失败。";
			return false;
		}
#endif

		if (bytesRead != sizeof(imageBase) || imageBase == 0)
		{
			error = L"游戏模块基址无效。";
			return false;
		}
		return true;
	}

	bool PatchInstanceLimit(HANDLE process, DWORD imageBase, BYTE maxInstances,
		CString& error)
	{
		const ULONG_PTR instructionAddress = imageBase + kPatchInstructionRva;
		BYTE current[3] = {};
		SIZE_T bytesRead = 0;
		if (!ReadProcessMemory(process,
			reinterpret_cast<const void*>(instructionAddress), current,
			sizeof(current), &bytesRead) || bytesRead != sizeof(current))
		{
			error = L"读取多开限制指令失败。";
			return false;
		}

		if (current[0] != 0x83 || current[1] != 0xFE || current[2] != 0x02)
		{
			error.Format(L"游戏版本不匹配：game.exe+0x%X 当前字节为 %02X %02X %02X。",
				kPatchInstructionRva, current[0], current[1], current[2]);
			return false;
		}

		DWORD oldProtect = 0;
		void* patchAddress = reinterpret_cast<void*>(
			static_cast<ULONG_PTR>(imageBase) + kPatchByteRva);
		if (!VirtualProtectEx(process, patchAddress, 1, PAGE_EXECUTE_READWRITE,
			&oldProtect))
		{
			error.Format(L"修改代码页保护失败：%s",
				static_cast<LPCWSTR>(GetWin32ErrorText(GetLastError())));
			return false;
		}

		SIZE_T bytesWritten = 0;
		const BOOL written = WriteProcessMemory(process, patchAddress,
			&maxInstances, sizeof(maxInstances), &bytesWritten);
		DWORD unusedProtect = 0;
		VirtualProtectEx(process, patchAddress, 1, oldProtect, &unusedProtect);
		FlushInstructionCache(process, patchAddress, 1);

		if (!written || bytesWritten != sizeof(maxInstances))
		{
			error.Format(L"写入多开上限失败：%s",
				static_cast<LPCWSTR>(GetWin32ErrorText(GetLastError())));
			return false;
		}
		return true;
	}

	struct WindowSearch
	{
		DWORD processId = 0;
		HWND window = nullptr;
	};

	BOOL CALLBACK FindGameWindowProc(HWND window, LPARAM parameter)
	{
		auto* search = reinterpret_cast<WindowSearch*>(parameter);
		DWORD processId = 0;
		GetWindowThreadProcessId(window, &processId);
		if (processId != search->processId || !IsWindowVisible(window))
			return TRUE;

		wchar_t className[128] = {};
		GetClassNameW(window, className, _countof(className));
		if (_wcsicmp(className, L"TianLongBaBuHJ WndClass") == 0)
		{
			search->window = window;
			return FALSE;
		}
		return TRUE;
	}

	bool WaitForGameWindow(HANDLE process, DWORD processId, DWORD timeoutMs)
	{
		for (DWORD elapsed = 0; elapsed < timeoutMs; elapsed += 250)
		{
			if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
				return false;

			WindowSearch search;
			search.processId = processId;
			EnumWindows(FindGameWindowProc, reinterpret_cast<LPARAM>(&search));
			if (search.window)
				return true;
			Sleep(250);
		}
		return false;
	}

	struct CloseWindowSearch
	{
		DWORD processId = 0;
	};

	BOOL CALLBACK CloseLauncherWindowProc(HWND window, LPARAM parameter)
	{
		auto* search = reinterpret_cast<CloseWindowSearch*>(parameter);
		DWORD processId = 0;
		GetWindowThreadProcessId(window, &processId);
		if (processId == search->processId)
			PostMessageW(window, WM_CLOSE, 0, 0);
		return TRUE;
	}

	void CloseOwnedLauncher(DWORD processId)
	{
		CloseWindowSearch search;
		search.processId = processId;
		EnumWindows(CloseLauncherWindowProc, reinterpret_cast<LPARAM>(&search));
	}
}

bool CTlbbLauncher::ValidateRoot(const CString& rootPath, CString& error)
{
	const CString root = NormalizeRoot(rootPath);
	if (root.IsEmpty())
	{
		error = L"请选择新天龙八部安装目录。";
		return false;
	}

	const CString launchExe = JoinPath(root, L"Launch.exe");
	const CString launchBin = JoinPath(root, L"Launch.bin");
	const CString gameExe = JoinPath(root, L"Bin\\Game.exe");
	if (GetFileAttributesW(launchExe) == INVALID_FILE_ATTRIBUTES ||
		GetFileAttributesW(launchBin) == INVALID_FILE_ATTRIBUTES ||
		GetFileAttributesW(gameExe) == INVALID_FILE_ATTRIBUTES)
	{
		error = L"目录中缺少 Launch.exe、Launch.bin 或 Bin\\Game.exe。";
		return false;
	}
	return true;
}

int CTlbbLauncher::CountGameProcesses(const CString& rootPath)
{
	const CString expectedPath = JoinPath(rootPath, L"Bin\\Game.exe");
	int count = 0;
	ScopedHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
	if (snapshot.Get() == INVALID_HANDLE_VALUE)
		return 0;

	PROCESSENTRY32W entry = {};
	entry.dwSize = sizeof(entry);
	if (!Process32FirstW(snapshot.Get(), &entry))
		return 0;

	do
	{
		if (_wcsicmp(entry.szExeFile, L"Game.exe") != 0)
			continue;

		CString processPath;
		if (QueryProcessPath(entry.th32ProcessID, processPath) &&
			SamePath(processPath, expectedPath))
		{
			++count;
		}
	} while (Process32NextW(snapshot.Get(), &entry));
	return count;
}

TlbbLaunchResult CTlbbLauncher::Launch(const CString& rootPath, int maxInstances)
{
	TlbbLaunchResult result;
	CString error;
	const CString root = NormalizeRoot(rootPath);
	if (!ValidateRoot(root, error))
	{
		result.message = error;
		return result;
	}

	if (maxInstances < 3 || maxInstances > 32)
	{
		result.message = L"多开上限必须在 3 到 32 之间。";
		return result;
	}

	const int currentCount = CountGameProcesses(root);
	if (currentCount >= maxInstances)
	{
		result.message.Format(L"当前已有 %d 个客户端，已达到上限 %d。",
			currentCount, maxInstances);
		return result;
	}

	DWORD launchBinPid = 0;
	bool launcherOwned = false;
	if (!StartOfficialLauncher(root, launchBinPid, launcherOwned, error))
	{
		result.message = error;
		return result;
	}

	ScopedHandle parentProcess(OpenProcess(
		PROCESS_CREATE_PROCESS | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
		FALSE, launchBinPid));
	if (!parentProcess.Get())
	{
		result.message.Format(L"打开 Launch.bin 失败：%s",
			static_cast<LPCWSTR>(GetWin32ErrorText(GetLastError())));
		return result;
	}

	SIZE_T attributeSize = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
	auto* attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
		HeapAlloc(GetProcessHeap(), 0, attributeSize));
	if (!attributeList ||
		!InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeSize))
	{
		if (attributeList)
			HeapFree(GetProcessHeap(), 0, attributeList);
		result.message = L"初始化父进程属性失败。";
		return result;
	}

	HANDLE parentHandle = parentProcess.Get();
	if (!UpdateProcThreadAttribute(attributeList, 0,
		PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &parentHandle,
		sizeof(parentHandle), nullptr, nullptr))
	{
		DeleteProcThreadAttributeList(attributeList);
		HeapFree(GetProcessHeap(), 0, attributeList);
		result.message = L"设置 Launch.bin 父进程失败。";
		return result;
	}

	const CString gamePath = JoinPath(root, L"Bin\\Game.exe");
	const CString gameDirectory = JoinPath(root, L"Bin");
	CString commandLine = L".\\Bin\\Game.exe -fl";
	std::vector<wchar_t> command(commandLine.GetLength() + 1);
	wcscpy_s(command.data(), command.size(), commandLine);

	STARTUPINFOEXW startup = {};
	startup.StartupInfo.cb = sizeof(startup);
	startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
	startup.StartupInfo.wShowWindow = SW_SHOW;
	startup.lpAttributeList = attributeList;
	PROCESS_INFORMATION processInfo = {};

	const DWORD flags = CREATE_SUSPENDED | CREATE_DEFAULT_ERROR_MODE |
		EXTENDED_STARTUPINFO_PRESENT;
	const BOOL created = CreateProcessW(gamePath, command.data(), nullptr,
		nullptr, FALSE, flags, nullptr, gameDirectory, &startup.StartupInfo,
		&processInfo);
	const DWORD createError = created ? ERROR_SUCCESS : GetLastError();

	DeleteProcThreadAttributeList(attributeList);
	HeapFree(GetProcessHeap(), 0, attributeList);

	if (!created)
	{
		result.message.Format(L"创建挂起游戏进程失败：%s",
			static_cast<LPCWSTR>(GetWin32ErrorText(createError)));
		return result;
	}

	ScopedHandle gameProcess(processInfo.hProcess);
	ScopedHandle gameThread(processInfo.hThread);
	DWORD imageBase = 0;
	if (!ReadRemoteImageBase(gameProcess.Get(), imageBase, error) ||
		!PatchInstanceLimit(gameProcess.Get(), imageBase,
			static_cast<BYTE>(maxInstances), error))
	{
		TerminateProcess(gameProcess.Get(), ERROR_BAD_EXE_FORMAT);
		result.message = error;
		return result;
	}

	if (ResumeThread(gameThread.Get()) == static_cast<DWORD>(-1))
	{
		const DWORD resumeError = GetLastError();
		TerminateProcess(gameProcess.Get(), resumeError);
		result.message.Format(L"恢复游戏主线程失败：%s",
			static_cast<LPCWSTR>(GetWin32ErrorText(resumeError)));
		return result;
	}

	if (!WaitForGameWindow(gameProcess.Get(), processInfo.dwProcessId, 30000))
	{
		TerminateProcess(gameProcess.Get(), ERROR_TIMEOUT);
		result.message = L"30 秒内未检测到游戏主窗口，已回收本次子进程。";
		return result;
	}

	DWORD mutexHandle = 0;
	SIZE_T bytesRead = 0;
	ReadProcessMemory(gameProcess.Get(),
		reinterpret_cast<const void*>(
			static_cast<ULONG_PTR>(imageBase) + kMutexHandleRva),
		&mutexHandle, sizeof(mutexHandle), &bytesRead);

	if (launcherOwned)
		CloseOwnedLauncher(launchBinPid);

	result.success = true;
	result.processId = processInfo.dwProcessId;
	result.message.Format(
		L"启动成功：PID %lu，模块基址 0x%08X，互斥体句柄 0x%X，上限 %d。",
		processInfo.dwProcessId, imageBase, mutexHandle, maxInstances);
	return result;
}
