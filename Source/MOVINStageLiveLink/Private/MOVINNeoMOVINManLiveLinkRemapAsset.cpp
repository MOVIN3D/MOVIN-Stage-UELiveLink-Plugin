#include "MOVINNeoMOVINManLiveLinkRemapAsset.h"

#include "BonePose.h"
#include "MOVINStageLiveLinkLog.h"
#include "Roles/LiveLinkAnimationTypes.h"

namespace
{
	/**
	 * Root and Hips are position, everything below them is a bone length.
	 *
	 * Hips travels with Root rather than with the limbs because its streamed translation is the
	 * pelvis height in the stream's root frame, and the remap replaces RootBone's rotation with the
	 * streamed one. Keeping the mesh's authored Hips offset in that frame would aim it sideways: on
	 * NeoMOVINMan_UE the pelvis lands 86 cm in front of the root instead of 94 cm above it.
	 */
	bool ShouldApplySourceTranslation(FName SourceBoneName, bool bApplySourceRootTranslation, bool bApplySourceBoneTranslations)
	{
		if (bApplySourceBoneTranslations)
		{
			return true;
		}

		return bApplySourceRootTranslation && (SourceBoneName == TEXT("Root") || SourceBoneName == TEXT("Hips"));
	}

	bool IsFingerBoneName(FName SourceBoneName)
	{
		const FString BoneName = SourceBoneName.ToString();
		return BoneName.Contains(TEXT("Thumb")) ||
			BoneName.Contains(TEXT("Index")) ||
			BoneName.Contains(TEXT("Middle")) ||
			BoneName.Contains(TEXT("Ring")) ||
			BoneName.Contains(TEXT("Little"));
	}

	const TArray<FName>& GetNeoMOVINManFingerBoneNames()
	{
		static const TArray<FName> FingerBoneNames = {
			TEXT("LeftHandThumb1"), TEXT("LeftHandThumb2"), TEXT("LeftHandThumb3"),
			TEXT("LeftHandIndex1"), TEXT("LeftHandIndex2"), TEXT("LeftHandIndex3"),
			TEXT("LeftHandMiddle1"), TEXT("LeftHandMiddle2"), TEXT("LeftHandMiddle3"),
			TEXT("LeftHandRing1"), TEXT("LeftHandRing2"), TEXT("LeftHandRing3"),
			TEXT("LeftHandPinky1"), TEXT("LeftHandPinky2"), TEXT("LeftHandPinky3"),

			TEXT("RightHandThumb1"), TEXT("RightHandThumb2"), TEXT("RightHandThumb3"),
			TEXT("RightHandIndex1"), TEXT("RightHandIndex2"), TEXT("RightHandIndex3"),
			TEXT("RightHandMiddle1"), TEXT("RightHandMiddle2"), TEXT("RightHandMiddle3"),
			TEXT("RightHandRing1"), TEXT("RightHandRing2"), TEXT("RightHandRing3"),
			TEXT("RightHandPinky1"), TEXT("RightHandPinky2"), TEXT("RightHandPinky3")
		};
		return FingerBoneNames;
	}

	void ResetTargetFingerBonesToRefPose(FCompactPose& OutPose)
	{
		const FBoneContainer& BoneContainer = OutPose.GetBoneContainer();
		for (const FName& TargetBoneName : GetNeoMOVINManFingerBoneNames())
		{
			const int32 MeshIndex = BoneContainer.GetPoseBoneIndexForBoneName(TargetBoneName);
			if (MeshIndex == INDEX_NONE)
			{
				continue;
			}

			const FCompactPoseBoneIndex CompactPoseIndex = BoneContainer.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshIndex));
			if (CompactPoseIndex == INDEX_NONE)
			{
				continue;
			}

