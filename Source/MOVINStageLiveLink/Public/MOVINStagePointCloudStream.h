#pragma once

#include "CoreMinimal.h"

/**
 * One assembled point cloud frame.
 *
 * Points are stored as FVector4f rather than FVector3f so the buffer is byte identical to a
 * PF_A32B32G32R32F texture's staging layout. That lets the renderer memcpy the slot straight into
 * the texture with no expansion pass. W is unused and left at zero.
 */
struct FMOVINStagePointCloudFrame
{
	/** Sized to the stream's MaxPoints. Only the first PointCount entries are meaningful. */
	TArray<FVector4f> Points;

	/** Points actually present in this frame. Less than SourceTotalPoints when chunks were lost. */
	int32 PointCount = 0;

	/** Point count the sender declared for this frame, before loss and before the MaxPoints cap. */
	int32 SourceTotalPoints = 0;

	/** Sender frame sequence number. */
	int32 Seq = -1;

	/** False when the frame was published with chunks missing. */
	bool bComplete = false;

	/** FPlatformTime::Seconds() at which assembly finished. */
	double CaptureTimeSeconds = 0.0;

	/** Exact bounds of the points in this frame, accumulated during conversion. */
	FVector3f BoundsMin = FVector3f::ZeroVector;
	FVector3f BoundsMax = FVector3f::ZeroVector;
};

/**
 * Frame ring handing assembled point clouds from the receive thread to the game thread.
 *
 * Four slots: one being written, one ready, one held by the consumer, and one spare so the producer
 * never has to wait on the consumer's timing. Consumers are expected to copy out and release
 * promptly - a renderer must not hold a slot across an asynchronous GPU upload, because that would
 * couple slot lifetime to GPU latency.
 *
 * Slot bookkeeping is guarded by a critical section rather than hand rolled atomics. The lock only
 * ever covers a handful of integer writes - never point data - and is taken roughly a hundred
 * times a second (produce 20 Hz, consume up to display rate), so the simplicity is worth far more
 * than the contention it avoids.
 */
class MOVINSTAGELIVELINK_API FMOVINStagePointCloudStream : public TSharedFromThis<FMOVINStagePointCloudStream, ESPMode::ThreadSafe>
{
public:
	static constexpr int32 NumSlots = 4;

	/**
	 * Texture width the renderer lays points out with. Lives here so the producer's MaxPoints and
	 * the renderer's texture dimensions cannot drift apart.
	 */
	static constexpr int32 TextureWidth = 1024;

	explicit FMOVINStagePointCloudStream(int32 InMaxPoints);

	int32 GetMaxPoints() const { return MaxPoints; }

	/**
	 * Producer side. Returns a slot to fill, or null when every slot is busy - in which case the
	 * caller should drop the frame rather than block.
	 */
	FMOVINStagePointCloudFrame* AcquireWriteSlot();

	/** Producer side. Makes the slot the newest ready frame; any older ready frame is recycled. */
	void PublishWriteSlot(FMOVINStagePointCloudFrame* Frame);

	/** Producer side. Returns a write slot without publishing it. */
	void AbandonWriteSlot(FMOVINStagePointCloudFrame* Frame);

	/**
	 * Consumer side. Returns the newest ready frame, or null when nothing new has arrived.
	 *
	 * Returning null on "no new frame" is load bearing: the cloud is produced at 20 fps while the
	 * game ticks far faster, and handing back the same frame again would make the renderer re-upload
	 * several megabytes per tick for no reason.
	 */
	FMOVINStagePointCloudFrame* AcquireLatestReadyFrame();

	/** Recycles a frame. Safe from any thread. Call as soon as the data has been copied out. */
	void ReleaseFrame(FMOVINStagePointCloudFrame* Frame);

	uint64 GetFramesPublished() const;
	uint64 GetFramesConsumed() const;

	/** Frames superseded before the consumer ever took them. Expected behaviour, worth surfacing. */
	uint64 GetFramesDroppedBeforeDisplay() const;

	/** Producer could not get a slot. Should stay at zero in a healthy system. */
	uint64 GetWriteSlotStarvations() const;

private:
	enum class ESlotState : uint8
	{
		Free,
		Writing,
		Ready,
		InUse
	};

	struct FSlot
	{
		FMOVINStagePointCloudFrame Frame;
		ESlotState State = ESlotState::Free;
		uint64 PublishOrder = 0;
	};

	int32 IndexOfChecked(const FMOVINStagePointCloudFrame* Frame) const;

	mutable FCriticalSection Lock;

	/** Fixed storage: handed out pointers must stay valid for the stream's lifetime. */
	FSlot Slots[NumSlots];

	int32 MaxPoints = 0;
	uint64 NextPublishOrder = 1;
	uint64 FramesPublished = 0;
	uint64 FramesConsumed = 0;
	uint64 FramesDropped = 0;
	uint64 WriteSlotStarvations = 0;
};
