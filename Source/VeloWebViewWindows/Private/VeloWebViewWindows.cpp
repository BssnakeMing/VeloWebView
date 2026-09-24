#include "VeloWebViewWindows.h"

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/ThreadSafeCounter.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if PLATFORM_WINDOWS
	#include "Windows/AllowWindowsPlatformTypes.h"
	#include <windows.h>
	#include <objbase.h>
	#if VELO_WEBVIEW_HAS_WEBVIEW2
	#include <intrin.h>
	#ifndef InterlockedIncrement
	#define InterlockedIncrement _InterlockedIncrement
	#endif
	#ifndef InterlockedDecrement
	#define InterlockedDecrement _InterlockedDecrement
	#endif
	#include <wrl.h>
	#include <wrl/client.h>
	#include <WebView2.h>
	#include "Windows/HideWindowsPlatformTypes.h"
	// UE's Windows wrapper intentionally undefines these macros after including
	// platform headers. WRL uses the Win32 spellings in template bodies, so keep
	// the intrinsic aliases visible for this translation unit.
	#ifndef InterlockedIncrement
	#define InterlockedIncrement _InterlockedIncrement
	#endif
	#ifndef InterlockedDecrement
	#define InterlockedDecrement _InterlockedDecrement
	#endif
	#else
	#include "Windows/HideWindowsPlatformTypes.h"
	#endif
#endif

DEFINE_LOG_CATEGORY(LogVeloWebViewWindows);

namespace VeloWebViewWindowsPrivate
{
	static FThreadSafeCounter GNextEnvironmentId;

	static void DispatchToGameThread(TFunction<void()>&& Function)
	{
		if (IsInGameThread())
		{
			Function();
		}
		else
		{
			AsyncTask(ENamedThreads::GameThread, MoveTemp(Function));
		}
	}

#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
	static FString TakeOwnedString(LPWSTR Value)
	{
		FString Result = Value ? FString(Value) : FString();
		if (Value)
		{
			::CoTaskMemFree(Value);
		}
		return Result;
	}

	static FString SuggestedFilenameFromDisposition(const FString& ContentDisposition)
	{
		int32 FilenamePos = ContentDisposition.Find(TEXT("filename="), ESearchCase::IgnoreCase);
		if (FilenamePos == INDEX_NONE)
		{
			return FString();
		}
		FString Filename = ContentDisposition.Mid(FilenamePos + 9).TrimStartAndEnd();
		if (Filename.StartsWith(TEXT("\"")) && Filename.EndsWith(TEXT("\"")) && Filename.Len() >= 2)
		{
			Filename = Filename.Mid(1, Filename.Len() - 2);
		}
		const int32 Semicolon = Filename.Find(TEXT(";"));
		if (Semicolon != INDEX_NONE)
		{
			Filename.LeftInline(Semicolon);
		}
		return Filename.TrimStartAndEnd();
	}

	static bool IsExternalURL(const FString& URL)
	{
		return URL.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase)
			|| URL.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase)
			|| URL.StartsWith(TEXT("mailto:"), ESearchCase::IgnoreCase)
			|| URL.StartsWith(TEXT("tel:"), ESearchCase::IgnoreCase);
	}

	static bool IsSystemURL(const FString& URL)
	{
		return URL.StartsWith(TEXT("mailto:"), ESearchCase::IgnoreCase)
			|| URL.StartsWith(TEXT("tel:"), ESearchCase::IgnoreCase);
	}

	#endif
}

