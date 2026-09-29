#include "MOVINOscPointCloudAssembler.h"

#include "HAL/PlatformTime.h"
#include "Misc/ByteSwap.h"
#include "Templates/TypeCompatibleBytes.h"

namespace
{
	/** Guards against a corrupt header asking for a hundreds of megabytes allocation. */
	constexpr int32 HardMaxPoints = 8 * 1000 * 1000;
	constexpr int32 HardMaxChunksPerFrame = 262144;

	constexpr double StatsPublishIntervalSeconds = 0.25;

	struct FPointConvertParams
	{
		/** Applied to the source components that become Unreal X and Z. */
		float Scale = 100.0f;

		/** Applied to the source X component, which becomes Unreal Y. Negative in the OpenGL branch. */
		float SignedScaleForUnrealY = 100.0f;

		FVector3f Offset = FVector3f::ZeroVector;
	};

	/**
	 * Byte swap, coordinate conversion and bounds accumulation in a single pass.
	 *
	 * Source is big endian float32 in the sender's Y up frame; the mapping matches
	 * FMOVINStageLiveLinkSource::ConvertOscTransform so the cloud lands in the same space as the
	 * skeleton: Unreal X = source Z, Unreal Y = +/- source X, Unreal Z = source Y.
	 *
	 * Deliberately kept separate from the destination offset calculation so that only this function
	 * has to move if the conversion is ever offloaded to a worker thread.
	 *
	 * No SIMD: UE has no packed 12 byte stride deinterleave helper, so it would mean raw intrinsics,
	 * and this loop is a few percent of one core.
	 */
	void ConvertChunkBigEndianFloat32(
		const uint8* RESTRICT Source,
		FVector4f* RESTRICT Destination,
		int32 NumPoints,
		const FPointConvertParams& Params,
		bool& bInOutHasBounds,
		FVector3f& InOutBoundsMin,
		FVector3f& InOutBoundsMax)
	{
		FVector3f Min = InOutBoundsMin;
		FVector3f Max = InOutBoundsMax;
		bool bHasBounds = bInOutHasBounds;

		for (int32 Index = 0; Index < NumPoints; ++Index, Source += 3 * sizeof(float))
		{
			uint32 RawX, RawY, RawZ;
			FMemory::Memcpy(&RawX, Source, sizeof(uint32));
			FMemory::Memcpy(&RawY, Source + 4, sizeof(uint32));
			FMemory::Memcpy(&RawZ, Source + 8, sizeof(uint32));

			// NETWORK_ORDER32 lowers to a single bswap on MSVC and is a no-op on big endian hosts.
			const float SourceX = BitCast<float>(NETWORK_ORDER32(RawX));
			const float SourceY = BitCast<float>(NETWORK_ORDER32(RawY));
			const float SourceZ = BitCast<float>(NETWORK_ORDER32(RawZ));

			const FVector3f Point(
				SourceZ * Params.Scale + Params.Offset.X,
				SourceX * Params.SignedScaleForUnrealY + Params.Offset.Y,
				SourceY * Params.Scale + Params.Offset.Z);

			Destination[Index] = FVector4f(Point.X, Point.Y, Point.Z, 0.0f);

			if (bHasBounds)
			{
				Min.X = FMath::Min(Min.X, Point.X);
				Min.Y = FMath::Min(Min.Y, Point.Y);
				Min.Z = FMath::Min(Min.Z, Point.Z);
				Max.X = FMath::Max(Max.X, Point.X);
				Max.Y = FMath::Max(Max.Y, Point.Y);
				Max.Z = FMath::Max(Max.Z, Point.Z);
			}
			else
			{
				Min = Point;
				Max = Point;
				bHasBounds = true;
			}
		}

		InOutBoundsMin = Min;
		InOutBoundsMax = Max;
		bInOutHasBounds = bHasBounds;
	}
}

