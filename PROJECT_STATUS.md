# Project status

## Completed

- Added a versioned, magic/size-checked shared protocol for BrowserHost and UE.
- Replaced the Win64 runtime with the standalone CEF 154 Release/non-component
  distribution; `ThirdParty/CEF/CEF_VERSION.json` records the custom build and
  `Release/libcef.dll` SHA-256.
- Added architecture, IPC, GPU-path, codec and third-party license notes.
- Added a named-pipe BrowserHost command server and CPU frame-mapping ring for
  the first IPC milestone. The Win64 Slate widget now consumes that ring and
  uploads BGRA frames to a transient Slate texture. `--allow-media-capture` is
  available for trusted WebRTC pages and `--enable-pixel-streaming` controls
  autoplay/background throttling.

## In progress

- UE D3D12 shared-texture path and GPU fence/ring synchronization.

## Not yet complete

- UE D3D12 `OpenSharedHandle` bridge and GPU fence/ring synchronization.
- Host-owned D3D11 texture ring and named GPU-handle transport (the current
  host publishes accelerated-frame metadata but does not yet copy CEF's
  transient handle into a cross-process GPU ring).
- Full IME/keyboard composition, JS allow-list request/response, popup texture,
  devtools and shipping staging tests.
- Standalone Consumer.exe and automated milestone tests.

## Known issues

- CEF binaries are downloaded during setup and are intentionally untracked.
- The fallback software ring is intentionally not a production 60 FPS GPU path.
- The current custom CEF runtime reports H.264 capability and passed a real
  H.264/AAC MP4 playback smoke test; codec licensing remains a release gate.
- The existing Native Overlay implementation remains the optional WebView2 path;
  it is independent of the CEF BrowserHost process.

## Next step

Run the BrowserHost input/video regression tests after CEF upgrades, then
continue the D3D12 shared-texture bridge and packaging validation.
