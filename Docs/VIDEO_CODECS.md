# Video codecs

The current runtime is a custom CEF build with the proprietary codec flags
listed in `Docs/CEF_VERSION.md`. MP4, YouTube and H.264 WebRTC Pixel Streaming
are technically enabled, but the
BrowserHost exposes `--allow-media-capture` for trusted `getUserMedia` pages
and `--enable-pixel-streaming` for autoplay/background-throttling behavior;
those switches cannot add a missing codec.

The CEF distribution, headers, `libcef.lib`, resources and BrowserHost were
replaced as one matching set. Third-party packages that advertise H.264 are not
drop-in replacements: their CEF major version, wrapper ABI, Chromium build flags
and license terms differ. They must not be mixed with `ThirdParty/CEF`.

For future upgrades, rebuild a matching custom CEF/Chromium runtime with
`proprietary_codecs=true`, `ffmpeg_branding="Chrome"` and `rtc_use_h264=true`, or
route H.264 through a separately licensed native media pipeline. The standalone
build documents are the starting point; they do not make codec distribution
legally safe.

At runtime test `RTCRtpSender.getCapabilities('video')` and
`RTCRtpReceiver.getCapabilities('video')` for `video/H264` and verify the
actual SDP offer/answer. WebRTC hardware encoder availability is GPU/driver
dependent and is separate from CEF's OSR shared-texture path. RTSP remains a
second-stage feature and is intentionally outside this MVP.

```javascript
const caps = RTCRtpSender.getCapabilities?.('video')?.codecs ?? [];
console.log(caps.map(c => `${c.mimeType}/${c.profile || ''}`));
```

Pixel Streaming must also be tested with the target Unreal signalling server,
hardware encoder and browser GPU policy; codec capability alone does not prove
that a stream can be negotiated or rendered.
