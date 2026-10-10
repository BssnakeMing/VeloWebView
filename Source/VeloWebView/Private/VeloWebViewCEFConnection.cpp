#include "VeloWebViewCEFConnection.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "BrowserProtocol/BrowserProtocol.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace VeloCEFPrivate
{
	using namespace VeloWebView::Protocol;

	static bool HasHostRuntime(const FString& HostPath)
	{
		const FString Directory = FPaths::GetPath(HostPath);
		return FPaths::FileExists(HostPath) &&
			FPaths::FileExists(FPaths::Combine(Directory, TEXT("libcef.dll"))) &&
			FPaths::FileExists(FPaths::Combine(Directory, TEXT("chrome_elf.dll"))) &&
			FPaths::FileExists(FPaths::Combine(Directory, TEXT("icudtl.dat"))) &&
			FPaths::FileExists(FPaths::Combine(Directory, TEXT("resources.pak")));
	}

	static FString MakeDefaultHostPath()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("VeloWebView"));
		if (!Plugin.IsValid()) return FString();
		const FString Base = Plugin->GetBaseDir();
		const TArray<FString> Candidates =
		{
			// Packaged builds stage RuntimeDependencies in the game's binary
			// directory rather than beside the plugin source.
			FPaths::Combine(FPlatformProcess::BaseDir(), TEXT("VeloWebViewBrowserHost.exe")),
			FPaths::Combine(FPaths::ProjectDir(), TEXT("Binaries/Win64/VeloWebViewBrowserHost.exe")),
			// CMake's documented build command produces a complete host/runtime
			// directory here. Never select an isolated EXE ahead of that bundle.
			FPaths::Combine(Base, TEXT("BrowserHost/build/Release/VeloWebViewBrowserHost.exe")),
			FPaths::Combine(Base, TEXT("BrowserHost/VeloWebViewBrowserHost.exe")),
			FPaths::Combine(Base, TEXT("BrowserHost/BrowserHost.exe")),
			FPaths::Combine(Base, TEXT("Binaries/Win64/VeloWebViewBrowserHost.exe")),
			FPaths::Combine(Base, TEXT("Binaries/Win64/BrowserHost.exe"))
		};
		for (const FString& Candidate : Candidates)
		{
			if (HasHostRuntime(Candidate)) return Candidate;
		}
		return Candidates.Num() > 0 ? Candidates[0] : FString();
	}

	static FString EscapeArgument(const FString& Value)
	{
		// CommandLineToArgvW only treats backslashes specially before a quote.
		// Doubling every backslash corrupts ordinary Windows directory paths.
		FString Escaped = TEXT("\"");
		int32 Backslashes = 0;
		for (const TCHAR Character : Value)
		{
			if (Character == TEXT('\\'))
			{
				++Backslashes;
				continue;
			}
			Escaped += FString::ChrN(Backslashes * (Character == TEXT('"') ? 2 : 1), TEXT('\\'));
			Backslashes = 0;
			if (Character == TEXT('"')) Escaped += TEXT('\\');
			Escaped += Character;
		}
		Escaped += FString::ChrN(Backslashes * 2, TEXT('\\'));
		return Escaped + TEXT("\"");
	}
}

class FVeloCEFConnection::FImpl
{
public:
	EVeloCEFConnectionState State = EVeloCEFConnectionState::Disconnected;
	FString LastError;
	FString LastURL;
	FString LastTitle;
	FString PipeName;
	FString InstanceId;
	uint32 NumericInstanceId = 0;
	FProcHandle Process;
	uint64 Sequence = 0;
	double NextConnectTime = 0.0;
	FIntPoint FrameCapacity = FIntPoint::ZeroValue;
#if PLATFORM_WINDOWS
	HANDLE Pipe = INVALID_HANDLE_VALUE;
	HANDLE FrameMapping = nullptr;
	uint8* FrameView = nullptr;
	uint64 LastFrameSequence = 0;
#endif

