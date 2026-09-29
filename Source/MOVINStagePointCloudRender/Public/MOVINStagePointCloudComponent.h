#pragma once

#include "CoreMinimal.h"
#include "NiagaraComponent.h"

#include <atomic>

#include "MOVINStagePointCloudComponent.generated.h"

class FMOVINStagePointCloudStream;
class UTexture2D;

/**
 * Renders the MOVIN Stage point cloud by feeding assembled frames into a Niagara GPU emitter
 * through a position texture.
 *
 * The component pulls the newest frame from the LiveLink source's side channel each tick, copies it
 * into a staging buffer, and uploads that buffer to a PF_A32B32G32R32F texture. The Niagara system
 * spawns MaxPoints particles once and each particle reads its position from the texture by
 * ExecIndex, masking itself to zero size when its index is past ActiveCount.
 *
 * Why a texture rather than a Niagara array data interface: SetNiagaraArrayPosition only accepts
 * TArray<FVector> (double precision, 24 bytes per point), allocates a converted copy and runs a
 * scalar large-world-coordinate conversion over every element, and in a non-PIE editor world it
 * reinitializes the system on every call.
 */
UCLASS(ClassGroup = (MOVINStage), meta = (BlueprintSpawnableComponent), DisplayName = "MOVIN Stage Point Cloud")
class MOVINSTAGEPOINTCLOUDRENDER_API UMOVINStagePointCloudComponent : public UNiagaraComponent
{
	GENERATED_BODY()

public:
	UMOVINStagePointCloudComponent(const FObjectInitializer& ObjectInitializer);

	//~ UActorComponent interface
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** Points read on the most recent upload. */
	UFUNCTION(BlueprintPure, Category = "MOVIN Stage Point Cloud")
	int32 GetLastUploadedPointCount() const { return LastUploadedPointCount; }

	/** Sender frame sequence number of the most recent upload, or -1 when nothing has arrived. */
	UFUNCTION(BlueprintPure, Category = "MOVIN Stage Point Cloud")
	int32 GetLastUploadedSeq() const { return LastUploadedSeq; }

	UFUNCTION(BlueprintPure, Category = "MOVIN Stage Point Cloud")
	bool IsStreamConnected() const;

	/** Position texture the Niagara system samples. Exposed for debugging and custom materials. */
	UFUNCTION(BlueprintPure, Category = "MOVIN Stage Point Cloud")
	UTexture2D* GetPositionTexture() const { return PositionTexture; }

public:
	/**
	 * Particle budget and texture size. Should match the LiveLink source's MaxPoints setting. A
	 * multiple of 1024 avoids wasting a partial texture row.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MOVIN Stage Point Cloud", meta = (ClampMin = "1024", UIMin = "1024"))
	int32 MaxPoints = 331776;

	/** Forwarded to the Niagara system as a user parameter. Interpretation is up to the system. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MOVIN Stage Point Cloud", meta = (ClampMin = "0.0"))
	float PointSize = 2.0f;

	/**
	 * Which LiveLink source to read from. Leave at the default invalid GUID to follow whichever
	 * source most recently started streaming point clouds.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MOVIN Stage Point Cloud|Advanced")
	FGuid SourceGuidFilter;

	/**
	 * Padding added to the bounds reported to the renderer, in centimetres.
	 *
	 * Bounds come from the frame's own extents, so a cloud that suddenly grows would otherwise be
	 * culled for one frame.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MOVIN Stage Point Cloud|Advanced", meta = (ClampMin = "0.0"))
	float BoundsPaddingCm = 200.0f;

	//~ Niagara user parameter names. Change these only to match a custom Niagara system.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MOVIN Stage Point Cloud|Parameter Names")
	FName PositionTextureParameterName = TEXT("PointCloudPositionTexture");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MOVIN Stage Point Cloud|Parameter Names")
	FName TextureWidthParameterName = TEXT("PointCloudTextureWidth");

	/** Needed to normalise UVs; a Niagara system cannot otherwise read the texture's height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MOVIN Stage Point Cloud|Parameter Names")
	FName TextureHeightParameterName = TEXT("PointCloudTextureHeight");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MOVIN Stage Point Cloud|Parameter Names")
	FName ActiveCountParameterName = TEXT("PointCloudActiveCount");

	/**
	 * This frame's extents, in the same space as the texture's positions. Lets an effect normalise a
	 * position - a height colour ramp, a sweeping scan plane - without hardcoding stage dimensions.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MOVIN Stage Point Cloud|Parameter Names")
	FName BoundsMinParameterName = TEXT("PointCloudBoundsMin");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MOVIN Stage Point Cloud|Parameter Names")
	FName BoundsMaxParameterName = TEXT("PointCloudBoundsMax");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MOVIN Stage Point Cloud|Parameter Names")
	FName PointSizeParameterName = TEXT("PointCloudPointSize");

private:
	void EnsureResources();
	void ReleaseResources();
	bool ResolveStream();
	void UploadFrame(const struct FMOVINStagePointCloudFrame& Frame);
	void PushStaticParameters();

	/** Position texture, PF_A32B32G32R32F, TextureWidth by ceil(MaxPoints / TextureWidth). */
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> PositionTexture;

	/**
	 * Two staging buffers alternating. UpdateTextureRegions reads the source asynchronously on the
	 * RHI thread, so the buffer it was given must not be touched until its cleanup callback fires.
	 */
	TArray<FVector4f> StagingBuffers[2];
	TSharedPtr<std::atomic<bool>> StagingBufferInFlight[2];
	int32 NextStagingBuffer = 0;

	TSharedPtr<FMOVINStagePointCloudStream, ESPMode::ThreadSafe> Stream;

	int32 TextureWidth = 0;
	int32 TextureHeight = 0;
	int32 AllocatedMaxPoints = 0;

	int32 LastUploadedPointCount = 0;
	int32 LastUploadedSeq = -1;
	float LastPushedPointSize = -1.0f;
	uint64 SkippedUploadsNoStagingBuffer = 0;
	bool bWarnedMissingAsset = false;
};
