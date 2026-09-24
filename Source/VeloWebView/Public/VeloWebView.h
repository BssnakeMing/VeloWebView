#pragma once

#include "CoreMinimal.h"
#include "Components/Widget.h"
#include "VeloWebViewTypes.h"
#include "VeloWebView.generated.h"

class SVeloWebView;
class IVeloWebViewPlatform;

DECLARE_LOG_CATEGORY_EXTERN(LogVeloWebView, Log, All);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVeloWebViewPageStarted, const FString&, URL);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVeloWebViewPageLoaded, const FString&, URL);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVeloWebViewPageError, const FVeloWebViewError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVeloWebViewURLChanged, const FString&, URL);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVeloWebViewTitleChanged, const FString&, Title);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVeloWebViewJavaScriptMessage, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FVeloWebViewConsoleMessage, const FString&, Message, const FString&, Source, int32, Line);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FVeloWebViewDownloadRequested, const FString&, URL, const FString&, SuggestedFilename, const FString&, MimeType);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FVeloWebViewPermissionRequested, EVeloWebViewPermission, Permission, const FString&, Origin);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVeloWebViewFullscreenChanged, bool, bIsFullscreen);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVeloWebViewBeforeNavigate, const FString&, URL);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FVeloWebViewNewWindowRequested, const FString&, URL, bool, bIsUserInitiated);

