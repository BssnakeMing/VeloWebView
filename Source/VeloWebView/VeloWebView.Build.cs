using System;
using System.IO;
using UnrealBuildTool;

public class VeloWebView : ModuleRules
{
	public VeloWebView(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp17;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"SlateCore",
			"UMG"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ApplicationCore",
			"InputCore",
			"Slate",
			"Projects"
		});

		// Android keeps using UE's Slate browser implementation. Win64
		// composited views use the out-of-process CEF BrowserHost connection and
		// must not load the WebBrowser module (and its in-process CEF runtime).
		if (Target.Platform == UnrealTargetPlatform.Android)
		{
			PrivateDependencyModuleNames.Add("WebBrowser");
		}

		PublicIncludePaths.AddRange(new string[]
		{
			ModuleDirectory + "/Public"
		});

		PrivateIncludePaths.AddRange(new string[]
		{
			ModuleDirectory + "/Private",
			Path.Combine(PluginDirectory, "Shared")
		});

		// Host.html is a runtime template rather than a cooked Unreal asset.  A
		// packaged Android build cannot read the plugin checkout path, so stage a
		// copy into the project's Content directory and let the platform file
		// system resolve it from the pak/OBB at runtime.
		RuntimeDependencies.Add(
			"$(ProjectDir)/Content/VeloWebView/Host.html",
			Path.Combine(PluginDirectory, "Content", "Host.html"),
			StagedFileType.UFS);
		RuntimeDependencies.Add(
			"$(ProjectDir)/Content/VeloWebView/CodecTest.html",
			Path.Combine(PluginDirectory, "Content", "CodecTest.html"),
			StagedFileType.UFS);

		// BrowserHost is an out-of-process application.  When the host has been
		// built and the downloaded CEF distribution is present, stage the host
		// beside the game binaries together with the complete CEF runtime.  The
		// Unreal process never links against libcef; these files are consumed only
		// by VeloWebViewBrowserHost.exe.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			string[] HostCandidates =
			{
				Path.Combine(PluginDirectory, "BrowserHost", "build", "Release", "VeloWebViewBrowserHost.exe"),
				Path.Combine(PluginDirectory, "BrowserHost", "VeloWebViewBrowserHost.exe"),
				Path.Combine(PluginDirectory, "Binaries", "Win64", "VeloWebViewBrowserHost.exe")
			};
			string HostPath = null;
			foreach (string Candidate in HostCandidates)
			{
				if (File.Exists(Candidate))
				{
					HostPath = Candidate;
					break;
				}
			}
			if (!string.IsNullOrEmpty(HostPath))
			{
				const string HostFileName = "VeloWebViewBrowserHost.exe";
				// Editor modules are DLLs emitted into the plugin's Binaries folder,
				// while a game/shipping target emits its executable dependencies into
				// the project's Binaries folder. Keep the host beside the executable
				// for game targets so CEF can be loaded from the same directory.
				string HostTarget = Target.Type == TargetType.Editor
					? "$(PluginDir)/Binaries/Win64/" + HostFileName
					: "$(BinaryOutputDir)/" + HostFileName;
				string PackagedHostPath = Path.Combine(PluginDirectory, "Binaries", "Win64", HostFileName);

				// A packaged plugin already contains the host in the editor module's
				// output directory. Registering that file as a source/target copy makes
				// UBT map the same path to itself and fail. Game targets intentionally
				// copy it from the plugin into the project's Binaries folder.
				if (Target.Type == TargetType.Editor && string.Equals(
					Path.GetFullPath(HostPath),
					Path.GetFullPath(PackagedHostPath),
					StringComparison.OrdinalIgnoreCase))
				{
					RuntimeDependencies.Add(HostTarget, StagedFileType.NonUFS);
				}
				else
				{
					RuntimeDependencies.Add(HostTarget, HostPath);
				}
			}
			string CefRoot = Path.Combine(PluginDirectory, "ThirdParty", "CEF");
			string CefRelease = Path.Combine(CefRoot, "Release");
			// Keep the executable and every CEF runtime file in one directory. In
			// the editor this is the plugin's staged bundle; game targets place the
			// bundle beside the packaged executable. Splitting chrome_elf.dll from
			// libcef.dll lets an older Unreal WebBrowser DLL be picked up at runtime.
			string CefTargetDirectory = Target.Type == TargetType.Editor
				? "$(PluginDir)/Binaries/Win64"
				: "$(BinaryOutputDir)";
			if (Directory.Exists(CefRelease))
			{
				foreach (string RuntimeFile in Directory.GetFiles(CefRelease, "*", SearchOption.TopDirectoryOnly))
				{
					string Extension = Path.GetExtension(RuntimeFile).ToLowerInvariant();
					if (Extension == ".dll" || Extension == ".dat" || Extension == ".bin" ||
						Extension == ".pak" || Extension == ".json")
					{
						RuntimeDependencies.Add(
							CefTargetDirectory + "/" + Path.GetFileName(RuntimeFile),
							RuntimeFile);
					}
				}
			}

			string CefResources = Path.Combine(CefRoot, "Resources");
			if (Directory.Exists(CefResources))
			{
				foreach (string ResourceFile in Directory.GetFiles(CefResources, "*", SearchOption.AllDirectories))
				{
					string Relative = ResourceFile.Substring(CefResources.Length).TrimStart(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
					RuntimeDependencies.Add(
						CefTargetDirectory + "/" + Relative.Replace('\\', '/'),
						ResourceFile);
				}
			}
		}
	}
}
