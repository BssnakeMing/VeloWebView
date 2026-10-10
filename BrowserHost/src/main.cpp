// Minimal standalone CEF BrowserHost for the VeloWebView protocol.
//
// This executable deliberately owns CEF.  The Unreal module must communicate
// with it over the versioned protocol in Shared/BrowserProtocol and must never
// load libcef.dll into UnrealEditor.exe or the game process.  The first host
// milestone reports frame metadata and validates both CEF OSR callbacks; the
// shared-texture copy/IPC transport is the next layer behind this seam.

#include "BrowserProtocol/BrowserProtocol.h"

#include "include/cef_app.h"
#include "include/cef_client.h"
#include "include/cef_display_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_render_handler.h"
#include "include/cef_parser.h"
#include "include/wrapper/cef_helpers.h"

#include <windows.h>
#include <shellapi.h>
#ifdef LoadString
#undef LoadString
#endif

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace VeloWebView::BrowserHost
{
using Protocol::EMessageType;
using Protocol::EFrameFlags;

struct FOptions
{
  std::wstring Url = L"about:blank";
  std::wstring CachePath;
  std::wstring LogFile;
  std::wstring PipeName;
  int Width = 1280;
  int Height = 720;
  int FrameRate = 60;
  std::uint32_t InstanceId = 0;
  bool DisableGpu = false;
  bool EnableDevTools = false;
  bool AllowMediaCapture = false;
  bool EnablePixelStreaming = false;
  bool ExternalBeginFrame = false;
};

std::wstring GetArgument(int Argc, wchar_t** Argv, const std::wstring& Name)
{
  for (int Index = 1; Index < Argc; ++Index)
  {
    const std::wstring Current = Argv[Index] ? Argv[Index] : L"";
    const std::wstring Prefix = Name + L"=";
    if (Current.rfind(Prefix, 0) == 0)
    {
      return Current.substr(Prefix.size());
    }
  }
  return {};
}

bool HasFlag(int Argc, wchar_t** Argv, const std::wstring& Name)
{
  for (int Index = 1; Index < Argc; ++Index)
  {
    if (Argv[Index] && Name == Argv[Index])
    {
      return true;
    }
  }
  return false;
}

std::wstring GetSwitchValue(const CefString& Value)
{
  return Value.ToWString();
}

int ParseInt(const std::wstring& Value, int Fallback, int Minimum, int Maximum)
{
  if (Value.empty())
  {
    return Fallback;
  }
  wchar_t* End = nullptr;
  const long Parsed = std::wcstol(Value.c_str(), &End, 10);
  if (End == Value.c_str() || *End != L'\0')
  {
    return Fallback;
  }
  return static_cast<int>(Parsed < Minimum ? Minimum : Parsed > Maximum ? Maximum : Parsed);
}

FOptions ParseOptions(int Argc, wchar_t** Argv)
{
  FOptions Options;
  if (const std::wstring Value = GetArgument(Argc, Argv, L"--url"); !Value.empty())
  {
    Options.Url = Value;
  }
  Options.CachePath = GetArgument(Argc, Argv, L"--cache-path");
  Options.LogFile = GetArgument(Argc, Argv, L"--log-file");
  Options.PipeName = GetArgument(Argc, Argv, L"--pipe");
  Options.Width = ParseInt(GetArgument(Argc, Argv, L"--width"), Options.Width, 1, 16384);
  Options.Height = ParseInt(GetArgument(Argc, Argv, L"--height"), Options.Height, 1, 16384);
  Options.FrameRate = ParseInt(GetArgument(Argc, Argv, L"--fps"), Options.FrameRate, 1, 120);
  Options.InstanceId = static_cast<std::uint32_t>(ParseInt(
      GetArgument(Argc, Argv, L"--instance-id"), 0, 0, 0x7fffffff));
  Options.DisableGpu = HasFlag(Argc, Argv, L"--disable-gpu");
  Options.EnableDevTools = HasFlag(Argc, Argv, L"--enable-devtools");
  Options.AllowMediaCapture = HasFlag(Argc, Argv, L"--allow-media-capture");
  Options.EnablePixelStreaming = HasFlag(Argc, Argv, L"--enable-pixel-streaming");
  Options.ExternalBeginFrame = HasFlag(Argc, Argv, L"--external-begin-frame");
  return Options;
}

std::uint64_t NowMicros()
{
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

// CPU compatibility ring.  GPU frames currently publish only metadata; when
// --disable-gpu is used this ring carries BGRA pixels without ever putting a
// large buffer on the control pipe.  The layout is shared with the UE module.
class FFrameRing
{
public:
  ~FFrameRing() { Close(); }

  bool Create(std::uint32_t InInstanceId, int InWidth, int InHeight)
  {
    Close();
    Width = InWidth < 1 ? 1u : static_cast<std::uint32_t>(InWidth);
    Height = InHeight < 1 ? 1u : static_cast<std::uint32_t>(InHeight);
    SlotStride = sizeof(Protocol::FFrameSlot) +
                 static_cast<std::size_t>(Width) * Height * 4u;
    Capacity = sizeof(Protocol::FFrameRingHeader) +
               SlotStride * Protocol::kFrameSlotCount;
    MappingName = L"Local\\VeloWebView_" + std::to_wstring(InInstanceId) + L"_frames";
    Mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        static_cast<DWORD>((Capacity >> 32) & 0xffffffffu),
        static_cast<DWORD>(Capacity & 0xffffffffu), MappingName.c_str());
    if (!Mapping)
    {
      return false;
    }
    View = static_cast<std::uint8_t*>(::MapViewOfFile(Mapping, FILE_MAP_ALL_ACCESS, 0, 0, Capacity));
    if (!View)
    {
      Close();
      return false;
    }
    std::memset(View, 0, Capacity);
    auto* Header = reinterpret_cast<Protocol::FFrameRingHeader*>(View);
    Header->Magic = Protocol::kMagic;
    Header->Version = Protocol::kVersion;
    Header->HeaderSize = sizeof(Protocol::FFrameRingHeader);
    Header->StructSize = sizeof(Protocol::FFrameRingHeader);
    Header->InstanceId = InInstanceId;
    Header->Width = Width;
    Header->Height = Height;
    Header->SlotCount = Protocol::kFrameSlotCount;
    Header->SlotStride = static_cast<std::uint32_t>(SlotStride);
    return true;
  }

  void Close()
  {
    if (View)
    {
      ::UnmapViewOfFile(View);
      View = nullptr;
    }
    if (Mapping)
    {
      ::CloseHandle(Mapping);
      Mapping = nullptr;
    }
  }

  std::uint32_t Publish(Protocol::FFrameSlot Frame, const void* Pixels)
  {
    if (!View)
    {
      return 0;
    }
    const std::uint32_t SlotIndex = static_cast<std::uint32_t>((Frame.Sequence - 1) % Protocol::kFrameSlotCount);
    auto* Slot = reinterpret_cast<Protocol::FFrameSlot*>(
        View + sizeof(Protocol::FFrameRingHeader) + SlotIndex * SlotStride);
    auto* PixelStorage = reinterpret_cast<std::uint8_t*>(Slot) + sizeof(Protocol::FFrameSlot);
    Slot->Sequence = 0;
    if (Pixels && Frame.PixelBytes > 0)
    {
      const std::size_t Bytes = std::min<std::size_t>(Frame.PixelBytes, SlotStride - sizeof(Protocol::FFrameSlot));
      std::memcpy(PixelStorage, Pixels, Bytes);
      Frame.PixelBytes = static_cast<std::uint32_t>(Bytes);
    }
    Frame.TextureSlot = SlotIndex;
    *Slot = Frame;
    ::MemoryBarrier();
    ::InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&Slot->Sequence), static_cast<LONG64>(Frame.Sequence));
    auto* Header = reinterpret_cast<Protocol::FFrameRingHeader*>(View);
    Header->LatestSlot = SlotIndex;
    ::InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&Header->LatestSequence), static_cast<LONG64>(Frame.Sequence));
    return SlotIndex;
  }

  const std::wstring& GetName() const { return MappingName; }
  std::uint32_t GetWidth() const { return Width; }
  std::uint32_t GetHeight() const { return Height; }

