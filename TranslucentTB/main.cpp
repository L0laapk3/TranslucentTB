// Standard API
#include <chrono>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Windows API
#include "arch.h"
#include <PathCch.h>
#include <ShlObj.h>
#include <wrl/wrappers/corewrappers.h>

// Local stuff
#include "autofree.hpp"
#include "autostart.hpp"
#include "blacklist.hpp"
#include "common.hpp"
#include "config.hpp"
#include "eventhook.hpp"
#include "messagewindow.hpp"
#include "resource.h"
#include "swcadata.hpp"
#include "traycontextmenu.hpp"
#include "ttberror.hpp"
#include "ttblog.hpp"
#include "util.hpp"
#ifdef STORE
#include "UWP.hpp"
#endif
#include "win32.hpp"
#include "window.hpp"
#include "windowclass.hpp"

#pragma region Data

enum class MONITORSTATE {
	Normal,				// Nothing special, act as it says in Config::TASKBAR_APPEARANCE.
	WindowMaximised,	// There is a window which is maximised on this monitor and dynamic windows is on. Display Config::DYNAMIC_APPEARANCE.
	StartMenuOpen		// The Start Menu is open on this monitor and dynamic start is on. Display with swca::ACCENT::ACCENT_NORMAL.
};

enum class EXITREASON {
	NewInstance,		// New instance told us to exit
	UserAction,			// Triggered by the user
	UserActionNoSave	// Triggered by the user, but doesn't saves config
};

static struct {
	EXITREASON exit_reason = EXITREASON::UserAction;
	Window main_taskbar;
	std::unordered_map<HMONITOR, std::pair<Window, MONITORSTATE>> taskbars;
	bool should_show_peek = true;
	bool is_running = true;
	std::wstring config_folder;
	std::wstring config_file;
	std::wstring exclude_file;
	bool peek_active = false;
} run;

static const std::unordered_map<swca::ACCENT, uint32_t> NORMAL_BUTTON_MAP = {
	{ swca::ACCENT::ACCENT_NORMAL,						IDM_NORMAL },
	{ swca::ACCENT::ACCENT_ENABLE_TRANSPARENTGRADIENT,	IDM_CLEAR  },
	{ swca::ACCENT::ACCENT_ENABLE_GRADIENT,				IDM_OPAQUE },
	{ swca::ACCENT::ACCENT_ENABLE_BLURBEHIND,			IDM_BLUR   },
	{ swca::ACCENT::ACCENT_ENABLE_FLUENT,				IDM_FLUENT }
};

static const std::unordered_map<swca::ACCENT, uint32_t> DYNAMIC_BUTTON_MAP = {
	{ swca::ACCENT::ACCENT_NORMAL,						IDM_DYNAMICWS_NORMAL },
	{ swca::ACCENT::ACCENT_ENABLE_TRANSPARENTGRADIENT,	IDM_DYNAMICWS_CLEAR  },
	{ swca::ACCENT::ACCENT_ENABLE_GRADIENT,				IDM_DYNAMICWS_OPAQUE },
	{ swca::ACCENT::ACCENT_ENABLE_BLURBEHIND,			IDM_DYNAMICWS_BLUR   },
	{ swca::ACCENT::ACCENT_ENABLE_FLUENT,				IDM_DYNAMICWS_FLUENT }
};

static const std::unordered_map<enum Config::PEEK, uint32_t> PEEK_BUTTON_MAP = {
	{ Config::PEEK::Enabled,		  IDM_PEEK          },
	{ Config::PEEK::DynamicGenerous,  IDM_DPEEKGENEROUS },
	{ Config::PEEK::Dynamic,		  IDM_DPEEK         },
	{ Config::PEEK::Disabled,		  IDM_NOPEEK        }
};

#pragma endregion

#pragma region That one function that does all the magic

