#include "VeloWebView.h"

#include "Async/Async.h"
#include "Engine/Texture2D.h"
#include "IVeloWebViewPlatform.h"
#include "Modules/ModuleManager.h"
#include "SVeloWebView.h"
#if PLATFORM_ANDROID
#include "SWebBrowserView.h"
#include "IWebBrowserCookieManager.h"
#include "IWebBrowserSingleton.h"
#include "WebBrowserModule.h"
#endif

#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RHITypes.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsWindow.h"
#endif

DEFINE_LOG_CATEGORY(LogVeloWebView);

namespace VeloWebViewPrivate
{
	FString EscapeJavaScriptString(const FString& Value)
	{
		FString Escaped = Value;
		Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
		Escaped.ReplaceInline(TEXT("\r"), TEXT("\\r"));
		Escaped.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		Escaped.ReplaceInline(TEXT("\t"), TEXT("\\t"));
		// Keep a URL substituted into Host.html from closing its script element.
		Escaped.ReplaceInline(TEXT("<"), TEXT("\\u003C"));
		Escaped.ReplaceInline(TEXT(">"), TEXT("\\u003E"));
		return Escaped;
	}

	bool IsExternalURL(const FString& URL)
	{
		return URL.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase)
			|| URL.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase)
			|| URL.StartsWith(TEXT("mailto:"), ESearchCase::IgnoreCase)
			|| URL.StartsWith(TEXT("tel:"), ESearchCase::IgnoreCase);
	}

	template <typename CallbackType>
	void DispatchToGameThread(const TWeakObjectPtr<UVeloWebView>& WeakWidget, CallbackType&& Callback)
	{
		AsyncTask(ENamedThreads::GameThread,
			[WeakWidget, Callback = Forward<CallbackType>(Callback)]() mutable
			{
				if (UVeloWebView* Widget = WeakWidget.Get())
			{
					Callback(*Widget);
				}
			});
	}

	TSharedPtr<IVeloWebViewPlatform> CreatePlatform(
		const FVeloWebViewPlatformCreateParams& Params,
		const FVeloWebViewPlatformCallbacks& Callbacks)
	{
		FName ModuleName;
#if PLATFORM_WINDOWS
		ModuleName = TEXT("VeloWebViewWindows");
#elif PLATFORM_ANDROID
		ModuleName = TEXT("VeloWebViewAndroid");
#endif

		if (ModuleName.IsNone())
		{
			return nullptr;
		}

		IVeloWebViewPlatformModule* Module = FModuleManager::Get().LoadModulePtr<IVeloWebViewPlatformModule>(ModuleName);
		return Module ? Module->CreatePlatform(Params, Callbacks) : nullptr;
	}
}

UVeloWebView::UVeloWebView(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetVisibility(ESlateVisibility::Visible);
}

UVeloWebView::~UVeloWebView() = default;

