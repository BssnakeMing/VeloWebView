#include "SVeloWebView.h"

#include "IVeloWebViewPlatform.h"
#include "VeloWebView.h"

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#if PLATFORM_ANDROID
#include "SWebBrowserView.h"
#include "WebBrowserModule.h"
#endif
#include "Framework/Application/SlateApplication.h"
#if PLATFORM_ANDROID
#include "Widgets/SVirtualWindow.h"
#endif
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Engine/Texture2D.h"
#include "BrowserProtocol/BrowserProtocol.h"

namespace
{
#if PLATFORM_WINDOWS
	// Keep these values in sync with cef_event_flags_t without including CEF
	// headers in the Unreal module.
	enum : uint32
	{
		CefEventCapsLock = 1u << 0,
		CefEventShift = 1u << 1,
		CefEventControl = 1u << 2,
		CefEventAlt = 1u << 3,
		CefEventLeftMouse = 1u << 4,
		CefEventMiddleMouse = 1u << 5,
		CefEventRightMouse = 1u << 6,
		CefEventCommand = 1u << 7,
		CefEventKeyPad = 1u << 9,
		CefEventLeft = 1u << 10,
		CefEventRight = 1u << 11,
		CefEventRepeat = 1u << 13
	};

	uint32 GetCefInputModifiers(const FInputEvent& Event)
	{
		uint32 Modifiers = 0;
		if (Event.IsShiftDown()) Modifiers |= CefEventShift;
		if (Event.IsControlDown()) Modifiers |= CefEventControl;
		if (Event.IsAltDown()) Modifiers |= CefEventAlt;
		if (Event.IsCommandDown()) Modifiers |= CefEventCommand;
		if (Event.AreCapsLocked()) Modifiers |= CefEventCapsLock;
		if (Event.IsRepeat()) Modifiers |= CefEventRepeat;
		return Modifiers;
	}

	uint32 GetCefKeyboardModifiers(const FKeyEvent& Event)
	{
		uint32 Modifiers = GetCefInputModifiers(Event);
		const FKey Key = Event.GetKey();
		if (Key == EKeys::LeftAlt || Key == EKeys::LeftCommand || Key == EKeys::LeftControl || Key == EKeys::LeftShift)
		{
			Modifiers |= CefEventLeft;
		}
		if (Key == EKeys::RightAlt || Key == EKeys::RightCommand || Key == EKeys::RightControl || Key == EKeys::RightShift)
		{
			Modifiers |= CefEventRight;
		}
		if (Key == EKeys::NumPadZero || Key == EKeys::NumPadOne || Key == EKeys::NumPadTwo ||
			Key == EKeys::NumPadThree || Key == EKeys::NumPadFour || Key == EKeys::NumPadFive ||
			Key == EKeys::NumPadSix || Key == EKeys::NumPadSeven || Key == EKeys::NumPadEight ||
			Key == EKeys::NumPadNine)
		{
			Modifiers |= CefEventKeyPad;
		}
		return Modifiers;
	}

	uint32 GetCefMouseModifiers(const FPointerEvent& Event)
	{
		uint32 Modifiers = GetCefInputModifiers(Event);
		if (Event.IsMouseButtonDown(EKeys::LeftMouseButton)) Modifiers |= CefEventLeftMouse;
		if (Event.IsMouseButtonDown(EKeys::MiddleMouseButton)) Modifiers |= CefEventMiddleMouse;
		if (Event.IsMouseButtonDown(EKeys::RightMouseButton)) Modifiers |= CefEventRightMouse;
		return Modifiers;
	}

	FIntPoint GetCefPointerPosition(const FGeometry& Geometry, const FPointerEvent& Event, const FIntPoint& FrameSize)
	{
		const FVector2D Local = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition());
		const FVector2D Size = Geometry.GetLocalSize();
		// The CPU ring currently caps the CEF surface to its initial allocation.
		// Map onto the displayed frame so DPI, render transforms and a stretched
		// 1280x720 surface still hit the DOM element under the pointer.
		const double ScaleX = FrameSize.X > 0 && Size.X > 0 ? FrameSize.X / Size.X : Geometry.Scale;
		const double ScaleY = FrameSize.Y > 0 && Size.Y > 0 ? FrameSize.Y / Size.Y : Geometry.Scale;
		return FIntPoint(FMath::RoundToInt(Local.X * ScaleX), FMath::RoundToInt(Local.Y * ScaleY));
	}

	uint32 GetWindowsKeyCode(const FKeyEvent& Event)
	{
		if (Event.GetKeyCode()) return Event.GetKeyCode();
		const uint32* KeyCode = nullptr;
		const uint32* CharCode = nullptr;
		FInputKeyManager::Get().GetCodesFromKey(Event.GetKey(), KeyCode, CharCode);
		return KeyCode ? *KeyCode : 0;
	}

	bool GetCefMouseButton(const FKey& Key, uint8& OutButton)
	{
		if (Key == EKeys::LeftMouseButton) { OutButton = VeloWebView::Protocol::MouseButtonLeft; return true; }
		if (Key == EKeys::MiddleMouseButton) { OutButton = VeloWebView::Protocol::MouseButtonMiddle; return true; }
		if (Key == EKeys::RightMouseButton) { OutButton = VeloWebView::Protocol::MouseButtonRight; return true; }
		return false;
	}