private:
  HANDLE Mapping = nullptr;
  std::uint8_t* View = nullptr;
  std::size_t Capacity = 0;
  std::size_t SlotStride = 0;
  std::uint32_t Width = 0;
  std::uint32_t Height = 0;
  std::wstring MappingName;
};

class FRenderHandler final : public CefRenderHandler
{
public:
  FRenderHandler(int InWidth, int InHeight, std::uint32_t InInstanceId,
                 std::shared_ptr<FFrameRing> InRing)
      : Width(InWidth), Height(InHeight), InstanceId(InInstanceId), Ring(std::move(InRing))
  {
  }

  bool IsGpuFrame() const { return bReceivedGpuFrame.load(std::memory_order_acquire); }

  void GetViewRect(CefRefPtr<CefBrowser>, CefRect& Rect) override
  {
    Rect = CefRect(0, 0, Width.load(std::memory_order_relaxed), Height.load(std::memory_order_relaxed));
  }

  void OnPaint(CefRefPtr<CefBrowser>, PaintElementType Type,
               const RectList& DirtyRects, const void* Buffer, int InWidth, int InHeight) override
  {
    // CPU pixels are intentionally not sent through the control pipe.  This is
    // only a compatibility/fallback signal until the shared-memory ring is
    // connected.  The production path must remain GPU -> GPU.
    if (Type != PET_VIEW)
    {
      return;
    }
    Protocol::FFrameSlot Frame;
    Frame.Sequence = ++Sequence;
    Frame.FrameId = Frame.Sequence;
    Frame.TimestampUs = NowMicros();
    Frame.Width = static_cast<std::uint32_t>(InWidth);
    Frame.Height = static_cast<std::uint32_t>(InHeight);
    Frame.Format = 1; // BGRA8 premultiplied
    Frame.Flags = Protocol::FrameCpuFallback |
        (DirtyRects.empty() ? 0u : Protocol::FrameHasDirtyRect);
    Frame.PixelBytes = static_cast<std::uint32_t>(InWidth * InHeight * 4ull);
    bReceivedGpuFrame.store(false, std::memory_order_release);
    PublishFrame(Frame, Buffer);
  }

#if defined(VELO_CEF_LEGACY_ACCELERATED_PAINT)
  void OnAcceleratedPaint(CefRefPtr<CefBrowser>, PaintElementType Type,
                          const RectList& DirtyRects, void* SharedHandle) override
  {
    PublishAccelerated(Type, DirtyRects, reinterpret_cast<std::uintptr_t>(SharedHandle));
  }
#else
  void OnAcceleratedPaint(CefRefPtr<CefBrowser>, PaintElementType Type,
                          const RectList& DirtyRects,
                          const CefAcceleratedPaintInfo& Info) override
  {
    PublishAccelerated(Type, DirtyRects,
                       reinterpret_cast<std::uintptr_t>(Info.shared_texture_handle));
  }
#endif