void UVeloWebView::EnsurePlatform()
{
	if (RenderMode == EVeloWebViewRenderMode::Composited)
	{
		return;
	}
	if (Platform.IsValid() || IsDesignTime() || !IsInGameThread())
	{
		return;
	}

	const TWeakObjectPtr<UVeloWebView> WeakThis(this);
	FVeloWebViewPlatformCallbacks Callbacks;
	Callbacks.OnPageStarted = [WeakThis](const FString& URL)
	{
		VeloWebViewPrivate::DispatchToGameThread(WeakThis, [URL](UVeloWebView& Widget) { Widget.HandlePageStarted(URL); });
	};
	Callbacks.OnPageLoaded = [WeakThis](const FString& URL)
	{
		VeloWebViewPrivate::DispatchToGameThread(WeakThis, [URL](UVeloWebView& Widget) { Widget.HandlePageLoaded(URL); });
	};
	Callbacks.OnPageError = [WeakThis](const FVeloWebViewError& Error)
	{
		VeloWebViewPrivate::DispatchToGameThread(WeakThis, [Error](UVeloWebView& Widget) { Widget.HandlePageError(Error); });
	};
	Callbacks.OnURLChanged = [WeakThis](const FString& URL)
	{
		VeloWebViewPrivate::DispatchToGameThread(WeakThis, [URL](UVeloWebView& Widget) { Widget.HandleURLChanged(URL); });
	};
	Callbacks.OnTitleChanged = [WeakThis](const FString& Title)
	{
		VeloWebViewPrivate::DispatchToGameThread(WeakThis, [Title](UVeloWebView& Widget) { Widget.HandleTitleChanged(Title); });
	};
	Callbacks.OnJavaScriptMessage = [WeakThis](const FString& Message)
	{
		VeloWebViewPrivate::DispatchToGameThread(WeakThis, [Message](UVeloWebView& Widget) { Widget.HandleJavaScriptMessage(Message); });
	};
	Callbacks.OnConsoleMessage = [WeakThis](const FString& Message, const FString& Source, int32 Line)
	{
		VeloWebViewPrivate::DispatchToGameThread(WeakThis, [Message, Source, Line](UVeloWebView& Widget) { Widget.HandleConsoleMessage(Message, Source, Line); });
	};
	Callbacks.OnDownloadRequested = [WeakThis](const FString& URL, const FString& Filename, const FString& MimeType)
	{
		VeloWebViewPrivate::DispatchToGameThread(WeakThis, [URL, Filename, MimeType](UVeloWebView& Widget) { Widget.HandleDownloadRequested(URL, Filename, MimeType); });
	};
	Callbacks.OnPermissionRequested = [WeakThis](EVeloWebViewPermission Permission, const FString& Origin)
	{
		VeloWebViewPrivate::DispatchToGameThread(WeakThis, [Permission, Origin](UVeloWebView& Widget) { Widget.HandlePermissionRequested(Permission, Origin); });
	};
	Callbacks.OnFullscreenChanged = [WeakThis](bool bFullscreen)
	{
		VeloWebViewPrivate::DispatchToGameThread(WeakThis, [bFullscreen](UVeloWebView& Widget) { Widget.HandleFullscreenChanged(bFullscreen); });
	};
	Callbacks.OnBeforeNavigate = [WeakThis](const FString& URL)
	{
		VeloWebViewPrivate::DispatchToGameThread(WeakThis, [URL](UVeloWebView& Widget) { Widget.HandleBeforeNavigate(URL); });
	};
	Callbacks.OnNewWindowRequested = [WeakThis](const FString& URL, bool bIsUserInitiated)
	{
		VeloWebViewPrivate::DispatchToGameThread(WeakThis, [URL, bIsUserInitiated](UVeloWebView& Widget)
		{
			Widget.HandleNewWindowRequested(URL, bIsUserInitiated);
		});
	};

	FVeloWebViewPlatformCreateParams Params;
	Params.InitialURL = InitialURL;
	Params.UserAgent = UserAgent;
	Params.bEnableJavaScript = bEnableJavaScript;
	Params.bMediaAutoplay = bMediaAutoplay;
	Params.bMuted = bMuted;
	Params.bTransparentBackground = bTransparentBackground;
	Params.bAllowMixedContent = bAllowMixedContent;
	Params.NewWindowPolicy = NewWindowPolicy;
	if (MyWebViewWidget.IsValid())
	{
		TSharedPtr<SWindow> HostSlateWindow = FSlateApplication::Get().FindWidgetWindow(MyWebViewWidget.ToSharedRef());
		Params.HostWindow = HostSlateWindow;
#if PLATFORM_WINDOWS
		if (HostSlateWindow.IsValid())
		{
			TSharedPtr<FGenericWindow> GenericWindow = HostSlateWindow->GetNativeWindow();
			if (GenericWindow.IsValid())
			{
				const TSharedPtr<FWindowsWindow> WindowsWindow = StaticCastSharedPtr<FWindowsWindow>(GenericWindow);
				if (WindowsWindow.IsValid())
				{
					Params.NativeWindowHandle = reinterpret_cast<void*>(WindowsWindow->GetHWnd());
				}
			}
		}
#endif
	}

#if PLATFORM_WINDOWS
	// Defer creation until Slate exposes a real host HWND during PIE startup.
	if (Params.NativeWindowHandle == nullptr)
	{
		return;
	}
#endif
	Platform = VeloWebViewPrivate::CreatePlatform(Params, Callbacks);
	if (!Platform.IsValid())
	{
		UE_LOG(LogVeloWebView, Warning, TEXT("No native VeloWebView platform is available for this target."));
		HandlePageError(FVeloWebViewError(1, TEXT("No native VeloWebView platform is available for this target."), InitialURL));
		return;
	}

	if (!Platform->Initialize(Params, Callbacks))
	{
		const FString Error = Platform->GetLastError().IsEmpty()
			? TEXT("VeloWebView platform initialization failed.")
			: Platform->GetLastError();
		UE_LOG(LogVeloWebView, Error, TEXT("%s"), *Error);
		HandlePageError(FVeloWebViewError(2, Error, InitialURL));
		return;
	}

	if (bPendingHTML)
	{
		Platform->LoadHTML(PendingHTML, PendingBaseURL);
		bPendingHTML = false;
		PendingHTML.Reset();
		PendingBaseURL.Reset();
	}
	else if (!PendingURL.IsEmpty())
	{
		Platform->LoadURL(PendingURL);
		PendingURL.Reset();
	}
}