#endif

	bool IsWebURL(const FString& URL)
	{
		return URL.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase)
			|| URL.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase);
	}

	bool IsSystemURL(const FString& URL)
	{
		return URL.StartsWith(TEXT("mailto:"), ESearchCase::IgnoreCase)
			|| URL.StartsWith(TEXT("tel:"), ESearchCase::IgnoreCase);
	}

	void InstallCompositedBridge(UVeloWebView& View)
	{
		FString Script = TEXT("(function(){window.VeloWebView=window.VeloWebView||{};"
			"window.VeloWebView.postMessage=function(m){if(window.ue&&window.ue.velobridge){window.ue.velobridge.postmessage(String(m));}};");
#if PLATFORM_ANDROID
		Script += TEXT("if(!window.__VeloPopupInterceptor){window.__VeloPopupInterceptor=true;"
			"window.VeloWebView.__open=function(url,user){try{var target=new URL(String(url),location.href).href;"
			"window.VeloWebView.postMessage('__VELO_NEW_WINDOW__'+(user?'1':'0')+target);}catch(e){}};"
			"window.open=function(url){if(url)window.VeloWebView.__open(url,!!(navigator.userActivation&&navigator.userActivation.isActive));return null;};"
			"document.addEventListener('click',function(e){var a=e.target&&e.target.closest&&e.target.closest('a[target]');"
			"if(a&&a.target&&a.target.toLowerCase()!=='_self'&&a.target.toLowerCase()!=='_top'&&a.target.toLowerCase()!=='_parent'){"
			"e.preventDefault();window.VeloWebView.__open(a.href,true);}},true);}");
#endif
		Script += TEXT("})();");
		View.ExecuteJavaScript(Script);
	}

	void OnGameThread(TWeakObjectPtr<UVeloWebView> WeakOwner, TFunction<void(UVeloWebView&)>&& Callback)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakOwner, Callback = MoveTemp(Callback)]() mutable
		{
			if (UVeloWebView* Widget = WeakOwner.Get()) Callback(*Widget);
		});
	}
}

