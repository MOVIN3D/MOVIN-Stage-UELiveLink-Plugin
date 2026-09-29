using UnrealBuildTool;

public class MOVINStageLiveLink : ModuleRules
{
	public MOVINStageLiveLink(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Precompile for game targets too, so the prebuilt package (Scripts/Build-Prebuilt.ps1) lets a
		// Blueprint-only project package a game without compiling the plugin itself.
		PrecompileForTargets = PrecompileTargetsType.Any;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"LiveLinkAnimationCore",
			"LiveLinkInterface"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Engine",
			"LiveLink",
			"Networking",
			"Sockets",
			"Slate",
			"SlateCore"
		});
	}
}