void UVeloWebView::QueueOrExecute(TFunction<void(IVeloWebViewPlatform&)> Action)
{
	if (Platform.IsValid())
	{
		Action(*Platform);
	}
}

#if PLATFORM_WINDOWS
void UVeloWebView::EnsureCompositedCEF(FIntPoint InitialSize)
{
	if (RenderMode != EVeloWebViewRenderMode::Composited || IsDesignTime() || !IsInGameThread())
	{
		return;
	}
	if (!CEFConnection.IsValid())
	{
		CEFConnection = MakeShared<FVeloCEFConnection>();
		FVeloCEFConnectionConfig Config;
		Config.InitialSize = InitialSize.X > 0 && InitialSize.Y > 0 ? InitialSize : FIntPoint(1920, 1080);
		Config.InitialURL = InitialURL;
		Config.bAllowMediaCapture = bAllowMediaCapture;
		Config.bEnablePixelStreaming = bEnablePixelStreaming;
		if (!CEFConnection->Start(Config))
		{
			UE_LOG(LogVeloWebView, Warning, TEXT("CEF BrowserHost unavailable: %s"), *CEFConnection->GetLastError());
			HandlePageError(FVeloWebViewError(-10, CEFConnection->GetLastError(), InitialURL));
		}
	}
}

void UVeloWebView::UpdateCompositedCEFFrame(const TArray<uint8>& Pixels, const FIntPoint& Size, uint64 Sequence)
{
	if (!IsInGameThread() || Pixels.Num() <= 0 || Size.X <= 0 || Size.Y <= 0 || Sequence <= CompositedTextureSequence)
	{
		return;
	}
	if (!CompositedTexture || CompositedTextureSize != Size)
	{
		// CreateTransient initializes the platform mip/resource correctly for
		// Slate. A manually assembled FTexturePlatformData can remain without a
		// valid RHI resource on some UE 5.4 renderers and produces a white brush.
		CompositedTexture = UTexture2D::CreateTransient(Size.X, Size.Y, PF_B8G8R8A8);
		CompositedTexture->NeverStream = true;
		CompositedTexture->SRGB = true;
		CompositedTexture->Filter = TF_Bilinear;
		CompositedTexture->CompressionSettings = TC_EditorIcon;
		CompositedTexture->UpdateResource();
		CompositedTextureSize = Size;
	}
	if (!CompositedTexture->GetResource()) return;

	TSharedRef<TArray<uint8>, ESPMode::ThreadSafe> FrameCopy = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>(Pixels);
	FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, Size.X, Size.Y);
	CompositedTexture->UpdateTextureRegions(
		0, 1, Region, static_cast<uint32>(Size.X) * 4, 4, FrameCopy->GetData(),
		[FrameCopy](uint8*, const FUpdateTextureRegion2D* InRegion)
		{
			delete InRegion;
		});
	CompositedTextureSequence = Sequence;
	if (CEFConnection.IsValid())
	{
		CEFConnection->MarkCpuFrameConsumed(Sequence);
	}
}
#else
void UVeloWebView::EnsureCompositedCEF(FIntPoint InitialSize) {}
#endif

#if PLATFORM_ANDROID
TSharedPtr<SWebBrowserView> UVeloWebView::GetCompositedBrowser() const
{
	return RenderMode == EVeloWebViewRenderMode::Composited && MyWebViewWidget.IsValid()
		? MyWebViewWidget->GetCompositedBrowser() : nullptr;
}

#endif

#if PLATFORM_ANDROID
bool UVeloWebView::FlushPendingCompositedNavigation()
{
	const TSharedPtr<SWebBrowserView> Browser = GetCompositedBrowser();
	if (!Browser || !bCompositedInitialized) return false;
	if (bPendingHTML)
	{
		UE_LOG(LogVeloWebView, Verbose, TEXT("Flushing composited HTML navigation"));
		const FString HTML = MoveTemp(PendingHTML);
		PendingHTML.Reset();
		FString BaseURL = PendingBaseURL.IsEmpty() ? TEXT("about:blank") : PendingBaseURL;
#if PLATFORM_WINDOWS
		// CEF's LoadString intercepts a resource request. about:blank does not
		// issue one, so give inline HTML a synthetic URL in composited mode.
		if (BaseURL.Equals(TEXT("about:blank"), ESearchCase::IgnoreCase))
		{
			BaseURL = TEXT("http://velowebview.local/");
		}
#endif
		bPendingHTML = false;
		PendingBaseURL.Reset();
		Browser->LoadString(HTML, BaseURL);
		return true;
	}
	if (!PendingURL.IsEmpty())
	{
		UE_LOG(LogVeloWebView, Verbose, TEXT("Flushing composited URL navigation: %s"), *PendingURL);
		const FString URL = MoveTemp(PendingURL);
		PendingURL.Reset();
		Browser->LoadURL(URL);
		return true;
	}
	return false;
}
#endif