void SVeloWebView::Construct(const FArguments& InArgs)
{
	Owner = InArgs._Owner;
	if (UVeloWebView* Widget = Owner.Get())
	{
		if (!Widget->IsDesignTime() && Widget->RenderMode == EVeloWebViewRenderMode::Composited)
		{
#if PLATFORM_ANDROID
			IWebBrowserModule& BrowserModule = IWebBrowserModule::Get();
			if (BrowserModule.IsWebModuleAvailable() && BrowserModule.GetSingleton())
			{
#if PLATFORM_ANDROID
				// Engine's Android browser draws to a Slate texture when its parent
				// is virtual; a regular SWindow selects the native topmost overlay.
				BrowserVirtualWindow = SNew(SVirtualWindow);
#endif
				const TWeakObjectPtr<UVeloWebView> WeakOwner = Owner;
				Browser = SNew(SWebBrowserView)
					.ParentWindow(BrowserVirtualWindow)
					.InitialURL(Widget->InitialURL.IsEmpty() ? TEXT("about:blank") : Widget->InitialURL)
					.SupportsTransparency(Widget->bTransparentBackground)
					.Visibility(EVisibility::Visible)
					.OnLoadStarted(FSimpleDelegate::CreateLambda([WeakOwner]()
					{
					OnGameThread(WeakOwner, [](UVeloWebView& View)
					{
						View.HandlePageStarted(View.GetURL());
						InstallCompositedBridge(View);
					});
				}))
					.OnLoadCompleted(FSimpleDelegate::CreateLambda([WeakOwner]()
					{
					OnGameThread(WeakOwner, [](UVeloWebView& View)
					{
						View.bCompositedInitialized = true;
						if (View.FlushPendingCompositedNavigation()) return;
						InstallCompositedBridge(View);
						View.HandlePageLoaded(View.GetURL());
					});
				}))
					.OnLoadError(FSimpleDelegate::CreateLambda([WeakOwner]()
					{
					OnGameThread(WeakOwner, [](UVeloWebView& View)
					{
						View.bCompositedInitialized = true;
						if (View.FlushPendingCompositedNavigation()) return;
						View.HandlePageError(FVeloWebViewError(-1, TEXT("Composited browser navigation failed"), View.GetURL()));
					});
				}))
					.OnUrlChanged(FOnTextChanged::CreateLambda([WeakOwner](const FText& URL)
					{
					OnGameThread(WeakOwner, [Address = URL.ToString()](UVeloWebView& View) { View.HandleURLChanged(Address); });
				}))
					.OnTitleChanged(FOnTextChanged::CreateLambda([WeakOwner](const FText& Title)
					{
					OnGameThread(WeakOwner, [Value = Title.ToString()](UVeloWebView& View) { View.HandleTitleChanged(Value); });
				}))
					.OnBeforeNavigation(SWebBrowserView::FOnBeforeBrowse::CreateLambda([WeakOwner](const FString& URL, const FWebNavigationRequest&) -> bool
					{
					OnGameThread(WeakOwner, [URL](UVeloWebView& View) { View.HandleBeforeNavigate(URL); });
					return false;
				}))
					.OnBeforePopup(FOnBeforePopupDelegate::CreateLambda([WeakOwner](FString URL, FString) -> bool
					{
					OnGameThread(WeakOwner, [URL](UVeloWebView& View)
					{
						const EVeloWebViewNewWindowPolicy Policy = View.NewWindowPolicy;
						if (Policy == EVeloWebViewNewWindowPolicy::OpenInSameView)
						{
							if (IsWebURL(URL)) View.LoadURL(URL);
							else if (IsSystemURL(URL)) FPlatformProcess::LaunchURL(*URL, nullptr, nullptr);
						}
						else if (Policy == EVeloWebViewNewWindowPolicy::OpenExternal && (IsWebURL(URL) || IsSystemURL(URL)))
						{
							FPlatformProcess::LaunchURL(*URL, nullptr, nullptr);
						}
						View.HandleNewWindowRequested(URL, false);
					});
					return true;
				}))
					.OnConsoleMessage(FOnConsoleMessageDelegate::CreateLambda([WeakOwner](const FString& Message, const FString& Source, int32 Line, EWebBrowserConsoleLogSeverity)
					{
					OnGameThread(WeakOwner, [Message, Source, Line](UVeloWebView& View) { View.HandleConsoleMessage(Message, Source, Line); });
				}));
				if (Widget->ScriptBridge) Browser->BindUObject(TEXT("velobridge"), Widget->ScriptBridge);
				UE_LOG(LogVeloWebView, Display, TEXT("Composited browser created: children=%d type=%s"),
					Browser->GetChildren()->Num(),
					Browser->GetChildren()->Num() > 0 ? *Browser->GetChildren()->GetChildAt(0)->GetType().ToString() : TEXT("<none>"));
				ChildSlot [ Browser.ToSharedRef() ];
				return;
			}
			Widget->HandlePageError(FVeloWebViewError(-1, TEXT("Slate browser backend is unavailable"), Widget->InitialURL));
#elif PLATFORM_WINDOWS
			ChildSlot
			[
				SNew(SImage)
				.Image(&CEFBrush)
			];
			return;
#endif
		}
	}

	ChildSlot
	[
		SNew(STextBlock)
		.Visibility(EVisibility::HitTestInvisible)
		.Justification(ETextJustify::Center)
		.Text_Lambda([WeakOwner = Owner]() -> FText
		{
			if (const UVeloWebView* Widget = WeakOwner.Get())
			{
				if (Widget->IsDesignTime())
				{
					const FString URL = Widget->InitialURL.IsEmpty() ? TEXT("about:blank") : Widget->InitialURL;
					return FText::FromString(FString::Printf(TEXT("Velo WebView\n%s"), *URL));
				}
			}
			return FText::GetEmpty();
		})
	];

}

