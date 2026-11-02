// Minimal standalone CEF BrowserHost for the VeloWebView protocol.
//
// This executable deliberately owns CEF.  The Unreal module must communicate
// with it over the versioned protocol in Shared/BrowserProtocol and must never
// load libcef.dll into UnrealEditor.exe or the game process.  The first host
// milestone reports frame metadata and validates both CEF OSR callbacks; the
// shared-texture copy/IPC transport is the next layer behind this seam.

#include "BrowserProtocol/BrowserProtocol.h"
#include "JavaScriptBridge/BridgeScript.h"

#include "include/cef_app.h"
#include "include/cef_client.h"
#include "include/cef_parser.h"
#include "include/cef_process_message.h"
#include "include/cef_render_process_handler.h"
#include "include/cef_display_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_render_handler.h"
#include "include/cef_v8.h"
#include "include/wrapper/cef_helpers.h"

#include <windows.h>
#include <shellapi.h>
#ifdef LoadString
#undef LoadString
#endif

#include <atomic>
#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
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
  bool EnableJavaScript = true;
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
  Options.EnableJavaScript = GetArgument(Argc, Argv, L"--javascript-enabled") != L"0";
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

  bool GetScreenInfo(CefRefPtr<CefBrowser> Browser, CefScreenInfo& Info) override
  {
    // The OSR surface and pipe input both use browser pixels. Keep popup
    // placement within that virtual screen, independent of desktop DPI.
    Info.device_scale_factor = 1.0f;
    CefRect Rect;
    GetViewRect(Browser, Rect);
    Info.rect = Rect;
    Info.available_rect = Info.rect;
    return true;
  }

  void OnPopupShow(CefRefPtr<CefBrowser>, bool Show) override
  {
    if (Show)
    {
      bPopupVisible = true;
      PopupPixels.clear();
    }
    else
    {
      ResetPopup();
    }
  }

  void ResetPopup()
  {
    bPopupVisible = false;
    PopupRect = CefRect();
    PopupPixels.clear();
    // A static page need not repaint when its popup closes. Republish the
    // clean view immediately so the last dropdown cannot remain in Slate.
    PublishCpuFrame();
  }

  void OnPopupSize(CefRefPtr<CefBrowser>, const CefRect& Rect) override
  {
    PopupRect = Rect;
    PopupPixels.clear();
    PublishCpuFrame();
  }

  bool IsOverPopup(int X, int Y) const
  {
    const CefRect Rect = GetPopupRect();
    return bPopupVisible && X >= Rect.x && Y >= Rect.y &&
        X < Rect.x + Rect.width && Y < Rect.y + Rect.height;
  }

  bool HasPopupOffset() const
  {
    const CefRect Rect = GetPopupRect();
    return Rect.x != PopupRect.x || Rect.y != PopupRect.y;
  }

  void ApplyPopupOffset(CefMouseEvent& Event) const
  {
    if (IsOverPopup(Event.x, Event.y))
    {
      const CefRect Rect = GetPopupRect();
      Event.x += PopupRect.x - Rect.x;
      Event.y += PopupRect.y - Rect.y;
    }
  }

  void OnPaint(CefRefPtr<CefBrowser>, PaintElementType Type,
               const RectList&, const void* Buffer, int InWidth, int InHeight) override
  {
    if (!Buffer || InWidth <= 0 || InHeight <= 0)
    {
      return;
    }
    const auto* Pixels = static_cast<const std::uint8_t*>(Buffer);
    const std::size_t Bytes = static_cast<std::size_t>(InWidth) * InHeight * 4;
    if (Type == PET_VIEW)
    {
      ViewWidth = InWidth;
      ViewHeight = InHeight;
      ViewPixels.assign(Pixels, Pixels + Bytes);
    }
    else if (Type == PET_POPUP && bPopupVisible)
    {
      PopupWidth = InWidth;
      PopupHeight = InHeight;
      PopupPixels.assign(Pixels, Pixels + Bytes);
    }
    else
    {
      return;
    }
    PublishCpuFrame();
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
    const int NewWidth = std::max(1, std::min(InWidth, MaxWidth));
    const int NewHeight = std::max(1, std::min(InHeight, MaxHeight));
    if (NewWidth != Width.load() || NewHeight != Height.load())
    {
      bPopupVisible = false;
      PopupRect = CefRect();
      PopupPixels.clear();
      ViewPixels.clear();
    }
    Width.store(NewWidth, std::memory_order_relaxed);
    Height.store(NewHeight, std::memory_order_relaxed);
  }