void UVeloWebView::LoadURL(const FString& URL)
{
	if (URL.IsEmpty())
	{
		return;
	}
	PendingURL = URL;
	bPendingHTML = false;
	PendingHTML.Reset();
	PendingBaseURL.Reset();
	#if PLATFORM_ANDROID
	if (const TSharedPtr<SWebBrowserView> Browser = GetCompositedBrowser())
	{
		FlushPendingCompositedNavigation();
		return;
	}
	#elif PLATFORM_WINDOWS
	if (RenderMode == EVeloWebViewRenderMode::Composited)
	{
		if (CEFConnection.IsValid()) CEFConnection->LoadURL(URL);
		return;
	}
	#endif
	QueueOrExecute([URL](IVeloWebViewPlatform& Native) { Native.LoadURL(URL); });
}

void UVeloWebView::LoadHTML(const FString& HTML, const FString& BaseURL)
{
	PendingHTML = HTML;
	PendingBaseURL = BaseURL;
	bPendingHTML = true;
	PendingURL.Reset();
	#if PLATFORM_ANDROID
	if (const TSharedPtr<SWebBrowserView> Browser = GetCompositedBrowser())
	{
		FlushPendingCompositedNavigation();
		return;
	}
	#elif PLATFORM_WINDOWS
	if (RenderMode == EVeloWebViewRenderMode::Composited)
	{
		if (CEFConnection.IsValid()) CEFConnection->LoadHTML(HTML, BaseURL);
		return;
	}
	#endif
	QueueOrExecute([HTML, BaseURL](IVeloWebViewPlatform& Native) { Native.LoadHTML(HTML, BaseURL); });
}

bool UVeloWebView::LoadHostHTML(const FString& InitialIframeURL)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("VeloWebView"));
	if (!Plugin.IsValid())
	{
		UE_LOG(LogVeloWebView, Error, TEXT("Unable to locate the VeloWebView plugin while loading Host.html."));
		return false;
	}

	FString HostHTML;
	FString LoadedHostPath;
	// Plugin content is available in the editor, but the plugin checkout path is
	// not present in a packaged Android build.  Build.cs stages this file under
	// project Content/VeloWebView so the normal UE platform file system can read
	// it from the pak/OBB.  Keep the plugin path as an editor/development
	// fallback for projects that have not been repackaged yet.
	const TArray<FString> HostPaths =
	{
		FPaths::Combine(FPaths::ProjectContentDir(), TEXT("VeloWebView/Host.html")),
		FPaths::Combine(Plugin->GetContentDir(), TEXT("Host.html"))
	};
	for (const FString& CandidatePath : HostPaths)
	{
		if (FFileHelper::LoadFileToString(HostHTML, *CandidatePath))
		{
			LoadedHostPath = CandidatePath;
			break;
		}
	}
	if (LoadedHostPath.IsEmpty())
	{
		UE_LOG(LogVeloWebView, Error, TEXT("Unable to read VeloWebView host page from packaged Content/VeloWebView or plugin Content."));
		return false;
	}
	UE_LOG(LogVeloWebView, Verbose, TEXT("Loaded VeloWebView host page: %s"), *LoadedHostPath);

	const FString IframeURL = InitialIframeURL.IsEmpty() ? TEXT("about:blank") : InitialIframeURL;
	HostHTML.ReplaceInline(
		TEXT("__VELO_HOST_IFRAME_URL__"),
		*VeloWebViewPrivate::EscapeJavaScriptString(IframeURL),
		ESearchCase::CaseSensitive);

	// Keep the synthetic host document on the same scheme as an HTTP iframe on
	// Android.  Otherwise the iframe becomes mixed content under the default
	// WebView policy and is silently blocked before it can reach a LAN server.
	// CEF/Windows keeps the HTTPS synthetic origin used by the existing host
	// page contract.
	FString HostBaseURL = TEXT("https://velowebview.local/");
#if PLATFORM_ANDROID
	if (IframeURL.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase))
	{
		HostBaseURL = TEXT("http://velowebview.local/");
	}
#endif
	LoadHTML(HostHTML, HostBaseURL);
	return true;
}

