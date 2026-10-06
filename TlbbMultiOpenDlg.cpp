#include "pch.h"
#include "resource.h"
#include "TlbbMultiOpenDlg.h"

#include <afxdlgs.h>
#include <memory>

namespace
{
	constexpr UINT WM_TLBB_LAUNCH_COMPLETE = WM_APP + 220;

	struct LaunchThreadContext
	{
		HWND receiver = nullptr;
		CString rootPath;
		int instanceLimit = 8;
	};

	struct LaunchThreadResult
	{
		TlbbLaunchResult launchResult;
	};
}

IMPLEMENT_DYNAMIC(CTlbbMultiOpenDlg, CDialogEx)

CTlbbMultiOpenDlg::CTlbbMultiOpenDlg(CWnd* parent)
	: CDialogEx(IDD_TLBB_MULTI_OPEN, parent),
	m_instanceLimit(8),
	m_launching(false)
{
}

void CTlbbMultiOpenDlg::DoDataExchange(CDataExchange* dataExchange)
{
	CDialogEx::DoDataExchange(dataExchange);
	DDX_Text(dataExchange, IDC_EDIT_TLBB_ROOT, m_rootPath);
	DDX_Text(dataExchange, IDC_EDIT_TLBB_LIMIT, m_instanceLimit);
	DDV_MinMaxInt(dataExchange, m_instanceLimit, 3, 32);
	DDX_Control(dataExchange, IDC_EDIT_TLBB_LOG, m_logEdit);
	DDX_Control(dataExchange, IDC_BUTTON_TLBB_START, m_startButton);
	DDX_Control(dataExchange, IDC_SPIN_TLBB_LIMIT, m_limitSpin);
}

BEGIN_MESSAGE_MAP(CTlbbMultiOpenDlg, CDialogEx)
	ON_BN_CLICKED(IDC_BUTTON_TLBB_BROWSE, &CTlbbMultiOpenDlg::OnBrowse)
	ON_BN_CLICKED(IDC_BUTTON_TLBB_REFRESH, &CTlbbMultiOpenDlg::OnRefresh)
	ON_BN_CLICKED(IDC_BUTTON_TLBB_START, &CTlbbMultiOpenDlg::OnStartGame)
	ON_MESSAGE(WM_TLBB_LAUNCH_COMPLETE, &CTlbbMultiOpenDlg::OnLaunchComplete)
END_MESSAGE_MAP()

BOOL CTlbbMultiOpenDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	m_rootPath = AfxGetApp()->GetProfileString(
		L"TlbbMultiOpen", L"RootPath", L"D:\\XTLBBCZ");
	m_instanceLimit = AfxGetApp()->GetProfileInt(
		L"TlbbMultiOpen", L"InstanceLimit", 8);
	if (m_instanceLimit < 3 || m_instanceLimit > 32)
		m_instanceLimit = 8;

	UpdateData(FALSE);
	m_limitSpin.SetRange32(3, 32);
	RefreshCount();
	AppendLog(L"已加载多开器。磁盘 Game.exe 不会被修改。 ");
	return TRUE;
}

void CTlbbMultiOpenDlg::OnCancel()
{
	if (m_launching)
	{
		AfxMessageBox(L"正在启动游戏，请等待当前操作完成。", MB_ICONINFORMATION);
		return;
	}
	CDialogEx::OnCancel();
}

void CTlbbMultiOpenDlg::OnBrowse()
{
	UpdateData(TRUE);
	CFolderPickerDialog picker(m_rootPath, OFN_PATHMUSTEXIST,
		this, sizeof(OPENFILENAME));
	if (picker.DoModal() == IDOK)
	{
		m_rootPath = picker.GetPathName();
		UpdateData(FALSE);
		RefreshCount();
	}
}

void CTlbbMultiOpenDlg::OnRefresh()
{
	UpdateData(TRUE);
	RefreshCount();
}

void CTlbbMultiOpenDlg::OnStartGame()
{
	if (!UpdateData(TRUE))
		return;

	CString error;
	if (!CTlbbLauncher::ValidateRoot(m_rootPath, error))
	{
		AppendLog(error);
		AfxMessageBox(error, MB_ICONERROR);
		return;
	}

	AfxGetApp()->WriteProfileString(L"TlbbMultiOpen", L"RootPath", m_rootPath);
	AfxGetApp()->WriteProfileInt(L"TlbbMultiOpen", L"InstanceLimit",
		m_instanceLimit);

	auto* context = new LaunchThreadContext;
	context->receiver = GetSafeHwnd();
	context->rootPath = m_rootPath;
	context->instanceLimit = m_instanceLimit;

	SetLaunching(true);
	AppendLog(L"正在准备官方启动器并创建挂起游戏进程...");
	if (!AfxBeginThread(LaunchThreadProc, context))
	{
		delete context;
		SetLaunching(false);
		AppendLog(L"创建启动工作线程失败。 ");
	}
}

UINT CTlbbMultiOpenDlg::LaunchThreadProc(LPVOID parameter)
{
	std::unique_ptr<LaunchThreadContext> context(
		reinterpret_cast<LaunchThreadContext*>(parameter));
	auto* result = new LaunchThreadResult;
	result->launchResult = CTlbbLauncher::Launch(
		context->rootPath, context->instanceLimit);

	if (!::PostMessageW(context->receiver, WM_TLBB_LAUNCH_COMPLETE, 0,
		reinterpret_cast<LPARAM>(result)))
	{
		delete result;
	}
	return 0;
}

LRESULT CTlbbMultiOpenDlg::OnLaunchComplete(WPARAM, LPARAM lParam)
{
	std::unique_ptr<LaunchThreadResult> result(
		reinterpret_cast<LaunchThreadResult*>(lParam));
	SetLaunching(false);
	AppendLog(result->launchResult.message);
	RefreshCount();

	if (!result->launchResult.success)
		AfxMessageBox(result->launchResult.message, MB_ICONERROR);
	return 0;
}

void CTlbbMultiOpenDlg::AppendLog(const CString& message)
{
	CString current;
	m_logEdit.GetWindowText(current);
	CString line;
	line.Format(L"[%s] %s\r\n",
		static_cast<LPCWSTR>(CTime::GetCurrentTime().Format(L"%H:%M:%S")),
		static_cast<LPCWSTR>(message));
	current += line;
	m_logEdit.SetWindowText(current);
	m_logEdit.SetSel(-1, -1);
	m_logEdit.LineScroll(m_logEdit.GetLineCount());
}

void CTlbbMultiOpenDlg::RefreshCount()
{
	const int count = CTlbbLauncher::CountGameProcesses(m_rootPath);
	CString text;
	text.Format(L"当前客户端：%d", count);
	SetDlgItemText(IDC_STATIC_TLBB_COUNT, text);
}

void CTlbbMultiOpenDlg::SetLaunching(bool launching)
{
	m_launching = launching;
	m_startButton.EnableWindow(!launching);
	GetDlgItem(IDC_BUTTON_TLBB_BROWSE)->EnableWindow(!launching);
	GetDlgItem(IDC_BUTTON_TLBB_REFRESH)->EnableWindow(!launching);
	GetDlgItem(IDC_EDIT_TLBB_ROOT)->EnableWindow(!launching);
	GetDlgItem(IDC_EDIT_TLBB_LIMIT)->EnableWindow(!launching);
	GetDlgItem(IDCANCEL)->EnableWindow(!launching);
	m_startButton.SetWindowText(launching ? L"启动中..." : L"启动一个客户端");
}