private:
  CefRect GetPopupRect() const
  {
    CefRect Rect = PopupRect;
    Rect.x = std::max(0, std::min(Rect.x, Width.load() - Rect.width));
    Rect.y = std::max(0, std::min(Rect.y, Height.load() - Rect.height));
    return Rect;
  }

  void PublishCpuFrame()
  {
    if (ViewPixels.empty()) return;

    const void* Pixels = ViewPixels.data();
    if (bPopupVisible && !PopupPixels.empty())
    {
      // PET_POPUP is a separate premultiplied BGRA surface (native select
      // options). Preserve the clean view for independent paints and hiding.
      CompositedPixels = ViewPixels;
      const CefRect Rect = GetPopupRect();
      const int CopyWidth = std::max(0, std::min({PopupWidth, Rect.width, ViewWidth - Rect.x}));
      const int CopyHeight = std::max(0, std::min({PopupHeight, Rect.height, ViewHeight - Rect.y}));
      for (int Y = 0; Y < CopyHeight; ++Y)
      {
        auto* Dest = CompositedPixels.data() +
            (static_cast<std::size_t>(Rect.y + Y) * ViewWidth + Rect.x) * 4;
        const auto* Source = PopupPixels.data() + static_cast<std::size_t>(Y) * PopupWidth * 4;
        for (int X = 0; X < CopyWidth; ++X, Dest += 4, Source += 4)
        {
          const unsigned InverseAlpha = 255u - Source[3];
          for (int Channel = 0; Channel < 4; ++Channel)
          {
            Dest[Channel] = static_cast<std::uint8_t>(std::min(255u,
                Source[Channel] + (Dest[Channel] * InverseAlpha + 127u) / 255u));
          }
        }
      }
      Pixels = CompositedPixels.data();
    }

    Protocol::FFrameSlot Frame;
    Frame.Sequence = ++Sequence;
    Frame.FrameId = Frame.Sequence;
    Frame.TimestampUs = NowMicros();
    Frame.Width = static_cast<std::uint32_t>(ViewWidth);
    Frame.Height = static_cast<std::uint32_t>(ViewHeight);
    Frame.Format = 1;
    // Always publish a full view-sized frame. No popup-specific IPC or Slate
    // overlay is needed, so UMG clipping and pointer coordinates still match.
    Frame.Flags = Protocol::FrameCpuFallback;
    Frame.PixelBytes = static_cast<std::uint32_t>(ViewPixels.size());
    bReceivedGpuFrame.store(false, std::memory_order_release);
    PublishFrame(Frame, Pixels);
  }

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
  // CefRenderHandler callbacks and pipe command tasks run on CEF's UI thread.
  bool bPopupVisible = false;
  CefRect PopupRect;
  int ViewWidth = 0;
  int ViewHeight = 0;
  int PopupWidth = 0;
  int PopupHeight = 0;
  std::vector<std::uint8_t> ViewPixels;
  std::vector<std::uint8_t> PopupPixels;
  std::vector<std::uint8_t> CompositedPixels;
  IMPLEMENT_REFCOUNTING(FRenderHandler);
};

// CEF callbacks never perform pipe I/O. Keep a bounded queue so a slow or
// disconnected Unreal consumer cannot block the browser's UI thread.
class FBrowserEvents
{
public:
  explicit FBrowserEvents(std::uint32_t InInstanceId) : InstanceId(InInstanceId) {}