void SetWindowBlur(const Window &window, const swca::ACCENT &appearance, const uint32_t &color)
{
	if (user32::SetWindowCompositionAttribute)
	{
		static std::unordered_map<Window, bool> is_normal;

		swca::ACCENTPOLICY policy = {
			appearance,
			2,
			(color & 0xFF00FF00) + ((color & 0x00FF0000) >> 16) + ((color & 0x000000FF) << 16),
			0
		};

		if (policy.nAccentState == swca::ACCENT::ACCENT_NORMAL)
		{
			if (is_normal.count(window) == 0 || !is_normal[window])
			{
				// WM_THEMECHANGED makes the taskbar reload the theme and reapply the normal effect.
				// Gotta memoize it because constantly sending it makes explorer's CPU usage jump.
				window.send_message(WM_THEMECHANGED);
				is_normal[window] = true;
			}
			return;
		}
		else if (policy.nAccentState == swca::ACCENT::ACCENT_ENABLE_FLUENT && policy.nColor >> 24 == 0x00)
		{
			// Fluent mode doesn't likes a completely 0 opacity
			policy.nColor = (0x01 << 24) + (policy.nColor & 0x00FFFFFF);
		}

		swca::WINCOMPATTRDATA data = {
			swca::WindowCompositionAttribute::WCA_ACCENT_POLICY,
			&policy,
			sizeof(policy)
		};

		user32::SetWindowCompositionAttribute(window, &data);
		is_normal[window] = false;
	}
}

#pragma endregion

#pragma region Configuration

void GetPaths()
{
#ifndef STORE
	AutoFree::CoTaskMem<wchar_t> appData;
	ErrorHandle(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, NULL, &appData), Error::Level::Fatal, L"Failed to determine configuration files locations!");
#else
	try
	{
		std::wstring appData_str = UWP::GetApplicationFolderPath(UWP::FolderType::Roaming);
		const wchar_t *appData = appData_str.c_str();
#endif

	AutoFree::Local<wchar_t> configFolder;
	AutoFree::Local<wchar_t> configFile;
	AutoFree::Local<wchar_t> excludeFile;

	ErrorHandle(PathAllocCombine(appData, NAME, PATHCCH_ALLOW_LONG_PATHS, &configFolder), Error::Level::Fatal, L"Failed to combine AppData folder and application name!");
	ErrorHandle(PathAllocCombine(configFolder, CONFIG_FILE, PATHCCH_ALLOW_LONG_PATHS, &configFile), Error::Level::Fatal, L"Failed to combine config folder and config file!");
	ErrorHandle(PathAllocCombine(configFolder, EXCLUDE_FILE, PATHCCH_ALLOW_LONG_PATHS, &excludeFile), Error::Level::Fatal, L"Failed to combine config folder and exclude file!");

	run.config_folder = configFolder;
	run.config_file = configFile;
	run.exclude_file = excludeFile;

#ifdef STORE
	}
	catch (const winrt::hresult_error &error)
	{
		ErrorHandle(error.code(), Error::Level::Fatal, L"Getting application folder paths failed!");
	}
#endif
}

void ApplyStock(const std::wstring &filename)
{
	std::wstring exeFolder_str = win32::GetExeLocation();
	exeFolder_str.erase(exeFolder_str.find_last_of(LR"(/\)") + 1);

	AutoFree::Local<wchar_t> stockFile;
	if (!ErrorHandle(PathAllocCombine(exeFolder_str.c_str(), filename.c_str(), PATHCCH_ALLOW_LONG_PATHS, &stockFile), Error::Level::Error, L"Failed to combine executable folder and config file!"))
	{
		return;
	}

	AutoFree::Local<wchar_t> configFile;
	if (!ErrorHandle(PathAllocCombine(run.config_folder.c_str(), filename.c_str(), PATHCCH_ALLOW_LONG_PATHS, &configFile), Error::Level::Error, L"Failed to combine config folder and config file!"))
	{
		return;
	}

	if (!win32::IsDirectory(run.config_folder))
	{
		if (!CreateDirectory(run.config_folder.c_str(), NULL))
		{
			if (!ErrorHandle(HRESULT_FROM_WIN32(GetLastError()), Error::Level::Error, L"Creating configuration files directory failed!"))
			{
				return;
			}
		}
	}

	if (!CopyFile(stockFile, configFile, FALSE))
	{
		ErrorHandle(HRESULT_FROM_WIN32(GetLastError()), Error::Level::Error, L"Copying stock configuration file failed!");
	}
}

