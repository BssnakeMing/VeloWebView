#include "IVeloWebViewPlatform.h"
#include "VeloWebViewAndroidBridge.h"
#include "Modules/ModuleManager.h"

#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"
#include "Android/AndroidWindow.h"

namespace VeloWebViewAndroidPrivate
{
	static EVeloWebViewPermission PermissionFromResources(const FString& Resources)
	{
		if (Resources.Contains(TEXT("VIDEO_CAPTURE"), ESearchCase::IgnoreCase))
		{
			return EVeloWebViewPermission::Camera;
		}
		if (Resources.Contains(TEXT("AUDIO_CAPTURE"), ESearchCase::IgnoreCase))
		{
			return EVeloWebViewPermission::Microphone;
		}
		if (Resources.Contains(TEXT("GEOLOCATION"), ESearchCase::IgnoreCase))
		{
			return EVeloWebViewPermission::Geolocation;
		}
		if (Resources.Contains(TEXT("NOTIFICATION"), ESearchCase::IgnoreCase))
		{
			return EVeloWebViewPermission::Notifications;
		}
		return EVeloWebViewPermission::Unknown;
	}

	/** Native Android implementation of the core platform contract. */
	class FVeloWebViewAndroidPlatform final
		: public IVeloWebViewPlatform
		, public TSharedFromThis<FVeloWebViewAndroidPlatform>
	{
	public:
		FVeloWebViewAndroidPlatform() = default;
		virtual ~FVeloWebViewAndroidPlatform() override { Shutdown(); }

		virtual bool Initialize(const FVeloWebViewPlatformCreateParams& InParams,
			const FVeloWebViewPlatformCallbacks& InCallbacks) override
		{
			if (State == EVeloWebViewState::Destroyed || State == EVeloWebViewState::Ready || State == EVeloWebViewState::Loading)
			{
				return State == EVeloWebViewState::Ready || State == EVeloWebViewState::Loading;
			}

			Params = InParams;
			Callbacks = InCallbacks;
			State = EVeloWebViewState::Initializing;
			LastError.Reset();

			InstanceId = FVeloWebViewAndroidBridge::CreateInstance(
				0,
				Params.bEnableJavaScript,
				Params.bMediaAutoplay,
				Params.bMuted,
				Params.bTransparentBackground,
				Params.bAllowMixedContent,
				Params.UserAgent);
			if (InstanceId <= 0)
			{
				State = EVeloWebViewState::Failed;
				LastError = TEXT("Android WebView could not be created (Java manager unavailable).");
				return false;
			}

			FVeloWebViewAndroidCallbacks AndroidCallbacks;
			AndroidCallbacks.OnPageStarted = [WeakThis = TWeakPtr<FVeloWebViewAndroidPlatform>(AsShared())](const FString& URL)
			{
				if (const TSharedPtr<FVeloWebViewAndroidPlatform> Self = WeakThis.Pin())
				{
					Self->State = EVeloWebViewState::Loading;
					if (Self->Callbacks.OnPageStarted) Self->Callbacks.OnPageStarted(URL);
				}
			};
			AndroidCallbacks.OnPageLoaded = [WeakThis = TWeakPtr<FVeloWebViewAndroidPlatform>(AsShared())](const FString& URL)
			{
				if (const TSharedPtr<FVeloWebViewAndroidPlatform> Self = WeakThis.Pin())
				{
					Self->State = EVeloWebViewState::Ready;
					if (Self->Callbacks.OnPageLoaded) Self->Callbacks.OnPageLoaded(URL);
				}
			};
			AndroidCallbacks.OnPageError = [WeakThis = TWeakPtr<FVeloWebViewAndroidPlatform>(AsShared())](int32 Code, const FString& Description, const FString& URL)
			{
				if (const TSharedPtr<FVeloWebViewAndroidPlatform> Self = WeakThis.Pin())
				{
					Self->State = EVeloWebViewState::Failed;
					Self->LastError = Description;
					if (Self->Callbacks.OnPageError) Self->Callbacks.OnPageError(FVeloWebViewError(Code, Description, URL));
				}
			};
			AndroidCallbacks.OnURLChanged = [WeakThis = TWeakPtr<FVeloWebViewAndroidPlatform>(AsShared())](const FString& URL)
			{
				if (const TSharedPtr<FVeloWebViewAndroidPlatform> Self = WeakThis.Pin())
				{
					Self->CurrentURL = URL;
					if (Self->Callbacks.OnURLChanged) Self->Callbacks.OnURLChanged(URL);
				}
			};
			AndroidCallbacks.OnTitleChanged = [WeakThis = TWeakPtr<FVeloWebViewAndroidPlatform>(AsShared())](const FString& Title)
			{
				if (const TSharedPtr<FVeloWebViewAndroidPlatform> Self = WeakThis.Pin())
				{
					Self->CurrentTitle = Title;
					if (Self->Callbacks.OnTitleChanged) Self->Callbacks.OnTitleChanged(Title);
				}
			};
			AndroidCallbacks.OnJavaScriptMessage = [WeakThis = TWeakPtr<FVeloWebViewAndroidPlatform>(AsShared())](const FString& Message)
			{
				if (const TSharedPtr<FVeloWebViewAndroidPlatform> Self = WeakThis.Pin())
				{
					if (Self->Callbacks.OnJavaScriptMessage) Self->Callbacks.OnJavaScriptMessage(Message);
				}
			};
			AndroidCallbacks.OnConsoleMessage = [WeakThis = TWeakPtr<FVeloWebViewAndroidPlatform>(AsShared())](const FString& Message, const FString& Source, int32 Line)
			{
				if (const TSharedPtr<FVeloWebViewAndroidPlatform> Self = WeakThis.Pin())
				{
					if (Self->Callbacks.OnConsoleMessage) Self->Callbacks.OnConsoleMessage(Message, Source, Line);
				}
			};
			AndroidCallbacks.OnDownloadRequested = [WeakThis = TWeakPtr<FVeloWebViewAndroidPlatform>(AsShared())](const FString& URL, const FString& Filename, const FString& Mime)
			{
				if (const TSharedPtr<FVeloWebViewAndroidPlatform> Self = WeakThis.Pin())
				{
					if (Self->Callbacks.OnDownloadRequested) Self->Callbacks.OnDownloadRequested(URL, Filename, Mime);
				}
			};
			AndroidCallbacks.OnPermissionRequested = [WeakThis = TWeakPtr<FVeloWebViewAndroidPlatform>(AsShared())](int32 Token, const FString& Resources, const FString& Origin)
			{
				if (const TSharedPtr<FVeloWebViewAndroidPlatform> Self = WeakThis.Pin())
				{
					const EVeloWebViewPermission Permission = PermissionFromResources(Resources);
					Self->PendingPermissionTokens.Add(Permission, Token);
					if (Self->Callbacks.OnPermissionRequested) Self->Callbacks.OnPermissionRequested(Permission, Origin);
				}
			};
			AndroidCallbacks.OnFullscreenChanged = [WeakThis = TWeakPtr<FVeloWebViewAndroidPlatform>(AsShared())](bool bFullscreen)
			{
				if (const TSharedPtr<FVeloWebViewAndroidPlatform> Self = WeakThis.Pin())
				{
					if (Self->Callbacks.OnFullscreenChanged) Self->Callbacks.OnFullscreenChanged(bFullscreen);
				}
			};
			AndroidCallbacks.OnBeforeNavigate = [WeakThis = TWeakPtr<FVeloWebViewAndroidPlatform>(AsShared())](const FString& URL)
			{
				if (const TSharedPtr<FVeloWebViewAndroidPlatform> Self = WeakThis.Pin())
				{
					if (Self->Callbacks.OnBeforeNavigate) Self->Callbacks.OnBeforeNavigate(URL);
				}
			};
			AndroidCallbacks.OnNewWindowRequested = [WeakThis = TWeakPtr<FVeloWebViewAndroidPlatform>(AsShared())](const FString& URL, bool bIsUserInitiated)
			{
				if (const TSharedPtr<FVeloWebViewAndroidPlatform> Self = WeakThis.Pin())
				{
					if (Self->Callbacks.OnNewWindowRequested) Self->Callbacks.OnNewWindowRequested(URL, bIsUserInitiated);
				}
			};

			FVeloWebViewAndroidBridge::SetCallbacks(InstanceId, AndroidCallbacks);
			FVeloWebViewAndroidBridge::SetNewWindowPolicy(InstanceId, static_cast<uint8>(Params.NewWindowPolicy));
			State = EVeloWebViewState::Ready;
			if (!Params.InitialURL.IsEmpty())
			{
				CurrentURL = Params.InitialURL;
				State = EVeloWebViewState::Loading;
				FVeloWebViewAndroidBridge::LoadURL(InstanceId, Params.InitialURL);
			}
			return true;
		}

		virtual void Shutdown() override
		{
			if (State == EVeloWebViewState::Destroyed)
			{
				return;
			}
			if (InstanceId > 0)
			{
				FVeloWebViewAndroidBridge::ClearCallbacks(InstanceId);
				FVeloWebViewAndroidBridge::DestroyInstance(InstanceId);
				InstanceId = 0;
			}
			PendingPermissionTokens.Reset();
			State = EVeloWebViewState::Destroyed;
		}

		virtual void SetGeometry(const FIntRect& ScreenRect, float Scale) override
		{
			if (InstanceId <= 0) return;
			(void)Scale;
			// UE may render Slate at a reduced mobile resolution (for example
			// 1280x720 on a 1920x1080 SurfaceView). Android View bounds use the
			// physical SurfaceView size, so map both axes independently. Density
			// is unrelated to this render-resolution scale.
			const FPlatformRect LogicalScreen = FAndroidWindow::GetScreenRect();
			int32 SurfaceWidth = 0;
			int32 SurfaceHeight = 0;
			FAndroidWindow::CalculateSurfaceSize(SurfaceWidth, SurfaceHeight);
			const int32 LogicalWidth = LogicalScreen.Right - LogicalScreen.Left;
			const int32 LogicalHeight = LogicalScreen.Bottom - LogicalScreen.Top;
			if (LogicalWidth <= 0 || LogicalHeight <= 0 || SurfaceWidth <= 0 || SurfaceHeight <= 0) return;

			const float ScaleX = static_cast<float>(SurfaceWidth) / LogicalWidth;
			const float ScaleY = static_cast<float>(SurfaceHeight) / LogicalHeight;
			const FIntRect NativeRect(
				FMath::RoundToInt((ScreenRect.Min.X - LogicalScreen.Left) * ScaleX),
				FMath::RoundToInt((ScreenRect.Min.Y - LogicalScreen.Top) * ScaleY),
				FMath::RoundToInt((ScreenRect.Max.X - LogicalScreen.Left) * ScaleX),
				FMath::RoundToInt((ScreenRect.Max.Y - LogicalScreen.Top) * ScaleY));
			if (bHasNativeBounds && NativeRect == LastNativeBounds) return;
			bHasNativeBounds = true;
			LastNativeBounds = NativeRect;
			FVeloWebViewAndroidBridge::SetBounds(InstanceId,
				static_cast<float>(NativeRect.Min.X), static_cast<float>(NativeRect.Min.Y),
				static_cast<float>(FMath::Max(0, NativeRect.Width())),
				static_cast<float>(FMath::Max(0, NativeRect.Height())), false);
		}

		virtual void SetVisibility(bool bVisible) override { FVeloWebViewAndroidBridge::SetVisibility(InstanceId, bVisible); }
		virtual void SetFocus(bool bFocused) override { FVeloWebViewAndroidBridge::SetFocus(InstanceId, bFocused); }
		virtual void LoadURL(const FString& URL) override { CurrentURL = URL; State = EVeloWebViewState::Loading; FVeloWebViewAndroidBridge::LoadURL(InstanceId, URL); }
		virtual void LoadHTML(const FString& HTML, const FString& BaseURL) override { State = EVeloWebViewState::Loading; FVeloWebViewAndroidBridge::LoadHTML(InstanceId, HTML, BaseURL); }
		virtual void Reload() override { FVeloWebViewAndroidBridge::Reload(InstanceId); }
		virtual void Stop() override { FVeloWebViewAndroidBridge::Stop(InstanceId); }
		virtual void GoBack() override { FVeloWebViewAndroidBridge::GoBack(InstanceId); }
		virtual void GoForward() override { FVeloWebViewAndroidBridge::GoForward(InstanceId); }
		virtual bool CanGoBack() const override { return FVeloWebViewAndroidBridge::CanGoBack(InstanceId); }
		virtual bool CanGoForward() const override { return FVeloWebViewAndroidBridge::CanGoForward(InstanceId); }
		virtual void ExecuteJavaScript(const FString& Script) override { FVeloWebViewAndroidBridge::ExecuteJavaScript(InstanceId, Script); }
		virtual void SetJavaScriptEnabled(bool bEnabled) override { FVeloWebViewAndroidBridge::SetJavaScriptEnabled(InstanceId, bEnabled); }
		virtual void SetMediaAutoplay(bool bEnabled) override { FVeloWebViewAndroidBridge::SetMediaAutoplay(InstanceId, bEnabled); }
		virtual void SetMuted(bool bMuted) override { FVeloWebViewAndroidBridge::SetMuted(InstanceId, bMuted); }
		virtual void SetUserAgent(const FString& UserAgent) override { FVeloWebViewAndroidBridge::SetUserAgent(InstanceId, UserAgent); }
		virtual void SetBackgroundTransparent(bool bTransparent) override { FVeloWebViewAndroidBridge::SetBackgroundTransparent(InstanceId, bTransparent); }
		virtual void SetAllowMixedContent(bool bAllow) override { FVeloWebViewAndroidBridge::SetAllowMixedContent(InstanceId, bAllow); }
		virtual void SetNewWindowPolicy(EVeloWebViewNewWindowPolicy Policy) override { FVeloWebViewAndroidBridge::SetNewWindowPolicy(InstanceId, static_cast<uint8>(Policy)); }
		virtual FString GetURL() const override { return CurrentURL.IsEmpty() ? FVeloWebViewAndroidBridge::GetURL(InstanceId) : CurrentURL; }
		virtual FString GetTitle() const override { return CurrentTitle.IsEmpty() ? FVeloWebViewAndroidBridge::GetTitle(InstanceId) : CurrentTitle; }
		virtual EVeloWebViewState GetState() const override { return State; }
		virtual void ClearCache() override { FVeloWebViewAndroidBridge::ClearCache(InstanceId); }
		virtual void ClearCookies() override { FVeloWebViewAndroidBridge::ClearCookies(); }

		virtual void SetCookie(const FString& URL, const FString& Name, const FString& Value,
			const FString& Domain, const FString& Path, bool bSecure) override
		{
			FString Cookie = Name + TEXT("=") + Value;
			if (!Domain.IsEmpty()) Cookie += TEXT("; Domain=") + Domain;
			if (!Path.IsEmpty()) Cookie += TEXT("; Path=") + Path;
			if (bSecure) Cookie += TEXT("; Secure");
			FVeloWebViewAndroidBridge::SetCookie(URL, Cookie);
		}
		virtual FString GetCookie(const FString& URL) const override { return FVeloWebViewAndroidBridge::GetCookie(URL); }
		virtual void DeleteCookie(const FString& URL, const FString& Name) override { FVeloWebViewAndroidBridge::DeleteCookie(URL, Name); }

		virtual void GrantPermission(EVeloWebViewPermission Permission) override
		{
			if (const int32* Token = PendingPermissionTokens.Find(Permission)) FVeloWebViewAndroidBridge::GrantPermission(*Token);
		}
		virtual void DenyPermission(EVeloWebViewPermission Permission) override
		{
			if (const int32* Token = PendingPermissionTokens.Find(Permission)) FVeloWebViewAndroidBridge::DenyPermission(*Token);
		}
		virtual bool IsReady() const override { return State == EVeloWebViewState::Ready || State == EVeloWebViewState::Loading; }
		virtual FString GetLastError() const override { return LastError; }

	private:
		int32 InstanceId = 0;
		EVeloWebViewState State = EVeloWebViewState::Uninitialized;
		FString LastError;
		FString CurrentURL;
		FString CurrentTitle;
		FVeloWebViewPlatformCreateParams Params;
		FVeloWebViewPlatformCallbacks Callbacks;
		FIntRect LastNativeBounds;
		bool bHasNativeBounds = false;
		TMap<EVeloWebViewPermission, int32> PendingPermissionTokens;
	};

	class FVeloWebViewAndroidModule final : public IVeloWebViewPlatformModule
	{
	public:
		virtual TSharedPtr<IVeloWebViewPlatform> CreatePlatform(
			const FVeloWebViewPlatformCreateParams&, const FVeloWebViewPlatformCallbacks&) override
		{
			return MakeShared<FVeloWebViewAndroidPlatform>();
		}
	};
}

IMPLEMENT_MODULE(VeloWebViewAndroidPrivate::FVeloWebViewAndroidModule, VeloWebViewAndroid)

// Native overlay geometry is expressed in physical viewport pixels; the Java
// manager converts only when an explicit dp flag is requested.