void FMOVINOscPointCloudAssembler::Initialize(const FMOVINOscPointCloudAssemblerConfig& InConfig)
{
	Config = InConfig;
	Config.MaxPoints = FMath::Clamp(Config.MaxPoints, 1, HardMaxPoints);
	Config.SeqResyncThreshold = FMath::Max(Config.SeqResyncThreshold, 1);
	if (Config.PositionScale <= 0.0f)
	{
		Config.PositionScale = 100.0f;
	}

	Stream = MakeShared<FMOVINStagePointCloudStream, ESPMode::ThreadSafe>(Config.MaxPoints);

	CurrentSeq = -1;
	Dest = nullptr;
	Stats = FMOVINOscPointCloudStats();
	PublishStats(true);
}

void FMOVINOscPointCloudAssembler::Shutdown()
{
	AbandonCurrentFrame();
	PublishStats(true);
	Stream.Reset();
}

void FMOVINOscPointCloudAssembler::NoteMalformedChunk()
{
	++Stats.MalformedChunks;
}

FMOVINOscPointCloudStats FMOVINOscPointCloudAssembler::SnapshotStats() const
{
	FScopeLock ScopeLock(&StatsLock);
	return PublishedStats;
}

int64 FMOVINOscPointCloudAssembler::ComputeChunkPointOffset(
	int32 ChunkIndex,
	int32 NumChunks,
	int32 ChunkNumPoints,
	int32 TotalPoints)
{
	if (ChunkIndex < 0 || NumChunks <= 0 || ChunkIndex >= NumChunks || ChunkNumPoints <= 0 || TotalPoints <= 0)
	{
		return -1;
	}

	if (ChunkIndex == NumChunks - 1)
	{
		const int64 Offset = static_cast<int64>(TotalPoints) - ChunkNumPoints;
		if (Offset < 0)
		{
			return -1;
		}

		if (NumChunks == 1)
		{
			// A single chunk must carry the whole frame.
			return Offset == 0 ? 0 : -1;
		}

		// Every preceding chunk carries the same stride, so the points ahead of the ragged final
		// chunk must divide evenly among them - and the final chunk can never be larger than a full
		// one. This is what catches a header whose NumChunks disagrees with its point counts, which
		// the offset arithmetic alone cannot see.
		const int64 PrecedingChunks = NumChunks - 1;
		if (Offset % PrecedingChunks != 0 || Offset / PrecedingChunks < ChunkNumPoints)
		{
			return -1;
		}

		return Offset;
	}

	// A non-last chunk carries the stride itself, so NumChunks must equal ceil(TotalPoints / stride).
	const int64 Stride = ChunkNumPoints;
	if (Stride * (NumChunks - 1) >= TotalPoints || Stride * NumChunks < TotalPoints)
	{
		return -1;
	}

	return Stride * ChunkIndex;
}

