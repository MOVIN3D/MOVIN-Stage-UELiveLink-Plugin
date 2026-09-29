#include "MOVINOscParser.h"

#include "Misc/ByteSwap.h"
#include "Templates/TypeCompatibleBytes.h"

#include <string.h>

namespace
{
	// Wire addresses the sender still emits with the legacy /VMC/ prefix. The plugin is otherwise
	// named OSC throughout; these literals and the Vmc* identifiers holding them stay until the
	// sender changes.
	const FString VmcRootAddress(TEXT("/VMC/Ext/Root/Pos"));
	const FString VmcBoneAddress(TEXT("/VMC/Ext/Bone/Pos"));
	const FString PointCloudAddress(TEXT("/MOVIN/PointCloud"));
	const FString MotionTypeNoFrame(TEXT(",ssfffffff"));
	const FString MotionTypeWithFrame(TEXT(",ssfffffffi"));

	// All three addresses are exactly 17 characters, so every padded block is exactly 20 bytes.
	constexpr int32 VmcAddressBlockBytes = 20;

	const uint8 VmcRootAddressBlock[VmcAddressBlockBytes] = {
		'/', 'V', 'M', 'C', '/', 'E', 'x', 't', '/', 'R', 'o', 'o', 't', '/', 'P', 'o', 's', 0, 0, 0
	};
	const uint8 VmcBoneAddressBlock[VmcAddressBlockBytes] = {
		'/', 'V', 'M', 'C', '/', 'E', 'x', 't', '/', 'B', 'o', 'n', 'e', '/', 'P', 'o', 's', 0, 0, 0
	};

	/** Branchless 20 byte compare: three loads, three xors, one test. */
	FORCEINLINE bool MatchAddressBlock20(const uint8* RESTRICT Data, const uint8* RESTRICT Literal)
	{
		uint64 DataLo, DataHi, LiteralLo, LiteralHi;
		uint32 DataTail, LiteralTail;
		FMemory::Memcpy(&DataLo, Data, sizeof(uint64));
		FMemory::Memcpy(&LiteralLo, Literal, sizeof(uint64));
		FMemory::Memcpy(&DataHi, Data + 8, sizeof(uint64));
		FMemory::Memcpy(&LiteralHi, Literal + 8, sizeof(uint64));
		FMemory::Memcpy(&DataTail, Data + 16, sizeof(uint32));
		FMemory::Memcpy(&LiteralTail, Literal + 16, sizeof(uint32));

		return ((DataLo ^ LiteralLo) | (DataHi ^ LiteralHi) | static_cast<uint64>(DataTail ^ LiteralTail)) == 0;
	}

	/** Reads a big endian int32 with no bounds check. The caller must have validated the range. */
	FORCEINLINE int32 ReadInt32BEUnchecked(const uint8* Data)
	{
		uint32 Raw;
		FMemory::Memcpy(&Raw, Data, sizeof(uint32));
		return static_cast<int32>(NETWORK_ORDER32(Raw));
	}

	/** OSC pads a string to a multiple of four bytes, terminator included. */
	FORCEINLINE int32 OscPaddedBlockLength(int32 StringLength)
	{
		return Align(StringLength + 1, 4);
	}

	/** ",iiiii" plus one 'f' per float. The header is five int32 values. */
	const ANSICHAR PointCloudTypeTagPrefix[] = ",iiiii";
	constexpr int32 PointCloudTypeTagPrefixLen = 6;
	constexpr int32 PointCloudHeaderBytes = 5 * sizeof(int32);
	constexpr int32 PointCloudBytesPerPoint = 3 * sizeof(float);
}

FMOVINOscAddressMatcher FMOVINOscAddressMatcher::Make(const ANSICHAR* Address)
{
	FMOVINOscAddressMatcher Result;
	if (Address == nullptr)
	{
		return Result;
	}

	int32 Length = 0;
	while (Address[Length] != '\0' && Length < MaxBlockBytes - 1)
	{
		++Length;
	}

	const int32 BlockBytes = OscPaddedBlockLength(Length);
	if (BlockBytes > MaxBlockBytes)
	{
		return Result;
	}

	for (int32 Index = 0; Index < Length; ++Index)
	{
		Result.Block[Index] = static_cast<uint8>(Address[Index]);
	}
	Result.BlockLen = BlockBytes;
	return Result;
}

FMOVINOscAddressMatcher FMOVINOscAddressMatcher::Make(const FString& Address)
{
	FMOVINOscAddressMatcher Result;

	const int32 Length = Address.Len();
	const int32 BlockBytes = OscPaddedBlockLength(Length);
	if (Length <= 0 || BlockBytes > MaxBlockBytes)
	{
		return Result;
	}

	// OSC addresses are ASCII. Mirrors the narrowing ReadPaddedString already performs.
	for (int32 Index = 0; Index < Length; ++Index)
	{
		Result.Block[Index] = static_cast<uint8>(Address[Index]);
	}
	Result.BlockLen = BlockBytes;
	return Result;
}