bool CheckAndRunWelcome()
{
	if (!win32::IsDirectory(run.config_folder))
	{
		// String concatenation is hard OK
		std::wstring message;
		message += L"Welcome to ";
		message += NAME;
		message += L"!\n\n";
		message += L"You can tweak the taskbar's appearance with the tray icon. If it's your cup of tea, you can also edit the configuration files, located at \"";
		message += run.config_folder;
		message += '"';
		message += L"\n\nDo you agree to the GPLv3 license?";

		if (MessageBox(NULL, message.c_str(), NAME, MB_ICONINFORMATION | MB_YESNO | MB_SETFOREGROUND) != IDYES)
		{
			return false;
		}
	}
	if (!win32::FileExists(run.config_file))
	{
		ApplyStock(CONFIG_FILE);
	}
	if (!win32::FileExists(run.exclude_file))
	{
		ApplyStock(EXCLUDE_FILE);
	}
	return true;
}

#pragma endregion

#pragma region Utilities

void RefreshHandles()
{
	if (Config::VERBOSE)
	{
		Log::OutputMessage(L"Refreshing taskbar handles");
	}

	// Older handles are invalid, so clear the map to be ready for new ones
	run.taskbars.clear();

	run.main_taskbar = Window::Find(L"Shell_TrayWnd");
	run.taskbars[run.main_taskbar.monitor()] = {
		run.main_taskbar,
		MONITORSTATE::Normal
	};

	Window secondtaskbar;
	while ((secondtaskbar = Window::Find(L"Shell_SecondaryTrayWnd", L"", Window::NullWindow, secondtaskbar)) != Window::NullWindow)
	{
		run.taskbars[secondtaskbar.monitor()] = { secondtaskbar, MONITORSTATE::Normal };
	}
}

void TogglePeek(const bool &status)
{
	static bool cached_peek = true;
	static Window cached_taskbar = Window(run.main_taskbar);

	if (status != cached_peek || cached_taskbar != run.main_taskbar)
	{
		Window _peek = Window::Find(L"TrayShowDesktopButtonWClass", L"", Window::Find(L"TrayNotifyWnd", L"", run.main_taskbar));

		if (!status)
		{
			SetWindowLong(_peek, GWL_EXSTYLE, GetWindowLong(_peek, GWL_EXSTYLE) | WS_EX_LAYERED);

			SetLayeredWindowAttributes(_peek, 0, 0, LWA_ALPHA);
		}
		else
		{
			SetWindowLong(_peek, GWL_EXSTYLE, GetWindowLong(_peek, GWL_EXSTYLE) & ~WS_EX_LAYERED);
		}

		cached_peek = status;
		cached_taskbar = Window(run.main_taskbar);
	}
}

#pragma endregion

#pragma region Tray

inline void ChangePopupItemText(HMENU menu, const uint32_t &item, std::wstring &new_text)
{
	#pragma clang diagnostic push
	#pragma clang diagnostic ignored "-Wmissing-field-initializers"
	MENUITEMINFO item_info = { sizeof(item_info), MIIM_STRING };
	#pragma clang diagnostic pop

	item_info.dwTypeData = new_text.data();
	SetMenuItemInfo(menu, item, false, &item_info);
}

void RefreshMenu(HMENU menu)
{
	static bool needs_to_check_fluent = true;
	if (needs_to_check_fluent)
	{
		if (!win32::IsAtLeastBuild(MIN_FLUENT_BUILD))
		{
			RemoveMenu(menu, IDM_FLUENT, MF_BYCOMMAND);
			RemoveMenu(menu, IDM_DYNAMICWS_FLUENT, MF_BYCOMMAND);
		}

		needs_to_check_fluent = false;
	}

	TrayContextMenu::RefreshBool(IDM_OPENLOG, menu, !Log::file().empty(), TrayContextMenu::ControlsEnabled);
	TrayContextMenu::RefreshBool(IDM_DYNAMICWS_COLOR, menu, Config::DYNAMIC_WS && !Config::DYNAMIC_USE_REGULAR_COLOR, TrayContextMenu::ControlsEnabled);

	const Autostart::StartupState state = Autostart::GetStartupState();
	TrayContextMenu::RefreshBool(IDM_AUTOSTART, menu, !(state == Autostart::StartupState::DisabledByUser
#ifdef STORE
		|| state == Autostart::StartupState::DisabledByPolicy
		|| state == Autostart::StartupState::EnabledByPolicy
#endif
		), TrayContextMenu::ControlsEnabled);

	std::wstring autostart_text;
	switch (state)
	{
	case Autostart::StartupState::DisabledByUser:
		autostart_text = L"Startup has been disabled in Task Manager";
		break;
#ifdef STORE
	case Autostart::StartupState::DisabledByPolicy:
		autostart_text = L"Startup has been disabled in Group Policy";
		break;
	case Autostart::StartupState::EnabledByPolicy:
		autostart_text = L"Startup has been enabled in Group Policy";
		break;
#endif
	case Autostart::StartupState::Enabled:
	case Autostart::StartupState::Disabled:
		autostart_text = L"Open at boot";
	}
	ChangePopupItemText(menu, IDM_AUTOSTART, autostart_text);

	TrayContextMenu::RefreshBool(IDM_AUTOSTART, menu, state == Autostart::StartupState::Enabled
#ifdef STORE
		|| state == Autostart::StartupState::EnabledByPolicy
#endif
		, TrayContextMenu::Toggle);
}