	void SetError(const FString& Error)
	{
		LastError = Error;
		State = EVeloCEFConnectionState::Failed;
	}

#if PLATFORM_WINDOWS
	bool Send(VeloWebView::Protocol::EMessageType Command, const FString& Payload = FString())
	{
		FTCHARToUTF8 UTF8(*Payload);
		return SendRaw(Command, UTF8.Get(), static_cast<uint32>(UTF8.Length()));
	}

	bool SendRaw(VeloWebView::Protocol::EMessageType Command, const void* Payload, uint32 PayloadSize)
	{
		if (Pipe == INVALID_HANDLE_VALUE || PayloadSize > VeloWebView::Protocol::kMaxMessageBytes - sizeof(VeloWebView::Protocol::FMessageHeader))
		{
			return false;
		}
		VeloWebView::Protocol::FMessageHeader Header;
		Header.Type = static_cast<uint16>(Command);
		Header.InstanceId = NumericInstanceId;
		Header.Sequence = ++Sequence;
		Header.Size = sizeof(Header) + PayloadSize;
		DWORD Written = 0;
		if (!::WriteFile(Pipe, &Header, sizeof(Header), &Written, nullptr) || Written != sizeof(Header))
		{
			SetError(TEXT("CEF BrowserHost pipe write failed."));
			return false;
		}
		if (PayloadSize > 0 && (!::WriteFile(Pipe, Payload, PayloadSize, &Written, nullptr) || Written != PayloadSize))
		{
			SetError(TEXT("CEF BrowserHost payload write failed."));
			return false;
		}
		return true;
	}