			OutPose[CompactPoseIndex] = BoneContainer.GetRefPoseTransform(CompactPoseIndex);
		}
	}

	const TMap<FName, FName>& GetOscToNeoMOVINManBoneMap()
	{
		static const TMap<FName, FName> BoneMap = {
			{ TEXT("Root"), TEXT("RootBone") },
			{ TEXT("Hips"), TEXT("Hips") },
			{ TEXT("Spine"), TEXT("Spine") },
			{ TEXT("Chest"), TEXT("Spine1") },
			{ TEXT("Neck"), TEXT("Neck") },
			{ TEXT("Head"), TEXT("Head") },

			{ TEXT("LeftShoulder"), TEXT("LeftShoulder") },
			{ TEXT("LeftUpperArm"), TEXT("LeftArm") },
			{ TEXT("LeftLowerArm"), TEXT("LeftForeArm") },
			{ TEXT("LeftHand"), TEXT("LeftHand") },
			{ TEXT("RightShoulder"), TEXT("RightShoulder") },
			{ TEXT("RightUpperArm"), TEXT("RightArm") },
			{ TEXT("RightLowerArm"), TEXT("RightForeArm") },
			{ TEXT("RightHand"), TEXT("RightHand") },

			{ TEXT("LeftUpperLeg"), TEXT("LeftUpLeg") },
			{ TEXT("LeftLowerLeg"), TEXT("LeftLeg") },
			{ TEXT("LeftFoot"), TEXT("LeftFoot") },
			{ TEXT("LeftToes"), TEXT("LeftToeBase") },
			{ TEXT("RightUpperLeg"), TEXT("RightUpLeg") },
			{ TEXT("RightLowerLeg"), TEXT("RightLeg") },
			{ TEXT("RightFoot"), TEXT("RightFoot") },
			{ TEXT("RightToes"), TEXT("RightToeBase") },

			{ TEXT("LeftThumbProximal"), TEXT("LeftHandThumb1") },
			{ TEXT("LeftThumbIntermediate"), TEXT("LeftHandThumb2") },
			{ TEXT("LeftThumbDistal"), TEXT("LeftHandThumb3") },
			{ TEXT("LeftIndexProximal"), TEXT("LeftHandIndex1") },
			{ TEXT("LeftIndexIntermediate"), TEXT("LeftHandIndex2") },
			{ TEXT("LeftIndexDistal"), TEXT("LeftHandIndex3") },
			{ TEXT("LeftMiddleProximal"), TEXT("LeftHandMiddle1") },
			{ TEXT("LeftMiddleIntermediate"), TEXT("LeftHandMiddle2") },
			{ TEXT("LeftMiddleDistal"), TEXT("LeftHandMiddle3") },
			{ TEXT("LeftRingProximal"), TEXT("LeftHandRing1") },
			{ TEXT("LeftRingIntermediate"), TEXT("LeftHandRing2") },
			{ TEXT("LeftRingDistal"), TEXT("LeftHandRing3") },
			{ TEXT("LeftLittleProximal"), TEXT("LeftHandPinky1") },
			{ TEXT("LeftLittleIntermediate"), TEXT("LeftHandPinky2") },
			{ TEXT("LeftLittleDistal"), TEXT("LeftHandPinky3") },

			{ TEXT("RightThumbProximal"), TEXT("RightHandThumb1") },
			{ TEXT("RightThumbIntermediate"), TEXT("RightHandThumb2") },
			{ TEXT("RightThumbDistal"), TEXT("RightHandThumb3") },
			{ TEXT("RightIndexProximal"), TEXT("RightHandIndex1") },
			{ TEXT("RightIndexIntermediate"), TEXT("RightHandIndex2") },
			{ TEXT("RightIndexDistal"), TEXT("RightHandIndex3") },
			{ TEXT("RightMiddleProximal"), TEXT("RightHandMiddle1") },
			{ TEXT("RightMiddleIntermediate"), TEXT("RightHandMiddle2") },
			{ TEXT("RightMiddleDistal"), TEXT("RightHandMiddle3") },
			{ TEXT("RightRingProximal"), TEXT("RightHandRing1") },
			{ TEXT("RightRingIntermediate"), TEXT("RightHandRing2") },
			{ TEXT("RightRingDistal"), TEXT("RightHandRing3") },
			{ TEXT("RightLittleProximal"), TEXT("RightHandPinky1") },
			{ TEXT("RightLittleIntermediate"), TEXT("RightHandPinky2") },
			{ TEXT("RightLittleDistal"), TEXT("RightHandPinky3") }
		};

		return BoneMap;
	}
}