class FVeloWebViewWindows::FImpl : public TSharedFromThis<FVeloWebViewWindows::FImpl>
{
public:
	FImpl()
		: State(EVeloWebViewWindowsState::Uninitialized)
		, HostWindow(nullptr)
		, bVisible(true)
		, bJavaScriptEnabled(true)
		, bMediaAutoplay(false)
		, bMuted(false)
		, bTransparent(false)
		, bComInitialized(false)
	{
		UserDataFolder = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("VeloWebView"),
			FString::Printf(TEXT("WebView_%d"), VeloWebViewWindowsPrivate::GNextEnvironmentId.Increment()));
	}

	~FImpl()
	{
		Shutdown();
	}

	bool Initialize(void* InHostWindow, const FVeloWebViewWindowsCallbacks& InCallbacks)
	{
		if (State == EVeloWebViewWindowsState::Destroyed || State == EVeloWebViewWindowsState::Initializing || IsReady())
		{
			UE_LOG(LogVeloWebViewWindows, Verbose, TEXT("Initialize ignored for an already initialized/destroyed instance"));
			return IsReady();
		}

		Callbacks = InCallbacks;
		HostWindow = InHostWindow;
		State = EVeloWebViewWindowsState::Initializing;

#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		HRESULT ComResult = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		if (SUCCEEDED(ComResult))
		{
			bComInitialized = true;
		}
		else if (ComResult != RPC_E_CHANGED_MODE)
		{
			Fail(static_cast<int32>(ComResult), TEXT("Unable to initialize the COM apartment"));
			return false;
		}

		if (!HostWindow)
		{
			// A host can be assigned later by SetHostWindow.  Do not create a
			// controller against a desktop window, which would outlive the widget.
			Fail(E_FAIL, TEXT("A native host window is required for WebView2"));
			return false;
		}

		if (!LoadWebView2Loader())
		{
			Fail(LoaderErrorCode, LoaderError.IsEmpty()
				? FString(TEXT("Unable to load WebView2Loader.dll. Install the official Evergreen Runtime and stage the official Win64 loader."))
				: LoaderError);
			return false;
		}

		CreateEnvironment();
		return true;
#else
		Fail(0, TEXT("This build does not contain the official Microsoft WebView2 SDK. Copy the SDK to ThirdParty/WebView2 and regenerate project files."));
		return false;
#endif
	}

	void Shutdown()
	{
		if (State == EVeloWebViewWindowsState::Destroyed)
		{
			return;
		}

#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		// Remove event handlers before releasing the controller.  This prevents a
		// late WebView2 callback from retaining this instance during PIE shutdown.
		UnregisterEvents();
		WebView.Reset();
		Controller.Reset();
		Environment.Reset();
		if (LoaderModule)
		{
			::FreeLibrary(LoaderModule);
			LoaderModule = nullptr;
		}
		if (bComInitialized)
		{
			::CoUninitialize();
			bComInitialized = false;
		}
#endif
		PendingURL.Empty();
		State = EVeloWebViewWindowsState::Destroyed;
	}

	EVeloWebViewWindowsState GetState() const { return State; }
	bool IsReady() const { return State == EVeloWebViewWindowsState::Ready || State == EVeloWebViewWindowsState::Loading; }

	void SetHostWindow(void* InHostWindow)
	{
		HostWindow = InHostWindow;
#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		if (Controller && HostWindow)
		{
			// WebView2's windowed controller uses the parent supplied at creation;
			// bounds are still valid after a host resize/move.
			ApplyBounds();
		}
#endif
	}

	void SetBounds(const FIntRect& InBounds)
	{
		Bounds = InBounds;
#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		ApplyBounds();
#endif
	}

	void SetVisibility(EVisibility InVisibility)
	{
		bVisible = InVisibility != EVisibility::Collapsed && InVisibility != EVisibility::Hidden;
#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		if (Controller)
		{
			Controller->put_IsVisible(bVisible ? 1 : 0);
		}
#endif
	}

	void LoadURL(const FString& InURL)
	{
		if (!IsReady())
		{
			PendingURL = InURL;
			return;
		}
#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		if (WebView)
		{
			State = EVeloWebViewWindowsState::Loading;
			WebView->Navigate(*InURL);
		}
#else
		(void)InURL;
#endif
	}

	void LoadHTML(const FString& InHTML)
	{
		if (!IsReady())
		{
			PendingHTML = InHTML;
			PendingURL.Empty();
			return;
		}
#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		if (WebView)
		{
			State = EVeloWebViewWindowsState::Loading;
			WebView->NavigateToString(*InHTML);
		}
#else
		(void)InHTML;
#endif
	}

	void Reload()
	{
#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		if (WebView && IsReady()) WebView->Reload();
#endif
	}

	void Stop()
	{
#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		if (WebView && IsReady()) WebView->Stop();
#endif
	}

	void GoBack()
	{
#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		if (WebView && IsReady()) WebView->GoBack();
#endif
	}

	void GoForward()
	{
#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		if (WebView && IsReady()) WebView->GoForward();
#endif
	}

	bool CanGoBack() const
	{
#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		BOOL Value = 0;
		return WebView && SUCCEEDED(WebView->get_CanGoBack(&Value)) && Value;
#else
		return false;
#endif
	}

	bool CanGoForward() const
	{
#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		BOOL Value = 0;
		return WebView && SUCCEEDED(WebView->get_CanGoForward(&Value)) && Value;
#else
		return false;
#endif
	}

	void ExecuteJavaScript(const FString& InScript)
	{
#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		if (WebView && IsReady())
		{
			WebView->ExecuteScript(*InScript, nullptr);
		}
#else
		(void)InScript;
#endif
	}

	void SetJavaScriptEnabled(bool bEnabled)
	{
		bJavaScriptEnabled = bEnabled;
#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		if (Settings) Settings->put_IsScriptEnabled(bEnabled ? 1 : 0);
#endif
	}

	void SetMediaAutoplay(bool bEnabled)
	{
		bMediaAutoplay = bEnabled;
		// Chromium's autoplay policy is selected when the environment is created.
		// If called after creation, apply a best-effort muted policy to existing
		// media without changing browser security flags.
		if (bEnabled && IsReady())
		{
			ExecuteJavaScript(TEXT("document.querySelectorAll('video,audio').forEach(function(e){e.autoplay=true;});"));
		}
	}

	void SetMuted(bool bInMuted)
	{
		bMuted = bInMuted;
		if (IsReady())
		{
			ExecuteJavaScript(bMuted
				? TEXT("document.querySelectorAll('video,audio').forEach(function(e){e.muted=true;});")
				: TEXT("document.querySelectorAll('video,audio').forEach(function(e){e.muted=false;});"));
		}
	}

	void SetUserAgent(const FString& InUserAgent)
	{
		UserAgent = InUserAgent;
#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		if (Settings)
		{
			Microsoft::WRL::ComPtr<ICoreWebView2Settings2> Settings2;
			if (SUCCEEDED(Settings.As(&Settings2)) && Settings2)
			{
				Settings2->put_UserAgent(InUserAgent.IsEmpty() ? nullptr : *InUserAgent);
			}
		}
#endif
	}

	void SetBackgroundTransparent(bool bInTransparent)
	{
		bTransparent = bInTransparent;
#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		if (Controller)
		{
			Microsoft::WRL::ComPtr<ICoreWebView2Controller2> Controller2;
			if (SUCCEEDED(Controller.As(&Controller2)) && Controller2)
			{
				COREWEBVIEW2_COLOR Color;
				Color.A = bTransparent ? 0 : 255;
				Color.R = Color.G = Color.B = 0;
				Controller2->put_DefaultBackgroundColor(Color);
			}
		}
#endif
	}

	void SetAllowMixedContent(bool bAllow)
	{
		bAllowMixedContent = bAllow;
		// WebView2 follows Chromium's mixed-content policy. There is no global
		// "allow all" switch in the stable SDK; keep the secure default and expose
		// the value for future per-request policy handling.
	}

	void SetNewWindowPolicy(EVeloWebViewNewWindowPolicy Policy)
	{
		NewWindowPolicy = Policy;
	}

	void SetFocus(bool bFocused)
	{
#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		if (Controller && bFocused)
		{
			Controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
		}
#else
		(void)bFocused;
#endif
	}

	void ClearCache()
	{
#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		if (WebView)
		{
			Microsoft::WRL::ComPtr<ICoreWebView2_13> WebView13;
			if (SUCCEEDED(WebView.As(&WebView13)) && WebView13)
			{
				Microsoft::WRL::ComPtr<ICoreWebView2Profile> Profile;
				if (SUCCEEDED(WebView13->get_Profile(&Profile)) && Profile)
				{
					Microsoft::WRL::ComPtr<ICoreWebView2Profile2> Profile2;
					if (SUCCEEDED(Profile.As(&Profile2)) && Profile2)
					{
						Profile2->ClearBrowsingDataAll(nullptr);
					}
				}
			}
		}
#endif
	}

	void ClearCookies()
	{
		CookieCache.Empty();
#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		if (WebView)
		{
			Microsoft::WRL::ComPtr<ICoreWebView2_2> WebView2;
			if (FAILED(WebView.As(&WebView2)) || !WebView2)
			{
				return;
			}
			Microsoft::WRL::ComPtr<ICoreWebView2CookieManager> CookieManager;
			if (SUCCEEDED(WebView2->get_CookieManager(&CookieManager)) && CookieManager)
			{
				CookieManager->DeleteAllCookies();
			}
		}
#endif
	}

	void SetCookie(const FString& URL, const FString& Name, const FString& Value,
		const FString& Domain, const FString& Path, bool bSecure)
	{
		CookieCache.Add(URL + TEXT("\n") + Name, Value);
#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		if (WebView)
		{
			Microsoft::WRL::ComPtr<ICoreWebView2_2> WebView2;
			if (FAILED(WebView.As(&WebView2)) || !WebView2)
			{
				return;
			}
			Microsoft::WRL::ComPtr<ICoreWebView2CookieManager> CookieManager;
			if (SUCCEEDED(WebView2->get_CookieManager(&CookieManager)) && CookieManager)
			{
				Microsoft::WRL::ComPtr<ICoreWebView2Cookie> Cookie;
				if (SUCCEEDED(CookieManager->CreateCookie(*Name, *Value, Domain.IsEmpty() ? nullptr : *Domain, Path.IsEmpty() ? TEXT("/") : *Path, &Cookie)) && Cookie)
				{
					Cookie->put_IsSecure(bSecure ? 1 : 0);
					CookieManager->AddOrUpdateCookie(Cookie.Get());
				}
			}
		}
#else
		(void)Domain;
		(void)Path;
		(void)bSecure;
#endif
	}

	FString GetCookie(const FString& URL) const
	{
		FString Result;
		for (const TPair<FString, FString>& Pair : CookieCache)
		{
			if (Pair.Key.StartsWith(URL + TEXT("\n")))
			{
				if (!Result.IsEmpty()) Result += TEXT("; ");
				Result += Pair.Key.Mid(URL.Len() + 1) + TEXT("=") + Pair.Value;
			}
		}
#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		// CookieManager::GetCookies is asynchronous in WebView2. The local cache
		// is returned synchronously; callers can observe updates after navigation.
#endif
		return Result;
	}

	void DeleteCookie(const FString& URL, const FString& Name)
	{
		CookieCache.Remove(URL + TEXT("\n") + Name);
#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		if (WebView)
		{
			Microsoft::WRL::ComPtr<ICoreWebView2_2> WebView2;
			if (FAILED(WebView.As(&WebView2)) || !WebView2)
			{
				return;
			}
			Microsoft::WRL::ComPtr<ICoreWebView2CookieManager> CookieManager;
			if (SUCCEEDED(WebView2->get_CookieManager(&CookieManager)) && CookieManager)
			{
				Microsoft::WRL::ComPtr<ICoreWebView2Cookie> Cookie;
				if (SUCCEEDED(CookieManager->CreateCookie(*Name, TEXT(""), nullptr, TEXT("/"), &Cookie)) && Cookie)
				{
					CookieManager->DeleteCookie(Cookie.Get());
				}
			}
		}
#endif
	}

	void GrantPermission(uint8 Permission)
	{
#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		if (Microsoft::WRL::ComPtr<ICoreWebView2PermissionRequestedEventArgs>* Pending = PendingPermissions.Find(Permission))
		{
			if (Pending->Get()) (*Pending)->put_State(COREWEBVIEW2_PERMISSION_STATE_ALLOW);
			PendingPermissions.Remove(Permission);
		}
#else
		(void)Permission;
#endif
	}

	void DenyPermission(uint8 Permission)
	{
#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		if (Microsoft::WRL::ComPtr<ICoreWebView2PermissionRequestedEventArgs>* Pending = PendingPermissions.Find(Permission))
		{
			if (Pending->Get()) (*Pending)->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
			PendingPermissions.Remove(Permission);
		}
#else
		(void)Permission;
#endif
	}

	FString GetURL() const
	{
#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		if (WebView)
		{
			LPWSTR Value = nullptr;
			if (SUCCEEDED(WebView->get_Source(&Value))) return VeloWebViewWindowsPrivate::TakeOwnedString(Value);
		}
#endif
		return LastURL;
	}

	FString GetTitle() const
	{
#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
		if (WebView)
		{
			LPWSTR Value = nullptr;
			if (SUCCEEDED(WebView->get_DocumentTitle(&Value))) return VeloWebViewWindowsPrivate::TakeOwnedString(Value);
		}
#endif
		return LastTitle;
	}

	FString GetLastError() const { return LastError; }

	void SetPendingURL(const FString& InURL)
	{
		PendingURL = InURL;
	}