  void SetSize(int InWidth, int InHeight)
  {
    // The first transport milestone has a fixed-size mapping. Keep CEF's
    // viewport within that allocation until the resize-aware GPU ring lands;
    // otherwise a larger OnPaint frame could overrun the shared slot.
    const int MaxWidth = Ring ? static_cast<int>(Ring->GetWidth()) : InWidth;
    const int MaxHeight = Ring ? static_cast<int>(Ring->GetHeight()) : InHeight;
    Width.store(std::max(1, std::min(InWidth, MaxWidth)), std::memory_order_relaxed);
    Height.store(std::max(1, std::min(InHeight, MaxHeight)), std::memory_order_relaxed);
  }

private:
  void PublishAccelerated(PaintElementType Type, const RectList& DirtyRects,
                          std::uintptr_t SharedHandle)
  {
    if (Type != PET_VIEW)
    {
      return;
    }
    Protocol::FFrameSlot Frame;
    Frame.Sequence = ++Sequence;
    Frame.FrameId = Frame.Sequence;
    Frame.TimestampUs = NowMicros();
    Frame.Width = static_cast<std::uint32_t>(Width.load(std::memory_order_relaxed));
    Frame.Height = static_cast<std::uint32_t>(Height.load(std::memory_order_relaxed));
    Frame.Format = 1; // BGRA8; CEF's Windows shared texture is D3D11 BGRA8.
    Frame.Flags = Protocol::FrameGpuSharedTexture |
        (DirtyRects.empty() ? 0u : Protocol::FrameHasDirtyRect);
    // The handle is valid only for the callback's lifetime in current CEF.
    // A future transport copies it into a host-owned ring before publishing.
    Frame.PixelBytes = static_cast<std::uint32_t>(SharedHandle != 0 ? 1 : 0);
    bReceivedGpuFrame.store(true, std::memory_order_release);
    PublishFrame(Frame, nullptr);
  }