#pragma endregion

#pragma region Main logic

bool isRealWindow(const HWND hwnd)
{
	TITLEBARINFO ti;
	HWND hwndTry, hwndWalk = NULL;

	if (!IsWindowVisible(hwnd))
		return FALSE;

	hwndTry = GetAncestor(hwnd, GA_ROOTOWNER);
	while (hwndTry != hwndWalk)
	{
		hwndWalk = hwndTry;
		hwndTry = GetLastActivePopup(hwndWalk);
		if (IsWindowVisible(hwndTry))
			break;
	}
	if (hwndWalk != hwnd)
		return FALSE;

	// the following removes some task tray programs and "Program Manager"
	ti.cbSize = sizeof(ti);
	GetTitleBarInfo(hwnd, &ti);
	if (ti.rgstate[0] & STATE_SYSTEM_INVISIBLE)
		return FALSE;

	// Tool windows should not be displayed either, these do not appear in the
	// task bar.
	if (GetWindowLong(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
		return FALSE;

	return TRUE;
}

BOOL CALLBACK EnumWindowsProcess(const HWND hWnd, LPARAM)
{
	const Window window = hWnd;
	// IsWindowCloaked should take care of checking if it's on the current desktop.
	// But that's undefined behavior.
	// So eh, do both but with IsWindowOnCurrentDesktop last.
	if (window.visible() && !window.get_attribute<BOOL>(DWMWA_CLOAKED) && !Blacklist::IsBlacklisted(window))
	{
		if (window.state() == SW_MAXIMIZE && window.on_current_desktop())
		{
			auto &taskbar = run.taskbars.at(window.monitor());
			if (Config::DYNAMIC_WS)
			{
				taskbar.second = MONITORSTATE::WindowMaximised;
			}

			if ((Config::PEEK == Config::PEEK::Dynamic || Config::PEEK == Config::PEEK::DynamicGenerous) && taskbar.first == run.main_taskbar)
			{
				run.should_show_peek = true;
			}
		}
		else {
			if (Config::PEEK == Config::PEEK::DynamicGenerous && !run.should_show_peek && window.state() != SW_SHOWMINIMIZED && isRealWindow(window))
			{
				run.should_show_peek = true;
			}
		}
	}
	return true;
}

void CALLBACK HandleAeroPeekEvent(HWINEVENTHOOK, const DWORD event, HWND, LONG, LONG, DWORD, DWORD)
{
	run.peek_active = event == 0x21;
}

void SetTaskbarBlur()
{
	static int counter = 10;

	if (counter >= 10)	// Change this if you want to change the time it takes for the program to update.
	{					// 1 = Config::SLEEP_TIME; we use 10 (assuming the default configuration value of 10),
						// because the difference is less noticeable and it has no large impact on CPU.
						// We can change this if we feel that CPU is more important than response time.
		run.should_show_peek = (Config::PEEK == Config::PEEK::Enabled);

		for (auto &taskbar : run.taskbars)
		{
			taskbar.second.second = MONITORSTATE::Normal; // Reset taskbar state
		}
		if (Config::DYNAMIC_WS || Config::PEEK == Config::PEEK::Dynamic || Config::PEEK == Config::PEEK::DynamicGenerous)
		{
			counter = 0;
			EnumWindows(&EnumWindowsProcess, NULL);
		}

		TogglePeek(run.should_show_peek);

		if (Config::DYNAMIC_START && Util::IsStartVisible())
		{
			// TODO: does this works correctly on multi-monitor
			run.taskbars.at(Window::Find(L"Windows.UI.Core.CoreWindow", L"Start").monitor()).second = MONITORSTATE::StartMenuOpen;
		}

		// TODO
		//if (true)
		//{
		//	HWND task_view = FindWindow(L"Windows.UI.Core.CoreWindow", L"Task view");
		//	if (task_view == GetForegroundWindow())
		//	{
		//		run.taskbars.at(MonitorFromWindow(task_view, MONITOR_DEFAULTTOPRIMARY)).state = Taskbar::StartMenuOpen;
		//	}
		//}

		if (Config::DYNAMIC_WS && Config::DYNAMIC_REGULAR_ON_PEEK && run.peek_active)
		{
			for (auto &taskbar : run.taskbars)
			{
				taskbar.second.second = MONITORSTATE::Normal;
			}
		}

		counter = 0;
	}

	for (const auto &taskbar : run.taskbars)
	{
		switch (taskbar.second.second)
		{
		case MONITORSTATE::StartMenuOpen:
			SetWindowBlur(taskbar.second.first, swca::ACCENT::ACCENT_NORMAL, NULL);
			break;
		case MONITORSTATE::WindowMaximised:
			SetWindowBlur(taskbar.second.first, Config::DYNAMIC_APPEARANCE,
				Config::DYNAMIC_USE_REGULAR_COLOR ? Config::TASKBAR_COLOR : Config::DYNAMIC_COLOR); // A window is maximised.
			break;
		case MONITORSTATE::Normal:
			SetWindowBlur(taskbar.second.first, Config::TASKBAR_APPEARANCE, Config::TASKBAR_COLOR);  // Taskbar should be normal, call using normal transparency settings
			break;
		}
	}
	counter++;
}

#pragma endregion

#pragma region Startup

void InitializeWindowsRuntime()
{
	static Microsoft::WRL::Wrappers::RoInitializeWrapper init(RO_INIT_SINGLETHREADED);
	ErrorHandle(init, Error::Level::Log, L"Initialization of Windows Runtime failed.");
}

void HardenProcess()
{
	PROCESS_MITIGATION_ASLR_POLICY aslr_policy;
	if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessASLRPolicy, &aslr_policy, sizeof(aslr_policy)))
	{
		aslr_policy.EnableForceRelocateImages = true;
		aslr_policy.DisallowStrippedImages = true;
		if (!SetProcessMitigationPolicy(ProcessASLRPolicy, &aslr_policy, sizeof(aslr_policy)))
		{
			ErrorHandle(HRESULT_FROM_WIN32(GetLastError()), Error::Level::Log, L"Couldn't disallow stripped images.");
		}
	}
	else
	{
		ErrorHandle(HRESULT_FROM_WIN32(GetLastError()), Error::Level::Log, L"Couldn't get current ASLR policy.");
	}

	PROCESS_MITIGATION_DYNAMIC_CODE_POLICY code_policy {};
	code_policy.ProhibitDynamicCode = true;
	code_policy.AllowThreadOptOut = false;
	code_policy.AllowRemoteDowngrade = false;
	if (!SetProcessMitigationPolicy(ProcessDynamicCodePolicy, &code_policy, sizeof(code_policy)))
	{
		ErrorHandle(HRESULT_FROM_WIN32(GetLastError()), Error::Level::Log, L"Couldn't disable dynamic code generation.");
	}

	PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY handle_policy {};
	handle_policy.RaiseExceptionOnInvalidHandleReference = true;
	handle_policy.HandleExceptionsPermanentlyEnabled = true;
	if (!SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy, &handle_policy, sizeof(handle_policy)))
	{
		ErrorHandle(HRESULT_FROM_WIN32(GetLastError()), Error::Level::Log, L"Couldn't enable strict handle checks.");
	}

	PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY extension_policy {};
	extension_policy.DisableExtensionPoints = true;
	if (!SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy, &extension_policy, sizeof(extension_policy)))
	{
		ErrorHandle(HRESULT_FROM_WIN32(GetLastError()), Error::Level::Log, L"Couldn't disable extension point DLLs.");
	}

	PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY signature_policy {};
	signature_policy.MitigationOptIn = true;
	if (!SetProcessMitigationPolicy(ProcessSignaturePolicy, &signature_policy, sizeof(signature_policy)))
	{
		ErrorHandle(HRESULT_FROM_WIN32(GetLastError()), Error::Level::Log, L"Couldn't enable image signature enforcement.");
	}


	PROCESS_MITIGATION_IMAGE_LOAD_POLICY load_policy {};
	load_policy.NoLowMandatoryLabelImages = true;
	load_policy.PreferSystem32Images = true;

	// https://blogs.msdn.microsoft.com/oldnewthing/20160602-00/?p=93556
	std::vector<wchar_t> volumePath(LONG_PATH);
	if (GetVolumePathName(win32::GetExeLocation().c_str(), volumePath.data(), LONG_PATH))
	{
		load_policy.NoRemoteImages = GetDriveType(volumePath.data()) != DRIVE_REMOTE;
	}
	else
	{
		ErrorHandle(HRESULT_FROM_WIN32(GetLastError()), Error::Level::Log, L"Unable to get drive root.");
	}

	if (!SetProcessMitigationPolicy(ProcessImageLoadPolicy, &load_policy, sizeof(load_policy)))
	{
		ErrorHandle(HRESULT_FROM_WIN32(GetLastError()), Error::Level::Log, L"Couldn't set image load policy.");
	}
}