void UVeloWebView::Reload()
{
#if PLATFORM_ANDROID
	if (const auto Browser = GetCompositedBrowser()) { Browser->Reload(); return; }
#elif PLATFORM_WINDOWS
	if (RenderMode == EVeloWebViewRenderMode::Composited && CEFConnection.IsValid()) { CEFConnection->Reload(); return; }
#endif
	QueueOrExecute([](IVeloWebViewPlatform& Native) { Native.Reload(); });
}
void UVeloWebView::Stop()
{
#if PLATFORM_ANDROID
	if (const auto Browser = GetCompositedBrowser()) { Browser->StopLoad(); return; }
#elif PLATFORM_WINDOWS
	if (RenderMode == EVeloWebViewRenderMode::Composited && CEFConnection.IsValid()) { CEFConnection->StopLoad(); return; }
#endif
	QueueOrExecute([](IVeloWebViewPlatform& Native) { Native.Stop(); });
}
void UVeloWebView::GoBack()
{
#if PLATFORM_ANDROID
	if (const auto Browser = GetCompositedBrowser()) { Browser->GoBack(); return; }
#elif PLATFORM_WINDOWS
	if (RenderMode == EVeloWebViewRenderMode::Composited && CEFConnection.IsValid()) { CEFConnection->GoBack(); return; }
#endif
	QueueOrExecute([](IVeloWebViewPlatform& Native) { Native.GoBack(); });
}
void UVeloWebView::GoForward()
{
#if PLATFORM_ANDROID
	if (const auto Browser = GetCompositedBrowser()) { Browser->GoForward(); return; }
#elif PLATFORM_WINDOWS
	if (RenderMode == EVeloWebViewRenderMode::Composited && CEFConnection.IsValid()) { CEFConnection->GoForward(); return; }
#endif
	QueueOrExecute([](IVeloWebViewPlatform& Native) { Native.GoForward(); });
}

bool UVeloWebView::CanGoBack() const
{
#if PLATFORM_ANDROID
	if (const auto Browser = GetCompositedBrowser()) return Browser->CanGoBack();
#endif
	return Platform.IsValid() && Platform->CanGoBack();
}
bool UVeloWebView::CanGoForward() const
{
#if PLATFORM_ANDROID
	if (const auto Browser = GetCompositedBrowser()) return Browser->CanGoForward();
#endif
	return Platform.IsValid() && Platform->CanGoForward();
}

void UVeloWebView::ExecuteJavaScript(const FString& Script)
{
	#if PLATFORM_ANDROID
	if (const auto Browser = GetCompositedBrowser())
	{
		Browser->ExecuteJavascript(Script);
		return;
	}
	#elif PLATFORM_WINDOWS
	if (RenderMode == EVeloWebViewRenderMode::Composited && CEFConnection.IsValid())
	{
		CEFConnection->ExecuteJavaScript(Script);
		return;
	}
	#endif
	QueueOrExecute([Script](IVeloWebViewPlatform& Native) { Native.ExecuteJavaScript(Script); });
}

void UVeloWebView::SetHostIframeURL(const FString& URL)
{
	const FString EscapedURL = VeloWebViewPrivate::EscapeJavaScriptString(URL);
	ExecuteJavaScript(FString::Printf(
		TEXT("if(window.VeloHost&&window.VeloHost.setIframeURL){window.VeloHost.setIframeURL(\"%s\");}"),
		*EscapedURL));
}

void UVeloWebView::RefreshHostIframe()
{
	ExecuteJavaScript(TEXT("if(window.VeloHost&&window.VeloHost.refreshIframe){window.VeloHost.refreshIframe();}"));
}

void UVeloWebView::SetJavaScriptEnabled(bool bEnabled)
{
	bEnableJavaScript = bEnabled;
	QueueOrExecute([bEnabled](IVeloWebViewPlatform& Native) { Native.SetJavaScriptEnabled(bEnabled); });
}

void UVeloWebView::SetMediaAutoplay(bool bEnabled)
{
	bMediaAutoplay = bEnabled;
	QueueOrExecute([bEnabled](IVeloWebViewPlatform& Native) { Native.SetMediaAutoplay(bEnabled); });
}

void UVeloWebView::SetMuted(bool bInMuted)
{
	bMuted = bInMuted;
	#if PLATFORM_ANDROID
	if (GetCompositedBrowser())
	{
		ExecuteJavaScript(bInMuted
			? TEXT("document.querySelectorAll('video,audio').forEach(function(e){e.muted=true;});")
			: TEXT("document.querySelectorAll('video,audio').forEach(function(e){e.muted=false;});"));
		return;
	}
	#elif PLATFORM_WINDOWS
	if (RenderMode == EVeloWebViewRenderMode::Composited && CEFConnection.IsValid())
	{
		CEFConnection->SetMuted(bInMuted);
		return;
	}
	#endif
	QueueOrExecute([bInMuted](IVeloWebViewPlatform& Native) { Native.SetMuted(bInMuted); });
}

void UVeloWebView::SetUserAgent(const FString& InUserAgent)
{
	UserAgent = InUserAgent;
	QueueOrExecute([InUserAgent](IVeloWebViewPlatform& Native) { Native.SetUserAgent(InUserAgent); });
}

