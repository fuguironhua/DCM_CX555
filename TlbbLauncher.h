#pragma once

struct TlbbLaunchResult
{
	bool success = false;
	DWORD processId = 0;
	CString message;
};

class CTlbbLauncher
{
public:
	static bool ValidateRoot(const CString& rootPath, CString& error);
	static int CountGameProcesses(const CString& rootPath);
	static TlbbLaunchResult Launch(const CString& rootPath, int maxInstances);
};
