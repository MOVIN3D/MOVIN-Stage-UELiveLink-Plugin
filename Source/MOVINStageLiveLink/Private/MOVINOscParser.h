#pragma once

#include "CoreMinimal.h"

enum class EMOVINOscPacketType : uint8
{
	Ignored,
	PointCloud,
	Motion,
	Malformed
};

struct FMOVINOscMotionPacket
{
	FString Address;
	FString ActorName;
	FString BoneName;
	FVector3f Position = FVector3f::ZeroVector;
	FQuat4f Rotation = FQuat4f::Identity;
	bool bIsRoot = false;
	bool bHasFrameIndex = false;
	int32 FrameIndex = 0;
};

/**
 * Address classification for the allocation-free receive path. Unlike ParsePacket this never
 * constructs an FString, which matters because point cloud datagrams dominate the packet rate
 * and carry a type tag string thousands of characters long.
 */
enum class EMOVINOscAddress : uint8
{
	Unknown,
	/**
	 * The two fixed motion addresses. They still carry the /VMC/ prefix on the wire because the
	 * sender cannot change them yet, so these identifiers and the literals behind them keep the
	 * name. Nothing else in the plugin is VMC.
	 */
	VmcRootPos,
	VmcBonePos,
	MovinPointCloud
};

/**
 * Precompiled OSC address block. The point cloud address is a sender CLI argument
 * (osc_streamer.py --osc_address), so it cannot be hardcoded like the two /VMC/ motion addresses.
 */
struct FMOVINOscAddressMatcher
{
	static constexpr int32 MaxBlockBytes = 64;

	/** Address bytes including the null terminator, zero padded to a 4 byte multiple. */
	uint8 Block[MaxBlockBytes] = {};
	int32 BlockLen = 0;

	static FMOVINOscAddressMatcher Make(const ANSICHAR* Address);
	static FMOVINOscAddressMatcher Make(const FString& Address);
};

/** How the point payload of a chunk is encoded on the wire. */
enum class EMOVINOscPointEncoding : uint8
{
	/** OSC 'f' arguments: big endian float32, interleaved x,y,z. */
	BigEndianFloat32XYZ
};

/**
 * Non-owning view of one reassembly chunk. Valid only for the duration of the dispatch call.
 *
 * This struct is deliberately protocol neutral: it describes "where does this chunk belong in
 * the frame, and where are its bytes" without naming OSC. A future binary protocol can fill the
 * same struct and reuse the assembler, ring buffer and renderer unchanged.
 */
struct FMOVINOscPointCloudChunkView
{
	int32 Seq = 0;
	int32 TotalPoints = 0;
	int32 ChunkIndex = 0;
	int32 NumChunks = 0;
	int32 ChunkNumPoints = 0;
	EMOVINOscPointEncoding Encoding = EMOVINOscPointEncoding::BigEndianFloat32XYZ;
	const uint8* PointData = nullptr;
};

class FMOVINOscParser
{
public:
	static EMOVINOscPacketType ParsePacket(const uint8* Data, int32 NumBytes, FMOVINOscMotionPacket& OutPacket);

	/**
	 * Classifies a datagram by its OSC address using raw byte comparison only.
	 * OutAddressBlockLen receives the padded address length so the caller can hand it to
	 * ParsePointCloudChunk without rescanning.
	 */
	static EMOVINOscAddress ClassifyAddress(
		const uint8* Data,
		int32 NumBytes,
		const FMOVINOscAddressMatcher& PointCloudMatcher,
		int32& OutAddressBlockLen);

	/**
	 * Parses a /MOVIN/PointCloud chunk header without allocating and without scanning the
	 * 3*N 'f' type tag characters. Validation relies on the tag length agreeing with the
	 * decoded chunk point count, which a mismatched tag cannot satisfy.
	 */
	static bool ParsePointCloudChunk(
		const uint8* Data,
		int32 NumBytes,
		int32 AddressBlockLen,
		FMOVINOscPointCloudChunkView& OutView);

private:
	static bool ReadPaddedString(const uint8* Data, int32 NumBytes, int32& InOutOffset, FString& OutString);
	static bool ReadFloatBE(const uint8* Data, int32 NumBytes, int32& InOutOffset, float& OutValue);
	static bool ReadInt32BE(const uint8* Data, int32 NumBytes, int32& InOutOffset, int32& OutValue);
};
