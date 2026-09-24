#include "VeloWebView.h"

#include "Async/Async.h"
#include "IVeloWebViewPlatform.h"
#include "Modules/ModuleManager.h"
#include "SVeloWebView.h"

#include "Framework/Application/SlateApplication.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsWindow.h"
#endif

DEFINE_LOG_CATEGORY(LogVeloWebView);

namespace VeloWebViewPrivate
{
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
	QueueOrExecute([URL](IVeloWebViewPlatform& Native) { Native.LoadURL(URL); });
}

void UVeloWebView::LoadHTML(const FString& HTML, const FString& BaseURL)
{
	PendingHTML = HTML;
	PendingBaseURL = BaseURL;
	bPendingHTML = true;
	PendingURL.Reset();
	QueueOrExecute([HTML, BaseURL](IVeloWebViewPlatform& Native) { Native.LoadHTML(HTML, BaseURL); });
}

void UVeloWebView::Reload() { QueueOrExecute([](IVeloWebViewPlatform& Native) { Native.Reload(); }); }
void UVeloWebView::Stop() { QueueOrExecute([](IVeloWebViewPlatform& Native) { Native.Stop(); }); }
void UVeloWebView::GoBack() { QueueOrExecute([](IVeloWebViewPlatform& Native) { Native.GoBack(); }); }
void UVeloWebView::GoForward() { QueueOrExecute([](IVeloWebViewPlatform& Native) { Native.GoForward(); }); }

bool UVeloWebView::CanGoBack() const { return Platform.IsValid() && Platform->CanGoBack(); }
bool UVeloWebView::CanGoForward() const { return Platform.IsValid() && Platform->CanGoForward(); }

void UVeloWebView::ExecuteJavaScript(const FString& Script)
{
	QueueOrExecute([Script](IVeloWebViewPlatform& Native) { Native.ExecuteJavaScript(Script); });
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
	QueueOrExecute([bVisible](IVeloWebViewPlatform& Native) { Native.SetVisibility(bVisible); });
}

FString UVeloWebView::GetURL() const { return Platform.IsValid() ? Platform->GetURL() : PendingURL; }
FString UVeloWebView::GetTitle() const { return Platform.IsValid() ? Platform->GetTitle() : FString(); }
EVeloWebViewState UVeloWebView::GetState() const { return Platform.IsValid() ? Platform->GetState() : EVeloWebViewState::Uninitialized; }
bool UVeloWebView::IsReady() const { return Platform.IsValid() && Platform->IsReady(); }

void UVeloWebView::ClearCache() { QueueOrExecute([](IVeloWebViewPlatform& Native) { Native.ClearCache(); }); }
void UVeloWebView::ClearCookies() { QueueOrExecute([](IVeloWebViewPlatform& Native) { Native.ClearCookies(); }); }

void UVeloWebView::SetCookie(const FString& URL, const FString& Name, const FString& Value,
	const FString& Domain, const FString& Path, bool bSecure)
{
	QueueOrExecute([=](IVeloWebViewPlatform& Native) { Native.SetCookie(URL, Name, Value, Domain, Path, bSecure); });
}

FString UVeloWebView::GetCookie(const FString& URL) const
{
	return Platform.IsValid() ? Platform->GetCookie(URL) : FString();
}

void UVeloWebView::DeleteCookie(const FString& URL, const FString& Name)
{
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
	return Platform.IsValid() ? Platform->GetLastError() : FString();
}

TSharedRef<SWidget> UVeloWebView::RebuildWidget()
{
	if (IsDesignTime())
	{
		return SNew(SVeloWebView).Owner(this);
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
	MyWebViewWidget.Reset();
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
}

void UVeloWebView::BeginDestroy()
{
	if (Platform.IsValid())
	{
		Platform->Shutdown();
		Platform.Reset();
	}
	MyWebViewWidget.Reset();
	Super::BeginDestroy();
}

void UVeloWebView::HandlePageStarted(const FString& URL) { OnPageStarted.Broadcast(URL); }
void UVeloWebView::HandlePageLoaded(const FString& URL) { OnPageLoaded.Broadcast(URL); }
void UVeloWebView::HandlePageError(const FVeloWebViewError& Error) { OnPageError.Broadcast(Error); }
void UVeloWebView::HandleURLChanged(const FString& URL) { OnURLChanged.Broadcast(URL); }
void UVeloWebView::HandleTitleChanged(const FString& Title) { OnTitleChanged.Broadcast(Title); }
void UVeloWebView::HandleJavaScriptMessage(const FString& Message) { OnJavaScriptMessage.Broadcast(Message); }
void UVeloWebView::HandleConsoleMessage(const FString& Message, const FString& Source, int32 Line) { OnConsoleMessage.Broadcast(Message, Source, Line); }
void UVeloWebView::HandleDownloadRequested(const FString& URL, const FString& SuggestedFilename, const FString& MimeType) { OnDownloadRequested.Broadcast(URL, SuggestedFilename, MimeType); }
void UVeloWebView::HandlePermissionRequested(EVeloWebViewPermission Permission, const FString& Origin) { OnPermissionRequested.Broadcast(Permission, Origin); }
void UVeloWebView::HandleFullscreenChanged(bool bIsFullscreen) { OnFullscreenChanged.Broadcast(bIsFullscreen); }
void UVeloWebView::HandleBeforeNavigate(const FString& URL) { OnBeforeNavigate.Broadcast(URL); }
void UVeloWebView::HandleNewWindowRequested(const FString& URL, bool bIsUserInitiated) { OnNewWindowRequested.Broadcast(URL, bIsUserInitiated); }

IMPLEMENT_MODULE(FDefaultModuleImpl, VeloWebView)
