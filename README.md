# DCM_CX555

This is the first clean framework layer. It keeps the old TaskThread -> Nearby -> TaskScript direction while replacing process-wide task state with per-window runtime state.

## Current flow
1. Load config/global INI from the executable directory, then config/global INI.
2. Enumerate visible top-level windows by process, title, and class name.
3. Check windows in the list and press Start.
4. Each checked HWND gets one WindowRuntime containing TaskScript, TaskHelperScript, and TaskHelperYan.
5. Each thread creates and releases its own dmsoft COM object on that same thread.
6. Stop requests cooperative shutdown, joins all three threads, unbinds dmsoft, releases objects, and removes the runtime.

## Pause design
Each window owns one CThreadControl shared by its three threads. Pause blocks at the loop gate; Stop wakes the condition variable, so a paused window still stops promptly. No cross-window global pause flags are used.

## Retained interfaces
CTaskThread exposes tClick, trClick, tMoveTo, FindPic, FindColor, and GetColorNum. Coordinates passed to these functions are client-area coordinates and are converted to screen coordinates before the dmsoft call.
CHttpClient is a client-only WinHTTP wrapper. No HTTP server thread or listening socket is created.
The bundled dmsoft wrapper remains in obj.h/obj.cpp and dm.dll is copied to the Release output.

## Build
Open DCM.sln and build Release|Win32. The current dmsoft wrapper is the 32-bit COM wrapper, so Win32 is the first target.

## Next migration order
- Move old per-window configuration into WindowRuntime.
- Migrate nearby scanning and map/object structures into CNearby.
- Migrate task states one by one into CTaskScript.
- Add message queues and server request/response correlation through CHttpClient.
- Add structured logging, runtime health, restart policy, and graceful window-close handling.
## 本次改动：MFC 启动时后台注册大漠与三线程生命周期日志

参考项目：

`D:\BaiduNetdiskDownload\大聪明0基础系列教程\第二章 实现自己的开发框架\DCM_18\DCM_CX5`

### 1. MFC 初始化阶段后台注册大漠

- `CDCMDlg::OnInitDialog()` 只负责创建后台注册线程，不再同步执行 `LoadLibrary + RegEx`。
- 后台线程完成 `CoInitializeEx`、加载 `DmReg.dll`、设置 `dm.dll` 路径、创建 `dmsoft`、调用 `RegEx`、`DmGuard` 和 `SetShowErrorMsg`。
- 注册结果通过 `WM_APP + 101` 投递回界面线程，界面可以先显示窗口，再更新注册状态。
- 用户点击启动时，如果注册仍未完成会提示“dm registration is still running”；注册成功后才允许创建窗口任务。
- 关闭程序时先通知注册线程退出并 `join()`，由注册线程释放 `dmsoft`、卸载 `DmReg.dll`，最后调用 `CoUninitialize`。

### 2. 每个窗口的三个线程日志

每个选中的窗口会创建以下三个线程：

- `TaskScript`
- `HelperScript`
- `HelperYan`

线程启动、创建 `dmsoft`、停止和释放对象时输出独立日志。日志同时写入 `OutputDebugString`，并通过窗口消息投递到界面“Debug output”控件，因此每个窗口的三个线程都会单独显示启动和停止记录。日志包含窗口句柄、窗口 ID、线程名称和线程 ID，例如：

```text
[window-thread][hwnd=0x0000000000123456][window-id=1][name=TaskScript][tid=1234] start
[window-thread][hwnd=0x0000000000123456][window-id=1][name=TaskScript][tid=1234] dmsoft created
[window-thread][hwnd=0x0000000000123456][window-id=1][name=TaskScript][tid=1234] stop / released
[dm-init][tid=4321] thread started
[dm-init][tid=4321] dm registered successfully; waiting for shutdown
[dm-init][tid=4321] thread stopped; dm object and DmReg.dll released
```

可在 Visual Studio“输出”窗口或 DebugView 中查看这些日志。
### 3. 按钮复用逻辑

- `Start / Stop` 共用 `IDC_BUTTON_START`：没有运行任务时显示 `Start`，存在运行任务时显示 `Stop`。
- `Pause / Resume` 共用 `IDC_BUTTON_PAUSE`：运行状态显示 `Pause`，暂停状态显示 `Resume`。
- 勾选窗口、刷新、启动、停止、暂停、恢复后都会同步按钮文字和可用状态。


## SleepEx / OnLoop dispatch

- Retain the reference project's `void CTaskThread::SleepEx(DWORD dwMilsec)` design.
- `SleepEx` waits in 100 ms slices, checks the stop state, and enters the pause gate so pause/resume and stop remain responsive.
- `CTaskThread::OnEventThreadRun()` reaches the concrete `OnLoop()` through virtual dispatch on every pass. `CNearby` remains an abstract middle layer, while `CTaskScript::OnLoop()` owns the concrete loop logic.

## Task selection lists

- The task palette is copied from the reference project: `棋局`, `反贼`, `老一`, `老二`, `老三`, `打怪`, `集合`, `新一`, `新二`, `新三`, `新活动`, `杀星`, `燕子`, `缥缈`, `洛加血`, `升级`, `种地`, `跑商`, `上架`, `打图`, `扩张`, `蹴鞠`, `扫荡`, `出售`, `单反贼`, `整理`, and `单燕2`.
- Double-click a task in `待选任务` to append it to `执行任务`.
- Double-click a task in `执行任务` to remove that row. Duplicate task entries are kept, matching the reference behavior.

## Task sequence execution

- The right `开始/结束` control is one shared task-sequence button. It does not replace the lower runtime `Start / Stop` button.
- When clicked to start, the rows in the third list (`执行任务`) are copied in their current order as numeric task IDs. The checked windows share this ordered schedule.
- `CTaskScript::OnLoop()` is reached on every thread-loop pass. It uses a `switch` on the current task ID, calls the matching reference-style task function, then advances to the next row.
- The task functions are currently named stubs only. Each stub prints its function name and task name; the real task body can be migrated into the same function later.
- Clicking `开始/结束` again stops the published sequence. The loop itself remains alive until the lower runtime `Start / Stop` button stops the three worker threads.

## CX5 task constructor initialization

- The reference `CTaskScript` constructor's address discovery block (the `Game.exe`, `UI_CEGUI.dll`, `KERNEL32.dll`, and `CEGUIBase.dll` module lookups, `jz` address cache, `tsg` expression cache, client-size check, and dmsoft resource path setup) is now represented by `TaskMemoryState` and `InitializeGameState()`.
- The current framework creates each window's dmsoft object inside its own worker thread. Therefore the dmsoft-dependent part is called from `CTaskScript::OnEventThreadStart()` rather than from the C++ object constructor, avoiding a null dmsoft object and preserving per-window/thread ownership.
- The recovered CX5 offsets are kept in `CTaskScript.cpp`; missing modules or a non-1024x768 client area are logged, while the framework continues into `OnLoop()` so the task dispatch path remains testable.

## Shared jz / tsg access

- Each `CWindowRuntime` owns one `TaskMemoryState` and passes the same object to `TaskScript`, `HelperScript`, and `HelperYan`.
- The compatibility members `jz` and `tsg` are provided by `CTaskThread`, so code in any of the three derived classes can use `jz[ROLEB]` or `TaskMemoryState::ROLEB`.
- Helper threads wait for `TaskScript::OnEventThreadStart()` to finish the address initialization before entering their loops. The cache is per window and is not shared between different game windows.