void SVeloWebView::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime,
	const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	UVeloWebView* Widget = Owner.Get();
	if (!Widget || Widget->IsDesignTime())
	{
		return;
	}
	if (Widget->RenderMode == EVeloWebViewRenderMode::Composited)
	{
#if PLATFORM_ANDROID
		if (Browser.IsValid())
		{
			if (!Widget->bCompositedInitialized && Browser->IsLoaded())
			{
				Widget->bCompositedInitialized = true;
				Widget->FlushPendingCompositedNavigation();
			}
			const bool bVisible = Widget->bRequestedVisibility && GetVisibility() != EVisibility::Collapsed && GetVisibility() != EVisibility::Hidden;
			if (bVisible != bLastNativeVisibility)
			{
				bLastNativeVisibility = bVisible;
				Browser->SetVisibility(bVisible ? EVisibility::Visible : EVisibility::Collapsed);
			}
		}
#elif PLATFORM_WINDOWS
		const float ResolutionScale = FMath::Clamp(Widget->CompositedResolutionScale, 0.5f, 2.0f);
		const FIntPoint NewSize(
			FMath::Max(1, FMath::RoundToInt(AllottedGeometry.GetLocalSize().X * AllottedGeometry.Scale * ResolutionScale)),
			FMath::Max(1, FMath::RoundToInt(AllottedGeometry.GetLocalSize().Y * AllottedGeometry.Scale * ResolutionScale)));
		if (!Widget->CEFConnection.IsValid())
		{
			// Allocate the fixed CPU ring after Slate supplies real geometry. This
			// avoids starting every browser at the old 1280x720 fallback size.
			Widget->EnsureCompositedCEF(NewSize);
		}
		if (Widget->CEFConnection.IsValid() && Widget->CEFConnection->GetFrameCapacity() != FIntPoint::ZeroValue &&
			(NewSize.X > Widget->CEFConnection->GetFrameCapacity().X || NewSize.Y > Widget->CEFConnection->GetFrameCapacity().Y))
		{
			// Recreate the host when a window grows beyond the fixed shared-memory
			// allocation. Pending URL/HTML state is retained and replayed below.
			Widget->CEFConnection->Stop();
			Widget->CEFConnection.Reset();
			bLastCEFConnected = false;
			Widget->EnsureCompositedCEF(NewSize);
		}
		if (Widget->CEFConnection.IsValid())
		{
			const EVeloCEFConnectionState PreviousState = Widget->CEFConnection->GetState();
			Widget->CEFConnection->Tick(InDeltaTime);
			if (Widget->CEFConnection->GetState() == EVeloCEFConnectionState::Failed &&
				PreviousState != EVeloCEFConnectionState::Failed)
			{
				UE_LOG(LogVeloWebView, Error, TEXT("%s"), *Widget->CEFConnection->GetLastError());
				Widget->HandlePageError(FVeloWebViewError(-10, Widget->CEFConnection->GetLastError(), Widget->GetURL()));
			}
			const bool bCEFConnected = Widget->CEFConnection->IsConnected();
			if (bCEFConnected && !bLastCEFConnected)
			{
				// Navigation can be requested immediately after the widget is
				// constructed, before the named pipe handshake completes. Replay the
				// widget's pending navigation once the host is actually connected.
				if (Widget->bPendingHTML)
				{
					Widget->CEFConnection->LoadHTML(Widget->PendingHTML, Widget->PendingBaseURL);
				}
				else if (!Widget->PendingURL.IsEmpty())
				{
					Widget->CEFConnection->LoadURL(Widget->PendingURL);
				}
			}
			TArray<uint8> Pixels;
			FIntPoint FrameSize = FIntPoint::ZeroValue;
			uint64 FrameSequence = 0;
			if (Widget->CEFConnection->ReadLatestCpuFrame(Pixels, FrameSize, FrameSequence))
			{
				Widget->UpdateCompositedCEFFrame(Pixels, FrameSize, FrameSequence);
				if (UTexture2D* Texture = Widget->GetCompositedCEFTexture())
				{
					CEFBrush.SetResourceObject(Texture);
					CEFBrush.ImageSize = FVector2D(FrameSize.X, FrameSize.Y);
					Invalidate(EInvalidateWidgetReason::Paint);
				}
			}
			const float Scale = FMath::Max(AllottedGeometry.Scale, 0.01f);
			if (bCEFConnected && (!bLastCEFConnected || NewSize != FIntPoint(LastGeometry.Width(), LastGeometry.Height()) || !FMath::IsNearlyEqual(Scale, LastScale)))
			{
				LastGeometry = FIntRect(0, 0, NewSize.X, NewSize.Y);
				LastScale = Scale;
				Widget->CEFConnection->Resize(NewSize, Scale);
			}
			const bool bVisible = Widget->bRequestedVisibility && GetVisibility() != EVisibility::Collapsed && GetVisibility() != EVisibility::Hidden;
			if (bCEFConnected && (!bLastCEFConnected || bVisible != bLastNativeVisibility))
			{
				bLastNativeVisibility = bVisible;
				Widget->CEFConnection->SetVisibility(bVisible);
			}
			bLastCEFConnected = bCEFConnected;
		}
#endif
		return;
	}
	if (!Widget->Platform.IsValid())
	{
		Widget->EnsurePlatform();
	}
	if (!Widget->Platform.IsValid())
	{
		return;
	}

	const FVector2D AbsolutePosition = AllottedGeometry.GetAbsolutePosition();
	const FVector2D AbsoluteSize = AllottedGeometry.GetLocalSize() * AllottedGeometry.Scale;
	const FIntRect NewGeometry(
		FMath::RoundToInt(AbsolutePosition.X),
		FMath::RoundToInt(AbsolutePosition.Y),
		FMath::RoundToInt(AbsolutePosition.X + AbsoluteSize.X),
		FMath::RoundToInt(AbsolutePosition.Y + AbsoluteSize.Y));
	const float Scale = FMath::Max(AllottedGeometry.Scale, 0.01f);

	const bool bGeometryChanged = NewGeometry != LastGeometry || !FMath::IsNearlyEqual(Scale, LastScale);
	const bool bVisible = Widget->bRequestedVisibility && GetVisibility() != EVisibility::Collapsed && GetVisibility() != EVisibility::Hidden;
	// Android can resize its native SurfaceView without changing Slate's
	// resolution-limited geometry. Let the platform compare the mapped bounds.
	if (bGeometryChanged || PLATFORM_ANDROID)
	{
		LastGeometry = NewGeometry;
		LastScale = Scale;
		Widget->Platform->SetGeometry(NewGeometry, Scale);
	}
	if (bVisible != bLastNativeVisibility)
	{
		bLastNativeVisibility = bVisible;
		Widget->Platform->SetVisibility(bVisible);
	}
}

