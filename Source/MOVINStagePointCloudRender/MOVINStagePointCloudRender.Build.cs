// Copyright MOVIN. All Rights Reserved.

using UnrealBuildTool;

public class MOVINStagePointCloudRender : ModuleRules
{
	public MOVINStagePointCloudRender(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Precompile for game targets too, so the prebuilt package (Scripts/Build-Prebuilt.ps1) lets a
		// Blueprint-only project package a game without compiling the plugin itself.
		PrecompileForTargets = PrecompileTargetsType.Any;

		// Niagara and LidarPointCloudRuntime are public because the two renderer components derive
		// from UNiagaraComponent and ULidarPointCloudComponent respectively.
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Niagara",
			"LidarPointCloudRuntime",
			"MOVINStageLiveLink"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"RenderCore",
			"RHI",
			"Projects"
		});
	}
}