  void PublishFrame(Protocol::FFrameSlot Frame, const void* Pixels)
  {
    if (Ring)
    {
      Ring->Publish(Frame, Pixels);
    }
    LastFrame = Frame;
    std::wcerr << L"[VeloWebViewBrowserHost] frame instance=" << InstanceId
               << L" seq=" << Frame.Sequence << L" " << Frame.Width << L"x"
               << Frame.Height << (Frame.Flags & Protocol::FrameGpuSharedTexture
                                       ? L" gpu" : L" cpu") << std::endl;
  }

  std::atomic<int> Width;
  std::atomic<int> Height;
  std::uint32_t InstanceId = 0;
  std::atomic<std::uint64_t> Sequence{0};
  std::atomic<bool> bReceivedGpuFrame{false};
  Protocol::FFrameSlot LastFrame{};
  std::shared_ptr<FFrameRing> Ring;
  IMPLEMENT_REFCOUNTING(FRenderHandler);
};

class FClient final : public CefClient,
                      public CefLifeSpanHandler,
                      public CefLoadHandler,
                      public CefDisplayHandler
{
public:
  explicit FClient(CefRefPtr<FRenderHandler> InRenderHandler)
      : RenderHandler(std::move(InRenderHandler))
  {
  }

  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return RenderHandler; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }

  void OnAfterCreated(CefRefPtr<CefBrowser> InBrowser) override
  {
    Browser = InBrowser;
    std::wcerr << L"[VeloWebViewBrowserHost] browser created id="
               << InBrowser->GetIdentifier() << std::endl;
  }

  void OnBeforeClose(CefRefPtr<CefBrowser>) override
  {
    Browser = nullptr;
    bClosed.store(true, std::memory_order_release);
  }

  void OnLoadStart(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, TransitionType) override
  {
    std::wcerr << L"[VeloWebViewBrowserHost] load started" << std::endl;
  }

  void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> Frame, int HttpStatusCode) override
  {
    std::wcerr << L"[VeloWebViewBrowserHost] load end status=" << HttpStatusCode
               << L" url=" << Frame->GetURL().ToWString() << std::endl;
  }

  void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, ErrorCode Code,
                  const CefString& Text, const CefString& FailedUrl) override
  {
    std::wcerr << L"[VeloWebViewBrowserHost] load error code=" << static_cast<int>(Code)
               << L" url=" << FailedUrl.ToWString() << L" " << Text.ToWString() << std::endl;
  }

  void OnAddressChange(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, const CefString& Url) override
  {
    std::wcerr << L"[VeloWebViewBrowserHost] url=" << Url.ToWString() << std::endl;
  }

  bool IsClosed() const { return bClosed.load(std::memory_order_acquire); }
  CefRefPtr<CefBrowser> GetBrowser() const { return Browser; }
  void HandleCommand(Protocol::EMessageType Type, const std::string& Payload)
  {
    CEF_REQUIRE_UI_THREAD();
    const CefRefPtr<CefBrowser> ActiveBrowser = Browser;
    if (!ActiveBrowser)
    {
      return;
    }
    switch (Type)
    {
      case Protocol::EMessageType::LoadUrl:
        ActiveBrowser->GetMainFrame()->LoadURL(CefString(Payload));
        break;
      case Protocol::EMessageType::LoadHtml:
      {
        const std::size_t Separator = Payload.find('\n');
        const CefString Html(Separator == std::string::npos ? Payload : Payload.substr(Separator + 1));
        const CefString BaseUrl(Separator == std::string::npos ? std::string("about:blank") : Payload.substr(0, Separator));
        const CefString EncodedHtml = CefURIEncode(Html, false);
        ActiveBrowser->GetMainFrame()->LoadURL(
            CefString(std::string("data:text/html;charset=utf-8,") + EncodedHtml.ToString()));
        break;
      }
      case Protocol::EMessageType::Reload: ActiveBrowser->Reload(); break;
      case Protocol::EMessageType::Stop: ActiveBrowser->StopLoad(); break;
      case Protocol::EMessageType::GoBack: ActiveBrowser->GoBack(); break;
      case Protocol::EMessageType::GoForward: ActiveBrowser->GoForward(); break;
      case Protocol::EMessageType::ExecuteJavaScript:
        ActiveBrowser->GetMainFrame()->ExecuteJavaScript(CefString(Payload), ActiveBrowser->GetMainFrame()->GetURL(), 0);
        break;
      case Protocol::EMessageType::Resize:
      {
        std::stringstream Stream(Payload);
        int NewWidth = 1;
        int NewHeight = 1;
        float Scale = 1.0f;
        char Comma = 0;
        Stream >> NewWidth >> Comma >> NewHeight >> Comma >> Scale;
        RenderHandler->SetSize(NewWidth, NewHeight);
        ActiveBrowser->GetHost()->WasResized();
        break;
      }
      case Protocol::EMessageType::SetFocus:
        ActiveBrowser->GetHost()->SetFocus(Payload == "1");
        break;
      case Protocol::EMessageType::SetVisibility:
        ActiveBrowser->GetHost()->WasHidden(Payload != "1");
        break;
      case Protocol::EMessageType::MouseMove:
      {
        if (Payload.size() != sizeof(Protocol::FMouseMovePayload)) break;
        Protocol::FMouseMovePayload Input{};
        std::memcpy(&Input, Payload.data(), sizeof(Input));
        CefMouseEvent Event;
        Event.x = Input.X; Event.y = Input.Y; Event.modifiers = Input.Modifiers;
        ActiveBrowser->GetHost()->SendMouseMoveEvent(Event, Input.Leave != 0);
        break;
      }
      case Protocol::EMessageType::MouseButton:
      {
        if (Payload.size() != sizeof(Protocol::FMouseButtonPayload)) break;
        Protocol::FMouseButtonPayload Input{};
        std::memcpy(&Input, Payload.data(), sizeof(Input));
        CefBrowserHost::MouseButtonType Button;
        if (Input.Button == Protocol::MouseButtonLeft) Button = MBT_LEFT;
        else if (Input.Button == Protocol::MouseButtonMiddle) Button = MBT_MIDDLE;
        else if (Input.Button == Protocol::MouseButtonRight) Button = MBT_RIGHT;
        else break;
        CefMouseEvent Event;
        Event.x = Input.X; Event.y = Input.Y; Event.modifiers = Input.Modifiers;
        ActiveBrowser->GetHost()->SendMouseClickEvent(Event, Button, Input.MouseUp != 0,
                                                       std::max(1, static_cast<int>(Input.ClickCount)));
        break;
      }
      case Protocol::EMessageType::MouseWheel:
      {
        if (Payload.size() != sizeof(Protocol::FMouseWheelPayload)) break;
        Protocol::FMouseWheelPayload Input{};
        std::memcpy(&Input, Payload.data(), sizeof(Input));
        CefMouseEvent Event;
        Event.x = Input.X; Event.y = Input.Y; Event.modifiers = Input.Modifiers;
        ActiveBrowser->GetHost()->SendMouseWheelEvent(Event, Input.DeltaX, Input.DeltaY);
        break;
      }
      case Protocol::EMessageType::Key:
      {
        if (Payload.size() != sizeof(Protocol::FKeyPayload)) break;
        Protocol::FKeyPayload Input{};
        std::memcpy(&Input, Payload.data(), sizeof(Input));
        if (Input.Type > Protocol::KeyEventChar) break;
        CefKeyEvent Event;
        Event.type = static_cast<cef_key_event_type_t>(Input.Type);
        Event.modifiers = Input.Modifiers;
        Event.windows_key_code = static_cast<int>(Input.WindowsKeyCode);
        Event.native_key_code = static_cast<int>(Input.NativeKeyCode);
        Event.is_system_key = Input.IsSystemKey != 0;
        Event.character = static_cast<char16_t>(Input.Character);
        Event.unmodified_character = static_cast<char16_t>(Input.UnmodifiedCharacter);
        ActiveBrowser->GetHost()->SendKeyEvent(Event);
        break;
      }
      case Protocol::EMessageType::SetMuted:
        ActiveBrowser->GetMainFrame()->ExecuteJavaScript(
            Payload == "1" ? "document.querySelectorAll('video,audio').forEach(e=>e.muted=true)" :
                              "document.querySelectorAll('video,audio').forEach(e=>e.muted=false)",
            ActiveBrowser->GetMainFrame()->GetURL(), 0);
        break;
      case Protocol::EMessageType::Shutdown:
        ActiveBrowser->GetHost()->CloseBrowser(true);
        break;
      default:
        break;
    }
  }