TSharedPtr<SWidget> SVeloWebView::GetCompositedInputWidget() const
{
#if PLATFORM_ANDROID
	if (!Browser.IsValid() || Browser->GetChildren()->Num() == 0)
	{
		return nullptr;
	}

	return Browser->GetChildren()->GetChildAt(0);
#else
	return nullptr;
#endif
}

FReply SVeloWebView::OnFocusReceived(const FGeometry& MyGeometry, const FFocusEvent& InFocusEvent)
{
	if (UVeloWebView* Widget = Owner.Get())
	{
		#if PLATFORM_ANDROID
		if (Widget->RenderMode == EVeloWebViewRenderMode::Composited && Browser.IsValid())
		{
			Browser->SetBrowserKeyboardFocus();
			return FReply::Handled().SetUserFocus(Browser.ToSharedRef(), InFocusEvent.GetCause());
		}
		#endif
		if (Widget->Platform.IsValid())
		{
			Widget->Platform->SetFocus(true);
		}
		#if PLATFORM_WINDOWS
		if (Widget->RenderMode == EVeloWebViewRenderMode::Composited && Widget->CEFConnection.IsValid())
		{
			Widget->CEFConnection->SetFocus(true);
		}
		#endif
	}
	return FReply::Handled();
}

FReply SVeloWebView::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	#if PLATFORM_ANDROID
	UE_LOG(LogVeloWebView, Display, TEXT("SVeloWebView mouse down: button=%s screen=(%.1f,%.1f) browser=%s"),
		*MouseEvent.GetEffectingButton().ToString(),
		MouseEvent.GetScreenSpacePosition().X,
		MouseEvent.GetScreenSpacePosition().Y,
		Browser.IsValid() ? TEXT("valid") : TEXT("invalid"));
	#else
	UE_LOG(LogVeloWebView, Display, TEXT("SVeloWebView mouse down: button=%s screen=(%.1f,%.1f) browser=cef"),
		*MouseEvent.GetEffectingButton().ToString(),
		MouseEvent.GetScreenSpacePosition().X,
		MouseEvent.GetScreenSpacePosition().Y);
	#endif
	if (UVeloWebView* Widget = Owner.Get())
	{
		#if PLATFORM_ANDROID
		if (Widget->RenderMode == EVeloWebViewRenderMode::Composited && Browser.IsValid())
		{
			FSlateApplication::Get().SetKeyboardFocus(Browser.ToSharedRef(), EFocusCause::Mouse);
			Browser->SetBrowserKeyboardFocus();
		}
		#endif
		if (Widget->Platform.IsValid())
		{
			Widget->Platform->SetFocus(true);
		}
		#if PLATFORM_WINDOWS
		if (Widget->RenderMode == EVeloWebViewRenderMode::Composited && Widget->CEFConnection.IsValid())
		{
			Widget->CEFConnection->SetFocus(true);
			if (Widget->CEFConnection->IsConnected())
			{
				uint8 Button = 0;
				if (GetCefMouseButton(MouseEvent.GetEffectingButton(), Button))
				{
					const FIntPoint Position = GetCefPointerPosition(MyGeometry, MouseEvent, Widget->CompositedTextureSize);
					if (Widget->CEFConnection->SendMouseButton(Position.X, Position.Y,
						GetCefMouseModifiers(MouseEvent), Button, false, 1))
					{
						LastCEFPointerPosition = Position;
						CEFClickCounts[Button] = 1;
						CEFPressedButtons |= 1u << Button;
						return FReply::Handled().CaptureMouse(AsShared()).SetUserFocus(AsShared(), EFocusCause::Mouse);
					}
				}
			}
		}
		#endif
	}
	if (const TSharedPtr<SWidget> InputWidget = GetCompositedInputWidget())
	{
		const FReply Reply = InputWidget->OnMouseButtonDown(MyGeometry, MouseEvent);
		UE_LOG(LogVeloWebView, Display, TEXT("Composited input fallback mouse down: type=%s handled=%d"),
			*InputWidget->GetType().ToString(), Reply.IsEventHandled() ? 1 : 0);
		if (Reply.IsEventHandled())
		{
			return Reply;
		}
	}
	return SCompoundWidget::OnMouseButtonDown(MyGeometry, MouseEvent);
}

