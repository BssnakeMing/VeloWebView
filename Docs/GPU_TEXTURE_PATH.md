# GPU texture path

CEF's Windows `CefAcceleratedPaintInfo.shared_texture_handle` is a temporary
NT handle. It must be consumed inside `OnAcceleratedPaint`; CEF may return a
different pooled handle on the next frame. The planned production path is:

```text
CEF shared D3D11 texture
  -> BrowserHost D3D11 ring (optional copy/fence)
  -> duplicated/shared NT handle
  -> UE D3D12 OpenSharedHandle
  -> UE-owned RHI texture (CopyTextureRegion)
  -> Slate/UMG/Material
```

`IBrowserTextureBridge` will hide the D3D12 implementation and leave room for
D3D11/Vulkan bridges. Until that bridge is enabled, `--disable-gpu` or an
unsupported adapter selects the explicit `OnPaint` CPU ring. That fallback is
kept for diagnostics and compatibility; it must not be mistaken for the normal
high-performance path.
