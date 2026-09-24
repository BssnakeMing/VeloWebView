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

		PublicIncludePaths.AddRange(new string[]
		{
			ModuleDirectory + "/Public"
		});

		PrivateIncludePaths.AddRange(new string[]
		{
			ModuleDirectory + "/Private"
		});
	}
}