void FMOVINOscPointCloudAssembler::HandleChunk(const FMOVINOscPointCloudChunkView& View)
{
	if (!Stream.IsValid())
	{
		return;
	}

	++Stats.ChunksReceived;

	if (View.Seq < CurrentSeq)
	{
		if (CurrentSeq - View.Seq > Config.SeqResyncThreshold)
		{
			// The sender restarted and its sequence counter went back to the beginning.
			++Stats.Resyncs;
			AbandonCurrentFrame();
			CurrentSeq = -1;
			bCurrentFramePublished = false;
		}
		else
		{
			++Stats.LateChunks;
			return;
		}
	}

	if (View.Seq == CurrentSeq && bCurrentFramePublished)
	{
		// Straggler for a frame already published. The Python reference republishes on every such
		// duplicate; dropping is correct and stops the same frame being emitted repeatedly.
		++Stats.LateChunks;
		return;
	}

	if (View.Seq != CurrentSeq)
	{
		if (CurrentSeq >= 0 && !bCurrentFramePublished && ChunksReceived > 0 && Config.bPartialPublish)
		{
			PublishFrame(false);
		}

		if (!BeginFrame(View))
		{
			return;
		}
	}
	else if (View.NumChunks != ExpectedNumChunks || View.TotalPoints != ExpectedTotalPoints)
	{
		// Header disagreement inside one sequence number: restart the frame rather than mix layouts.
		++Stats.HeaderMismatches;
		if (!BeginFrame(View))
		{
			return;
		}
	}

	if (View.ChunkIndex >= ExpectedNumChunks)
	{
		++Stats.BadOffsetChunks;
		return;
	}

	if (ChunkReceived[View.ChunkIndex])
	{
		++Stats.DuplicateChunks;
		return;
	}

	const bool bIsLastChunk = View.ChunkIndex == ExpectedNumChunks - 1;

	const int64 Offset = ComputeChunkPointOffset(
		View.ChunkIndex, ExpectedNumChunks, View.ChunkNumPoints, ExpectedTotalPoints);

	if (Offset < 0)
	{
		// Only reachable from a corrupt or spoofed header, but it would scatter write out of range.
		++Stats.BadOffsetChunks;
		return;
	}

	ChunkReceived[View.ChunkIndex] = true;
	++ChunksReceived;
	while (ContiguousPrefixChunks < ExpectedNumChunks && ChunkReceived[ContiguousPrefixChunks])
	{
		++ContiguousPrefixChunks;
	}

	if (bIsLastChunk)
	{
		LastChunkNumPoints = View.ChunkNumPoints;
	}
	else if (StridePointsPerChunk == 0)
	{
		StridePointsPerChunk = View.ChunkNumPoints;
	}

	const int32 WriteStart = static_cast<int32>(Offset);
	const int32 WriteCount = FMath::Clamp(Config.MaxPoints - WriteStart, 0, View.ChunkNumPoints);
	if (WriteCount > 0 && Dest != nullptr && WriteStart + WriteCount <= Dest->Points.Num())
	{
		FPointConvertParams Params;
		Params.Scale = Config.PositionScale;
		Params.SignedScaleForUnrealY = Config.bNegateSourceX ? -Config.PositionScale : Config.PositionScale;
		Params.Offset = Config.OffsetCm;

		ConvertChunkBigEndianFloat32(
			View.PointData,
			Dest->Points.GetData() + WriteStart,
			WriteCount,
			Params,
			bFrameHasBounds,
			FrameBoundsMin,
			FrameBoundsMax);

		PointsWritten += WriteCount;
	}

	// Chunks entirely past the cap still count as received so the frame can still complete.
	if (ChunksReceived == ExpectedNumChunks)
	{
		PublishFrame(true);
	}
}

bool FMOVINOscPointCloudAssembler::BeginFrame(const FMOVINOscPointCloudChunkView& View)
{
	AbandonCurrentFrame();

	if (View.TotalPoints <= 0 || View.TotalPoints > HardMaxPoints ||
		View.NumChunks <= 0 || View.NumChunks > HardMaxChunksPerFrame)
	{
		++Stats.RejectedHeaders;
		CurrentSeq = -1;
		return false;
	}

	CurrentSeq = View.Seq;
	ExpectedNumChunks = View.NumChunks;
	ExpectedTotalPoints = View.TotalPoints;
	ChunksReceived = 0;
	PointsWritten = 0;
	ContiguousPrefixChunks = 0;
	StridePointsPerChunk = ExpectedNumChunks == 1 ? View.ChunkNumPoints : 0;
	LastChunkNumPoints = 0;
	bCurrentFramePublished = false;
	bFrameHasBounds = false;
	FrameBoundsMin = FVector3f::ZeroVector;
	FrameBoundsMax = FVector3f::ZeroVector;

	// Init never shrinks the allocation, so this settles into a plain memset after warmup.
	ChunkReceived.Init(false, ExpectedNumChunks);

	Dest = Stream->AcquireWriteSlot();
	if (Dest == nullptr)
	{
		++Stats.SlotStarvations;
		CurrentSeq = -1;
		return false;
	}

	const int32 SlotPoints = FMath::Min(ExpectedTotalPoints, Config.MaxPoints);
	Dest->Points.SetNumUninitialized(SlotPoints, EAllowShrinking::No);
	Dest->PointCount = 0;
	Dest->Seq = CurrentSeq;
	Dest->bComplete = false;
	return true;
}