FReply SVeloWebView::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	#if PLATFORM_WINDOWS
	if (UVeloWebView* Widget = Owner.Get(); Widget && Widget->RenderMode == EVeloWebViewRenderMode::Composited &&
		Widget->CEFConnection.IsValid() && Widget->CEFConnection->IsConnected())
	{
		uint8 Button = 0;
		if (GetCefMouseButton(MouseEvent.GetEffectingButton(), Button))
		{
			const FIntPoint Position = GetCefPointerPosition(MyGeometry, MouseEvent, Widget->CompositedTextureSize);
			if (Widget->CEFConnection->SendMouseButton(Position.X, Position.Y, GetCefMouseModifiers(MouseEvent), Button, true, CEFClickCounts[Button]))
			{
				LastCEFPointerPosition = Position;
				CEFPressedButtons &= ~(1u << Button);
				return CEFPressedButtons == 0 ? FReply::Handled().ReleaseMouseCapture() : FReply::Handled();
			}
		}
	}
	#endif
	if (const TSharedPtr<SWidget> InputWidget = GetCompositedInputWidget())
	{
		const FReply Reply = InputWidget->OnMouseButtonUp(MyGeometry, MouseEvent);
		if (Reply.IsEventHandled())
		{
			return Reply;
		}
	}
	return SCompoundWidget::OnMouseButtonUp(MyGeometry, MouseEvent);
}

FReply SVeloWebView::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	#if PLATFORM_WINDOWS
	if (UVeloWebView* Widget = Owner.Get(); Widget && Widget->RenderMode == EVeloWebViewRenderMode::Composited &&
		Widget->CEFConnection.IsValid() && Widget->CEFConnection->IsConnected())
	{
		const FIntPoint Position = GetCefPointerPosition(MyGeometry, MouseEvent, Widget->CompositedTextureSize);
		if (Widget->CEFConnection->SendMouseMove(Position.X, Position.Y, GetCefMouseModifiers(MouseEvent)))
		{
			LastCEFPointerPosition = Position;
			return FReply::Handled();
		}
	}
	#endif
	if (const TSharedPtr<SWidget> InputWidget = GetCompositedInputWidget())
	{
		const FReply Reply = InputWidget->OnMouseMove(MyGeometry, MouseEvent);
		if (Reply.IsEventHandled())
		{
			return Reply;
		}
	}
	return SCompoundWidget::OnMouseMove(MyGeometry, MouseEvent);
}

void SVeloWebView::OnMouseLeave(const FPointerEvent& MouseEvent)
{
	#if PLATFORM_WINDOWS
	if (UVeloWebView* Widget = Owner.Get(); Widget && Widget->RenderMode == EVeloWebViewRenderMode::Composited &&
		Widget->CEFConnection.IsValid() && Widget->CEFConnection->IsConnected() && !HasMouseCapture())
	{
		Widget->CEFConnection->SendMouseMove(0, 0, GetCefMouseModifiers(MouseEvent), true);
	}
	#endif
	if (const TSharedPtr<SWidget> InputWidget = GetCompositedInputWidget())
	{
		InputWidget->OnMouseLeave(MouseEvent);
	}
	SCompoundWidget::OnMouseLeave(MouseEvent);
}

void SVeloWebView::OnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent)
{
#if PLATFORM_WINDOWS
	if (UVeloWebView* Widget = Owner.Get(); Widget && Widget->CEFConnection.IsValid() && Widget->CEFConnection->IsConnected())
	{
		// If Slate transfers capture or the window deactivates during a drag,
		// release CEF's buttons too; otherwise the DOM remains in a stuck drag.
		for (uint8 Button = 0; Button < 3; ++Button)
		{
			if (CEFPressedButtons & (1u << Button))
			{
				Widget->CEFConnection->SendMouseButton(LastCEFPointerPosition.X, LastCEFPointerPosition.Y,
					0, Button, true, CEFClickCounts[Button]);
			}
		}
	}
	CEFPressedButtons = 0;
#endif
	SCompoundWidget::OnMouseCaptureLost(CaptureLostEvent);
}

