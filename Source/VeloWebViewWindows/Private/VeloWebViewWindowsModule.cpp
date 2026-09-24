#include "IVeloWebViewPlatform.h"
#include "Modules/ModuleManager.h"
#include "VeloWebViewWindows.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace VeloWebViewWindowsPrivate
{
	static EVeloWebViewPermission ToPermission(uint8 NativePermission)
	{
		switch (NativePermission)
		{
		case 1: return EVeloWebViewPermission::Microphone;
		case 2: return EVeloWebViewPermission::Camera;
		case 3: return EVeloWebViewPermission::Geolocation;
		case 4: return EVeloWebViewPermission::Notifications;
		case 5: return EVeloWebViewPermission::Clipboard;
		default: return EVeloWebViewPermission::Unknown;
		}
	}

	static uint8 ToNativePermission(EVeloWebViewPermission Permission)
	{
		switch (Permission)
		{
		case EVeloWebViewPermission::Microphone: return 1;
		case EVeloWebViewPermission::Camera: return 2;
		case EVeloWebViewPermission::Geolocation: return 3;
		case EVeloWebViewPermission::Notifications: return 4;
		case EVeloWebViewPermission::Clipboard: return 5;
		default: return 0;
		}
	}

	static EVeloWebViewState ToState(EVeloWebViewWindowsState State)
	{
		switch (State)
		{
		case EVeloWebViewWindowsState::Initializing: return EVeloWebViewState::Initializing;
		case EVeloWebViewWindowsState::Ready: return EVeloWebViewState::Ready;
		case EVeloWebViewWindowsState::Loading: return EVeloWebViewState::Loading;
		case EVeloWebViewWindowsState::Failed: return EVeloWebViewState::Failed;
		case EVeloWebViewWindowsState::Destroyed: return EVeloWebViewState::Destroyed;
		default: return EVeloWebViewState::Uninitialized;
		}
	}
}

class FVeloWebViewWindowsPlatform final : public IVeloWebViewPlatform
{
public:
	FVeloWebViewWindowsPlatform()
		: Native(MakeUnique<FVeloWebViewWindows>())
	{
	}

	virtual bool Initialize(const FVeloWebViewPlatformCreateParams& Params,
		const FVeloWebViewPlatformCallbacks& Callbacks) override
	{
		HostWindow = Params.NativeWindowHandle;
		FVeloWebViewWindowsCallbacks NativeCallbacks;
		NativeCallbacks.OnPageStarted = Callbacks.OnPageStarted;
		NativeCallbacks.OnPageLoaded = Callbacks.OnPageLoaded;
		NativeCallbacks.OnPageError = [Callback = Callbacks.OnPageError](const FVeloWebViewWindowsError& Error)
		{
			if (Callback) Callback(FVeloWebViewError(Error.ErrorCode, Error.Description, Error.URL));
		};
		NativeCallbacks.OnURLChanged = Callbacks.OnURLChanged;
		NativeCallbacks.OnTitleChanged = Callbacks.OnTitleChanged;
		NativeCallbacks.OnJavaScriptMessage = Callbacks.OnJavaScriptMessage;
		NativeCallbacks.OnConsoleMessage = Callbacks.OnConsoleMessage;
		NativeCallbacks.OnDownloadRequested = Callbacks.OnDownloadRequested;
		NativeCallbacks.OnPermissionRequested = [Callback = Callbacks.OnPermissionRequested](uint8 Permission, const FString& Origin)
		{
			if (Callback) Callback(VeloWebViewWindowsPrivate::ToPermission(Permission), Origin);
		};
		NativeCallbacks.OnFullscreenChanged = Callbacks.OnFullscreenChanged;
		NativeCallbacks.OnNavigationRequested = Callbacks.OnBeforeNavigate;
		NativeCallbacks.OnNewWindowRequested = Callbacks.OnNewWindowRequested;
		Native->SetNewWindowPolicy(Params.NewWindowPolicy);

		if (!Params.InitialURL.IsEmpty()) Native->SetPendingURL(Params.InitialURL);
		const bool bInitialized = Native->Initialize(HostWindow, NativeCallbacks);
		Native->SetJavaScriptEnabled(Params.bEnableJavaScript);
		Native->SetMediaAutoplay(Params.bMediaAutoplay);
		Native->SetMuted(Params.bMuted);
		Native->SetUserAgent(Params.UserAgent);
		Native->SetBackgroundTransparent(Params.bTransparentBackground);
		Native->SetAllowMixedContent(Params.bAllowMixedContent);
		return bInitialized;
	}

	virtual void Shutdown() override { if (Native) Native->Shutdown(); }