private:
  CefRefPtr<FRenderHandler> RenderHandler;
  CefRefPtr<CefBrowser> Browser;
  std::atomic<bool> bClosed{false};
  IMPLEMENT_REFCOUNTING(FClient);
};

void DispatchCommand(CefRefPtr<FClient> Client, Protocol::EMessageType Type,
                     std::string Payload)
{
  if (Client)
  {
    Client->HandleCommand(Type, Payload);
  }
}

// Keep command dispatch independent of CEF's base::Bind implementation.  A
// plain CefTask also works with CEF distributions whose prebuilt wrapper was
// compiled without exporting the internal callback reference-count symbols.
class FCommandTask final : public CefTask
{
public:
  FCommandTask(CefRefPtr<FClient> InClient, Protocol::EMessageType InType,
               std::string InPayload)
      : Client(std::move(InClient)), Type(InType), Payload(std::move(InPayload))
  {
  }

  void Execute() override { DispatchCommand(Client, Type, Payload); }

private:
  CefRefPtr<FClient> Client;
  Protocol::EMessageType Type;
  std::string Payload;
  IMPLEMENT_REFCOUNTING(FCommandTask);
};

class FPipeServer final
{
public:
  FPipeServer(std::wstring InName, CefRefPtr<FClient> InClient)
      : Name(std::move(InName)), Client(std::move(InClient))
  {
  }

