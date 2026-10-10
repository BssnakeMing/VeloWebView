#pragma once

// The protocol is deliberately independent of Unreal and CEF.  It is shared by
// BrowserHost.exe and the plugin, and is kept POD-only so a packaged game can
// exchange messages with a host built by a different toolchain.

#include <cstddef>
#include <cstdint>

namespace VeloWebView
{
namespace Protocol
{
	constexpr std::uint32_t kMagic = 0x56454C4Fu; // "VELO"
	constexpr std::uint16_t kVersion = 1;
	constexpr std::uint32_t kMaxMessageBytes = 1024u * 1024u;
	constexpr std::uint32_t kFrameSlotCount = 3;

	enum class EMessageType : std::uint16_t
	{
		Hello = 1,
		HelloReply,
		CreateBrowser,
		DestroyBrowser,
		LoadUrl,
		LoadHtml,
		GoBack,
		GoForward,
		Reload,
		Stop,
		Resize,
		SetFocus,
		SetVisibility,
		SetFrameRate,
		ExecuteJavaScript,
		SetJavaScriptEnabled,
		SetMuted,
		ClearCookies,
		SetCookie,
		DeleteCookie,
		OpenDevTools,
		CloseDevTools,
		Shutdown,
		MouseMove,
		MouseButton,
		MouseWheel,
		Key,
		Character,
		FrameAvailable,
		PageStarted,
		PageLoaded,
		PageError,
		UrlChanged,
		TitleChanged,
		ConsoleMessage,
		JavaScriptMessage,
		BrowserCrashed,
		HostLog
	};

	enum EFrameFlags : std::uint32_t
	{
		FrameNone = 0,
		FrameGpuSharedTexture = 1u << 0,
		FrameCpuFallback = 1u << 1,
		FramePopup = 1u << 2,
		FrameHasDirtyRect = 1u << 3
	};

	// Input payloads are POD and use the same values as CEF's public enums.
	// Keeping them in the shared ABI avoids locale/encoding problems on the
	// named pipe and lets the BrowserHost forward events without reconstructing
	// Slate objects.
	enum EMouseButton : std::uint8_t
	{
		MouseButtonLeft = 0,
		MouseButtonMiddle = 1,
		MouseButtonRight = 2
	};

	enum EKeyEventType : std::uint8_t
	{
		KeyEventRawKeyDown = 0,
		KeyEventKeyDown = 1,
		KeyEventKeyUp = 2,
		KeyEventChar = 3
	};

#pragma pack(push, 1)
	struct FMessageHeader
	{
		std::uint32_t Magic = kMagic;
		std::uint16_t Version = kVersion;
		std::uint16_t Type = 0;
		std::uint32_t Size = sizeof(FMessageHeader);
		std::uint32_t InstanceId = 0;
		std::uint64_t Sequence = 0;
	};

	struct FResizePayload
	{
		std::uint32_t Width = 1;
		std::uint32_t Height = 1;
		float DeviceScaleFactor = 1.0f;
	};

	struct FMouseMovePayload
	{
		std::int32_t X = 0;
		std::int32_t Y = 0;
		std::uint32_t Modifiers = 0;
		std::uint8_t Leave = 0;
		std::uint8_t Reserved[3] = {0, 0, 0};
	};

	struct FMouseButtonPayload
	{
		std::int32_t X = 0;
		std::int32_t Y = 0;
		std::uint32_t Modifiers = 0;
		std::uint8_t Button = MouseButtonLeft;
		std::uint8_t MouseUp = 0;
		std::uint8_t ClickCount = 1;
		std::uint8_t Reserved = 0;
	};

	struct FMouseWheelPayload
	{
		std::int32_t X = 0;
		std::int32_t Y = 0;
		std::int32_t DeltaX = 0;
		std::int32_t DeltaY = 0;
		std::uint32_t Modifiers = 0;
	};

	struct FKeyPayload
	{
		std::uint8_t Type = KeyEventRawKeyDown;
		std::uint8_t IsSystemKey = 0;
		std::uint16_t Reserved = 0;
		std::uint32_t WindowsKeyCode = 0;
		std::uint32_t NativeKeyCode = 0;
		std::uint32_t Character = 0;
		std::uint32_t UnmodifiedCharacter = 0;
		std::uint32_t Modifiers = 0;
	};

	struct FFrameSlot
	{
		// The producer stores all metadata first and publishes Sequence last.
		// Consumers must copy the sequence, metadata and sequence again before
		// consuming pixels.  This prevents resize/frame tearing.
		std::uint64_t Sequence = 0;
		std::uint64_t FrameId = 0;
		std::uint64_t TimestampUs = 0;
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
		std::uint32_t Format = 0; // 1 = BGRA8 premultiplied
		std::uint32_t Flags = FrameNone;
		std::uint32_t TextureSlot = 0;
		std::uint32_t Reserved = 0;
		std::uint64_t GpuFenceValue = 0;
		std::uint64_t HostTextureHandle = 0; // host-owned handle/token, never CEF's transient handle
		std::uint32_t PixelBytes = 0;
		std::int32_t DirtyLeft = 0;
		std::int32_t DirtyTop = 0;
		std::int32_t DirtyRight = 0;
		std::int32_t DirtyBottom = 0;
	};

	struct FFrameRingHeader
	{
		std::uint32_t Magic = kMagic;
		std::uint16_t Version = kVersion;
		std::uint16_t HeaderSize = sizeof(FFrameRingHeader);
		std::uint32_t StructSize = sizeof(FFrameRingHeader);
		std::uint32_t InstanceId = 0;
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
		std::uint32_t SlotCount = kFrameSlotCount;
		std::uint32_t SlotStride = 0;
		std::uint64_t LatestSequence = 0;
		std::uint32_t LatestSlot = 0;
		std::uint32_t Flags = 0;
	};
#pragma pack(pop)
	static_assert(sizeof(FMessageHeader) == 24, "Protocol header ABI changed");
	static_assert(sizeof(FFrameSlot) == 84, "Frame slot ABI changed");
	static_assert(sizeof(FMouseMovePayload) == 16, "Mouse move ABI changed");
	static_assert(sizeof(FMouseButtonPayload) == 16, "Mouse button ABI changed");
	static_assert(sizeof(FMouseWheelPayload) == 20, "Mouse wheel ABI changed");
	static_assert(sizeof(FKeyPayload) == 24, "Key payload ABI changed");

	inline bool IsValid(const FMessageHeader& Header, std::size_t AvailableBytes)
	{
		return Header.Magic == kMagic && Header.Version == kVersion &&
			Header.Size >= sizeof(FMessageHeader) && Header.Size <= AvailableBytes &&
			Header.Size <= kMaxMessageBytes;
	}

	inline bool IsValid(const FFrameRingHeader& Header, std::size_t AvailableBytes)
	{
		return Header.Magic == kMagic && Header.Version == kVersion &&
			Header.HeaderSize >= sizeof(FFrameRingHeader) &&
			Header.StructSize >= sizeof(FFrameRingHeader) &&
			Header.SlotCount > 0 && Header.SlotCount <= 8 &&
			Header.SlotStride >= sizeof(FFrameSlot) &&
			static_cast<std::size_t>(Header.HeaderSize) <= AvailableBytes;
	}

}
}