void UVeloWebView::SetBackgroundTransparent(bool bTransparent)
{
	bTransparentBackground = bTransparent;
	QueueOrExecute([bTransparent](IVeloWebViewPlatform& Native) { Native.SetBackgroundTransparent(bTransparent); });
}

void UVeloWebView::SetAllowMixedContent(bool bAllow)
{
	bAllowMixedContent = bAllow;
	QueueOrExecute([bAllow](IVeloWebViewPlatform& Native) { Native.SetAllowMixedContent(bAllow); });
}

void UVeloWebView::SetNewWindowPolicy(EVeloWebViewNewWindowPolicy Policy)
{
	NewWindowPolicy = Policy;
	QueueOrExecute([Policy](IVeloWebViewPlatform& Native) { Native.SetNewWindowPolicy(Policy); });
}

void UVeloWebView::SetWebViewVisibility(bool bVisible)
{
	bRequestedVisibility = bVisible;
#if PLATFORM_WINDOWS
	if (RenderMode == EVeloWebViewRenderMode::Composited && CEFConnection.IsValid())
	{
		CEFConnection->SetVisibility(bVisible);
		return;
	}
#endif
	QueueOrExecute([bVisible](IVeloWebViewPlatform& Native) { Native.SetVisibility(bVisible); });
}

FString UVeloWebView::GetURL() const
{
#if PLATFORM_ANDROID
	if (const auto Browser = GetCompositedBrowser()) return Browser->GetUrl();
#elif PLATFORM_WINDOWS
	if (RenderMode == EVeloWebViewRenderMode::Composited && CEFConnection.IsValid()) return CEFConnection->GetURL();
#endif
	return Platform.IsValid() ? Platform->GetURL() : PendingURL;
}
FString UVeloWebView::GetTitle() const
{
#if PLATFORM_ANDROID
	if (const auto Browser = GetCompositedBrowser()) return Browser->GetTitleText().ToString();
#elif PLATFORM_WINDOWS
	if (RenderMode == EVeloWebViewRenderMode::Composited && CEFConnection.IsValid()) return CEFConnection->GetTitle();
#endif
	return Platform.IsValid() ? Platform->GetTitle() : FString();
}
EVeloWebViewState UVeloWebView::GetState() const
{
#if PLATFORM_ANDROID
	if (const auto Browser = GetCompositedBrowser()) return Browser->IsLoading() ? EVeloWebViewState::Loading : EVeloWebViewState::Ready;
#elif PLATFORM_WINDOWS
	if (RenderMode == EVeloWebViewRenderMode::Composited && CEFConnection.IsValid())
	{
		if (CEFConnection->GetState() == EVeloCEFConnectionState::Failed) return EVeloWebViewState::Failed;
		return CEFConnection->IsConnected() ? EVeloWebViewState::Ready : EVeloWebViewState::Initializing;
	}
#endif
	return Platform.IsValid() ? Platform->GetState() : EVeloWebViewState::Uninitialized;
}
bool UVeloWebView::IsReady() const
{
#if PLATFORM_ANDROID
	if (const auto Browser = GetCompositedBrowser()) return Browser->IsLoaded() || Browser->IsLoading();
#elif PLATFORM_WINDOWS
	if (RenderMode == EVeloWebViewRenderMode::Composited && CEFConnection.IsValid()) return CEFConnection->IsConnected();
#endif
	return Platform.IsValid() && Platform->IsReady();
}

void UVeloWebView::ClearCache() { QueueOrExecute([](IVeloWebViewPlatform& Native) { Native.ClearCache(); }); }
void UVeloWebView::ClearCookies()
{
	if (RenderMode == EVeloWebViewRenderMode::Composited)
	{
		CompositedCookieCache.Reset();
#if PLATFORM_ANDROID
		if (IWebBrowserSingleton* Singleton = IWebBrowserModule::Get().GetSingleton())
		{
			if (const TSharedPtr<IWebBrowserCookieManager> Cookies = Singleton->GetCookieManager()) Cookies->DeleteCookies();
		}
#elif PLATFORM_WINDOWS
		if (CEFConnection.IsValid()) CEFConnection->ClearCookies();
#endif
		return;
	}
	QueueOrExecute([](IVeloWebViewPlatform& Native) { Native.ClearCookies(); });
}

