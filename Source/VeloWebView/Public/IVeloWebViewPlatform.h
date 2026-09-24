#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Templates/Function.h"
#include "VeloWebViewTypes.h"

class SWindow;

/**
 * Callbacks emitted by a platform WebView implementation. Implementations may
 * receive events on a platform thread; the core widget is responsible for
 * marshaling them to the game thread before broadcasting Blueprint delegates.
 */
struct VELOWEBVIEW_API FVeloWebViewPlatformCallbacks
{
	TFunction<void(const FString&)> OnPageStarted;
	TFunction<void(const FString&)> OnPageLoaded;
	TFunction<void(const FVeloWebViewError&)> OnPageError;
	TFunction<void(const FString&)> OnURLChanged;
	TFunction<void(const FString&)> OnTitleChanged;
	TFunction<void(const FString&)> OnJavaScriptMessage;
	TFunction<void(const FString&, const FString&, int32)> OnConsoleMessage;
	TFunction<void(const FString&, const FString&, const FString&)> OnDownloadRequested;
	TFunction<void(EVeloWebViewPermission, const FString&)> OnPermissionRequested;
	TFunction<void(bool)> OnFullscreenChanged;
	TFunction<void(const FString&)> OnBeforeNavigate;
	TFunction<void(const FString&, bool)> OnNewWindowRequested;
};

/** Parameters captured when a native WebView instance is created. */
struct VELOWEBVIEW_API FVeloWebViewPlatformCreateParams
{
	FString InitialURL;
	FString UserAgent;
	bool bEnableJavaScript = true;
	bool bMediaAutoplay = true;
	bool bMuted = false;
	bool bTransparentBackground = false;
	bool bAllowMixedContent = false;
	EVeloWebViewNewWindowPolicy NewWindowPolicy = EVeloWebViewNewWindowPolicy::OpenInSameView;
	TWeakPtr<SWindow> HostWindow;
	void* NativeWindowHandle = nullptr;
};

/** Native implementation used by the UMG/Blueprint-facing core module. */
class VELOWEBVIEW_API IVeloWebViewPlatform
{
public:
	virtual ~IVeloWebViewPlatform() = default;

	virtual bool Initialize(const FVeloWebViewPlatformCreateParams& Params,
		const FVeloWebViewPlatformCallbacks& Callbacks) = 0;
	virtual void Shutdown() = 0;

	virtual void SetGeometry(const FIntRect& ScreenRect, float Scale) = 0;
	virtual void SetVisibility(bool bVisible) = 0;
	virtual void SetFocus(bool bFocused) = 0;

	virtual void LoadURL(const FString& URL) = 0;
	virtual void LoadHTML(const FString& HTML, const FString& BaseURL) = 0;
	virtual void Reload() = 0;
	virtual void Stop() = 0;
	virtual void GoBack() = 0;
	virtual void GoForward() = 0;
	virtual bool CanGoBack() const = 0;
	virtual bool CanGoForward() const = 0;
	virtual void ExecuteJavaScript(const FString& Script) = 0;

	virtual void SetJavaScriptEnabled(bool bEnabled) = 0;
	virtual void SetMediaAutoplay(bool bEnabled) = 0;
	virtual void SetMuted(bool bMuted) = 0;
	virtual void SetUserAgent(const FString& UserAgent) = 0;
	virtual void SetBackgroundTransparent(bool bTransparent) = 0;
	virtual void SetAllowMixedContent(bool bAllow) = 0;
	virtual void SetNewWindowPolicy(EVeloWebViewNewWindowPolicy Policy) = 0;

	virtual FString GetURL() const = 0;
	virtual FString GetTitle() const = 0;
	virtual EVeloWebViewState GetState() const = 0;

	virtual void ClearCache() = 0;
	virtual void ClearCookies() = 0;
	virtual void SetCookie(const FString& URL, const FString& Name, const FString& Value,
		const FString& Domain, const FString& Path, bool bSecure) = 0;
	virtual FString GetCookie(const FString& URL) const = 0;
	virtual void DeleteCookie(const FString& URL, const FString& Name) = 0;

	virtual void GrantPermission(EVeloWebViewPermission Permission) = 0;
	virtual void DenyPermission(EVeloWebViewPermission Permission) = 0;

	virtual bool IsReady() const = 0;
	virtual FString GetLastError() const = 0;
};

/** Optional module-side factory. Platform modules implement this interface. */
class VELOWEBVIEW_API IVeloWebViewPlatformModule : public IModuleInterface
{
public:
	virtual TSharedPtr<IVeloWebViewPlatform> CreatePlatform(
		const FVeloWebViewPlatformCreateParams& Params,
		const FVeloWebViewPlatformCallbacks& Callbacks) = 0;
};