private:
	void Fail(int32 ErrorCode, const FString& Description)
	{
		State = EVeloWebViewWindowsState::Failed;
		FVeloWebViewWindowsError Error;
		Error.ErrorCode = ErrorCode;
		Error.Description = Description.IsEmpty() ? TEXT("Unknown WebView2 error") : Description;
		Error.URL = LastURL;
		LastError = Error.Description;
		if (Callbacks.OnPageError)
		{
			FVeloWebViewWindowsCallbacks LocalCallbacks = Callbacks;
			VeloWebViewWindowsPrivate::DispatchToGameThread([LocalCallbacks, Error]() mutable
			{
				if (LocalCallbacks.OnPageError) LocalCallbacks.OnPageError(Error);
			});
		}
		UE_LOG(LogVeloWebViewWindows, Error, TEXT("%s"), *Error.Description);
	}

	void Fail(int32 ErrorCode, const TCHAR* Description)
	{
		Fail(ErrorCode, FString(Description ? Description : TEXT("Unknown WebView2 error")));
	}

#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
	using FCreateEnvironmentWithOptions = HRESULT(STDAPICALLTYPE*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);

	bool LoadWebView2Loader()
	{
		if (LoaderModule) return true;

		const FString PluginLoader = FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT("VeloWebView/Binaries/Win64/WebView2Loader.dll"));
		DWORD BareLoadError = ERROR_SUCCESS;
		DWORD PluginLoadError = ERROR_SUCCESS;

		// Prefer the copy staged with this plugin.  Falling back to the normal
		// DLL search path keeps installed/plugin-hosted layouts working.
		LoaderModule = ::LoadLibraryW(*PluginLoader);
		if (!LoaderModule)
		{
			PluginLoadError = ::GetLastError();
			LoaderModule = ::LoadLibraryW(L"WebView2Loader.dll");
			if (!LoaderModule)
			{
				BareLoadError = ::GetLastError();
				const DWORD ReportedError = PluginLoadError != ERROR_SUCCESS ? PluginLoadError : BareLoadError;
				LoaderErrorCode = static_cast<int32>(HRESULT_FROM_WIN32(ReportedError));
				LoaderError = FString::Printf(
					TEXT("Unable to load WebView2Loader.dll (Win32 error %lu / HRESULT 0x%08X). Plugin path: %s; default search error: %lu. Ensure the staged DLL is the official Win64 (x64) build and install the Evergreen Runtime."),
					static_cast<unsigned long>(ReportedError),
					static_cast<unsigned int>(LoaderErrorCode),
					*PluginLoader,
					static_cast<unsigned long>(BareLoadError));
				return false;
			}
		}

		#pragma warning(push)
		#pragma warning(disable : 4191)
		CreateEnvironmentWithOptions = reinterpret_cast<FCreateEnvironmentWithOptions>(::GetProcAddress(LoaderModule, "CreateCoreWebView2EnvironmentWithOptions"));
		#pragma warning(pop)
		if (!CreateEnvironmentWithOptions)
		{
			const DWORD ExportError = ::GetLastError();
			LoaderErrorCode = static_cast<int32>(HRESULT_FROM_WIN32(ExportError));
			LoaderError = FString::Printf(
				TEXT("WebView2Loader.dll was loaded but does not export CreateCoreWebView2EnvironmentWithOptions (Win32 error %lu / HRESULT 0x%08X). Verify that the official WebView2 loader is staged."),
				static_cast<unsigned long>(ExportError),
				static_cast<unsigned int>(LoaderErrorCode));
			::FreeLibrary(LoaderModule);
			LoaderModule = nullptr;
			return false;
		}

		return true;
	}

	void CreateEnvironment()
	{
		IFileManager::Get().MakeDirectory(*UserDataFolder, true);
		const TWeakPtr<FImpl> WeakImpl = AsShared();
		HRESULT Result = CreateEnvironmentWithOptions(
			nullptr,
			*UserDataFolder,
			nullptr,
			Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
				[WeakImpl](HRESULT ErrorCode, ICoreWebView2Environment* InEnvironment) -> HRESULT
				{
					const TSharedPtr<FImpl> Self = WeakImpl.Pin();
					if (!Self)
					{
						return S_OK;
					}
					if (FAILED(ErrorCode) || !InEnvironment)
					{
						Self->Fail(static_cast<int32>(ErrorCode), TEXT("WebView2 Runtime is not installed or could not create an environment"));
						return ErrorCode;
					}
					Self->Environment = InEnvironment;
					Self->CreateController();
					return S_OK;
				}).Get());

		if (FAILED(Result))
		{
			Fail(static_cast<int32>(Result), TEXT("CreateCoreWebView2EnvironmentWithOptions failed"));
		}
	}

	void CreateController()
	{
		HWND Parent = reinterpret_cast<HWND>(HostWindow);
		if (!Environment || !Parent)
		{
			Fail(E_INVALIDARG, TEXT("Invalid host HWND for WebView2 controller"));
			return;
		}

		HRESULT Result = Environment->CreateCoreWebView2Controller(
			Parent,
			Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
				[WeakImpl = TWeakPtr<FImpl>(AsShared())](HRESULT ErrorCode, ICoreWebView2Controller* InController) -> HRESULT
				{
					const TSharedPtr<FImpl> Self = WeakImpl.Pin();
					if (!Self)
					{
						return S_OK;
					}
					if (FAILED(ErrorCode) || !InController)
					{
						Self->Fail(static_cast<int32>(ErrorCode), TEXT("WebView2 controller creation failed"));
						return ErrorCode;
					}
					Self->Controller = InController;
					Self->Controller->put_IsVisible(Self->bVisible ? 1 : 0);
					Self->ApplyBounds();
					if (FAILED(Self->Controller->get_CoreWebView2(&Self->WebView)) || !Self->WebView)
					{
						Self->Fail(E_FAIL, TEXT("WebView2 controller did not provide a CoreWebView2 instance"));
						return E_FAIL;
					}
					if (FAILED(Self->WebView->get_Settings(&Self->Settings)) || !Self->Settings)
					{
						Self->Fail(E_FAIL, TEXT("WebView2 settings are unavailable"));
						return E_FAIL;
					}
					Self->ConfigureSettings();
					Self->RegisterEvents();
					Self->InstallBridge();
					Self->State = EVeloWebViewWindowsState::Ready;
					if (!Self->PendingHTML.IsEmpty())
					{
						const FString HTML = MoveTemp(Self->PendingHTML);
						Self->LoadHTML(HTML);
					}
					else if (!Self->PendingURL.IsEmpty())
					{
						const FString URL = MoveTemp(Self->PendingURL);
						Self->LoadURL(URL);
					}
					return S_OK;
				}).Get());

		if (FAILED(Result))
		{
			Fail(static_cast<int32>(Result), TEXT("CreateCoreWebView2Controller failed"));
		}
	}

	void ConfigureSettings()
	{
		Settings->put_IsScriptEnabled(bJavaScriptEnabled ? 1 : 0);
		Settings->put_IsWebMessageEnabled(1);
		Settings->put_AreDefaultScriptDialogsEnabled(1);
		Settings->put_IsStatusBarEnabled(0);
		Settings->put_AreDevToolsEnabled(!UE_BUILD_SHIPPING);
		SetUserAgent(UserAgent);
		SetBackgroundTransparent(bTransparent);
	}

		void InstallBridge()
		{
			static const TCHAR* BridgeScript =
				TEXT("(function(){window.VeloWebView=window.VeloWebView||{};"
				"window.VeloWebView.postMessage=function(m){"
				"try{window.chrome.webview.postMessage(String(m));}catch(e){}};"
				"if(!window.__VeloConsoleInstalled){window.__VeloConsoleInstalled=true;"
				"['log','warn','error','info','debug'].forEach(function(k){var o=console[k];"
				"console[k]=function(){try{var a=Array.prototype.slice.call(arguments).map(function(v){"
				"try{return typeof v==='string'?v:JSON.stringify(v);}catch(e){return String(v);}});"
				"window.chrome.webview.postMessage('__VELO_CONSOLE__'+JSON.stringify({message:a.join(' '),source:String(location.href),line:0}));"
				"}catch(e){}if(o){try{o.apply(console,arguments);}catch(e){}}};});}})();");
			WebView->AddScriptToExecuteOnDocumentCreated(BridgeScript, nullptr);
		}

	void RegisterEvents()
	{
		using Microsoft::WRL::Callback;

		WebView->add_NewWindowRequested(
			Callback<ICoreWebView2NewWindowRequestedEventHandler>(
				[WeakImpl = TWeakPtr<FImpl>(AsShared())](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* Args) -> HRESULT
				{
					const TSharedPtr<FImpl> Self = WeakImpl.Pin();
					if (!Self || !Args) return S_OK;

					LPWSTR RawURL = nullptr;
					const FString URL = SUCCEEDED(Args->get_Uri(&RawURL))
						? VeloWebViewWindowsPrivate::TakeOwnedString(RawURL) : FString();
					BOOL bUserInitiated = 0;
					Args->get_IsUserInitiated(&bUserInitiated);
					// Prevent WebView2 from creating an unmanaged native window. Run the
					// selected action after this COM callback to avoid reentrant Navigate.
					Args->put_Handled(1);
					const EVeloWebViewNewWindowPolicy Policy = Self->NewWindowPolicy;
					FVeloWebViewWindowsCallbacks Local = Self->Callbacks;
					AsyncTask(ENamedThreads::GameThread, [WeakImpl, URL, bWasUserInitiated = bUserInitiated != 0, Policy, Local]() mutable
					{
						const TSharedPtr<FImpl> Instance = WeakImpl.Pin();
						if (!Instance || Instance->State == EVeloWebViewWindowsState::Destroyed) return;
						if (!URL.IsEmpty() && !URL.Equals(TEXT("about:blank"), ESearchCase::IgnoreCase))
						{
							if (Policy == EVeloWebViewNewWindowPolicy::OpenInSameView)
							{
								if (VeloWebViewWindowsPrivate::IsSystemURL(URL))
								{
									FPlatformProcess::LaunchURL(*URL, nullptr, nullptr);
								}
								else
								{
									Instance->LoadURL(URL);
								}
							}
							else if (Policy == EVeloWebViewNewWindowPolicy::OpenExternal && VeloWebViewWindowsPrivate::IsExternalURL(URL))
							{
								FPlatformProcess::LaunchURL(*URL, nullptr, nullptr);
							}
						}
						if (Local.OnNewWindowRequested) Local.OnNewWindowRequested(URL, bWasUserInitiated);
					});
					return S_OK;
				}).Get(), &NewWindowRequestedToken);

		WebView->add_NavigationStarting(
			Callback<ICoreWebView2NavigationStartingEventHandler>(
				[this, WeakImpl = TWeakPtr<FImpl>(AsShared())](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* Args) -> HRESULT
				{
					const TSharedPtr<FImpl> Self = WeakImpl.Pin();
					if (!Self) return S_OK;
					LPWSTR RawURL = nullptr;
					if (Args && SUCCEEDED(Args->get_Uri(&RawURL)))
					{
						const FString URL = VeloWebViewWindowsPrivate::TakeOwnedString(RawURL);
						LastURL = URL;
						if (Callbacks.OnNavigationRequested)
						{
							FVeloWebViewWindowsCallbacks Local = Callbacks;
							VeloWebViewWindowsPrivate::DispatchToGameThread([Local, URL]() mutable { if (Local.OnNavigationRequested) Local.OnNavigationRequested(URL); });
						}
						if (Callbacks.OnPageStarted)
						{
							FVeloWebViewWindowsCallbacks Local = Callbacks;
							VeloWebViewWindowsPrivate::DispatchToGameThread([Local, URL]() mutable { if (Local.OnPageStarted) Local.OnPageStarted(URL); });
						}
					}
					return S_OK;
				}).Get(), &NavigationStartingToken);

		WebView->add_SourceChanged(
			Callback<ICoreWebView2SourceChangedEventHandler>(
				[this, WeakImpl = TWeakPtr<FImpl>(AsShared())](ICoreWebView2* Sender, ICoreWebView2SourceChangedEventArgs*) -> HRESULT
				{
					const TSharedPtr<FImpl> Self = WeakImpl.Pin();
					if (!Self) return S_OK;
					LPWSTR RawURL = nullptr;
					if (Sender && SUCCEEDED(Sender->get_Source(&RawURL)))
					{
						const FString URL = VeloWebViewWindowsPrivate::TakeOwnedString(RawURL);
						LastURL = URL;
						if (Callbacks.OnURLChanged)
						{
							FVeloWebViewWindowsCallbacks Local = Callbacks;
							VeloWebViewWindowsPrivate::DispatchToGameThread([Local, URL]() mutable { if (Local.OnURLChanged) Local.OnURLChanged(URL); });
						}
					}
					return S_OK;
				}).Get(), &SourceChangedToken);

		WebView->add_DocumentTitleChanged(
			Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
				[this, WeakImpl = TWeakPtr<FImpl>(AsShared())](ICoreWebView2* Sender, IUnknown*) -> HRESULT
				{
					const TSharedPtr<FImpl> Self = WeakImpl.Pin();
					if (!Self) return S_OK;
					LPWSTR RawTitle = nullptr;
					if (Sender && SUCCEEDED(Sender->get_DocumentTitle(&RawTitle)))
					{
						const FString Title = VeloWebViewWindowsPrivate::TakeOwnedString(RawTitle);
						LastTitle = Title;
						if (Callbacks.OnTitleChanged)
						{
							FVeloWebViewWindowsCallbacks Local = Callbacks;
							VeloWebViewWindowsPrivate::DispatchToGameThread([Local, Title]() mutable { if (Local.OnTitleChanged) Local.OnTitleChanged(Title); });
						}
					}
					return S_OK;
				}).Get(), &DocumentTitleChangedToken);

		WebView->add_NavigationCompleted(
			Callback<ICoreWebView2NavigationCompletedEventHandler>(
				[this, WeakImpl = TWeakPtr<FImpl>(AsShared())](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* Args) -> HRESULT
				{
					const TSharedPtr<FImpl> Self = WeakImpl.Pin();
					if (!Self) return S_OK;
					BOOL Success = 0;
					COREWEBVIEW2_WEB_ERROR_STATUS ErrorStatus = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
					if (Args)
					{
						Args->get_IsSuccess(&Success);
						Args->get_WebErrorStatus(&ErrorStatus);
					}
					if (Success)
					{
						State = EVeloWebViewWindowsState::Ready;
						if (Callbacks.OnPageLoaded)
						{
							FVeloWebViewWindowsCallbacks Local = Callbacks;
							const FString URL = LastURL;
							VeloWebViewWindowsPrivate::DispatchToGameThread([Local, URL]() mutable { if (Local.OnPageLoaded) Local.OnPageLoaded(URL); });
						}
					}
					else
					{
						FVeloWebViewWindowsError Error;
						Error.ErrorCode = static_cast<int32>(ErrorStatus);
						Error.Description = TEXT("Navigation did not complete successfully");
						Error.URL = LastURL;
						if (Callbacks.OnPageError)
						{
							FVeloWebViewWindowsCallbacks Local = Callbacks;
							VeloWebViewWindowsPrivate::DispatchToGameThread([Local, Error]() mutable { if (Local.OnPageError) Local.OnPageError(Error); });
						}
					}
					return S_OK;
				}).Get(), &NavigationCompletedToken);

		WebView->add_WebMessageReceived(
			Callback<ICoreWebView2WebMessageReceivedEventHandler>(
				[this, WeakImpl = TWeakPtr<FImpl>(AsShared())](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* Args) -> HRESULT
				{
					const TSharedPtr<FImpl> Self = WeakImpl.Pin();
					if (!Self) return S_OK;
					LPWSTR RawMessage = nullptr;
					if (Args && SUCCEEDED(Args->TryGetWebMessageAsString(&RawMessage)))
						{
							const FString Message = VeloWebViewWindowsPrivate::TakeOwnedString(RawMessage);
							static const FString ConsolePrefix = TEXT("__VELO_CONSOLE__");
							if (Message.StartsWith(ConsolePrefix) && Callbacks.OnConsoleMessage)
							{
								FString ConsoleMessage;
								FString ConsoleSource = LastURL;
								int32 ConsoleLine = 0;
								const FString Payload = Message.Mid(ConsolePrefix.Len());
								TSharedPtr<FJsonObject> JsonObject;
								const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Payload);
								if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
								{
									JsonObject->TryGetStringField(TEXT("message"), ConsoleMessage);
									JsonObject->TryGetStringField(TEXT("source"), ConsoleSource);
									double LineValue = 0.0;
									if (JsonObject->TryGetNumberField(TEXT("line"), LineValue)) ConsoleLine = FMath::RoundToInt(LineValue);
								}
								else
								{
									ConsoleMessage = Payload;
								}
								FVeloWebViewWindowsCallbacks Local = Callbacks;
								VeloWebViewWindowsPrivate::DispatchToGameThread([Local, ConsoleMessage, ConsoleSource, ConsoleLine]() mutable
								{
									if (Local.OnConsoleMessage) Local.OnConsoleMessage(ConsoleMessage, ConsoleSource, ConsoleLine);
								});
							}
							else if (Callbacks.OnJavaScriptMessage)
							{
								FVeloWebViewWindowsCallbacks Local = Callbacks;
								VeloWebViewWindowsPrivate::DispatchToGameThread([Local, Message]() mutable { if (Local.OnJavaScriptMessage) Local.OnJavaScriptMessage(Message); });
						}
					}
					return S_OK;
				}).Get(), &WebMessageReceivedToken);

		WebView->add_ContainsFullScreenElementChanged(
			Callback<ICoreWebView2ContainsFullScreenElementChangedEventHandler>(
				[this, WeakImpl = TWeakPtr<FImpl>(AsShared())](ICoreWebView2* Sender, IUnknown*) -> HRESULT
				{
					const TSharedPtr<FImpl> Self = WeakImpl.Pin();
					if (!Self) return S_OK;
					BOOL Fullscreen = 0;
					if (Sender) Sender->get_ContainsFullScreenElement(&Fullscreen);
					if (Callbacks.OnFullscreenChanged)
					{
						FVeloWebViewWindowsCallbacks Local = Callbacks;
						VeloWebViewWindowsPrivate::DispatchToGameThread([Local, Fullscreen]() mutable { if (Local.OnFullscreenChanged) Local.OnFullscreenChanged(Fullscreen != 0); });
					}
					return S_OK;
					}).Get(), &FullscreenToken);

		// DownloadStarting was added on ICoreWebView2_4. Querying the extension
		// keeps the module compatible with older Evergreen runtimes while still
		// exposing the event whenever the runtime supports it.
		Microsoft::WRL::ComPtr<ICoreWebView2_4> WebView4;
		if (SUCCEEDED(WebView.As(&WebView4)) && WebView4)
		{
			WebView4->add_DownloadStarting(
				Callback<ICoreWebView2DownloadStartingEventHandler>(
					[this, WeakImpl = TWeakPtr<FImpl>(AsShared())](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* Args) -> HRESULT
					{
						const TSharedPtr<FImpl> Self = WeakImpl.Pin();
						if (!Self) return S_OK;
						Microsoft::WRL::ComPtr<ICoreWebView2DownloadOperation> Operation;
						if (Args && SUCCEEDED(Args->get_DownloadOperation(&Operation)) && Operation)
						{
							LPWSTR RawURL = nullptr;
							LPWSTR RawDisposition = nullptr;
							LPWSTR RawMime = nullptr;
							Operation->get_Uri(&RawURL);
							Operation->get_ContentDisposition(&RawDisposition);
							Operation->get_MimeType(&RawMime);
							const FString URL = VeloWebViewWindowsPrivate::TakeOwnedString(RawURL);
							const FString Disposition = VeloWebViewWindowsPrivate::TakeOwnedString(RawDisposition);
							const FString MimeType = VeloWebViewWindowsPrivate::TakeOwnedString(RawMime);
							FString Filename = VeloWebViewWindowsPrivate::SuggestedFilenameFromDisposition(Disposition);
							if (Filename.IsEmpty()) Filename = FPaths::GetCleanFilename(URL);
							if (Callbacks.OnDownloadRequested)
							{
								FVeloWebViewWindowsCallbacks Local = Callbacks;
								VeloWebViewWindowsPrivate::DispatchToGameThread([Local, URL, Filename, MimeType]() mutable
								{
									if (Local.OnDownloadRequested) Local.OnDownloadRequested(URL, Filename, MimeType);
								});
							}
						}
						return S_OK;
					}).Get(), &DownloadStartingToken);
		}

		WebView->add_PermissionRequested(
			Callback<ICoreWebView2PermissionRequestedEventHandler>(
				[this, WeakImpl = TWeakPtr<FImpl>(AsShared())](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* Args) -> HRESULT
				{
					const TSharedPtr<FImpl> Self = WeakImpl.Pin();
					if (!Self) return S_OK;
					LPWSTR RawURI = nullptr;
					COREWEBVIEW2_PERMISSION_KIND Kind = COREWEBVIEW2_PERMISSION_KIND_UNKNOWN_PERMISSION;
					if (Args)
					{
						Args->get_Uri(&RawURI);
						Args->get_PermissionKind(&Kind);
					}
					const FString URI = VeloWebViewWindowsPrivate::TakeOwnedString(RawURI);
					const uint8 PermissionKey = static_cast<uint8>(Kind);
					if (Args)
					{
						Microsoft::WRL::ComPtr<ICoreWebView2PermissionRequestedEventArgs> SavedArgs = Args;
						PendingPermissions.Add(PermissionKey, MoveTemp(SavedArgs));
					}
					// Default is intentionally left unchanged (deny/ask according to
					// Runtime policy); sensitive permissions are never auto-granted.
					if (Callbacks.OnPermissionRequested)
					{
						FVeloWebViewWindowsCallbacks Local = Callbacks;
						VeloWebViewWindowsPrivate::DispatchToGameThread([Local, Kind, URI]() mutable { if (Local.OnPermissionRequested) Local.OnPermissionRequested(static_cast<uint8>(Kind), URI); });
					}
					return S_OK;
				}).Get(), &PermissionRequestedToken);
	}

	void UnregisterEvents()
	{
		if (!WebView) return;
		if (NavigationStartingToken.value) WebView->remove_NavigationStarting(NavigationStartingToken);
		if (NewWindowRequestedToken.value) WebView->remove_NewWindowRequested(NewWindowRequestedToken);
		if (SourceChangedToken.value) WebView->remove_SourceChanged(SourceChangedToken);
		if (DocumentTitleChangedToken.value) WebView->remove_DocumentTitleChanged(DocumentTitleChangedToken);
		if (NavigationCompletedToken.value) WebView->remove_NavigationCompleted(NavigationCompletedToken);
		if (WebMessageReceivedToken.value) WebView->remove_WebMessageReceived(WebMessageReceivedToken);
		if (FullscreenToken.value) WebView->remove_ContainsFullScreenElementChanged(FullscreenToken);
		if (DownloadStartingToken.value)
		{
			Microsoft::WRL::ComPtr<ICoreWebView2_4> WebView4;
			if (SUCCEEDED(WebView.As(&WebView4)) && WebView4) WebView4->remove_DownloadStarting(DownloadStartingToken);
		}
		if (PermissionRequestedToken.value) WebView->remove_PermissionRequested(PermissionRequestedToken);
		NavigationStartingToken = {};
		NewWindowRequestedToken = {};
		SourceChangedToken = {};
		DocumentTitleChangedToken = {};
		NavigationCompletedToken = {};
		WebMessageReceivedToken = {};
		FullscreenToken = {};
		DownloadStartingToken = {};
		PermissionRequestedToken = {};
	}

	void ApplyBounds()
	{
		if (!Controller) return;
		RECT Rect;
		Rect.left = Bounds.Min.X;
		Rect.top = Bounds.Min.Y;
		Rect.right = Bounds.Max.X;
		Rect.bottom = Bounds.Max.Y;
		Controller->put_Bounds(Rect);
	}