	virtual void SetGeometry(const FIntRect& ScreenRect, float Scale) override
	{
		FIntRect LocalRect = ScreenRect;
#if PLATFORM_WINDOWS
		if (HostWindow)
		{
			POINT MinPoint{ScreenRect.Min.X, ScreenRect.Min.Y};
			POINT MaxPoint{ScreenRect.Max.X, ScreenRect.Max.Y};
			::ScreenToClient(reinterpret_cast<HWND>(HostWindow), &MinPoint);
			::ScreenToClient(reinterpret_cast<HWND>(HostWindow), &MaxPoint);
			LocalRect = FIntRect(MinPoint.x, MinPoint.y, MaxPoint.x, MaxPoint.y);
		}
#endif
		(void)Scale;
		Native->SetBounds(LocalRect);
	}

	virtual void SetVisibility(bool bVisible) override
	{
		Native->SetVisibility(bVisible ? EVisibility::Visible : EVisibility::Collapsed);
	}
	virtual void SetFocus(bool bFocused) override { Native->SetFocus(bFocused); }
	virtual void LoadURL(const FString& URL) override { Native->LoadURL(URL); }
	virtual void LoadHTML(const FString& HTML, const FString& BaseURL) override
	{
		if (BaseURL.IsEmpty() || BaseURL == TEXT("about:blank")) Native->LoadHTML(HTML);
		else Native->LoadHTML(FString::Printf(TEXT("<base href=\"%s\">%s"), *BaseURL, *HTML));
	}
	virtual void Reload() override { Native->Reload(); }
	virtual void Stop() override { Native->Stop(); }
	virtual void GoBack() override { Native->GoBack(); }
	virtual void GoForward() override { Native->GoForward(); }
	virtual bool CanGoBack() const override { return Native->CanGoBack(); }
	virtual bool CanGoForward() const override { return Native->CanGoForward(); }
	virtual void ExecuteJavaScript(const FString& Script) override { Native->ExecuteJavaScript(Script); }
	virtual void SetJavaScriptEnabled(bool bEnabled) override { Native->SetJavaScriptEnabled(bEnabled); }
	virtual void SetMediaAutoplay(bool bEnabled) override { Native->SetMediaAutoplay(bEnabled); }
	virtual void SetMuted(bool bMuted) override { Native->SetMuted(bMuted); }
	virtual void SetUserAgent(const FString& UserAgent) override { Native->SetUserAgent(UserAgent); }
	virtual void SetBackgroundTransparent(bool bTransparent) override { Native->SetBackgroundTransparent(bTransparent); }
	virtual void SetAllowMixedContent(bool bAllow) override { Native->SetAllowMixedContent(bAllow); }
	virtual void SetNewWindowPolicy(EVeloWebViewNewWindowPolicy Policy) override { Native->SetNewWindowPolicy(Policy); }
	virtual FString GetURL() const override { return Native->GetURL(); }
	virtual FString GetTitle() const override { return Native->GetTitle(); }
	virtual EVeloWebViewState GetState() const override { return VeloWebViewWindowsPrivate::ToState(Native->GetState()); }
	virtual void ClearCache() override { Native->ClearCache(); }
	virtual void ClearCookies() override { Native->ClearCookies(); }
	virtual void SetCookie(const FString& URL, const FString& Name, const FString& Value,
		const FString& Domain, const FString& Path, bool bSecure) override { Native->SetCookie(URL, Name, Value, Domain, Path, bSecure); }
	virtual FString GetCookie(const FString& URL) const override { return Native->GetCookie(URL); }
	virtual void DeleteCookie(const FString& URL, const FString& Name) override { Native->DeleteCookie(URL, Name); }
	virtual void GrantPermission(EVeloWebViewPermission Permission) override { Native->GrantPermission(VeloWebViewWindowsPrivate::ToNativePermission(Permission)); }
	virtual void DenyPermission(EVeloWebViewPermission Permission) override { Native->DenyPermission(VeloWebViewWindowsPrivate::ToNativePermission(Permission)); }
	virtual bool IsReady() const override { return Native->IsReady(); }
	virtual FString GetLastError() const override { return Native->GetLastError(); }

private:
	TUniquePtr<FVeloWebViewWindows> Native;
	void* HostWindow = nullptr;
};

class FVeloWebViewWindowsModule final : public IVeloWebViewPlatformModule
{
public:
	virtual void StartupModule() override {}
	virtual void ShutdownModule() override {}
	virtual TSharedPtr<IVeloWebViewPlatform> CreatePlatform(
		const FVeloWebViewPlatformCreateParams& Params,
		const FVeloWebViewPlatformCallbacks& Callbacks) override
	{
		(void)Params;
		(void)Callbacks;
		return MakeShared<FVeloWebViewWindowsPlatform>();
	}
};

IMPLEMENT_MODULE(FVeloWebViewWindowsModule, VeloWebViewWindows)