EMOVINOscAddress FMOVINOscParser::ClassifyAddress(
	const uint8* Data,
	int32 NumBytes,
	const FMOVINOscAddressMatcher& PointCloudMatcher,
	int32& OutAddressBlockLen)
{
	OutAddressBlockLen = 0;

	if (Data == nullptr || Data[0] != '/')
	{
		return EMOVINOscAddress::Unknown;
	}

	// Point cloud first: it is the overwhelming majority of the packet rate.
	if (PointCloudMatcher.BlockLen > 0 && NumBytes >= PointCloudMatcher.BlockLen)
	{
		const bool bMatches = PointCloudMatcher.BlockLen == VmcAddressBlockBytes
			? MatchAddressBlock20(Data, PointCloudMatcher.Block)
			: FMemory::Memcmp(Data, PointCloudMatcher.Block, PointCloudMatcher.BlockLen) == 0;

		if (bMatches)
		{
			OutAddressBlockLen = PointCloudMatcher.BlockLen;
			return EMOVINOscAddress::MovinPointCloud;
		}
	}

	if (NumBytes >= VmcAddressBlockBytes)
	{
		if (MatchAddressBlock20(Data, VmcBoneAddressBlock))
		{
			OutAddressBlockLen = VmcAddressBlockBytes;
			return EMOVINOscAddress::VmcBonePos;
		}
		if (MatchAddressBlock20(Data, VmcRootAddressBlock))
		{
			OutAddressBlockLen = VmcAddressBlockBytes;
			return EMOVINOscAddress::VmcRootPos;
		}
	}

	return EMOVINOscAddress::Unknown;
}

bool FMOVINOscParser::ParsePointCloudChunk(
	const uint8* Data,
	int32 NumBytes,
	int32 AddressBlockLen,
	FMOVINOscPointCloudChunkView& OutView)
{
	OutView = FMOVINOscPointCloudChunkView();

	if (Data == nullptr || AddressBlockLen <= 0 || AddressBlockLen >= NumBytes)
	{
		return false;
	}

	// Locate the type tag terminator. memchr is SIMD accelerated in the CRT, which matters
	// because the tag is 3 characters per point (over 1700 bytes at 595 points per chunk).
	const uint8* const TagStart = Data + AddressBlockLen;
	const int32 TagMaxBytes = NumBytes - AddressBlockLen;
	const uint8* const Terminator = static_cast<const uint8*>(memchr(TagStart, 0, static_cast<size_t>(TagMaxBytes)));
	if (Terminator == nullptr)
	{
		return false;
	}

	const int32 TagLen = static_cast<int32>(Terminator - TagStart);
	if (TagLen < PointCloudTypeTagPrefixLen)
	{
		return false;
	}

	// Validate by prefix and arithmetic only. The 3*N 'f' characters are never scanned.
	for (int32 Index = 0; Index < PointCloudTypeTagPrefixLen; ++Index)
	{
		if (TagStart[Index] != static_cast<uint8>(PointCloudTypeTagPrefix[Index]))
		{
			return false;
		}
	}

	const int32 DeclaredFloats = TagLen - PointCloudTypeTagPrefixLen;
	if (DeclaredFloats <= 0 || DeclaredFloats % 3 != 0)
	{
		return false;
	}

	// Matches the sender's _osc_pack_string / _pad4 exactly.
	const int32 ArgsOffset = AddressBlockLen + OscPaddedBlockLength(TagLen);
	if (ArgsOffset < 0 || ArgsOffset + PointCloudHeaderBytes > NumBytes)
	{
		return false;
	}

	const uint8* const Args = Data + ArgsOffset;
	const int32 Seq = ReadInt32BEUnchecked(Args);
	const int32 TotalPoints = ReadInt32BEUnchecked(Args + 4);
	const int32 ChunkIndex = ReadInt32BEUnchecked(Args + 8);
	const int32 NumChunks = ReadInt32BEUnchecked(Args + 12);
	const int32 ChunkNumPoints = ReadInt32BEUnchecked(Args + 16);

	// Cross check: the type tag's float count must equal 3 * chunk_n. A mismatched tag cannot
	// produce a consistent pair, which is what makes the full 'f' scan unnecessary.
	if (ChunkNumPoints <= 0 || ChunkNumPoints != DeclaredFloats / 3)
	{
		return false;
	}

	if (TotalPoints <= 0 || NumChunks <= 0 || ChunkIndex < 0 || ChunkIndex >= NumChunks)
	{
		return false;
	}

	const int64 PayloadBytes = static_cast<int64>(ChunkNumPoints) * PointCloudBytesPerPoint;
	if (static_cast<int64>(ArgsOffset) + PointCloudHeaderBytes + PayloadBytes > NumBytes)
	{
		return false;
	}

	OutView.Seq = Seq;
	OutView.TotalPoints = TotalPoints;
	OutView.ChunkIndex = ChunkIndex;
	OutView.NumChunks = NumChunks;
	OutView.ChunkNumPoints = ChunkNumPoints;
	OutView.Encoding = EMOVINOscPointEncoding::BigEndianFloat32XYZ;
	OutView.PointData = Args + PointCloudHeaderBytes;
	return true;
}