	bool TryConnect()
	{
		if (State != EVeloCEFConnectionState::Starting || PipeName.IsEmpty()) return false;
		const FString PipePath = FString::Printf(TEXT("\\\\.\\pipe\\%s"), *PipeName);
		HANDLE Candidate = ::CreateFileW(*PipePath, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (Candidate == INVALID_HANDLE_VALUE)
		{
			return false;
		}
		Pipe = Candidate;
		State = EVeloCEFConnectionState::Connected;
		if (!Send(VeloWebView::Protocol::EMessageType::Hello, InstanceId))
		{
			::CloseHandle(Pipe);
			Pipe = INVALID_HANDLE_VALUE;
			State = EVeloCEFConnectionState::Failed;
			return false;
		}
		return true;
	}

	void CloseFrameMapping()
	{
		if (FrameView)
		{
			::UnmapViewOfFile(FrameView);
			FrameView = nullptr;
		}
		if (FrameMapping)
		{
			::CloseHandle(FrameMapping);
			FrameMapping = nullptr;
		}
		LastFrameSequence = 0;
	}

	void TryOpenFrameMapping()
	{
		if (FrameView || NumericInstanceId == 0) return;
		const FString Name = FString::Printf(TEXT("Local\\VeloWebView_%u_frames"), NumericInstanceId);
		FrameMapping = ::OpenFileMappingW(FILE_MAP_READ, false, *Name);
		if (FrameMapping)
		{
			FrameView = static_cast<uint8*>(::MapViewOfFile(FrameMapping, FILE_MAP_READ, 0, 0, 0));
			if (!FrameView) CloseFrameMapping();
		}
	}

	bool ReadLatestCpuFrame(TArray<uint8>& OutPixels, FIntPoint& OutSize, uint64& OutSequence)
	{
		TryOpenFrameMapping();
		if (!FrameView) return false;
		const auto* Header = reinterpret_cast<const VeloWebView::Protocol::FFrameRingHeader*>(FrameView);
		if (!VeloWebView::Protocol::IsValid(*Header, sizeof(VeloWebView::Protocol::FFrameRingHeader)) ||
			Header->InstanceId != NumericInstanceId || Header->SlotCount == 0 || Header->LatestSlot >= Header->SlotCount)
		{
			CloseFrameMapping();
			return false;
		}
		const uint64 FrameSequence = Header->LatestSequence;
		if (FrameSequence == 0 || FrameSequence == LastFrameSequence) return false;
		const uint8* SlotAddress = FrameView + Header->HeaderSize +
			static_cast<size_t>(Header->LatestSlot) * Header->SlotStride;
		const auto* Slot = reinterpret_cast<const VeloWebView::Protocol::FFrameSlot*>(SlotAddress);
		const uint64 SlotSequence = Slot->Sequence;
		if (SlotSequence != FrameSequence || (Slot->Flags & VeloWebView::Protocol::FrameCpuFallback) == 0 ||
			Slot->Format != 1 || Slot->Width == 0 || Slot->Height == 0)
		{
			return false;
		}
		const size_t ExpectedBytes = static_cast<size_t>(Slot->Width) * Slot->Height * 4;
		const size_t Bytes = FMath::Min<size_t>(Slot->PixelBytes, ExpectedBytes);
		if (Bytes == 0) return false;
		OutPixels.SetNumUninitialized(static_cast<int32>(Bytes));
		FMemory::Memcpy(OutPixels.GetData(), SlotAddress + sizeof(VeloWebView::Protocol::FFrameSlot), Bytes);
		if (Slot->Sequence != FrameSequence) return false;
		OutSize = FIntPoint(static_cast<int32>(Slot->Width), static_cast<int32>(Slot->Height));
		OutSequence = FrameSequence;
		return true;
	}
#endif
};

FVeloCEFConnection::FVeloCEFConnection()
	: Impl(MakeUnique<FImpl>())
{
}

FVeloCEFConnection::~FVeloCEFConnection()
{
	Stop();
}

bool FVeloCEFConnection::Start(const FVeloCEFConnectionConfig& Config)
{
	Stop();
#if !PLATFORM_WINDOWS
	Impl->SetError(TEXT("The external CEF BrowserHost is currently supported on Win64 only."));
	return false;
#else
	const FString HostPath = Config.BrowserHostPath.IsEmpty() ? VeloCEFPrivate::MakeDefaultHostPath() : Config.BrowserHostPath;
	if (HostPath.IsEmpty() || !FPaths::FileExists(HostPath))
	{
		Impl->SetError(FString::Printf(TEXT("CEF BrowserHost executable was not found: %s"), *HostPath));
		return false;
	}
	if (!VeloCEFPrivate::HasHostRuntime(HostPath))
	{
		Impl->SetError(FString::Printf(TEXT("CEF BrowserHost runtime is incomplete beside %s. Stage libcef.dll, chrome_elf.dll, icudtl.dat and resources.pak with the host."), *HostPath));
		return false;
	}

	Impl->InstanceId = Config.InstanceId.IsEmpty()
		? FGuid::NewGuid().ToString(EGuidFormats::Digits)
		: Config.InstanceId;
	Impl->NumericInstanceId = FCString::Strtoui64(*Impl->InstanceId, nullptr, 10) & 0x7fffffff;
	if (Impl->NumericInstanceId == 0)
	{
		Impl->NumericInstanceId = GetTypeHash(Impl->InstanceId) & 0x7fffffff;
	}
	Impl->PipeName = FString::Printf(TEXT("VeloWebView_CEF_%u_%s"), FPlatformProcess::GetCurrentProcessId(), *Impl->InstanceId);
	// CEF 120+ enforces a process singleton per root_cache_path, including
	// incognito browsers. Each out-of-process widget needs its own root or a
	// second host silently relaunches the first and exits without an IPC pipe.
	const FString CachePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("VeloWebView/CEF"), Impl->PipeName));
	if (!IFileManager::Get().MakeDirectory(*CachePath, true))
	{
		Impl->SetError(FString::Printf(TEXT("Unable to create CEF BrowserHost data directory: %s"), *CachePath));
		return false;
	}
	const FString Args = FString::Printf(TEXT("--pipe=%s --instance-id=%s --cache-path=%s --width=%d --height=%d --fps=%d%s%s%s%s"),
		*VeloCEFPrivate::EscapeArgument(Impl->PipeName),
		*FString::Printf(TEXT("%u"), Impl->NumericInstanceId),
		*VeloCEFPrivate::EscapeArgument(CachePath),
		Config.InitialSize.X, Config.InitialSize.Y, Config.FrameRate,
		Config.InitialURL.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" --url=%s"), *VeloCEFPrivate::EscapeArgument(Config.InitialURL)),
		Config.bDisableGPU ? TEXT(" --disable-gpu") : TEXT(""),
		Config.bAllowMediaCapture ? TEXT(" --allow-media-capture") : TEXT(""),
		Config.bEnablePixelStreaming ? TEXT(" --enable-pixel-streaming") : TEXT(""));
	Impl->Process = FPlatformProcess::CreateProc(*HostPath, *Args, true, false, false, nullptr, 0,
		*FPaths::GetPath(HostPath), nullptr, nullptr);
	if (!Impl->Process.IsValid())
	{
		Impl->SetError(FString::Printf(TEXT("Unable to launch CEF BrowserHost: %s"), *HostPath));
		return false;
	}
	Impl->LastError.Reset();
	Impl->LastURL = Config.InitialURL;
	Impl->FrameCapacity = Config.InitialSize;
	Impl->State = EVeloCEFConnectionState::Starting;
	Impl->NextConnectTime = FPlatformTime::Seconds();
	return true;