  ~FPipeServer() { Stop(); }

  bool Start()
  {
    if (Name.empty())
    {
      return false;
    }
    Worker = std::thread([this]() { Run(); });
    return true;
  }

  void Stop()
  {
    bStop.store(true, std::memory_order_release);
    if (Pipe != INVALID_HANDLE_VALUE)
    {
      ::CancelIoEx(Pipe, nullptr);
      ::DisconnectNamedPipe(Pipe);
      ::CloseHandle(Pipe);
      Pipe = INVALID_HANDLE_VALUE;
    }
    if (Worker.joinable())
    {
      Worker.join();
    }
  }

private:
  void Run()
  {
    const std::wstring PipePath = L"\\\\.\\pipe\\" + Name;
    while (!bStop.load(std::memory_order_acquire))
    {
      HANDLE NewPipe = ::CreateNamedPipeW(
          PipePath.c_str(), PIPE_ACCESS_DUPLEX,
          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
          1, Protocol::kMaxMessageBytes, Protocol::kMaxMessageBytes, 100,
          nullptr);
      if (NewPipe == INVALID_HANDLE_VALUE)
      {
        return;
      }
      Pipe = NewPipe;
      const BOOL Connected = ::ConnectNamedPipe(NewPipe, nullptr) ||
          ::GetLastError() == ERROR_PIPE_CONNECTED;
      if (!Connected)
      {
        ::CloseHandle(NewPipe);
        Pipe = INVALID_HANDLE_VALUE;
        continue;
      }

      while (!bStop.load(std::memory_order_acquire))
      {
        Protocol::FMessageHeader Header{};
        DWORD Read = 0;
        if (!::ReadFile(NewPipe, &Header, sizeof(Header), &Read, nullptr) ||
            Read != sizeof(Header))
        {
          break;
        }
        if (!Protocol::IsValid(Header, Header.Size))
        {
          break;
        }
        const std::uint32_t PayloadBytes = Header.Size - sizeof(Header);
        std::string Payload(PayloadBytes, '\0');
        if (PayloadBytes > 0 &&
            (!::ReadFile(NewPipe, Payload.data(), PayloadBytes, &Read, nullptr) ||
             Read != PayloadBytes))
        {
          break;
        }
        const auto Type = static_cast<Protocol::EMessageType>(Header.Type);
        CefRefPtr<FClient> KeepClient = Client;
        CefPostTask(TID_UI, new FCommandTask(KeepClient, Type, Payload));
      }
      ::DisconnectNamedPipe(NewPipe);
      ::CloseHandle(NewPipe);
      Pipe = INVALID_HANDLE_VALUE;
    }
  }

