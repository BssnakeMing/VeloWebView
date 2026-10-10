#pragma once

#include "CoreMinimal.h"

/** Connection state for the optional out-of-process CEF BrowserHost. */
enum class EVeloCEFConnectionState : uint8
{
	Disconnected,
	Starting,
	Connected,
	Failed
};

/** Configuration used when starting a BrowserHost instance. */
struct VELOWEBVIEW_API FVeloCEFConnectionConfig
{
	/** Optional absolute path. The plugin's BrowserHost search paths are used when empty. */
	FString BrowserHostPath;
	FString InitialURL;
	FString InstanceId;
	FIntPoint InitialSize = FIntPoint(1280, 720);
	int32 FrameRate = 60;
	/** Until the D3D12 bridge is enabled, force CEF's CPU ring so Slate can display frames. */
	bool bDisableGPU = true;
	bool bAllowMediaCapture = false;
	bool bEnablePixelStreaming = false;
};

/**
 * Small, dependency-free client for the external BrowserHost process.
 *
 * The client deliberately does not include CEF headers.  BrowserHost owns all
 * CEF objects and the UE module talks to it through a versioned named-pipe
 * protocol.  This keeps libcef out of UnrealEditor/Game and leaves room for a
 * D3D12 texture bridge in a later change.
 */
class VELOWEBVIEW_API FVeloCEFConnection : public TSharedFromThis<FVeloCEFConnection>
{
public:
	FVeloCEFConnection();
	~FVeloCEFConnection();

	bool Start(const FVeloCEFConnectionConfig& Config);
	void Tick(float DeltaSeconds);
	void Stop();

	EVeloCEFConnectionState GetState() const;
	bool IsConnected() const;
	/** Maximum CPU frame size allocated by this BrowserHost instance. */
	FIntPoint GetFrameCapacity() const;
	/** Copies the newest CPU fallback frame from BrowserHost's shared mapping. */
	bool ReadLatestCpuFrame(TArray<uint8>& OutPixels, FIntPoint& OutSize, uint64& OutSequence);
	/** Marks a frame as consumed after its RHI texture upload was accepted. */
	void MarkCpuFrameConsumed(uint64 Sequence);
	FString GetLastError() const;
	FString GetURL() const;
	FString GetTitle() const;

	bool LoadURL(const FString& URL);
	bool LoadHTML(const FString& HTML, const FString& BaseURL);
	bool Reload();
	bool StopLoad();
	bool GoBack();
	bool GoForward();
	bool ExecuteJavaScript(const FString& Script);
	bool Resize(const FIntPoint& Size, float Scale);
	bool SetVisibility(bool bVisible);
	bool SetFocus(bool bFocused);
	bool SetJavaScriptEnabled(bool bEnabled);
	bool SetMuted(bool bMuted);
	bool SendMouseMove(int32 X, int32 Y, uint32 Modifiers, bool bLeave = false);
	bool SendMouseButton(int32 X, int32 Y, uint32 Modifiers, uint8 Button, bool bMouseUp, uint8 ClickCount = 1);
	bool SendMouseWheel(int32 X, int32 Y, int32 DeltaX, int32 DeltaY, uint32 Modifiers);
	bool SendKey(uint8 Type, uint32 WindowsKeyCode, uint32 NativeKeyCode, uint32 Character,
		uint32 UnmodifiedCharacter, uint32 Modifiers, bool bSystemKey = false);
	bool ClearCookies();
	bool SetCookie(const FString& URL, const FString& Name, const FString& Value,
		const FString& Domain, const FString& Path, bool bSecure);
	bool DeleteCookie(const FString& URL, const FString& Name);

private:
	class FImpl;
	TUniquePtr<FImpl> Impl;
};
