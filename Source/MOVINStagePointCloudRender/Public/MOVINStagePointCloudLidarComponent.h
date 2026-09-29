#pragma once

#include "CoreMinimal.h"
#include "LidarPointCloudComponent.h"
#include "LidarPointCloudShared.h"

#include "MOVINStagePointCloudLidarComponent.generated.h"

class FMOVINStagePointCloudStream;
class ULidarPointCloud;

/**
 * Renders the MOVIN Stage point cloud through the engine's Lidar Point Cloud renderer.
 *
 * This is the drop-in path: it needs no authored asset, so placing the component is enough to see
 * points. It reuses a production point renderer, which brings screen space point sizing, circular
 * sprites and ray tracing support for free.
 *
 * The trade-off against UMOVINStagePointCloudComponent (the Niagara path) is memory traffic: a
 * FLidarPointCloudPoint is 20 bytes where only 12 carry data, and the renderer's buffer update
 * copies per element rather than in bulk. At a few hundred thousand points that costs several
 * milliseconds of game thread time per received frame - fine at 20 fps, but the Niagara path is
 * cheaper when you have an authored system.
 */
UCLASS(ClassGroup = (MOVINStage), meta = (BlueprintSpawnableComponent), DisplayName = "MOVIN Stage Point Cloud (Lidar Renderer)")
class MOVINSTAGEPOINTCLOUDRENDER_API UMOVINStagePointCloudLidarComponent : public ULidarPointCloudComponent
{
	GENERATED_BODY()

public:
	// ULidarPointCloudComponent has no FObjectInitializer constructor, so this cannot forward one.
	UMOVINStagePointCloudLidarComponent();

	//~ UActorComponent interface
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	UFUNCTION(BlueprintPure, Category = "MOVIN Stage Point Cloud")
	int32 GetLastFramePointCount() const { return LastFramePointCount; }

	UFUNCTION(BlueprintPure, Category = "MOVIN Stage Point Cloud")
	int32 GetLastFrameSeq() const { return LastFrameSeq; }

	UFUNCTION(BlueprintPure, Category = "MOVIN Stage Point Cloud")
	bool IsStreamConnected() const;

public:
	/** Upper bound on points held per frame. Sizes the scratch buffer so ticking never allocates. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MOVIN Stage Point Cloud", meta = (ClampMin = "1"))
	int32 MaxPoints = 331776;

	/**
	 * Which LiveLink source to read from. Leave at the default invalid GUID to follow whichever
	 * source most recently started streaming point clouds.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MOVIN Stage Point Cloud|Advanced")
	FGuid SourceGuidFilter;

	/** Extra room, in centimetres, added around the frame extents when sizing the cloud volume. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MOVIN Stage Point Cloud|Advanced", meta = (ClampMin = "0.0"))
	float BoundsPaddingCm = 200.0f;

	/**
	 * Smallest half extent used for the cloud volume, in centimetres.
	 *
	 * The volume is always centred on the component origin so inserted positions are used verbatim;
	 * this floor stops an early sparse frame from producing a tiny volume.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MOVIN Stage Point Cloud|Advanced", meta = (ClampMin = "1.0"))
	float MinimumHalfExtentCm = 500.0f;

private:
	bool ResolveStream();
	void PushFrame(const struct FMOVINStagePointCloudFrame& Frame);

	UPROPERTY(Transient)
	TObjectPtr<ULidarPointCloud> DynamicCloud;

	/** Reused so a tick never allocates. */
	TArray<FLidarPointCloudPoint> ScratchPoints;

	TSharedPtr<FMOVINStagePointCloudStream, ESPMode::ThreadSafe> Stream;

	int32 LastFramePointCount = 0;
	int32 LastFrameSeq = -1;
};