  std::wstring Name;
  CefRefPtr<FClient> Client;
  std::thread Worker;
  std::atomic<bool> bStop{false};
  HANDLE Pipe = INVALID_HANDLE_VALUE;
};

class FApp final : public CefApp
{
public:
  FApp(bool InDisableGpu, bool InAllowMediaCapture, bool InEnablePixelStreaming)
      : bDisableGpu(InDisableGpu), bAllowMediaCapture(InAllowMediaCapture),
        bEnablePixelStreaming(InEnablePixelStreaming) {}

  void OnBeforeCommandLineProcessing(const CefString&, CefRefPtr<CefCommandLine> CommandLine) override
  {
    if (bDisableGpu)
    {
      CommandLine->AppendSwitch("disable-gpu");
      CommandLine->AppendSwitch("disable-gpu-compositing");
    }
    if (bAllowMediaCapture)
    {
      // This grants getUserMedia in the host. Production callers should only
      // pass the switch for trusted pixel-streaming origins.
      CommandLine->AppendSwitch("enable-media-stream");
    }
    if (bEnablePixelStreaming)
    {
      CommandLine->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");
      CommandLine->AppendSwitch("disable-background-timer-throttling");
      CommandLine->AppendSwitch("disable-renderer-backgrounding");
    }
  }

private:
  bool bDisableGpu = false;
  bool bAllowMediaCapture = false;
  bool bEnablePixelStreaming = false;
  IMPLEMENT_REFCOUNTING(FApp);
};