  void Publish(EMessageType Type, const std::string& Payload)
  {
    if (Payload.size() > Protocol::kMaxMessageBytes - sizeof(Protocol::FMessageHeader)) return;
    std::lock_guard<std::mutex> Lock(Mutex);
    if (Packets.size() >= 1024 || QueuedBytes + Payload.size() > 4 * Protocol::kMaxMessageBytes) return;
    Protocol::FMessageHeader Header;
    Header.Type = static_cast<std::uint16_t>(Type);
    Header.Size += static_cast<std::uint32_t>(Payload.size());
    Header.InstanceId = InstanceId;
    Header.Sequence = ++Sequence;
    std::string Packet(sizeof(Header), '\0');
    std::memcpy(Packet.data(), &Header, sizeof(Header));
    Packet += Payload;
    QueuedBytes += Packet.size();
    Packets.push_back(std::move(Packet));
  }

  bool Pop(std::string& Packet)
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    if (Packets.empty()) return false;
    Packet = std::move(Packets.front());
    Packets.pop_front();
    QueuedBytes -= Packet.size();
    return true;
  }

  std::uint32_t GetInstanceId() const { return InstanceId; }
  bool HasPending() const
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    return !Packets.empty();
  }

private:
  const std::uint32_t InstanceId;
  std::uint64_t Sequence = 0;
  std::size_t QueuedBytes = 0;
  mutable std::mutex Mutex;
  std::deque<std::string> Packets;
};

class FRenderProcessHandler final : public CefRenderProcessHandler,
                                    public CefV8Handler
{
public:
  void OnContextCreated(CefRefPtr<CefBrowser> Browser, CefRefPtr<CefFrame> Frame,
                        CefRefPtr<CefV8Context> Context) override
  {
    if (!Frame->IsMain()) return;
    const auto Global = Context->GetGlobal();
    Global->SetValue("__veloPostMessage",
                     CefV8Value::CreateFunction("__veloPostMessage", this),
                     V8_PROPERTY_ATTRIBUTE_DONTDELETE);
    Global->SetValue("__veloNativePostMessage",
                     CefV8Value::CreateFunction("__veloNativePostMessage", this),
                     V8_PROPERTY_ATTRIBUTE_DONTDELETE);
    CefRefPtr<CefV8Value> Result;
    CefRefPtr<CefV8Exception> Exception;
    if (!Context->Eval(VeloWebView::JavaScriptBridgeSource, Frame->GetURL(), 0, Result, Exception))
      std::wcerr << L"[VeloWebViewBrowserHost] bridge injection failed: "
                 << (Exception ? Exception->GetMessage().ToWString() : L"unknown error") << std::endl;
  }

  bool Execute(const CefString& Name, CefRefPtr<CefV8Value> Object,
               const CefV8ValueList& Arguments, CefRefPtr<CefV8Value>& ReturnValue,
               CefString& Exception) override
  {
    if ((Name != "__veloPostMessage" && Name != "__veloNativePostMessage") || Arguments.size() != 1 ||
        !Arguments[0]->IsString())
    {
      Exception = "__veloPostMessage requires one string argument";
      return true;
    }
    const auto Context = CefV8Context::GetCurrentContext();
    const auto Frame = Context ? Context->GetFrame() : nullptr;
    if (!Frame || !Frame->IsMain()) return true;
    if (Arguments[0]->GetStringValue().ToString().size() > Protocol::kMaxMessageBytes - sizeof(Protocol::FMessageHeader))
    {
      Exception = "VeloWebView message exceeds the transport size limit";
      return true;
    }
    const auto Message = CefProcessMessage::Create("VeloWebView.JavaScriptMessage");
    Message->GetArgumentList()->SetString(0, Arguments[0]->GetStringValue());
    Frame->SendProcessMessage(PID_BROWSER, Message);
    return true;
  }

private:
  IMPLEMENT_REFCOUNTING(FRenderProcessHandler);
};