FReply SVeloWebView::OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	#if PLATFORM_WINDOWS
	if (UVeloWebView* Widget = Owner.Get(); Widget && Widget->RenderMode == EVeloWebViewRenderMode::Composited &&
		Widget->CEFConnection.IsValid() && Widget->CEFConnection->IsConnected())
	{
		const FIntPoint Position = GetCefPointerPosition(MyGeometry, MouseEvent, Widget->CompositedTextureSize);
		const int32 TrueDelta = FMath::RoundToInt(MouseEvent.GetWheelDelta() * 120.0f);
		const int32 DeltaX = MouseEvent.IsShiftDown() ? TrueDelta : 0;
		const int32 DeltaY = MouseEvent.IsShiftDown() ? 0 : TrueDelta;
		if (FMath::Abs(TrueDelta) > 0 && Widget->CEFConnection->SendMouseWheel(
			Position.X, Position.Y, DeltaX, DeltaY, GetCefMouseModifiers(MouseEvent)))
		{
			return FReply::Handled();
		}
	}
	#endif
	if (const TSharedPtr<SWidget> InputWidget = GetCompositedInputWidget())
	{
		const FReply Reply = InputWidget->OnMouseWheel(MyGeometry, MouseEvent);
		if (Reply.IsEventHandled())
		{
			return Reply;
		}
	}
	return SCompoundWidget::OnMouseWheel(MyGeometry, MouseEvent);
}

FReply SVeloWebView::OnMouseButtonDoubleClick(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	#if PLATFORM_WINDOWS
	if (UVeloWebView* Widget = Owner.Get(); Widget && Widget->RenderMode == EVeloWebViewRenderMode::Composited &&
		Widget->CEFConnection.IsValid() && Widget->CEFConnection->IsConnected())
	{
		uint8 Button = 0;
		if (GetCefMouseButton(MouseEvent.GetEffectingButton(), Button))
		{
			const FIntPoint Position = GetCefPointerPosition(MyGeometry, MouseEvent, Widget->CompositedTextureSize);
			if (Widget->CEFConnection->SendMouseButton(Position.X, Position.Y, GetCefMouseModifiers(MouseEvent), Button, false, 2))
			{
				LastCEFPointerPosition = Position;
				CEFClickCounts[Button] = 2;
				CEFPressedButtons |= 1u << Button;
				return FReply::Handled().CaptureMouse(AsShared()).SetUserFocus(AsShared(), EFocusCause::Mouse);
			}
		}
	}
	#endif
	if (const TSharedPtr<SWidget> InputWidget = GetCompositedInputWidget())
	{
		const FReply Reply = InputWidget->OnMouseButtonDoubleClick(MyGeometry, MouseEvent);
		if (Reply.IsEventHandled())
		{
			return Reply;
		}
	}
	return SCompoundWidget::OnMouseButtonDoubleClick(MyGeometry, MouseEvent);
}

FReply SVeloWebView::OnTouchStarted(const FGeometry& MyGeometry, const FPointerEvent& TouchEvent)
{
	if (const TSharedPtr<SWidget> InputWidget = GetCompositedInputWidget())
	{
		const FReply Reply = InputWidget->OnTouchStarted(MyGeometry, TouchEvent);
		if (Reply.IsEventHandled())
		{
			return Reply;
		}
	}
	return SCompoundWidget::OnTouchStarted(MyGeometry, TouchEvent);
}

FReply SVeloWebView::OnTouchMoved(const FGeometry& MyGeometry, const FPointerEvent& TouchEvent)
{
	if (const TSharedPtr<SWidget> InputWidget = GetCompositedInputWidget())
	{
		const FReply Reply = InputWidget->OnTouchMoved(MyGeometry, TouchEvent);
		if (Reply.IsEventHandled())
		{
			return Reply;
		}
	}
	return SCompoundWidget::OnTouchMoved(MyGeometry, TouchEvent);
}

FReply SVeloWebView::OnTouchEnded(const FGeometry& MyGeometry, const FPointerEvent& TouchEvent)
{
	if (const TSharedPtr<SWidget> InputWidget = GetCompositedInputWidget())
	{
		const FReply Reply = InputWidget->OnTouchEnded(MyGeometry, TouchEvent);
		if (Reply.IsEventHandled())
		{
			return Reply;
		}
	}
	return SCompoundWidget::OnTouchEnded(MyGeometry, TouchEvent);
}

FReply SVeloWebView::OnTouchForceChanged(const FGeometry& MyGeometry, const FPointerEvent& TouchEvent)
{
	if (const TSharedPtr<SWidget> InputWidget = GetCompositedInputWidget())
	{
		const FReply Reply = InputWidget->OnTouchForceChanged(MyGeometry, TouchEvent);
		if (Reply.IsEventHandled())
		{
			return Reply;
		}
	}
	return SCompoundWidget::OnTouchForceChanged(MyGeometry, TouchEvent);
}

FReply SVeloWebView::OnTouchFirstMove(const FGeometry& MyGeometry, const FPointerEvent& TouchEvent)
{
	if (const TSharedPtr<SWidget> InputWidget = GetCompositedInputWidget())
	{
		const FReply Reply = InputWidget->OnTouchFirstMove(MyGeometry, TouchEvent);
		if (Reply.IsEventHandled())
		{
			return Reply;
		}
	}
	return SCompoundWidget::OnTouchFirstMove(MyGeometry, TouchEvent);
}