int RunBrowser(const CefMainArgs& Args, const FOptions& Options)
{
  CefSettings Settings;
  Settings.no_sandbox = true;
  Settings.windowless_rendering_enabled = true;
  Settings.multi_threaded_message_loop = false;
  Settings.external_message_pump = false;
  Settings.log_severity = LOGSEVERITY_WARNING;
  if (!Options.CachePath.empty())
  {
    // The singleton is keyed by the root, not just by the profile directory.
    CefString(&Settings.root_cache_path) = CefString(Options.CachePath);
    CefString(&Settings.cache_path) = CefString(Options.CachePath);
  }
  else
  {
    // Standalone hosts also need isolation from other CEF applications and
    // widget hosts. Leave cache_path empty to retain an incognito profile.
    const auto Root = std::filesystem::temp_directory_path() / L"VeloWebView" /
        (std::to_wstring(::GetCurrentProcessId()) + L"_" + std::to_wstring(Options.InstanceId));
    std::filesystem::create_directories(Root);
    CefString(&Settings.root_cache_path) = CefString(Root.wstring());
  }
  if (!Options.LogFile.empty())
  {
    CefString(&Settings.log_file) = CefString(Options.LogFile);
  }

  CefRefPtr<FApp> App = new FApp(Options.DisableGpu, Options.AllowMediaCapture,
                                 Options.EnablePixelStreaming);
  const CefMainArgs MainArgs = Args;
  const bool bInitialized = CefInitialize(MainArgs, Settings, App, nullptr);
  if (!bInitialized)
  {
    std::wcerr << L"[VeloWebViewBrowserHost] CefInitialize failed: "
               << CefGetExitCode() << std::endl;
    return CefGetExitCode();
  }

  CefWindowInfo WindowInfo;
  WindowInfo.SetAsWindowless(nullptr);
  WindowInfo.shared_texture_enabled = Options.DisableGpu ? 0 : 1;
  WindowInfo.external_begin_frame_enabled = Options.ExternalBeginFrame ? 1 : 0;

  CefBrowserSettings BrowserSettings;
  BrowserSettings.windowless_frame_rate = Options.FrameRate;
  // The first composited consumer uses the CPU BGRA ring. Keep the browser
  // surface opaque so an unset/transparent page cannot appear as a blank Slate
  // brush while the GPU texture bridge is disabled.
  BrowserSettings.background_color = 0xffffffff;

  auto FrameRing = std::make_shared<FFrameRing>();
  if (!FrameRing->Create(Options.InstanceId, Options.Width, Options.Height))
  {
    std::wcerr << L"[VeloWebViewBrowserHost] unable to create frame mapping" << std::endl;
  }
  CefRefPtr<FRenderHandler> RenderHandler =
      new FRenderHandler(Options.Width, Options.Height, Options.InstanceId, FrameRing);
  CefRefPtr<FClient> Client = new FClient(RenderHandler);
  FPipeServer PipeServer(Options.PipeName, Client);
  if (!Options.PipeName.empty() && !PipeServer.Start())
  {
    std::wcerr << L"[VeloWebViewBrowserHost] unable to create control pipe" << std::endl;
  }
  CefRefPtr<CefBrowser> Browser = CefBrowserHost::CreateBrowserSync(
      WindowInfo, Client, CefString(Options.Url), BrowserSettings, nullptr, nullptr);
  if (!Browser)
  {
    std::wcerr << L"[VeloWebViewBrowserHost] CefBrowserHost::CreateBrowserSync failed" << std::endl;
    CefShutdown();
    return 2;
  }

  if (Options.EnableDevTools)
  {
    Browser->GetHost()->ShowDevTools(CefWindowInfo(), Client, CefBrowserSettings(), CefPoint());
  }

  // The first transport milestone is deliberately explicit: CEF owns the
  // frame callback; IPC/shared-texture ring integration is added behind the
  // protocol boundary without changing the UMG-facing API.
  CefRunMessageLoop();
  if (CefRefPtr<CefBrowser> ActiveBrowser = Client->GetBrowser())
  {
    ActiveBrowser->GetHost()->CloseBrowser(true);
  }
  PipeServer.Stop();
  CefShutdown();
  return 0;
}
} // namespace VeloWebView::BrowserHost

int APIENTRY wWinMain(HINSTANCE Instance, HINSTANCE, LPWSTR, int)
{
  int Argc = 0;
  LPWSTR* WideArgv = ::CommandLineToArgvW(::GetCommandLineW(), &Argc);
  if (!WideArgv)
  {
    return 1;
  }

  // On Windows CEF's CefMainArgs intentionally takes the module instance;
  // CEF reads the full command line itself for subprocess dispatch.
  CefMainArgs MainArgs(Instance);
  const int SubprocessExitCode = CefExecuteProcess(MainArgs, nullptr, nullptr);
  if (SubprocessExitCode >= 0)
  {
    ::LocalFree(WideArgv);
    return SubprocessExitCode;
  }

  const VeloWebView::BrowserHost::FOptions Options =
      VeloWebView::BrowserHost::ParseOptions(Argc, WideArgv);
  const int ExitCode = VeloWebView::BrowserHost::RunBrowser(MainArgs, Options);
  ::LocalFree(WideArgv);
  return ExitCode;
}
