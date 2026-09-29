#pragma once

#include "CoreMinimal.h"
#include "LiveLinkSourceSettings.h"

#include "MOVINStageLiveLinkSourceSettings.generated.h"

UENUM()
enum class EMOVINOscCoordinateSystem : uint8
{
	/** Sender frame with +X right, +Y up, +Z forward. */
	Unity,

	/** Sender frame with +X left, +Y up, +Z forward, as used by the MOVIN Python viewer. */
	OpenGL
};

UENUM()
enum class EMOVINOscPointCloudCoordinateMode : uint8
{
	/** Follow the motion coordinate system. Makes a cloud/skeleton mismatch impossible. */
	Inherit,
	Unity,
	OpenGL
};

UENUM()
enum class EMOVINOscFingerNameMode : uint8
{
	/** Use the incoming OSC finger bone names unchanged. */
	Osc,

	/** Remap finger names to the MOVIN Man / Notch convention. */
	MovinManNotch
};

/**
 * Settings shown for a MOVIN Stage source in the Live Link panel.
 *
 * The factory is a plain menu entry with no creation dialog, so this is where the source is
 * configured after it has been added. Editing any value here restarts the receiver and rewrites
 * ConnectionString, so a Live Link preset saved afterwards captures the change.
 */
UCLASS()
class MOVINSTAGELIVELINK_API UMOVINStageLiveLinkSourceSettings : public ULiveLinkSourceSettings
{
	GENERATED_BODY()

public:
	//~ Motion

	/** UDP endpoint to listen on. Both motion and point cloud arrive here. */
	UPROPERTY(EditAnywhere, Category = "MOVIN Stage|Motion")
	FString Endpoint = TEXT("0.0.0.0:11235");

	/**
	 * Prepended to every published subject name, for example "Stage_" to publish "Stage_MOVIN_0".
	 *
	 * Subjects are created automatically, one per actor name the stream carries, so there is no
	 * actor list here: whatever MOVIN Stage sends appears in the Live Link panel on its first packet.
	 *
	 * Live Link resolves subjects by name alone, so two sources publishing the same name - a second
	 * MOVIN product streaming into the same project, most likely - leave the Anim Blueprint picking
	 * between them arbitrarily. A prefix makes this source's subjects unambiguous.
	 *
	 * Empty by default so existing subject names and Anim Blueprint references are untouched. Only
	 * the published name changes; the incoming actor name is never altered.
	 */
	UPROPERTY(EditAnywhere, Category = "MOVIN Stage|Motion")
	FString SubjectPrefix;

	/** Metres to centimetres. */
	UPROPERTY(EditAnywhere, Category = "MOVIN Stage|Motion", meta = (ClampMin = "0.001"))
	float Scale = 100.0f;

	/** Seconds without traffic before the source reports "No motion". */
	UPROPERTY(EditAnywhere, Category = "MOVIN Stage|Motion", meta = (ClampMin = "0.01"))
	float TimeoutSeconds = 1.0f;

	UPROPERTY(EditAnywhere, Category = "MOVIN Stage|Motion")
	EMOVINOscCoordinateSystem Coordinates = EMOVINOscCoordinateSystem::Unity;

	UPROPERTY(EditAnywhere, Category = "MOVIN Stage|Motion")
	EMOVINOscFingerNameMode FingerNames = EMOVINOscFingerNameMode::MovinManNotch;

	/** Applies a stream level rotation offset to bone rotations. Never affects positions. */
	UPROPERTY(EditAnywhere, Category = "MOVIN Stage|Motion")
	bool bApplyRotationOffset = true;

	/** Unity Euler degrees, applied when bApplyRotationOffset is set. */
	UPROPERTY(EditAnywhere, Category = "MOVIN Stage|Motion", meta = (EditCondition = "bApplyRotationOffset"))
	FVector RotationOffset = FVector(0.0, -90.0, -90.0);

	//~ Point cloud

	/** Receive and reassemble /MOVIN/PointCloud. Off reproduces the motion only behaviour exactly. */
	UPROPERTY(EditAnywhere, Category = "MOVIN Stage|Point Cloud")
	bool bEnablePointCloud = false;

	/** Upper bound on points per frame. A multiple of 1024 avoids a partial renderer texture row. */
	UPROPERTY(EditAnywhere, Category = "MOVIN Stage|Point Cloud", meta = (EditCondition = "bEnablePointCloud", ClampMin = "1024"))
	int32 MaxPoints = 331776;

	UPROPERTY(EditAnywhere, Category = "MOVIN Stage|Point Cloud", meta = (EditCondition = "bEnablePointCloud"))
	EMOVINOscPointCloudCoordinateMode PointCloudCoordinates = EMOVINOscPointCloudCoordinateMode::Inherit;

	/** Zero inherits the motion Scale. */
	UPROPERTY(EditAnywhere, Category = "MOVIN Stage|Point Cloud", meta = (EditCondition = "bEnablePointCloud", ClampMin = "0.0"))
	float PointCloudScale = 0.0f;

	/** Translation applied in Unreal space, folded into the conversion loop for free. */
	UPROPERTY(EditAnywhere, Category = "MOVIN Stage|Point Cloud", meta = (EditCondition = "bEnablePointCloud"))
	FVector PointCloudOffset = FVector::ZeroVector;

	/** Must match the sender's --osc_address. */
	UPROPERTY(EditAnywhere, Category = "MOVIN Stage|Point Cloud", meta = (EditCondition = "bEnablePointCloud"))
	FString PointCloudAddress = TEXT("/MOVIN/PointCloud");

	/** Publish frames that are still missing chunks rather than dropping them. */
	UPROPERTY(EditAnywhere, Category = "MOVIN Stage|Point Cloud", meta = (EditCondition = "bEnablePointCloud"))
	bool bPointCloudPartialPublish = true;

	/** A backwards sequence jump larger than this is treated as a sender restart. */
	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "MOVIN Stage|Point Cloud", meta = (EditCondition = "bEnablePointCloud", ClampMin = "1"))
	int32 SeqResyncThreshold = 100;

	//~ Network

	/** SO_RCVBUF request. One point cloud frame arrives as a single multi megabyte burst. */
	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "MOVIN Stage|Network", meta = (ClampMin = "65536"))
	int32 ReceiveBufferBytes = 32 * 1024 * 1024;

	/** Datagrams drained per readable notification. Must cover one frame burst. */
	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "MOVIN Stage|Network", meta = (ClampMin = "1"))
	int32 MaxDatagramsPerWait = 8192;
};
