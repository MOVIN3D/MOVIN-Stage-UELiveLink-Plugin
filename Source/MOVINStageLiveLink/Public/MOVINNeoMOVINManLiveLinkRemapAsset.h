#pragma once

#include "LiveLinkRemapAsset.h"

#include "MOVINNeoMOVINManLiveLinkRemapAsset.generated.h"

/**
 * Retarget asset required by the Live Link Pose node when driving a NeoMOVINMan rig from a
 * MOVIN Stage subject. Translates the OSC bone names to the NeoMOVINMan convention.
 *
 * The display name states the product because the Retarget Asset dropdown also lists the stock
 * LiveLinkRemapAsset, and a project may be running the MOVIN LiveLink (Tracin) plugin at the same
 * time. Picking this one for a non-Stage subject leaves the character half posed with no error, so
 * the first frame logs a warning when nothing matched - see BuildPoseFromAnimationData.
 */
UCLASS(BlueprintType, Blueprintable, DisplayName = "MOVIN Stage OSC Remap (NeoMOVINMan)")
class MOVINSTAGELIVELINK_API UMOVINNeoMOVINManLiveLinkRemapAsset : public ULiveLinkRemapAsset
{
	GENERATED_BODY()

public:
	virtual FName GetRemappedBoneName_Implementation(FName BoneName) const override;
	virtual void BuildPoseFromAnimationData(float DeltaTime, const FLiveLinkSkeletonStaticData* InSkeletonData, const FLiveLinkAnimationFrameData* InFrameData, FCompactPose& OutPose) override;

	/**
	 * Place the body where the stream says it is: Root takes the streamed ground position and Hips
	 * the streamed pelvis offset above it. Hips is included deliberately - the stream's root frame is
	 * turned 90 degrees from the mesh's RootBone bind frame, so leaving Hips on its authored offset
	 * would put the pelvis 86 cm in front of the root instead of 94 cm above it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MOVIN Stage")
	bool bApplySourceRootTranslation = true;

	/**
	 * Overwrite every other bone's translation with the streamed one as well.
	 *
	 * Off by default so the character keeps the mesh's own bone lengths and only rotations come from
	 * the stream. MOVIN Stage streams bone offsets calibrated to the performer - on the reference
	 * capture the upper arm was 1.25x and the shin 0.91x the NeoMOVINMan lengths - and applying them
	 * both stretches the skin and, because they can change with recalibration, moves the reference an
	 * IK Retargeter measures limb extension against. Turn on to reproduce the performer's proportions
	 * exactly, at the cost of those two effects.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MOVIN Stage")
	bool bApplySourceBoneTranslations = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MOVIN Stage")
	bool bLogFirstFrame = true;

private:
	bool bDidLogFirstFrame = false;
};