void InitializeTray(const HINSTANCE &hInstance)
{
	static MessageWindow window(L"TrayWindow", NAME, hInstance);

	window.RegisterCallback(NEW_TTB_INSTANCE, [](Window, WPARAM, LPARAM) {
		run.exit_reason = EXITREASON::NewInstance;
		run.is_running = false;
		return 0;
	});

	// TODO: doesn't works
	window.RegisterCallback(WM_DISPLAYCHANGE, [](Window, WPARAM, LPARAM) {
		RefreshHandles();
		return 0;
	});

	window.RegisterCallback(WM_TASKBARCREATED, [](Window, WPARAM, LPARAM) {
		RefreshHandles();
		return 0;
	});

	window.RegisterCallback(WM_CLOSE, [](Window, WPARAM, LPARAM) {
		run.exit_reason = EXITREASON::UserAction;
		run.is_running = false;
		return 0;
	});

#ifdef STORE
	window.RegisterCallback(WM_QUERYENDSESSION, [](Window, WPARAM, LPARAM) {
		// https://docs.microsoft.com/en-us/windows/uwp/porting/desktop-to-uwp-extensions#updates
		RegisterApplicationRestart(NULL, NULL);
		return TRUE;
	});
#endif


	static TrayContextMenu tray(window, MAKEINTRESOURCE(TRAYICON), MAKEINTRESOURCE(IDR_POPUP_MENU), hInstance);

	tray.BindEnum(IDM_BLUR, IDM_FLUENT, Config::TASKBAR_APPEARANCE, NORMAL_BUTTON_MAP);
	tray.BindEnum(IDM_DYNAMICWS_BLUR, IDM_DYNAMICWS_CLEAR, Config::DYNAMIC_APPEARANCE, DYNAMIC_BUTTON_MAP);
	tray.BindEnum(IDM_PEEK, IDM_NOPEEK, Config::PEEK, PEEK_BUTTON_MAP);

	for (const auto &button_pair : DYNAMIC_BUTTON_MAP)
	{
		tray.BindBool(button_pair.second, Config::DYNAMIC_WS, TrayContextMenu::ControlsEnabled);
	}

	tray.BindBool(IDM_DYNAMICWS_PEEK,          Config::DYNAMIC_WS,                TrayContextMenu::ControlsEnabled);
	tray.BindBool(IDM_DYNAMICWS,               Config::DYNAMIC_WS,                TrayContextMenu::Toggle);
	tray.BindBool(IDM_DYNAMICWS_REGULAR_COLOR, Config::DYNAMIC_USE_REGULAR_COLOR, TrayContextMenu::Toggle);
	tray.BindBool(IDM_DYNAMICWS_PEEK,          Config::DYNAMIC_REGULAR_ON_PEEK,   TrayContextMenu::Toggle);
	tray.BindBool(IDM_DYNAMICSTART,            Config::DYNAMIC_START,             TrayContextMenu::Toggle);
	tray.BindBool(IDM_VERBOSE,                 Config::VERBOSE,                   TrayContextMenu::Toggle);

	tray.RegisterContextMenuCallback(IDM_EXITWITHOUTSAVING, [](unsigned int) {
		run.exit_reason = EXITREASON::UserActionNoSave;
		run.is_running = false;
	});

	tray.RegisterContextMenuCallback(IDM_EXIT, [](unsigned int) {
		run.exit_reason = EXITREASON::UserAction;
		run.is_running = false;
	});

	tray.RegisterContextMenuCallback(IDM_COLOR,           [](unsigned int) {
		Util::PickColor(Config::TASKBAR_COLOR);
	});
	tray.RegisterContextMenuCallback(IDM_DYNAMICWS_COLOR, [](unsigned int) {
		Util::PickColor(Config::DYNAMIC_COLOR);
	});

	tray.RegisterCustomRefresh(RefreshMenu);

	tray.RegisterContextMenuCallback(IDM_OPENLOG, [](unsigned int) {
		std::thread([] {
			Util::EditFile(Log::file());
		}).detach();
	});

	tray.RegisterContextMenuCallback(IDM_CLEARBLACKLISTCACHE, [](unsigned int) {
		Blacklist::ClearCache();
	});

	tray.RegisterContextMenuCallback(IDM_RELOADSETTINGS, [](unsigned int) {
		Config::Parse(run.config_file);
	});

	tray.RegisterContextMenuCallback(IDM_EDITSETTINGS, [](unsigned int) {
		Config::Save(run.config_file);
		std::thread([] {
			Util::EditFile(run.config_file);
			Config::Parse(run.config_file);
		}).detach();
	});

	tray.RegisterContextMenuCallback(IDM_RETURNTODEFAULTSETTINGS, [](unsigned int) {
		ApplyStock(CONFIG_FILE);
		Config::Parse(run.config_file);
	});

	tray.RegisterContextMenuCallback(IDM_RELOADDYNAMICBLACKLIST, [](unsigned int) {
		Blacklist::Parse(run.exclude_file);
	});

	tray.RegisterContextMenuCallback(IDM_EDITDYNAMICBLACKLIST, [](unsigned int) {
		std::thread([] {
			Util::EditFile(run.exclude_file);
			Blacklist::Parse(run.exclude_file);
		}).detach();
	});

	tray.RegisterContextMenuCallback(IDM_RETURNTODEFAULTBLACKLIST, [](unsigned int) {
		ApplyStock(EXCLUDE_FILE);
		Blacklist::Parse(run.exclude_file);
	});

	tray.RegisterContextMenuCallback(IDM_AUTOSTART, [](unsigned int) {
		Autostart::SetStartupState(Autostart::GetStartupState() == Autostart::StartupState::Enabled ? Autostart::StartupState::Disabled : Autostart::StartupState::Enabled);
	});

	tray.RegisterContextMenuCallback(IDM_TIPS, [](unsigned int) {
		Util::OpenLink(L"https://github.com/TranslucentTB/TranslucentTB/wiki/Tips-and-tricks-for-a-better-looking-taskbar");
	});
}