#endif // PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2

	EVeloWebViewWindowsState State;
	void* HostWindow;
	FIntRect Bounds;
	bool bVisible;
	bool bJavaScriptEnabled;
	bool bMediaAutoplay;
	bool bMuted;
	bool bTransparent;
	bool bAllowMixedContent = false;
	EVeloWebViewNewWindowPolicy NewWindowPolicy = EVeloWebViewNewWindowPolicy::OpenInSameView;
	bool bComInitialized;
	FString UserAgent;
	FString UserDataFolder;
	FString LastURL;
	FString LastTitle;
	FString LastError;
	FString PendingURL;
	FString PendingHTML;
	FVeloWebViewWindowsCallbacks Callbacks;
	TMap<FString, FString> CookieCache;

#if PLATFORM_WINDOWS && VELO_WEBVIEW_HAS_WEBVIEW2
	HMODULE LoaderModule = nullptr;
	FCreateEnvironmentWithOptions CreateEnvironmentWithOptions = nullptr;
	FString LoaderError;
	int32 LoaderErrorCode = static_cast<int32>(HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND));
	Microsoft::WRL::ComPtr<ICoreWebView2Environment> Environment;
	Microsoft::WRL::ComPtr<ICoreWebView2Controller> Controller;
	Microsoft::WRL::ComPtr<ICoreWebView2> WebView;
	Microsoft::WRL::ComPtr<ICoreWebView2Settings> Settings;
	EventRegistrationToken NavigationStartingToken{};
	EventRegistrationToken NewWindowRequestedToken{};
	EventRegistrationToken SourceChangedToken{};
	EventRegistrationToken DocumentTitleChangedToken{};
	EventRegistrationToken NavigationCompletedToken{};
		EventRegistrationToken WebMessageReceivedToken{};
		EventRegistrationToken FullscreenToken{};
		EventRegistrationToken DownloadStartingToken{};
		EventRegistrationToken PermissionRequestedToken{};
	TMap<uint8, Microsoft::WRL::ComPtr<ICoreWebView2PermissionRequestedEventArgs>> PendingPermissions;