#endif
}

void FVeloCEFConnection::Tick(float DeltaSeconds)
{
	(void)DeltaSeconds;
#if PLATFORM_WINDOWS
	if (Impl->State == EVeloCEFConnectionState::Starting && FPlatformTime::Seconds() >= Impl->NextConnectTime)
	{
		Impl->NextConnectTime = FPlatformTime::Seconds() + 0.1;
		Impl->TryConnect();
	}
	if ((Impl->State == EVeloCEFConnectionState::Starting || Impl->State == EVeloCEFConnectionState::Connected) &&
		Impl->Process.IsValid() && !FPlatformProcess::IsProcRunning(Impl->Process))
	{
		Impl->SetError(Impl->State == EVeloCEFConnectionState::Connected
			? TEXT("CEF BrowserHost exited unexpectedly.")
			: TEXT("CEF BrowserHost exited before opening its IPC pipe."));
		FPlatformProcess::CloseProc(Impl->Process);
		Impl->Process.Reset();
	}
#endif
}

void FVeloCEFConnection::Stop()
{
#if PLATFORM_WINDOWS
	if (Impl->Pipe != INVALID_HANDLE_VALUE)
	{
		Impl->Send(VeloWebView::Protocol::EMessageType::Shutdown);
		::FlushFileBuffers(Impl->Pipe);
		::CloseHandle(Impl->Pipe);
		Impl->Pipe = INVALID_HANDLE_VALUE;
	}
	Impl->CloseFrameMapping();
	if (Impl->Process.IsValid())
	{
		if (FPlatformProcess::IsProcRunning(Impl->Process)) FPlatformProcess::TerminateProc(Impl->Process, true);
		FPlatformProcess::CloseProc(Impl->Process);
		Impl->Process.Reset();
	}
#endif
	Impl->State = EVeloCEFConnectionState::Disconnected;
}

EVeloCEFConnectionState FVeloCEFConnection::GetState() const { return Impl->State; }
bool FVeloCEFConnection::IsConnected() const { return Impl->State == EVeloCEFConnectionState::Connected; }
FIntPoint FVeloCEFConnection::GetFrameCapacity() const { return Impl->FrameCapacity; }
bool FVeloCEFConnection::ReadLatestCpuFrame(TArray<uint8>& OutPixels, FIntPoint& OutSize, uint64& OutSequence)
{
#if PLATFORM_WINDOWS
	return Impl->ReadLatestCpuFrame(OutPixels, OutSize, OutSequence);
#else
	return false;
#endif
}
void FVeloCEFConnection::MarkCpuFrameConsumed(uint64 Sequence)
{
#if PLATFORM_WINDOWS
	if (Sequence > Impl->LastFrameSequence)
	{
		Impl->LastFrameSequence = Sequence;
	}
#else
	(void)Sequence;
#endif
}
FString FVeloCEFConnection::GetLastError() const { return Impl->LastError; }
FString FVeloCEFConnection::GetURL() const { return Impl->LastURL; }
FString FVeloCEFConnection::GetTitle() const { return Impl->LastTitle; }