class FClient final : public CefClient,
                      public CefLifeSpanHandler,
                      public CefLoadHandler,
                      public CefDisplayHandler
{
public:
  FClient(CefRefPtr<FRenderHandler> InRenderHandler, std::shared_ptr<FBrowserEvents> InEvents)
      : RenderHandler(std::move(InRenderHandler)), Events(std::move(InEvents))
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
    CefQuitMessageLoop();
  }

  void OnLoadStart(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> Frame, TransitionType) override
  {
    if (Frame->IsMain())
    {
      RenderHandler->ResetPopup();
      Events->Publish(EMessageType::PageStarted, Frame->GetURL().ToString());
    }
    std::wcerr << L"[VeloWebViewBrowserHost] load started" << std::endl;
  }

  void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> Frame, int HttpStatusCode) override
  {
    if (Frame->IsMain()) Events->Publish(EMessageType::PageLoaded, Frame->GetURL().ToString());
    std::wcerr << L"[VeloWebViewBrowserHost] load end status=" << HttpStatusCode
               << L" url=" << Frame->GetURL().ToWString() << std::endl;
  }

  void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> Frame, ErrorCode Code,
                  const CefString& Text, const CefString& FailedUrl) override
  {
    if (Frame->IsMain() && Code != ERR_ABORTED)
    {
      const auto Error = CefDictionaryValue::Create();
      Error->SetInt("code", static_cast<int>(Code));
      Error->SetString("description", Text);
      Error->SetString("url", FailedUrl);
      Events->Publish(EMessageType::PageError, CefWriteJSON(CefValueFor(Error), JSON_WRITER_DEFAULT).ToString());
    }
    std::wcerr << L"[VeloWebViewBrowserHost] load error code=" << static_cast<int>(Code)
               << L" url=" << FailedUrl.ToWString() << L" " << Text.ToWString() << std::endl;
  }

  void OnAddressChange(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> Frame, const CefString& Url) override
  {
    if (Frame->IsMain()) Events->Publish(EMessageType::UrlChanged, Url.ToString());
    std::wcerr << L"[VeloWebViewBrowserHost] url=" << Url.ToWString() << std::endl;
  }

  void OnTitleChange(CefRefPtr<CefBrowser>, const CefString& Title) override
  {
    Events->Publish(EMessageType::TitleChanged, Title.ToString());
  }

  bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t, const CefString& Message,
                        const CefString& Source, int Line) override
  {
    const auto Console = CefDictionaryValue::Create();
    Console->SetString("message", Message);
    Console->SetString("source", Source);
    Console->SetInt("line", Line);
    Events->Publish(EMessageType::ConsoleMessage, CefWriteJSON(CefValueFor(Console), JSON_WRITER_DEFAULT).ToString());
    return false;
  }

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> Frame,
                                CefProcessId Source, CefRefPtr<CefProcessMessage> Message) override
  {
    if (Source != PID_RENDERER || !Frame->IsMain() || Message->GetName() != "VeloWebView.JavaScriptMessage") return false;
    const auto Arguments = Message->GetArgumentList();
    if (Arguments->GetSize() == 1 && Arguments->GetType(0) == VTYPE_STRING)
      Events->Publish(EMessageType::JavaScriptMessage, Arguments->GetString(0).ToString());
    return true;
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
      case Protocol::EMessageType::Hello:
        Events->Publish(EMessageType::HelloReply, "1");
        break;
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
        ActiveBrowser->GetHost()->NotifyScreenInfoChanged();
        ActiveBrowser->GetHost()->WasResized();
        break;
      }
      case Protocol::EMessageType::SetFocus:
        ActiveBrowser->GetHost()->SetFocus(Payload == "1");
        break;
      case Protocol::EMessageType::SetJavaScriptEnabled:
      {
        const auto Context = ActiveBrowser->GetHost()->GetRequestContext();
        if (Context)
        {
          Context->SetContentSetting("", "", CEF_CONTENT_SETTING_TYPE_JAVASCRIPT,
              Payload == "1" ? CEF_CONTENT_SETTING_VALUE_ALLOW : CEF_CONTENT_SETTING_VALUE_BLOCK);
        }
        break;
      }
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
        RenderHandler->ApplyPopupOffset(Event);
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
        if (!Input.MouseUp)
        {
          bMouseDownOnView = !RenderHandler->IsOverPopup(Event.x, Event.y);
        }
        else if (bMouseDownOnView && RenderHandler->IsOverPopup(Event.x, Event.y) &&
                 RenderHandler->HasPopupOffset())
        {
          // Opening a repositioned popup must not select the option that now
          // covers the select control when the initiating mouse-up arrives.
          break;
        }
        RenderHandler->ApplyPopupOffset(Event);
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
        RenderHandler->ApplyPopupOffset(Event);
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
  static CefRefPtr<CefValue> CefValueFor(CefRefPtr<CefDictionaryValue> Dictionary)
  {
    const auto Value = CefValue::Create();
    Value->SetDictionary(Dictionary);
    return Value;
  }
  CefRefPtr<FRenderHandler> RenderHandler;
  std::shared_ptr<FBrowserEvents> Events;
  CefRefPtr<CefBrowser> Browser;
  std::atomic<bool> bClosed{false};
  bool bMouseDownOnView = false;
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
  FPipeServer(std::wstring InName, CefRefPtr<FClient> InClient,
              std::shared_ptr<FBrowserEvents> InEvents)
      : Name(std::move(InName)), Client(std::move(InClient)), Events(std::move(InEvents))
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
          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
          1, Protocol::kMaxMessageBytes, Protocol::kMaxMessageBytes, 100,
          nullptr);
      if (NewPipe == INVALID_HANDLE_VALUE)
      {
        return;
      }
      Pipe = NewPipe;
      BOOL Connected = FALSE;
      while (!bStop.load(std::memory_order_acquire))
      {
        Connected = ::ConnectNamedPipe(NewPipe, nullptr) || ::GetLastError() == ERROR_PIPE_CONNECTED;
        if (Connected) break;
        if (::GetLastError() != ERROR_PIPE_LISTENING) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      if (!Connected)
      {
        ::CloseHandle(NewPipe);
        Pipe = INVALID_HANDLE_VALUE;
        continue;
      }

      std::array<char, sizeof(Protocol::FMessageHeader)> HeaderBytes{};
      std::size_t HeaderRead = 0;
      Protocol::FMessageHeader IncomingHeader{};
      std::string IncomingPayload;
      std::size_t PayloadRead = 0;
      std::string PendingWrite;
      std::size_t WriteOffset = 0;
      while (!bStop.load(std::memory_order_acquire))
      {
        DWORD Available = 0;
        if (!::PeekNamedPipe(NewPipe, nullptr, 0, nullptr, &Available, nullptr))
        {
          break;
        }

        DWORD Read = 0;
        if (HeaderRead < HeaderBytes.size() && Available > 0)
        {
          const DWORD Amount = static_cast<DWORD>(std::min<std::size_t>(Available, HeaderBytes.size() - HeaderRead));
          if (!::ReadFile(NewPipe, HeaderBytes.data() + HeaderRead, Amount, &Read, nullptr)) break;
          HeaderRead += Read;
          Available -= Read;
        }

        if (HeaderRead == HeaderBytes.size() && IncomingPayload.empty() && PayloadRead == 0)
        {
          std::memcpy(&IncomingHeader, HeaderBytes.data(), sizeof(IncomingHeader));
          if (!Protocol::IsValid(IncomingHeader, IncomingHeader.Size) ||
              IncomingHeader.InstanceId != Events->GetInstanceId()) break;
          const std::size_t PayloadBytes = IncomingHeader.Size - sizeof(IncomingHeader);
          IncomingPayload.resize(PayloadBytes);
          if (PayloadBytes == 0)
          {
            DispatchIncoming(IncomingHeader, IncomingPayload);
            HeaderRead = 0;
            IncomingPayload.clear();
          }
        }

        if (HeaderRead == HeaderBytes.size() && IncomingPayload.size() > PayloadRead && Available > 0)
        {
          const DWORD Amount = static_cast<DWORD>(std::min<std::size_t>(Available, IncomingPayload.size() - PayloadRead));
          if (!::ReadFile(NewPipe, IncomingPayload.data() + PayloadRead, Amount, &Read, nullptr)) break;
          PayloadRead += Read;
          if (PayloadRead == IncomingPayload.size())
          {
            DispatchIncoming(IncomingHeader, IncomingPayload);
            HeaderRead = 0;
            IncomingPayload.clear();
            PayloadRead = 0;
          }
        }

        bool bWroteAny = false;
        if (PendingWrite.empty() || WriteOffset == PendingWrite.size())
        {
          PendingWrite.clear();
          WriteOffset = 0;
          Events->Pop(PendingWrite);
        }
        if (!PendingWrite.empty())
        {
          const DWORD Amount = static_cast<DWORD>(std::min<std::size_t>(PendingWrite.size() - WriteOffset, 64 * 1024));
          if (::WriteFile(NewPipe, PendingWrite.data() + WriteOffset, Amount, &Read, nullptr))
          {
            bWroteAny = Read > 0;
            WriteOffset += Read;
          }
          else if (::GetLastError() != ERROR_NO_DATA && ::GetLastError() != ERROR_PIPE_BUSY)
            break;
        }

        if (Available == 0 && !bWroteAny)
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      ::DisconnectNamedPipe(NewPipe);
      ::CloseHandle(NewPipe);
      Pipe = INVALID_HANDLE_VALUE;
    }
  }

  void DispatchIncoming(const Protocol::FMessageHeader& Header, const std::string& Payload)
  {
    CefRefPtr<FClient> KeepClient = Client;
    const auto Type = static_cast<Protocol::EMessageType>(Header.Type);
    CefPostTask(TID_UI, new FCommandTask(KeepClient, Type, Payload));
  }

  std::wstring Name;
  CefRefPtr<FClient> Client;
  std::shared_ptr<FBrowserEvents> Events;
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

  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return RenderHandler; }

