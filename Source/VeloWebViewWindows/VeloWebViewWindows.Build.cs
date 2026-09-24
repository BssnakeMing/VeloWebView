using UnrealBuildTool;
using System;
using System.IO;

/// <summary>
/// Build rules for the optional WebView2 implementation.  The plugin is still
/// buildable on a machine which does not have the WebView2 SDK checked in: in
/// that case the module compiles a small, explicit runtime-error fallback.
/// </summary>
public class VeloWebViewWindows : ModuleRules
{
	private static bool IsWin64PE(string FilePath)
	{
		try
		{
			byte[] Bytes = File.ReadAllBytes(FilePath);
			if (Bytes.Length < 0x40 || Bytes[0] != (byte)'M' || Bytes[1] != (byte)'Z')
			{
				return false;
			}

			int PeOffset = BitConverter.ToInt32(Bytes, 0x3C);
			if (PeOffset < 0 || PeOffset > Bytes.Length - 6 ||
				Bytes[PeOffset] != (byte)'P' || Bytes[PeOffset + 1] != (byte)'E' ||
				Bytes[PeOffset + 2] != 0 || Bytes[PeOffset + 3] != 0)
			{
				return false;
			}

			return BitConverter.ToUInt16(Bytes, PeOffset + 4) == 0x8664;
		}
		catch
		{
			return false;
		}
	}

	public VeloWebViewWindows(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"ApplicationCore",
				"VeloWebView"
			});

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"SlateCore",
				"Json"
			});

		// Keep all WebView2 paths relative to the plugin.  This makes packaged
		// builds independent of an installed Visual Studio SDK location.
		string WebView2Root = Path.GetFullPath(Path.Combine(ModuleDirectory, "../../ThirdParty/WebView2"));
		string IncludePath = Path.Combine(WebView2Root, "include");
		string HeaderPath = Path.Combine(IncludePath, "WebView2.h");
		bool bHasWebView2Headers = File.Exists(HeaderPath);

		// The source uses this definition instead of testing for a header.  UBT
		// and IntelliSense therefore see the same configuration as the compiler.
		PublicDefinitions.Add("VELO_WEBVIEW_HAS_WEBVIEW2=" + (bHasWebView2Headers ? "1" : "0"));

		if (bHasWebView2Headers)
		{
			PublicSystemIncludePaths.Add(IncludePath);
		}

		// WebView2Loader is loaded with LoadLibrary/GetProcAddress.  An import
		// library is optional and is deliberately not required for compilation.
		// Stage the official redistributable DLL when it has been supplied.
		string[] LoaderCandidates =
		{
			Path.Combine(WebView2Root, "bin", "Win64", "WebView2Loader.dll"),
			Path.Combine(WebView2Root, "bin", "x64", "WebView2Loader.dll"),
			Path.Combine(WebView2Root, "WebView2Loader.dll")
		};

		foreach (string LoaderPath in LoaderCandidates)
		{
			if (File.Exists(LoaderPath))
			{
				if (Target.Platform == UnrealTargetPlatform.Win64 && !IsWin64PE(LoaderPath))
				{
					throw new BuildException("WebView2Loader.dll is not an x64 (AMD64) PE image: {0}. Replace it with the official win-x64 loader.", LoaderPath);
				}
				RuntimeDependencies.Add("$(BinaryOutputDir)/WebView2Loader.dll", LoaderPath);
				break;
			}
		}

		// Some SDK distributions ship the loader import library.  Linking it is
		// not necessary for this implementation, but adding it when present keeps
		// the module friendly to projects which use delay/import symbols elsewhere.
		string[] LibraryCandidates =
		{
			Path.Combine(WebView2Root, "lib", "Win64", "WebView2LoaderStatic.lib"),
			Path.Combine(WebView2Root, "lib", "x64", "WebView2LoaderStatic.lib"),
			Path.Combine(WebView2Root, "lib", "Win64", "WebView2Loader.lib"),
			Path.Combine(WebView2Root, "lib", "x64", "WebView2Loader.lib")
		};

		foreach (string LibraryPath in LibraryCandidates)
		{
			if (File.Exists(LibraryPath))
			{
				PublicAdditionalLibraries.Add(LibraryPath);
				break;
			}
		}

		if (Target.Platform != UnrealTargetPlatform.Win64)
		{
			// The module is normally allow-listed for Win64 in the uplugin.  This
			// definition also makes accidental cross-compilation harmless.
			PublicDefinitions.Add("VELO_WEBVIEW_WINDOWS_UNSUPPORTED=1");
		}
	}
}