#if PLATFORM_WINDOWS
#define VELO_CEF_SEND(Command, Payload) return Impl->Send(VeloWebView::Protocol::EMessageType::Command, Payload)
#else
#define VELO_CEF_SEND(Command, Payload) return false
#endif

bool FVeloCEFConnection::LoadURL(const FString& URL) { Impl->LastURL = URL; VELO_CEF_SEND(LoadUrl, URL); }
bool FVeloCEFConnection::LoadHTML(const FString& HTML, const FString& BaseURL)
{
	const FString Payload = BaseURL + TEXT("\n") + HTML;
	VELO_CEF_SEND(LoadHtml, Payload);
}
bool FVeloCEFConnection::Reload() { VELO_CEF_SEND(Reload, FString()); }
bool FVeloCEFConnection::StopLoad() { VELO_CEF_SEND(Stop, FString()); }
bool FVeloCEFConnection::GoBack() { VELO_CEF_SEND(GoBack, FString()); }
bool FVeloCEFConnection::GoForward() { VELO_CEF_SEND(GoForward, FString()); }
bool FVeloCEFConnection::ExecuteJavaScript(const FString& Script) { VELO_CEF_SEND(ExecuteJavaScript, Script); }
bool FVeloCEFConnection::Resize(const FIntPoint& Size, float Scale)
{
	const FString Payload = FString::Printf(TEXT("%d,%d,%.6f"), Size.X, Size.Y, Scale);
	VELO_CEF_SEND(Resize, Payload);
}
bool FVeloCEFConnection::SetVisibility(bool bVisible) { VELO_CEF_SEND(SetVisibility, bVisible ? TEXT("1") : TEXT("0")); }
bool FVeloCEFConnection::SetFocus(bool bFocused) { VELO_CEF_SEND(SetFocus, bFocused ? TEXT("1") : TEXT("0")); }
bool FVeloCEFConnection::SetJavaScriptEnabled(bool bEnabled) { VELO_CEF_SEND(SetJavaScriptEnabled, bEnabled ? TEXT("1") : TEXT("0")); }
bool FVeloCEFConnection::SetMuted(bool bMuted) { VELO_CEF_SEND(SetMuted, bMuted ? TEXT("1") : TEXT("0")); }
bool FVeloCEFConnection::SendMouseMove(int32 X, int32 Y, uint32 Modifiers, bool bLeave)
{
#if PLATFORM_WINDOWS
	VeloWebView::Protocol::FMouseMovePayload Payload;
	Payload.X = X; Payload.Y = Y; Payload.Modifiers = Modifiers; Payload.Leave = bLeave ? 1 : 0;
	return Impl->SendRaw(VeloWebView::Protocol::EMessageType::MouseMove, &Payload, sizeof(Payload));
#else
	return false;
#endif
}
bool FVeloCEFConnection::SendMouseButton(int32 X, int32 Y, uint32 Modifiers, uint8 Button, bool bMouseUp, uint8 ClickCount)
{
#if PLATFORM_WINDOWS
	VeloWebView::Protocol::FMouseButtonPayload Payload;
	Payload.X = X; Payload.Y = Y; Payload.Modifiers = Modifiers; Payload.Button = Button;
	Payload.MouseUp = bMouseUp ? 1 : 0; Payload.ClickCount = FMath::Max<uint8>(1, ClickCount);
	return Impl->SendRaw(VeloWebView::Protocol::EMessageType::MouseButton, &Payload, sizeof(Payload));
#else
	return false;
#endif
}
bool FVeloCEFConnection::SendMouseWheel(int32 X, int32 Y, int32 DeltaX, int32 DeltaY, uint32 Modifiers)
{
#if PLATFORM_WINDOWS
	VeloWebView::Protocol::FMouseWheelPayload Payload;
	Payload.X = X; Payload.Y = Y; Payload.DeltaX = DeltaX; Payload.DeltaY = DeltaY; Payload.Modifiers = Modifiers;
	return Impl->SendRaw(VeloWebView::Protocol::EMessageType::MouseWheel, &Payload, sizeof(Payload));
#else
	return false;
#endif
}
bool FVeloCEFConnection::SendKey(uint8 Type, uint32 WindowsKeyCode, uint32 NativeKeyCode, uint32 Character,
	uint32 UnmodifiedCharacter, uint32 Modifiers, bool bSystemKey)
{
#if PLATFORM_WINDOWS
	VeloWebView::Protocol::FKeyPayload Payload;
	Payload.Type = Type; Payload.IsSystemKey = bSystemKey ? 1 : 0;
	Payload.WindowsKeyCode = WindowsKeyCode; Payload.NativeKeyCode = NativeKeyCode;
	Payload.Character = Character; Payload.UnmodifiedCharacter = UnmodifiedCharacter; Payload.Modifiers = Modifiers;
	if (Type != VeloWebView::Protocol::KeyEventChar && NativeKeyCode == 0)
	{
		// CEF expects the Windows message lParam (scan code/extended/repeat/up
		// bits), while Slate exposes a VK code. Preserve both so DOM event.code,
		// shortcuts and navigation keys are interpreted correctly.
		const uint32 ScanCode = ::MapVirtualKeyExW(WindowsKeyCode, MAPVK_VK_TO_VSC_EX, ::GetKeyboardLayout(0));
		Payload.NativeKeyCode = 1u | ((ScanCode & 0xffu) << 16);
		if (ScanCode & 0xff00u) Payload.NativeKeyCode |= 1u << 24;
		if (bSystemKey) Payload.NativeKeyCode |= 1u << 29;
		if (Modifiers & (1u << 13)) Payload.NativeKeyCode |= 1u << 30;
		if (Type == VeloWebView::Protocol::KeyEventKeyUp) Payload.NativeKeyCode |= (1u << 30) | (1u << 31);
	}
	if (Type == VeloWebView::Protocol::KeyEventChar && (Modifiers & ((1u << 2) | (1u << 3))) == ((1u << 2) | (1u << 3)))
	{
		const SHORT Scan = ::VkKeyScanExW(static_cast<WCHAR>(Character), ::GetKeyboardLayout(0));
		if (Scan != -1 && ((Scan >> 8) & 0xff) == 6)
		{
			Payload.Modifiers &= ~((1u << 2) | (1u << 3));
			Payload.Modifiers |= 1u << 12; // AltGr
		}
	}
	return Impl->SendRaw(VeloWebView::Protocol::EMessageType::Key, &Payload, sizeof(Payload));
#else
	return false;
#endif
}
bool FVeloCEFConnection::ClearCookies() { VELO_CEF_SEND(ClearCookies, FString()); }
bool FVeloCEFConnection::SetCookie(const FString& URL, const FString& Name, const FString& Value,
	const FString& Domain, const FString& Path, bool bSecure)
{
	const FString Payload = FString::Printf(TEXT("%s\n%s\n%s\n%s\n%s\n%d"), *URL, *Name, *Value, *Domain, *Path, bSecure ? 1 : 0);
	VELO_CEF_SEND(SetCookie, Payload);
}
bool FVeloCEFConnection::DeleteCookie(const FString& URL, const FString& Name)
{
	VELO_CEF_SEND(DeleteCookie, URL + TEXT("\n") + Name);
}

#undef VELO_CEF_SEND
