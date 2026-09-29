#pragma once

#include "CoreMinimal.h"
#include "MOVINStagePointCloudStream.h"
#include "MOVINOscParser.h"

struct FMOVINOscPointCloudAssemblerConfig
{
	/** Destination cap. Also sizes the ring slots. */
	int32 MaxPoints = 331776;

	/** A backwards sequence jump larger than this is treated as a sender restart. */
	int32 SeqResyncThreshold = 100;

	/** Publish frames that are still missing chunks when the next frame starts. */
	bool bPartialPublish = true;

	/** Metres to centimetres, matching FMOVINStageLiveLinkConnectionSettings::PositionScale. */
	float PositionScale = 100.0f;

	/** True for the OpenGL coordinate branch, which negates the source X component. */
	bool bNegateSourceX = false;

	/** Translation applied in Unreal space, folded into the conversion loop for free. */
	FVector3f OffsetCm = FVector3f::ZeroVector;
};

struct FMOVINOscPointCloudStats
{
	uint64 ChunksReceived = 0;
	uint64 ChunksLost = 0;
	uint64 DuplicateChunks = 0;
	uint64 LateChunks = 0;
	uint64 BadOffsetChunks = 0;
	uint64 HeaderMismatches = 0;
	uint64 RejectedHeaders = 0;
	uint64 Resyncs = 0;
	uint64 MalformedChunks = 0;
	uint64 SlotStarvations = 0;
	uint64 CompleteFrames = 0;
	uint64 PartialFrames = 0;
	uint64 FramesPublished = 0;
	int32 LastPointCount = 0;
	int32 LastSourceTotalPoints = 0;
	double LastPublishTimeSeconds = 0.0;
};

/**
 * Reassembles /MOVIN/PointCloud chunks into whole frames and publishes them to the ring.
 *
 * Every member is owned by the receive thread and deliberately unsynchronized - the only
 * cross-thread object is the stream. Keeping that invariant is what would let the whole assembler
 * move to a worker thread later without writing any synchronization code.
 */
class FMOVINOscPointCloudAssembler
{
public:
	void Initialize(const FMOVINOscPointCloudAssemblerConfig& InConfig);
	void Shutdown();

	bool IsInitialized() const { return Stream.IsValid(); }

	TSharedPtr<FMOVINStagePointCloudStream, ESPMode::ThreadSafe> GetStream() const { return Stream; }

	/** Receive thread only. */
	void HandleChunk(const FMOVINOscPointCloudChunkView& View);

	/** Receive thread only. Records a datagram that carried the right address but failed to parse. */
	void NoteMalformedChunk();

	FMOVINOscPointCloudStats SnapshotStats() const;

	/**
	 * Destination point index for a chunk, or a negative value when the header is inconsistent.
	 *
	 * Every chunk except the last carries exactly max_pts_per_chunk points, so the position follows
	 * from the chunk's own point count - max_pts_per_chunk itself is never on the wire. That also
	 * makes the rule stateless, so it is correct even when the last chunk arrives first.
	 * Verified against osc_streamer.py:818-826.
	 *
	 * Kept separate from conversion so only the conversion has to move if it is ever offloaded.
	 */
	static int64 ComputeChunkPointOffset(int32 ChunkIndex, int32 NumChunks, int32 ChunkNumPoints, int32 TotalPoints);

private:
	bool BeginFrame(const FMOVINOscPointCloudChunkView& View);
	void AbandonCurrentFrame();
	void PublishFrame(bool bComplete);
	void CompactGaps();
	void PublishStats(bool bForce);

	FMOVINOscPointCloudAssemblerConfig Config;
	TSharedPtr<FMOVINStagePointCloudStream, ESPMode::ThreadSafe> Stream;

	//~ Current frame state. Receive thread owned.
	int32 CurrentSeq = -1;
	int32 ExpectedNumChunks = 0;
	int32 ExpectedTotalPoints = 0;
	int32 ChunksReceived = 0;
	int32 PointsWritten = 0;
	int32 ContiguousPrefixChunks = 0;

	/** Point count of a non-last chunk. Every non-last chunk carries max_pts_per_chunk points. */
	int32 StridePointsPerChunk = 0;

	/** Point count of the ragged final chunk, recorded when it arrives. */
	int32 LastChunkNumPoints = 0;

	bool bCurrentFramePublished = false;
	bool bFrameHasBounds = false;
	FVector3f FrameBoundsMin = FVector3f::ZeroVector;
	FVector3f FrameBoundsMax = FVector3f::ZeroVector;
	TBitArray<> ChunkReceived;
	FMOVINStagePointCloudFrame* Dest = nullptr;

	//~ Stats. Plain members on the receive thread, snapshotted for the game thread.
	FMOVINOscPointCloudStats Stats;
	double NextStatsPublishTime = 0.0;
	mutable FCriticalSection StatsLock;
	FMOVINOscPointCloudStats PublishedStats;
};