FReply SVeloWebView::OnTouchGesture(const FGeometry& MyGeometry, const FPointerEvent& GestureEvent)
{
	if (const TSharedPtr<SWidget> InputWidget = GetCompositedInputWidget())
	{
		const FReply Reply = InputWidget->OnTouchGesture(MyGeometry, GestureEvent);
		if (Reply.IsEventHandled())
		{
			return Reply;
		}
	}
	return SCompoundWidget::OnTouchGesture(MyGeometry, GestureEvent);
}

FReply SVeloWebView::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	#if PLATFORM_WINDOWS
	if (UVeloWebView* Widget = Owner.Get(); Widget && Widget->RenderMode == EVeloWebViewRenderMode::Composited &&
		Widget->CEFConnection.IsValid() && Widget->CEFConnection->IsConnected() && GetWindowsKeyCode(InKeyEvent) != 0 &&
		Widget->CEFConnection->SendKey(VeloWebView::Protocol::KeyEventRawKeyDown, GetWindowsKeyCode(InKeyEvent),
			0, InKeyEvent.GetCharacter(), InKeyEvent.GetCharacter(), GetCefKeyboardModifiers(InKeyEvent),
			InKeyEvent.IsAltDown() && !InKeyEvent.IsControlDown()))
	{
		return FReply::Handled();
	}
	#endif
	if (const TSharedPtr<SWidget> InputWidget = GetCompositedInputWidget())
	{
		const FReply Reply = InputWidget->OnKeyDown(MyGeometry, InKeyEvent);
		if (Reply.IsEventHandled())
		{
			return Reply;
		}
	}
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

FReply SVeloWebView::OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	#if PLATFORM_WINDOWS
	if (UVeloWebView* Widget = Owner.Get(); Widget && Widget->RenderMode == EVeloWebViewRenderMode::Composited &&
		Widget->CEFConnection.IsValid() && Widget->CEFConnection->IsConnected() && GetWindowsKeyCode(InKeyEvent) != 0 &&
		Widget->CEFConnection->SendKey(VeloWebView::Protocol::KeyEventKeyUp, GetWindowsKeyCode(InKeyEvent),
			0, InKeyEvent.GetCharacter(), InKeyEvent.GetCharacter(), GetCefKeyboardModifiers(InKeyEvent),
			InKeyEvent.IsAltDown() && !InKeyEvent.IsControlDown()))
	{
		return FReply::Handled();
	}
	#endif
	if (const TSharedPtr<SWidget> InputWidget = GetCompositedInputWidget())
	{
		const FReply Reply = InputWidget->OnKeyUp(MyGeometry, InKeyEvent);
		if (Reply.IsEventHandled())
		{
			return Reply;
		}
	}
	return SCompoundWidget::OnKeyUp(MyGeometry, InKeyEvent);
}

FReply SVeloWebView::OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent)
{
	#if PLATFORM_WINDOWS
	if (UVeloWebView* Widget = Owner.Get(); Widget && Widget->RenderMode == EVeloWebViewRenderMode::Composited &&
		Widget->CEFConnection.IsValid() && Widget->CEFConnection->IsConnected() &&
		Widget->CEFConnection->SendKey(VeloWebView::Protocol::KeyEventChar,
			static_cast<uint32>(InCharacterEvent.GetCharacter()), 0,
			static_cast<uint32>(InCharacterEvent.GetCharacter()), static_cast<uint32>(InCharacterEvent.GetCharacter()),
			GetCefInputModifiers(InCharacterEvent)))
	{
		return FReply::Handled();
	}
	#endif
	if (const TSharedPtr<SWidget> InputWidget = GetCompositedInputWidget())
	{
		const FReply Reply = InputWidget->OnKeyChar(MyGeometry, InCharacterEvent);
		if (Reply.IsEventHandled())
		{
			return Reply;
		}
	}
	return SCompoundWidget::OnKeyChar(MyGeometry, InCharacterEvent);
}

void SVeloWebView::OnFocusLost(const FFocusEvent& InFocusEvent)
{
	#if PLATFORM_WINDOWS
	if (UVeloWebView* Widget = Owner.Get(); Widget && Widget->RenderMode == EVeloWebViewRenderMode::Composited && Widget->CEFConnection.IsValid())
	{
		Widget->CEFConnection->SetFocus(false);
	}
	#endif
	if (const TSharedPtr<SWidget> InputWidget = GetCompositedInputWidget())
	{
		InputWidget->OnFocusLost(InFocusEvent);
	}
	SCompoundWidget::OnFocusLost(InFocusEvent);
}