EMOVINOscPacketType FMOVINOscParser::ParsePacket(const uint8* Data, int32 NumBytes, FMOVINOscMotionPacket& OutPacket)
{
	OutPacket = FMOVINOscMotionPacket();

	if (Data == nullptr || NumBytes <= 0 || Data[0] != '/')
	{
		return EMOVINOscPacketType::Ignored;
	}

	int32 Offset = 0;
	if (!ReadPaddedString(Data, NumBytes, Offset, OutPacket.Address))
	{
		return EMOVINOscPacketType::Malformed;
	}

	if (OutPacket.Address == PointCloudAddress)
	{
		return EMOVINOscPacketType::PointCloud;
	}

	OutPacket.bIsRoot = OutPacket.Address == VmcRootAddress;
	if (!OutPacket.bIsRoot && OutPacket.Address != VmcBoneAddress)
	{
		return EMOVINOscPacketType::Ignored;
	}

	FString TypeTags;
	if (!ReadPaddedString(Data, NumBytes, Offset, TypeTags))
	{
		return EMOVINOscPacketType::Malformed;
	}

	if (TypeTags != MotionTypeNoFrame && TypeTags != MotionTypeWithFrame)
	{
		return EMOVINOscPacketType::Ignored;
	}

	if (!ReadPaddedString(Data, NumBytes, Offset, OutPacket.ActorName) ||
		!ReadPaddedString(Data, NumBytes, Offset, OutPacket.BoneName))
	{
		return EMOVINOscPacketType::Malformed;
	}

	float Values[7] = {};
	for (float& Value : Values)
	{
		if (!ReadFloatBE(Data, NumBytes, Offset, Value))
		{
			return EMOVINOscPacketType::Malformed;
		}
	}

	OutPacket.Position = FVector3f(Values[0], Values[1], Values[2]);
	OutPacket.Rotation = FQuat4f(Values[3], Values[4], Values[5], Values[6]);

	if (TypeTags == MotionTypeWithFrame)
	{
		OutPacket.bHasFrameIndex = true;
		if (!ReadInt32BE(Data, NumBytes, Offset, OutPacket.FrameIndex))
		{
			return EMOVINOscPacketType::Malformed;
		}
	}

	return EMOVINOscPacketType::Motion;
}

bool FMOVINOscParser::ReadPaddedString(const uint8* Data, int32 NumBytes, int32& InOutOffset, FString& OutString)
{
	if (InOutOffset < 0 || InOutOffset >= NumBytes)
	{
		return false;
	}

	const int32 Start = InOutOffset;
	int32 End = Start;
	while (End < NumBytes && Data[End] != 0)
	{
		++End;
	}

	if (End >= NumBytes)
	{
		return false;
	}

	OutString.Reset(End - Start);
	for (int32 Index = Start; Index < End; ++Index)
	{
		OutString.AppendChar(static_cast<TCHAR>(Data[Index]));
	}

	const int32 RawLengthWithTerminator = End - Start + 1;
	InOutOffset = Start + RawLengthWithTerminator + ((4 - (RawLengthWithTerminator % 4)) % 4);
	return InOutOffset <= NumBytes;
}

bool FMOVINOscParser::ReadFloatBE(const uint8* Data, int32 NumBytes, int32& InOutOffset, float& OutValue)
{
	if (InOutOffset < 0 || InOutOffset + 4 > NumBytes)
	{
		return false;
	}

	const uint32 Raw =
		(static_cast<uint32>(Data[InOutOffset]) << 24) |
		(static_cast<uint32>(Data[InOutOffset + 1]) << 16) |
		(static_cast<uint32>(Data[InOutOffset + 2]) << 8) |
		static_cast<uint32>(Data[InOutOffset + 3]);
	FMemory::Memcpy(&OutValue, &Raw, sizeof(float));
	InOutOffset += 4;
	return true;
}

bool FMOVINOscParser::ReadInt32BE(const uint8* Data, int32 NumBytes, int32& InOutOffset, int32& OutValue)
{
	if (InOutOffset < 0 || InOutOffset + 4 > NumBytes)
	{
		return false;
	}

	const uint32 Raw =
		(static_cast<uint32>(Data[InOutOffset]) << 24) |
		(static_cast<uint32>(Data[InOutOffset + 1]) << 16) |
		(static_cast<uint32>(Data[InOutOffset + 2]) << 8) |
		static_cast<uint32>(Data[InOutOffset + 3]);
	OutValue = static_cast<int32>(Raw);
	InOutOffset += 4;
	return true;
}