void UVeloWebView::SetCookie(const FString& URL, const FString& Name, const FString& Value,
	const FString& Domain, const FString& Path, bool bSecure)
{
	if (RenderMode == EVeloWebViewRenderMode::Composited)
	{
		CompositedCookieCache.FindOrAdd(URL).Add(Name, Value);
#if PLATFORM_ANDROID
		if (IWebBrowserSingleton* Singleton = IWebBrowserModule::Get().GetSingleton())
		{
			if (const TSharedPtr<IWebBrowserCookieManager> Cookies = Singleton->GetCookieManager())
			{
				FWebBrowserCookie Cookie;
				Cookie.Name = Name;
				Cookie.Value = Value;
				Cookie.Domain = Domain;
				Cookie.Path = Path;
				Cookie.bSecure = bSecure;
				Cookie.bHttpOnly = false;
				Cookie.bHasExpires = false;
				Cookies->SetCookie(URL, Cookie);
			}
		}
#elif PLATFORM_WINDOWS
		if (CEFConnection.IsValid()) CEFConnection->SetCookie(URL, Name, Value, Domain, Path, bSecure);
#endif
		return;
	}
	QueueOrExecute([=](IVeloWebViewPlatform& Native) { Native.SetCookie(URL, Name, Value, Domain, Path, bSecure); });
}

FString UVeloWebView::GetCookie(const FString& URL) const
{
	if (RenderMode == EVeloWebViewRenderMode::Composited)
	{
		TArray<FString> Entries;
		if (const TMap<FString, FString>* Cookies = CompositedCookieCache.Find(URL))
		{
			for (const TPair<FString, FString>& Entry : *Cookies) Entries.Add(Entry.Key + TEXT("=") + Entry.Value);
		}
		return FString::Join(Entries, TEXT("; "));
	}
	return Platform.IsValid() ? Platform->GetCookie(URL) : FString();
}

void UVeloWebView::DeleteCookie(const FString& URL, const FString& Name)
{
	if (RenderMode == EVeloWebViewRenderMode::Composited)
	{
		if (TMap<FString, FString>* Cached = CompositedCookieCache.Find(URL)) Cached->Remove(Name);
#if PLATFORM_ANDROID
		if (IWebBrowserSingleton* Singleton = IWebBrowserModule::Get().GetSingleton())
		{
			if (const TSharedPtr<IWebBrowserCookieManager> Cookies = Singleton->GetCookieManager()) Cookies->DeleteCookies(URL, Name);
		}
#elif PLATFORM_WINDOWS
		if (CEFConnection.IsValid()) CEFConnection->DeleteCookie(URL, Name);
#endif
		return;
	}
	QueueOrExecute([=](IVeloWebViewPlatform& Native) { Native.DeleteCookie(URL, Name); });
}

void UVeloWebView::GrantPermission(EVeloWebViewPermission Permission)
{
	QueueOrExecute([Permission](IVeloWebViewPlatform& Native) { Native.GrantPermission(Permission); });
}

void UVeloWebView::DenyPermission(EVeloWebViewPermission Permission)
{
	QueueOrExecute([Permission](IVeloWebViewPlatform& Native) { Native.DenyPermission(Permission); });
}

FString UVeloWebView::GetLastError() const
{
	#if PLATFORM_WINDOWS
	if (RenderMode == EVeloWebViewRenderMode::Composited && CEFConnection.IsValid()) return CEFConnection->GetLastError();
	#endif
	return Platform.IsValid() ? Platform->GetLastError() : FString();
}

TSharedRef<SWidget> UVeloWebView::RebuildWidget()
{
	if (IsDesignTime())
	{
		return SNew(SVeloWebView).Owner(this);
	}
	if (RenderMode == EVeloWebViewRenderMode::Composited && !ScriptBridge)
	{
		ScriptBridge = NewObject<UVeloWebViewScriptBridge>(this);
		ScriptBridge->SetOwner(this);
	}

	MyWebViewWidget = SNew(SVeloWebView).Owner(this);
	return MyWebViewWidget.ToSharedRef();
}

void UVeloWebView::ReleaseSlateResources(bool bReleaseChildren)
{
	if (Platform.IsValid())
	{
		Platform->Shutdown();
		Platform.Reset();
	}
	if (CEFConnection.IsValid())
	{
		CEFConnection->Stop();
		CEFConnection.Reset();
	}
	CompositedTexture = nullptr;
	CompositedTextureSize = FIntPoint::ZeroValue;
	CompositedTextureSequence = 0;
	MyWebViewWidget.Reset();
	#if PLATFORM_ANDROID
	bCompositedInitialized = false;
	#endif
	Super::ReleaseSlateResources(bReleaseChildren);
}

