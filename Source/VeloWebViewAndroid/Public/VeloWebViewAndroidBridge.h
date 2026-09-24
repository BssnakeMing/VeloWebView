#pragma once

#include "CoreMinimal.h"

/**
 * Callbacks emitted by the Android WebView bridge.  Callbacks are always
 * delivered on the Unreal game thread by the native implementation.  The
 * bridge deliberately does not depend on the core VeloWebView UObject module;
 * this keeps the platform module loadable independently and avoids a module
 * dependency cycle.
 */
struct VELOWEBVIEWANDROID_API FVeloWebViewAndroidCallbacks
{
	TFunction<void(const FString&)> OnPageStarted;
	TFunction<void(const FString&)> OnPageLoaded;
	TFunction<void(int32, const FString&, const FString&)> OnPageError;
	TFunction<void(const FString&)> OnURLChanged;
	TFunction<void(const FString&)> OnTitleChanged;
	TFunction<void(const FString&)> OnJavaScriptMessage;
	TFunction<void(const FString&, const FString&, int32)> OnConsoleMessage;
	TFunction<void(const FString&, const FString&, const FString&)> OnDownloadRequested;
	/** Token, resource names, origin.  The token is passed to Grant/DenyPermission. */
	TFunction<void(int32, const FString&, const FString&)> OnPermissionRequested;
	TFunction<void(bool)> OnFullscreenChanged;
	TFunction<void(const FString&)> OnBeforeNavigate;
	TFunction<void(const FString&, bool)> OnNewWindowRequested;
};

/**
 * Thin C++/JNI facade for com.velowebview.VeloWebViewManager.
 *
 * All methods are safe to call when Android is not the active target; in that
 * case they return a neutral value and never reference JNI types.  Android
 * methods enqueue work on the Java UI thread.  Native callbacks are marshalled
 * back to the Unreal game thread before invoking the TFunctions above.
 */
class VELOWEBVIEWANDROID_API FVeloWebViewAndroidBridge
{
public:
	static bool IsAvailable();

	static int32 CreateInstance(
		int32 RequestedId,
		bool bEnableJavaScript,
		bool bMediaAutoplay,
		bool bMuted,
		bool bTransparentBackground,
		bool bAllowMixedContent,
		const FString& UserAgent = FString());

	static void DestroyInstance(int32 InstanceId);
	static void SetCallbacks(int32 InstanceId, const FVeloWebViewAndroidCallbacks& Callbacks);
	static void ClearCallbacks(int32 InstanceId);

	static void SetBounds(int32 InstanceId, float X, float Y, float Width, float Height, bool bCoordinatesAreDp = false);
	static void SetVisibility(int32 InstanceId, bool bVisible);
	static void SetFocus(int32 InstanceId, bool bFocused);

	static void LoadURL(int32 InstanceId, const FString& URL);
	static void LoadHTML(int32 InstanceId, const FString& HTML, const FString& BaseURL);
	static void Reload(int32 InstanceId);
	static void Stop(int32 InstanceId);
	static void GoBack(int32 InstanceId);
	static void GoForward(int32 InstanceId);
	static bool CanGoBack(int32 InstanceId);
	static bool CanGoForward(int32 InstanceId);
	static void ExecuteJavaScript(int32 InstanceId, const FString& Script);

	static void SetJavaScriptEnabled(int32 InstanceId, bool bEnabled);
	static void SetMediaAutoplay(int32 InstanceId, bool bEnabled);
	static void SetMuted(int32 InstanceId, bool bMuted);
	static void SetUserAgent(int32 InstanceId, const FString& UserAgent);
	static void SetBackgroundTransparent(int32 InstanceId, bool bTransparent);
	static void SetAllowMixedContent(int32 InstanceId, bool bAllowed);
	static void SetNewWindowPolicy(int32 InstanceId, uint8 Policy);

	static FString GetURL(int32 InstanceId);
	static FString GetTitle(int32 InstanceId);

	static void SetCookie(const FString& URL, const FString& Cookie);
	static FString GetCookie(const FString& URL);
	static void DeleteCookie(const FString& URL, const FString& Name);
	static void ClearCookies();
	static void ClearCache(int32 InstanceId);

	static void GrantPermission(int32 PermissionToken);
	static void DenyPermission(int32 PermissionToken);

	/** Optional lifecycle hooks for GameActivity UPL additions. */
	static void OnActivityPause();
	static void OnActivityResume();
	static void OnActivityDestroyed();
	static bool HandleBackPressed();

	/** Android density used by the overlay.  Returns 1 on non-Android. */
	static float GetDisplayDensity();
};