#endif
};

FVeloWebViewWindows::FVeloWebViewWindows()
	: Impl(MakeShared<FImpl>())
{
}

FVeloWebViewWindows::~FVeloWebViewWindows() = default;

bool FVeloWebViewWindows::Initialize(void* HostWindowHandle, const FVeloWebViewWindowsCallbacks& InCallbacks)
{
	return Impl->Initialize(HostWindowHandle, InCallbacks);
}

void FVeloWebViewWindows::Shutdown() { Impl->Shutdown(); }
EVeloWebViewWindowsState FVeloWebViewWindows::GetState() const { return Impl->GetState(); }
bool FVeloWebViewWindows::IsReady() const { return Impl->IsReady(); }
void FVeloWebViewWindows::SetHostWindow(void* HostWindowHandle) { Impl->SetHostWindow(HostWindowHandle); }
void FVeloWebViewWindows::SetBounds(const FIntRect& BoundsInHostClientPixels) { Impl->SetBounds(BoundsInHostClientPixels); }
void FVeloWebViewWindows::SetVisibility(EVisibility InVisibility) { Impl->SetVisibility(InVisibility); }
void FVeloWebViewWindows::LoadURL(const FString& URL) { Impl->LoadURL(URL); }
void FVeloWebViewWindows::LoadHTML(const FString& HTML) { Impl->LoadHTML(HTML); }
void FVeloWebViewWindows::Reload() { Impl->Reload(); }
void FVeloWebViewWindows::Stop() { Impl->Stop(); }
void FVeloWebViewWindows::GoBack() { Impl->GoBack(); }
void FVeloWebViewWindows::GoForward() { Impl->GoForward(); }
bool FVeloWebViewWindows::CanGoBack() const { return Impl->CanGoBack(); }
bool FVeloWebViewWindows::CanGoForward() const { return Impl->CanGoForward(); }
void FVeloWebViewWindows::ExecuteJavaScript(const FString& Script) { Impl->ExecuteJavaScript(Script); }
void FVeloWebViewWindows::SetJavaScriptEnabled(bool bEnabled) { Impl->SetJavaScriptEnabled(bEnabled); }
void FVeloWebViewWindows::SetMediaAutoplay(bool bEnabled) { Impl->SetMediaAutoplay(bEnabled); }
void FVeloWebViewWindows::SetMuted(bool bMuted) { Impl->SetMuted(bMuted); }
void FVeloWebViewWindows::SetUserAgent(const FString& UserAgent) { Impl->SetUserAgent(UserAgent); }
void FVeloWebViewWindows::SetBackgroundTransparent(bool bTransparent) { Impl->SetBackgroundTransparent(bTransparent); }
void FVeloWebViewWindows::SetAllowMixedContent(bool bAllow) { Impl->SetAllowMixedContent(bAllow); }
void FVeloWebViewWindows::SetNewWindowPolicy(EVeloWebViewNewWindowPolicy Policy) { Impl->SetNewWindowPolicy(Policy); }
void FVeloWebViewWindows::SetFocus(bool bFocused) { Impl->SetFocus(bFocused); }
void FVeloWebViewWindows::ClearCache() { Impl->ClearCache(); }
void FVeloWebViewWindows::ClearCookies() { Impl->ClearCookies(); }
void FVeloWebViewWindows::SetCookie(const FString& URL, const FString& Name, const FString& Value,
	const FString& Domain, const FString& Path, bool bSecure) { Impl->SetCookie(URL, Name, Value, Domain, Path, bSecure); }
FString FVeloWebViewWindows::GetCookie(const FString& URL) const { return Impl->GetCookie(URL); }
void FVeloWebViewWindows::DeleteCookie(const FString& URL, const FString& Name) { Impl->DeleteCookie(URL, Name); }
void FVeloWebViewWindows::GrantPermission(uint8 Permission) { Impl->GrantPermission(Permission); }
void FVeloWebViewWindows::DenyPermission(uint8 Permission) { Impl->DenyPermission(Permission); }
FString FVeloWebViewWindows::GetURL() const { return Impl->GetURL(); }
FString FVeloWebViewWindows::GetTitle() const { return Impl->GetTitle(); }
FString FVeloWebViewWindows::GetLastError() const { return Impl->GetLastError(); }
void FVeloWebViewWindows::SetPendingURL(const FString& URL) { Impl->SetPendingURL(URL); }