void UVeloWebView::SynchronizeProperties()
{
	Super::SynchronizeProperties();
	if (Platform.IsValid())
	{
		Platform->SetJavaScriptEnabled(bEnableJavaScript);
		Platform->SetMediaAutoplay(bMediaAutoplay);
		Platform->SetMuted(bMuted);
		Platform->SetUserAgent(UserAgent);
		Platform->SetBackgroundTransparent(bTransparentBackground);
		Platform->SetAllowMixedContent(bAllowMixedContent);
		Platform->SetNewWindowPolicy(NewWindowPolicy);
		Platform->SetVisibility(bRequestedVisibility);
	}
	#if PLATFORM_WINDOWS
	if (RenderMode == EVeloWebViewRenderMode::Composited && CEFConnection.IsValid())
	{
		CEFConnection->SetJavaScriptEnabled(bEnableJavaScript);
		CEFConnection->SetMuted(bMuted);
		CEFConnection->SetVisibility(bRequestedVisibility);
	}
	#endif
}

void UVeloWebView::BeginDestroy()
{
	if (Platform.IsValid())
	{
		Platform->Shutdown();
		Platform.Reset();
	}
	if (CEFConnection.IsValid())
	{
		CEFConnection->Stop();
		CEFConnection.Reset();
	}
	CompositedTexture = nullptr;
	CompositedTextureSize = FIntPoint::ZeroValue;
	CompositedTextureSequence = 0;
	MyWebViewWidget.Reset();
	Super::BeginDestroy();
}

void UVeloWebView::HandlePageStarted(const FString& URL) { OnPageStarted.Broadcast(URL); }
void UVeloWebView::HandlePageLoaded(const FString& URL) { OnPageLoaded.Broadcast(URL); }
void UVeloWebView::HandlePageError(const FVeloWebViewError& Error) { OnPageError.Broadcast(Error); }
void UVeloWebView::HandleURLChanged(const FString& URL) { OnURLChanged.Broadcast(URL); }
void UVeloWebView::HandleTitleChanged(const FString& Title) { OnTitleChanged.Broadcast(Title); }
void UVeloWebView::HandleJavaScriptMessage(const FString& Message)
{
#if !UE_BUILD_SHIPPING
	UE_LOG(LogVeloWebView, Display, TEXT("JavaScript message: %s"), *Message);
#endif
	OnJavaScriptMessage.Broadcast(Message);
}
void UVeloWebView::HandleConsoleMessage(const FString& Message, const FString& Source, int32 Line) { OnConsoleMessage.Broadcast(Message, Source, Line); }
void UVeloWebView::HandleDownloadRequested(const FString& URL, const FString& SuggestedFilename, const FString& MimeType) { OnDownloadRequested.Broadcast(URL, SuggestedFilename, MimeType); }
void UVeloWebView::HandlePermissionRequested(EVeloWebViewPermission Permission, const FString& Origin) { OnPermissionRequested.Broadcast(Permission, Origin); }
void UVeloWebView::HandleFullscreenChanged(bool bIsFullscreen) { OnFullscreenChanged.Broadcast(bIsFullscreen); }
void UVeloWebView::HandleBeforeNavigate(const FString& URL) { OnBeforeNavigate.Broadcast(URL); }
void UVeloWebView::HandleNewWindowRequested(const FString& URL, bool bIsUserInitiated) { OnNewWindowRequested.Broadcast(URL, bIsUserInitiated); }

void UVeloWebViewScriptBridge::PostMessage(const FString& Message)
{
	#if !UE_BUILD_SHIPPING
	if (Message == TEXT("VeloLayerTest")) UE_LOG(LogVeloWebView, Display, TEXT("Layer test JavaScript bridge received"));
	#endif
	const TWeakObjectPtr<UVeloWebView> WeakOwner = Owner;
	VeloWebViewPrivate::DispatchToGameThread(WeakOwner, [Message](UVeloWebView& Widget)
	{
		static const FString PopupPrefix = TEXT("__VELO_NEW_WINDOW__");
		if (Message.StartsWith(PopupPrefix) && Message.Len() > PopupPrefix.Len())
		{
			const bool bUserInitiated = Message[PopupPrefix.Len()] == TCHAR('1');
			const FString URL = Message.Mid(PopupPrefix.Len() + 1);
			if (Widget.NewWindowPolicy == EVeloWebViewNewWindowPolicy::OpenInSameView)
			{
				if (URL.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase) || URL.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase)) Widget.LoadURL(URL);
				else if (VeloWebViewPrivate::IsExternalURL(URL)) FPlatformProcess::LaunchURL(*URL, nullptr, nullptr);
			}
			else if (Widget.NewWindowPolicy == EVeloWebViewNewWindowPolicy::OpenExternal && VeloWebViewPrivate::IsExternalURL(URL))
			{
				FPlatformProcess::LaunchURL(*URL, nullptr, nullptr);
			}
			Widget.HandleNewWindowRequested(URL, bUserInitiated);
			return;
		}
		Widget.HandleJavaScriptMessage(Message);
	});
}

IMPLEMENT_MODULE(FDefaultModuleImpl, VeloWebView)
