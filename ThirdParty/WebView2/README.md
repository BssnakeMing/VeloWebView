# WebView2 SDK staging

The staged files in this checkout came from the official Microsoft
`Microsoft.Web.WebView2` NuGet package. If you receive a source-only copy, run
`Scripts/Install-WebView2SDK.ps1` from the plugin root to download and stage
the header and x64 loader. The Windows `Build.cs` detects `include/WebView2.h`
and enables the SDK path only when it is present.

Expected layout:

```text
ThirdParty/WebView2/
  include/WebView2.h
  lib/x64/WebView2Loader.lib       (optional)
  bin/x64/WebView2Loader.dll       (stage for packaged builds)
```

The end user also needs the official Evergreen WebView2 Runtime. Do not commit
files obtained from an untrusted mirror.
