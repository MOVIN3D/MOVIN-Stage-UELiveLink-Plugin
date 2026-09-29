#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"

/**
 * OSC byte emitters shared by the parser tests.
 *
 * A named namespace rather than an anonymous one: unity builds concatenate translation units, so
 * two test files each declaring these in an anonymous namespace would collide.
 */
namespace MOVINOscTestUtil
{
	inline void AppendPaddedString(TArray<uint8>& Bytes, const ANSICHAR* Text)
	{
		const int32 Start = Bytes.Num();
		while (*Text != '\0')
		{
			Bytes.Add(static_cast<uint8>(*Text++));
		}
		Bytes.Add(0);
		while ((Bytes.Num() - Start) % 4 != 0)
		{
			Bytes.Add(0);
		}
	}

	inline void AppendFloatBE(TArray<uint8>& Bytes, float Value)
	{
		uint32 Raw = 0;
		FMemory::Memcpy(&Raw, &Value, sizeof(float));
		Bytes.Add(static_cast<uint8>((Raw >> 24) & 0xff));
		Bytes.Add(static_cast<uint8>((Raw >> 16) & 0xff));
		Bytes.Add(static_cast<uint8>((Raw >> 8) & 0xff));
		Bytes.Add(static_cast<uint8>(Raw & 0xff));
	}

	inline void AppendInt32BE(TArray<uint8>& Bytes, int32 Value)
	{
		const uint32 Raw = static_cast<uint32>(Value);
		Bytes.Add(static_cast<uint8>((Raw >> 24) & 0xff));
		Bytes.Add(static_cast<uint8>((Raw >> 16) & 0xff));
		Bytes.Add(static_cast<uint8>((Raw >> 8) & 0xff));
		Bytes.Add(static_cast<uint8>(Raw & 0xff));
	}

	/** Source space point pattern used by the reassembly tests: point i is (i, 2i, 3i). */
	inline FVector3f MakeTestSourcePoint(int32 GlobalPointIndex)
	{
		const float Base = static_cast<float>(GlobalPointIndex);
		return FVector3f(Base, Base * 2.0f, Base * 3.0f);
	}

	/**
	 * Builds one /MOVIN/PointCloud datagram exactly as osc_streamer.py does:
	 * address, ",iiiii" + 'f' * (3 * chunk_n), then the five int32 header values and the payload.
	 */
	inline TArray<uint8> BuildPointCloudMessage(
		const ANSICHAR* Address,
		int32 Seq,
		int32 TotalPoints,
		int32 ChunkIndex,
		int32 NumChunks,
		int32 ChunkNumPoints,
		int32 FirstGlobalPointIndex)
	{
		TArray<uint8> Bytes;
		AppendPaddedString(Bytes, Address);

		FString TypeTags(TEXT(",iiiii"));
		for (int32 Index = 0; Index < ChunkNumPoints * 3; ++Index)
		{
			TypeTags.AppendChar(TEXT('f'));
		}
		AppendPaddedString(Bytes, TCHAR_TO_ANSI(*TypeTags));

		AppendInt32BE(Bytes, Seq);
		AppendInt32BE(Bytes, TotalPoints);
		AppendInt32BE(Bytes, ChunkIndex);
		AppendInt32BE(Bytes, NumChunks);
		AppendInt32BE(Bytes, ChunkNumPoints);

		for (int32 Index = 0; Index < ChunkNumPoints; ++Index)
		{
			const FVector3f Point = MakeTestSourcePoint(FirstGlobalPointIndex + Index);
			AppendFloatBE(Bytes, Point.X);
			AppendFloatBE(Bytes, Point.Y);
			AppendFloatBE(Bytes, Point.Z);
		}

		return Bytes;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