int WINAPI wWinMain(const HINSTANCE hInstance, HINSTANCE, wchar_t *, int)
{
	HardenProcess();

	// If there already is another instance running, tell it to exit
	if (!win32::IsSingleInstance())
	{
		Window::Find(NAME, L"TrayWindow").send_message(NEW_TTB_INSTANCE);
	}

	InitializeWindowsRuntime();

	// Get configuration file paths
	GetPaths();

	// If the configuration files don't exist, restore the files and show welcome to the users
	if (!CheckAndRunWelcome())
	{
		return EXIT_FAILURE;
	}

	// Parse our configuration
	Config::Parse(run.config_file);
	Blacklist::Parse(run.exclude_file);

	// Initialize GUI
	InitializeTray(hInstance);

	// Populate our vectors
	RefreshHandles();

	// Undoc'd, allows to detect when Aero Peek starts and stops
	EventHook peek_hook(0x21, 0x22, HandleAeroPeekEvent, WINEVENT_OUTOFCONTEXT);

	// Message loop
	while (run.is_running)
	{
		MSG msg;
		if (PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		SetTaskbarBlur();
		std::this_thread::sleep_for(std::chrono::milliseconds(Config::SLEEP_TIME));
	}

	// If it's a new instance, don't save or restore taskbar to default
	if (run.exit_reason != EXITREASON::NewInstance)
	{
		if (run.exit_reason != EXITREASON::UserActionNoSave)
		{
			Config::Save(run.config_file);
		}

		// Restore default taskbar appearance
		TogglePeek(true);
		for (const auto &taskbar : run.taskbars)
		{
			SetWindowBlur(taskbar.second.first, swca::ACCENT::ACCENT_NORMAL, NULL);
		}
	}

	return EXIT_SUCCESS;
}

#pragma endregion