FName UMOVINNeoMOVINManLiveLinkRemapAsset::GetRemappedBoneName_Implementation(FName BoneName) const
{
	if (const FName* RemappedName = GetOscToNeoMOVINManBoneMap().Find(BoneName))
	{
		return *RemappedName;
	}

	return BoneName;
}

void UMOVINNeoMOVINManLiveLinkRemapAsset::BuildPoseFromAnimationData(float DeltaTime, const FLiveLinkSkeletonStaticData* InSkeletonData, const FLiveLinkAnimationFrameData* InFrameData, FCompactPose& OutPose)
{
	if (InSkeletonData == nullptr || InFrameData == nullptr)
	{
		return;
	}

	const int32 TransformCount = FMath::Min(InSkeletonData->BoneNames.Num(), InFrameData->Transforms.Num());
	int32 MatchedBoneCount = 0;
	int32 TranslatedBoneCount = 0;
	for (int32 Index = 0; Index < TransformCount; ++Index)
	{
		const FName SourceBoneName = InSkeletonData->BoneNames[Index];
		if (IsFingerBoneName(SourceBoneName))
		{
			continue;
		}

		const FName TargetBoneName = GetRemappedBoneName(SourceBoneName);
		const int32 MeshIndex = OutPose.GetBoneContainer().GetPoseBoneIndexForBoneName(TargetBoneName);
		if (MeshIndex == INDEX_NONE)
		{
			continue;
		}

		const FCompactPoseBoneIndex CompactPoseIndex = OutPose.GetBoneContainer().MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshIndex));
		if (CompactPoseIndex == INDEX_NONE)
		{
			continue;
		}

		++MatchedBoneCount;
		const FTransform& SourceTransform = InFrameData->Transforms[Index];
		FTransform TargetTransform = OutPose[CompactPoseIndex];
		TargetTransform.SetRotation(SourceTransform.GetRotation().GetNormalized());

		if (ShouldApplySourceTranslation(SourceBoneName, bApplySourceRootTranslation, bApplySourceBoneTranslations))
		{
			TargetTransform.SetTranslation(SourceTransform.GetTranslation());
			++TranslatedBoneCount;
		}

		OutPose[CompactPoseIndex] = TargetTransform;
	}

	ResetTargetFingerBonesToRefPose(OutPose);

	if (bLogFirstFrame && !bDidLogFirstFrame)
	{
		bDidLogFirstFrame = true;

		if (MatchedBoneCount == 0 && TransformCount > 0)
		{
			// Nothing matched at all. Either this asset is on a Live Link Pose node bound to a
			// subject from a different product - a project running both MOVIN plugins makes that a
			// one-click mistake, and the pose then looks broken rather than misconfigured - or the
			// target Skeleton is not a NeoMOVINMan rig. Both are settings, not stream faults.
			UE_LOG(LogMOVINStageLiveLink, Warning,
				TEXT("MOVIN Stage OSC remap matched none of the %d streamed bones against this Skeleton. ")
				TEXT("Check that the Live Link Pose node's Subject is a MOVIN Stage subject and that its ")
				TEXT("Skeleton is a NeoMOVINMan rig - this asset only maps the MOVIN Stage OSC bone names. ")
				TEXT("First streamed bone was '%s'."),
				TransformCount,
				*InSkeletonData->BoneNames[0].ToString());
		}
		else
		{
			UE_LOG(LogMOVINStageLiveLink, Log,
				TEXT("MOVIN Stage OSC remap active. SourceBones=%d FrameTransforms=%d Matched=%d Translated=%d RootTranslation=%s BoneTranslations=%s"),
				InSkeletonData->BoneNames.Num(),
				InFrameData->Transforms.Num(),
				MatchedBoneCount,
				TranslatedBoneCount,
				bApplySourceRootTranslation ? TEXT("true") : TEXT("false"),
				bApplySourceBoneTranslations ? TEXT("true") : TEXT("false"));
		}
	}
}