/**
 * High-performance native WebView hosted by UMG. The widget deliberately keeps
 * platform details out of Blueprint; Windows uses WebView2 composition and
 * Android uses a native WebView overlay.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Velo Web View"))
class VELOWEBVIEW_API UVeloWebView : public UWidget
{
	GENERATED_BODY()

public:
	UVeloWebView(const FObjectInitializer& ObjectInitializer);
	virtual ~UVeloWebView() override;

	/** URL navigated to immediately after native initialization. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VeloWebView")
	FString InitialURL;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VeloWebView")
	bool bEnableJavaScript = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VeloWebView")
	bool bMediaAutoplay = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VeloWebView")
	bool bMuted = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VeloWebView")
	bool bTransparentBackground = false;

	/** Keep false unless a trusted page requires HTTP subresources on HTTPS. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VeloWebView|Security")
	bool bAllowMixedContent = false;

	/** Requests for target="_blank" and window.open(). Defaults to this widget. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VeloWebView|Navigation")
	EVeloWebViewNewWindowPolicy NewWindowPolicy = EVeloWebViewNewWindowPolicy::OpenInSameView;

	/** Empty restores the platform default user agent. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VeloWebView")
	FString UserAgent;

	UPROPERTY(BlueprintAssignable, Category = "VeloWebView|Events")
	FVeloWebViewPageStarted OnPageStarted;
	UPROPERTY(BlueprintAssignable, Category = "VeloWebView|Events")
	FVeloWebViewPageLoaded OnPageLoaded;
	UPROPERTY(BlueprintAssignable, Category = "VeloWebView|Events")
	FVeloWebViewPageError OnPageError;
	UPROPERTY(BlueprintAssignable, Category = "VeloWebView|Events")
	FVeloWebViewURLChanged OnURLChanged;
	UPROPERTY(BlueprintAssignable, Category = "VeloWebView|Events")
	FVeloWebViewTitleChanged OnTitleChanged;
	UPROPERTY(BlueprintAssignable, Category = "VeloWebView|Events")
	FVeloWebViewJavaScriptMessage OnJavaScriptMessage;
	UPROPERTY(BlueprintAssignable, Category = "VeloWebView|Events")
	FVeloWebViewConsoleMessage OnConsoleMessage;
	UPROPERTY(BlueprintAssignable, Category = "VeloWebView|Events")
	FVeloWebViewDownloadRequested OnDownloadRequested;
	UPROPERTY(BlueprintAssignable, Category = "VeloWebView|Events")
	FVeloWebViewPermissionRequested OnPermissionRequested;
	UPROPERTY(BlueprintAssignable, Category = "VeloWebView|Events")
	FVeloWebViewFullscreenChanged OnFullscreenChanged;
	UPROPERTY(BlueprintAssignable, Category = "VeloWebView|Events")
	FVeloWebViewBeforeNavigate OnBeforeNavigate;
	/** Reports a new-window request. URL can be about:blank, or empty for a blocked Android popup. */
	UPROPERTY(BlueprintAssignable, Category = "VeloWebView|Events")
	FVeloWebViewNewWindowRequested OnNewWindowRequested;

	UFUNCTION(BlueprintCallable, Category = "VeloWebView")
	void LoadURL(const FString& URL);

	UFUNCTION(BlueprintCallable, Category = "VeloWebView")
	void LoadHTML(const FString& HTML, const FString& BaseURL);

	UFUNCTION(BlueprintCallable, Category = "VeloWebView")
	void Reload();
	
	UFUNCTION(BlueprintCallable, Category = "VeloWebView")
	void Stop();

	UFUNCTION(BlueprintCallable, Category = "VeloWebView")
	void GoBack();

	UFUNCTION(BlueprintCallable, Category = "VeloWebView")
	void GoForward();

	UFUNCTION(BlueprintPure, Category = "VeloWebView")
	bool CanGoBack() const;

	UFUNCTION(BlueprintPure, Category = "VeloWebView")
	bool CanGoForward() const;

	UFUNCTION(BlueprintCallable, Category = "VeloWebView|JavaScript")
	void ExecuteJavaScript(const FString& Script);

	UFUNCTION(BlueprintCallable, Category = "VeloWebView|JavaScript")
	void SetJavaScriptEnabled(bool bEnabled);

	UFUNCTION(BlueprintCallable, Category = "VeloWebView|Media")
	void SetMediaAutoplay(bool bEnabled);

	UFUNCTION(BlueprintCallable, Category = "VeloWebView|Media")
	void SetMuted(bool bInMuted);

	UFUNCTION(BlueprintCallable, Category = "VeloWebView")
	void SetUserAgent(const FString& InUserAgent);

	UFUNCTION(BlueprintCallable, Category = "VeloWebView")
	void SetBackgroundTransparent(bool bTransparent);

	UFUNCTION(BlueprintCallable, Category = "VeloWebView|Security")
	void SetAllowMixedContent(bool bAllow);

	UFUNCTION(BlueprintCallable, Category = "VeloWebView|Navigation")
	void SetNewWindowPolicy(EVeloWebViewNewWindowPolicy Policy);

	/** Blueprint-friendly name avoids hiding UWidget::SetVisibility(ESlateVisibility). */
	UFUNCTION(BlueprintCallable, Category = "VeloWebView", meta = (DisplayName = "Set Visibility"))
	void SetWebViewVisibility(bool bVisible);

	UFUNCTION(BlueprintPure, Category = "VeloWebView")
	FString GetURL() const;

	UFUNCTION(BlueprintPure, Category = "VeloWebView")
	FString GetTitle() const;

	UFUNCTION(BlueprintPure, Category = "VeloWebView")
	EVeloWebViewState GetState() const;

	UFUNCTION(BlueprintPure, Category = "VeloWebView")
	bool IsReady() const;

	UFUNCTION(BlueprintCallable, Category = "VeloWebView|Storage")
	void ClearCache();

	UFUNCTION(BlueprintCallable, Category = "VeloWebView|Storage")
	void ClearCookies();

	UFUNCTION(BlueprintCallable, Category = "VeloWebView|Storage")
	void SetCookie(const FString& URL, const FString& Name, const FString& Value,
		const FString& Domain, const FString& Path, bool bSecure);

	UFUNCTION(BlueprintPure, Category = "VeloWebView|Storage")
	FString GetCookie(const FString& URL) const;

	UFUNCTION(BlueprintCallable, Category = "VeloWebView|Storage")
	void DeleteCookie(const FString& URL, const FString& Name);

	UFUNCTION(BlueprintCallable, Category = "VeloWebView|Permissions")
	void GrantPermission(EVeloWebViewPermission Permission);

	UFUNCTION(BlueprintCallable, Category = "VeloWebView|Permissions")
	void DenyPermission(EVeloWebViewPermission Permission);

	/** Returns a platform/runtime error captured during initialization. */
	UFUNCTION(BlueprintPure, Category = "VeloWebView")
	FString GetLastError() const;

	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;
	virtual void SynchronizeProperties() override;
	virtual void BeginDestroy() override;

private:
	friend class SVeloWebView;

	void HandlePageStarted(const FString& URL);
	void HandlePageLoaded(const FString& URL);
	void HandlePageError(const FVeloWebViewError& Error);
	void HandleURLChanged(const FString& URL);
	void HandleTitleChanged(const FString& Title);
	void HandleJavaScriptMessage(const FString& Message);
	void HandleConsoleMessage(const FString& Message, const FString& Source, int32 Line);
	void HandleDownloadRequested(const FString& URL, const FString& SuggestedFilename, const FString& MimeType);
	void HandlePermissionRequested(EVeloWebViewPermission Permission, const FString& Origin);
	void HandleFullscreenChanged(bool bIsFullscreen);
	void HandleBeforeNavigate(const FString& URL);
	void HandleNewWindowRequested(const FString& URL, bool bIsUserInitiated);

	void EnsurePlatform();
	void QueueOrExecute(TFunction<void(IVeloWebViewPlatform&)> Action);

	TSharedPtr<SVeloWebView> MyWebViewWidget;
	TSharedPtr<IVeloWebViewPlatform> Platform;
	FString PendingURL;
	FString PendingHTML;
	FString PendingBaseURL;
	bool bPendingHTML = false;
	bool bRequestedVisibility = true;
};
