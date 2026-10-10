# IPC protocol

The protocol is defined in `Shared/BrowserProtocol/BrowserProtocol.h`. Every
message starts with `FMessageHeader` and is rejected unless `magic`, protocol
version, struct size and payload size are valid. Frame mappings start with
`FFrameRingHeader` and contain three `FFrameSlot` records followed by pixel
storage. Producers publish metadata before the sequence number; consumers read
the sequence twice to avoid tearing during resize or slot reuse.

Control messages use a named pipe whose name is derived from the instance id.
Large pixel buffers never travel through the pipe. Frame metadata contains
width, height, format, slot, frame id, sequence, timestamp, dirty rectangle and
flags. `FrameGpuSharedTexture` marks a host-owned shared-handle frame;
`FrameCpuFallback` marks an `OnPaint` frame. The transient handle supplied by
CEF is never published directly because CEF may recycle it as soon as
`OnAcceleratedPaint` returns.

The command set covers create/destroy, URL/HTML navigation, back/forward,
reload/stop, resize, focus/visibility, frame rate, JavaScript, devtools and
shutdown. Input and browser event message types are reserved in the same enum
so adding keyboard/IME and JS request/response messages does not change the
transport ABI.

Windows input payloads are now binary POD records for mouse move/button/wheel
and CEF key events. `BrowserHost/tests/input_smoke.py` starts an offscreen host,
sends mouse, right-click, double-click, text, Unicode, shortcut, wheel and
slider-drag input through the named pipe, then checks DOM markers in the shared
BGRA ring. It is a transport/DOM regression test and does not inject OS input.
