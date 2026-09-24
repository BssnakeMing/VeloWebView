using System.IO;
using UnrealBuildTool;

/// <summary>
/// Android implementation for VeloWebView.  The module is intentionally kept
/// independent from the UMG module; the public bridge is consumed by the core
/// module through the normal platform abstraction (or via LoadModule).
/// </summary>
public class VeloWebViewAndroid : ModuleRules
{
	public VeloWebViewAndroid(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"VeloWebView"
			});

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				// The Android platform module implements the public core interface;
				// the core module discovers us at runtime, so this is not a cycle.
				"VeloWebView"
			});

		if (Target.Platform == UnrealTargetPlatform.Android)
		{
			// FAndroidApplication/FJavaWrapper live in these runtime modules.
			PrivateDependencyModuleNames.Add("ApplicationCore");
			PrivateDependencyModuleNames.Add("Launch");

			string UplPath = Path.Combine(ModuleDirectory, "Android", "VeloWebViewAndroid_UPL.xml");
			AdditionalPropertiesForReceipt.Add("AndroidPlugin", UplPath);
		}
	}
}