private:
  bool bDisableGpu = false;
  bool bAllowMediaCapture = false;
  bool bEnablePixelStreaming = false;
  CefRefPtr<FRenderProcessHandler> RenderHandler = new FRenderProcessHandler();
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

  CefRequestContext::GetGlobalContext()->SetContentSetting(
      "", "", CEF_CONTENT_SETTING_TYPE_JAVASCRIPT,
      Options.EnableJavaScript ? CEF_CONTENT_SETTING_VALUE_ALLOW : CEF_CONTENT_SETTING_VALUE_BLOCK);

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
  auto BrowserEvents = std::make_shared<FBrowserEvents>(Options.InstanceId);
  CefRefPtr<FClient> Client = new FClient(RenderHandler, BrowserEvents);
  FPipeServer PipeServer(Options.PipeName, Client, BrowserEvents);
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

  // The pipe remains active while CEF pumps its normal browser and renderer events.
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
  const VeloWebView::BrowserHost::FOptions Options =
      VeloWebView::BrowserHost::ParseOptions(Argc, WideArgv);
  CefRefPtr<VeloWebView::BrowserHost::FApp> App = new VeloWebView::BrowserHost::FApp(
      Options.DisableGpu, Options.AllowMediaCapture, Options.EnablePixelStreaming);
  const int SubprocessExitCode = CefExecuteProcess(MainArgs, App, nullptr);
  if (SubprocessExitCode >= 0)
  {
    ::LocalFree(WideArgv);
    return SubprocessExitCode;
  }

  const int ExitCode = VeloWebView::BrowserHost::RunBrowser(MainArgs, Options);
  ::LocalFree(WideArgv);
  return ExitCode;
}