void FMOVINOscPointCloudAssembler::AbandonCurrentFrame()
{
	if (Dest != nullptr && Stream.IsValid())
	{
		Stream->AbandonWriteSlot(Dest);
	}
	Dest = nullptr;
}

void FMOVINOscPointCloudAssembler::PublishFrame(bool bComplete)
{
	if (Dest == nullptr)
	{
		return;
	}

	if (bComplete)
	{
		Dest->PointCount = FMath::Min(ExpectedTotalPoints, Config.MaxPoints);
		++Stats.CompleteFrames;
	}
	else if (ContiguousPrefixChunks == ChunksReceived)
	{
		// Tail loss only, so the received prefix is already dense. No move needed.
		Dest->PointCount = FMath::Min(PointsWritten, Dest->Points.Num());
		++Stats.PartialFrames;
	}
	else
	{
		// Interior holes. Compact rather than zero fill: zeros would pile every missing point onto
		// the origin as a visible spike.
		CompactGaps();
		++Stats.PartialFrames;
	}

	Dest->PointCount = FMath::Clamp(Dest->PointCount, 0, Dest->Points.Num());
	Dest->SourceTotalPoints = ExpectedTotalPoints;
	Dest->Seq = CurrentSeq;
	Dest->bComplete = bComplete;
	Dest->CaptureTimeSeconds = FPlatformTime::Seconds();
	Dest->BoundsMin = bFrameHasBounds ? FrameBoundsMin : FVector3f::ZeroVector;
	Dest->BoundsMax = bFrameHasBounds ? FrameBoundsMax : FVector3f::ZeroVector;

	Stats.ChunksLost += static_cast<uint64>(FMath::Max(0, ExpectedNumChunks - ChunksReceived));
	Stats.LastPointCount = Dest->PointCount;
	Stats.LastSourceTotalPoints = ExpectedTotalPoints;
	Stats.LastPublishTimeSeconds = Dest->CaptureTimeSeconds;
	++Stats.FramesPublished;

	Stream->PublishWriteSlot(Dest);
	Dest = nullptr;
	bCurrentFramePublished = true;

	PublishStats(false);
}

void FMOVINOscPointCloudAssembler::CompactGaps()
{
	FVector4f* const Data = Dest->Points.GetData();
	const int32 Capacity = Dest->Points.Num();
	int32 WriteCursor = 0;

	for (int32 ChunkIndex = 0; ChunkIndex < ExpectedNumChunks; ++ChunkIndex)
	{
		if (!ChunkReceived[ChunkIndex])
		{
			continue;
		}

		const bool bIsLastChunk = ChunkIndex == ExpectedNumChunks - 1;
		int32 SourceOffset;
		int32 Count;

		if (bIsLastChunk)
		{
			Count = LastChunkNumPoints;
			SourceOffset = ExpectedTotalPoints - Count;
		}
		else
		{
			Count = StridePointsPerChunk;
			SourceOffset = ChunkIndex * StridePointsPerChunk;
		}

		if (Count <= 0 || SourceOffset < 0 || SourceOffset >= Capacity)
		{
			continue;
		}

		Count = FMath::Min(Count, Capacity - SourceOffset);
		if (Count <= 0)
		{
			continue;
		}

		// WriteCursor never exceeds SourceOffset, so this always moves data downwards.
		if (WriteCursor != SourceOffset)
		{
			FMemory::Memmove(Data + WriteCursor, Data + SourceOffset, static_cast<size_t>(Count) * sizeof(FVector4f));
		}
		WriteCursor += Count;
	}

	Dest->PointCount = WriteCursor;
}

void FMOVINOscPointCloudAssembler::PublishStats(bool bForce)
{
	const double Now = FPlatformTime::Seconds();
	if (!bForce && Now < NextStatsPublishTime)
	{
		return;
	}

	NextStatsPublishTime = Now + StatsPublishIntervalSeconds;

	FScopeLock ScopeLock(&StatsLock);
	PublishedStats = Stats;
}
