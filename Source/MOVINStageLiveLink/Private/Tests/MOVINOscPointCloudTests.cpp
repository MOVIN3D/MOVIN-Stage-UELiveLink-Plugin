#if WITH_DEV_AUTOMATION_TESTS

#include "MOVINOscCapturedPointCloudData.h"
#include "MOVINStageLiveLinkSource.h"
#include "MOVINStageLiveLinkSourceSettings.h"
#include "MOVINOscParser.h"
#include "MOVINOscTestHelpers.h"
#include "MOVINOscPointCloudAssembler.h"
#include "MOVINOscUdpReceiveThread.h"
#include "Common/UdpSocketBuilder.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMOVINOscClassifyAddressTest, "MOVIN.Stage.Parser.ClassifyAddress", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMOVINOscPointCloudChunkTest, "MOVIN.Stage.Parser.PointCloudChunk", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMOVINOscPointCloudOffsetRuleTest, "MOVIN.Stage.PointCloud.OffsetRule", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMOVINOscPointCloudConvertTest, "MOVIN.Stage.PointCloud.Convert", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMOVINOscPointCloudReassemblyTest, "MOVIN.Stage.PointCloud.Reassembly", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMOVINOscSettingsRoundTripTest, "MOVIN.Stage.Settings.RoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMOVINOscCapturedPointCloudTest, "MOVIN.Stage.PointCloud.CapturedBytes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMOVINOscUdpRoundTripTest, "MOVIN.Stage.PointCloud.UdpRoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMOVINOscSourceSettingsPanelTest, "MOVIN.Stage.Settings.Panel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMOVINOscExclusiveBindTest, "MOVIN.Stage.Network.ExclusiveBind", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
	using MOVINOscTestUtil::AppendPaddedString;
	using MOVINOscTestUtil::BuildPointCloudMessage;
	using MOVINOscTestUtil::MakeTestSourcePoint;

	const ANSICHAR* DefaultPointCloudAddress = "/MOVIN/PointCloud";

	FMOVINOscAddressMatcher DefaultMatcher()
	{
		return FMOVINOscAddressMatcher::Make(DefaultPointCloudAddress);
	}

	/** Chunk layout the sender produces for a given total and max_pts_per_chunk. */
	struct FSenderChunk
	{
		int32 ChunkIndex = 0;
		int32 ChunkNumPoints = 0;
		int32 ExpectedStart = 0;
	};

	TArray<FSenderChunk> BuildSenderChunkLayout(int32 TotalPoints, int32 MaxPointsPerChunk)
	{
		// Mirrors osc_streamer.py:818-826 exactly.
		TArray<FSenderChunk> Chunks;
		const int32 NumChunks = (TotalPoints + MaxPointsPerChunk - 1) / MaxPointsPerChunk;
		for (int32 ChunkIndex = 0; ChunkIndex < NumChunks; ++ChunkIndex)
		{
			const int32 Start = ChunkIndex * MaxPointsPerChunk;
			const int32 End = FMath::Min(TotalPoints, Start + MaxPointsPerChunk);
			Chunks.Add(FSenderChunk{ ChunkIndex, End - Start, Start });
		}
		return Chunks;
	}

	/** Feeds a datagram straight through the parser into the assembler. */
	bool FeedChunk(
		FMOVINOscPointCloudAssembler& Assembler,
		int32 Seq,
		int32 TotalPoints,
		int32 ChunkIndex,
		int32 NumChunks,
		int32 ChunkNumPoints,
		int32 FirstGlobalPointIndex)
	{
		const TArray<uint8> Bytes = BuildPointCloudMessage(
			DefaultPointCloudAddress, Seq, TotalPoints, ChunkIndex, NumChunks, ChunkNumPoints, FirstGlobalPointIndex);

		int32 AddressBlockLen = 0;
		const EMOVINOscAddress Address =
			FMOVINOscParser::ClassifyAddress(Bytes.GetData(), Bytes.Num(), DefaultMatcher(), AddressBlockLen);
		if (Address != EMOVINOscAddress::MovinPointCloud)
		{
			return false;
		}

		FMOVINOscPointCloudChunkView View;
		if (!FMOVINOscParser::ParsePointCloudChunk(Bytes.GetData(), Bytes.Num(), AddressBlockLen, View))
		{
			return false;
		}

		Assembler.HandleChunk(View);
		return true;
	}

	FMOVINOscPointCloudAssemblerConfig UnitScaleConfig(int32 MaxPoints)
	{
		// Scale 1 keeps the expected values readable: Unreal (x,y,z) = source (z,x,y).
		FMOVINOscPointCloudAssemblerConfig Config;
		Config.MaxPoints = MaxPoints;
		Config.PositionScale = 1.0f;
		Config.bNegateSourceX = false;
		return Config;
	}

	FVector4f ExpectedUnrealPoint(int32 GlobalPointIndex, float Scale, bool bNegateSourceX)
	{
		const FVector3f Source = MakeTestSourcePoint(GlobalPointIndex);
		return FVector4f(
			Source.Z * Scale,
			Source.X * (bNegateSourceX ? -Scale : Scale),
			Source.Y * Scale,
			0.0f);
	}
}

bool FMOVINOscClassifyAddressTest::RunTest(const FString& Parameters)
{
	const FMOVINOscAddressMatcher Matcher = DefaultMatcher();
	int32 BlockLen = 0;

	{
		TArray<uint8> Bytes;
		AppendPaddedString(Bytes, "/MOVIN/PointCloud");
		TestEqual(TEXT("point cloud address"),
			FMOVINOscParser::ClassifyAddress(Bytes.GetData(), Bytes.Num(), Matcher, BlockLen),
			EMOVINOscAddress::MovinPointCloud);
		TestEqual(TEXT("point cloud block length"), BlockLen, 20);
	}

	{
		TArray<uint8> Bytes;
		AppendPaddedString(Bytes, "/VMC/Ext/Root/Pos");
		TestEqual(TEXT("root address"),
			FMOVINOscParser::ClassifyAddress(Bytes.GetData(), Bytes.Num(), Matcher, BlockLen),
			EMOVINOscAddress::VmcRootPos);
		TestEqual(TEXT("root block length"), BlockLen, 20);
	}

	{
		TArray<uint8> Bytes;
		AppendPaddedString(Bytes, "/VMC/Ext/Bone/Pos");
		TestEqual(TEXT("bone address"),
			FMOVINOscParser::ClassifyAddress(Bytes.GetData(), Bytes.Num(), Matcher, BlockLen),
			EMOVINOscAddress::VmcBonePos);
	}

	{
		TArray<uint8> Bytes;
		AppendPaddedString(Bytes, "/VMC/Ext/Blend/Val");
		TestEqual(TEXT("unrelated address"),
			FMOVINOscParser::ClassifyAddress(Bytes.GetData(), Bytes.Num(), Matcher, BlockLen),
			EMOVINOscAddress::Unknown);
	}

	{
		// Truncated below one padded address block must not read past the buffer.
		TArray<uint8> Bytes;
		AppendPaddedString(Bytes, "/MOVIN/PointCloud");
		Bytes.SetNum(12);
		TestEqual(TEXT("truncated buffer"),
			FMOVINOscParser::ClassifyAddress(Bytes.GetData(), Bytes.Num(), Matcher, BlockLen),
			EMOVINOscAddress::Unknown);
	}

	{
		// A custom sender address (--osc_address) of a different length takes the memcmp path.
		const FMOVINOscAddressMatcher CustomMatcher = FMOVINOscAddressMatcher::Make("/MOVIN/Cloud");
		TArray<uint8> Bytes;
		AppendPaddedString(Bytes, "/MOVIN/Cloud");
		TestEqual(TEXT("custom address block length"), CustomMatcher.BlockLen, 16);
		TestEqual(TEXT("custom address"),
			FMOVINOscParser::ClassifyAddress(Bytes.GetData(), Bytes.Num(), CustomMatcher, BlockLen),
			EMOVINOscAddress::MovinPointCloud);
		TestEqual(TEXT("custom block length"), BlockLen, 16);
	}

	{
		// The default address must not match when the sender was configured differently.
		const FMOVINOscAddressMatcher CustomMatcher = FMOVINOscAddressMatcher::Make("/MOVIN/Cloud");
		TArray<uint8> Bytes;
		AppendPaddedString(Bytes, "/MOVIN/PointCloud");
		TestEqual(TEXT("non matching custom address"),
			FMOVINOscParser::ClassifyAddress(Bytes.GetData(), Bytes.Num(), CustomMatcher, BlockLen),
			EMOVINOscAddress::Unknown);
	}

	return true;
}

bool FMOVINOscPointCloudChunkTest::RunTest(const FString& Parameters)
{
	{
		const TArray<uint8> Bytes = BuildPointCloudMessage(DefaultPointCloudAddress, 7, 250, 1, 3, 100, 100);

		int32 AddressBlockLen = 0;
		TestEqual(TEXT("classified"),
			FMOVINOscParser::ClassifyAddress(Bytes.GetData(), Bytes.Num(), DefaultMatcher(), AddressBlockLen),
			EMOVINOscAddress::MovinPointCloud);

		FMOVINOscPointCloudChunkView View;
		TestTrue(TEXT("parsed"), FMOVINOscParser::ParsePointCloudChunk(Bytes.GetData(), Bytes.Num(), AddressBlockLen, View));
		TestEqual(TEXT("seq"), View.Seq, 7);
		TestEqual(TEXT("total points"), View.TotalPoints, 250);
		TestEqual(TEXT("chunk index"), View.ChunkIndex, 1);
		TestEqual(TEXT("num chunks"), View.NumChunks, 3);
		TestEqual(TEXT("chunk point count"), View.ChunkNumPoints, 100);

		// Address block 20, type tag ",iiiii" + 300 'f' = 306 chars -> Align(307,4) = 308, then 5 int32.
		const int32 ExpectedPayloadOffset = 20 + 308 + 20;
		TestEqual(TEXT("payload offset"), static_cast<int32>(View.PointData - Bytes.GetData()), ExpectedPayloadOffset);
	}

	{
		// Truncated payload: the header parses but the declared points do not fit.
		TArray<uint8> Bytes = BuildPointCloudMessage(DefaultPointCloudAddress, 1, 100, 0, 1, 100, 0);
		Bytes.SetNum(Bytes.Num() - 4);

		int32 AddressBlockLen = 0;
		FMOVINOscParser::ClassifyAddress(Bytes.GetData(), Bytes.Num(), DefaultMatcher(), AddressBlockLen);

		FMOVINOscPointCloudChunkView View;
		TestFalse(TEXT("truncated payload rejected"),
			FMOVINOscParser::ParsePointCloudChunk(Bytes.GetData(), Bytes.Num(), AddressBlockLen, View));
	}

	{
		// Type tag float count disagreeing with chunk_n is the cross check that lets us skip the
		// full 'f' scan. Build a tag for 4 points but declare 8 in the header.
		TArray<uint8> Bytes;
		AppendPaddedString(Bytes, DefaultPointCloudAddress);
		AppendPaddedString(Bytes, ",iiiiifffffffffffff");
		MOVINOscTestUtil::AppendInt32BE(Bytes, 1);
		MOVINOscTestUtil::AppendInt32BE(Bytes, 8);
		MOVINOscTestUtil::AppendInt32BE(Bytes, 0);
		MOVINOscTestUtil::AppendInt32BE(Bytes, 1);
		MOVINOscTestUtil::AppendInt32BE(Bytes, 8);
		for (int32 Index = 0; Index < 24; ++Index)
		{
			MOVINOscTestUtil::AppendFloatBE(Bytes, 0.0f);
		}

		int32 AddressBlockLen = 0;
		FMOVINOscParser::ClassifyAddress(Bytes.GetData(), Bytes.Num(), DefaultMatcher(), AddressBlockLen);

		FMOVINOscPointCloudChunkView View;
		TestFalse(TEXT("type tag count mismatch rejected"),
			FMOVINOscParser::ParsePointCloudChunk(Bytes.GetData(), Bytes.Num(), AddressBlockLen, View));
	}

	{
		// Wrong type tag prefix.
		TArray<uint8> Bytes;
		AppendPaddedString(Bytes, DefaultPointCloudAddress);
		AppendPaddedString(Bytes, ",iiiifffff");

		int32 AddressBlockLen = 0;
		FMOVINOscParser::ClassifyAddress(Bytes.GetData(), Bytes.Num(), DefaultMatcher(), AddressBlockLen);

		FMOVINOscPointCloudChunkView View;
		TestFalse(TEXT("bad type tag prefix rejected"),
			FMOVINOscParser::ParsePointCloudChunk(Bytes.GetData(), Bytes.Num(), AddressBlockLen, View));
	}

	return true;
}

bool FMOVINOscPointCloudOffsetRuleTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		int32 TotalPoints;
		int32 MaxPointsPerChunk;
	};

	// The last two entries cover a voxel downsampled stream, where the point count changes every
	// frame and never lines up with the chunk size.
	const FCase Cases[] = {
		{ 330000, 595 },
		{ 330000, 100 },
		{ 400000, 595 },
		{ 101, 100 },
		{ 100, 100 },
		{ 1, 100 },
		{ 250, 95 },
		{ 7, 3 },
		{ 328741, 595 },
		{ 71, 595 }
	};

	for (const FCase& Case : Cases)
	{
		const TArray<FSenderChunk> Chunks = BuildSenderChunkLayout(Case.TotalPoints, Case.MaxPointsPerChunk);
		TestTrue(FString::Printf(TEXT("layout non empty for total %d"), Case.TotalPoints), Chunks.Num() > 0);

		for (const FSenderChunk& Chunk : Chunks)
		{
			const int64 Offset = FMOVINOscPointCloudAssembler::ComputeChunkPointOffset(
				Chunk.ChunkIndex, Chunks.Num(), Chunk.ChunkNumPoints, Case.TotalPoints);

			TestEqual(
				FString::Printf(TEXT("offset total=%d max=%d chunk=%d"), Case.TotalPoints, Case.MaxPointsPerChunk, Chunk.ChunkIndex),
				static_cast<int32>(Offset),
				Chunk.ExpectedStart);
		}
	}

	// Inconsistent headers must be rejected rather than producing an out of range write.
	TestTrue(TEXT("negative index rejected"),
		FMOVINOscPointCloudAssembler::ComputeChunkPointOffset(-1, 4, 100, 400) < 0);
	TestTrue(TEXT("index past chunk count rejected"),
		FMOVINOscPointCloudAssembler::ComputeChunkPointOffset(4, 4, 100, 400) < 0);
	TestTrue(TEXT("overrunning chunk rejected"),
		FMOVINOscPointCloudAssembler::ComputeChunkPointOffset(3, 4, 200, 400) < 0);
	TestTrue(TEXT("zero point chunk rejected"),
		FMOVINOscPointCloudAssembler::ComputeChunkPointOffset(0, 4, 0, 400) < 0);

	return true;
}

bool FMOVINOscPointCloudConvertTest::RunTest(const FString& Parameters)
{
	// Unity branch: Unreal (x,y,z) = source (z, x, y) * Scale, matching ConvertOscTransform.
	{
		FMOVINOscPointCloudAssembler Assembler;
		FMOVINOscPointCloudAssemblerConfig Config = UnitScaleConfig(8);
		Config.PositionScale = 100.0f;
		Assembler.Initialize(Config);

		TestTrue(TEXT("unity chunk fed"), FeedChunk(Assembler, 0, 4, 0, 1, 4, 0));

		const TSharedPtr<FMOVINStagePointCloudStream, ESPMode::ThreadSafe> Stream = Assembler.GetStream();
		FMOVINStagePointCloudFrame* Frame = Stream->AcquireLatestReadyFrame();
		TestNotNull(TEXT("unity frame published"), Frame);
		if (Frame != nullptr)
		{
			TestEqual(TEXT("unity point count"), Frame->PointCount, 4);
			for (int32 Index = 0; Index < 4; ++Index)
			{
				const FVector4f Expected = ExpectedUnrealPoint(Index, 100.0f, false);
				TestTrue(FString::Printf(TEXT("unity point %d"), Index),
					Frame->Points[Index].Equals(Expected, 0.001f));
			}
			Stream->ReleaseFrame(Frame);
		}
		Assembler.Shutdown();
	}

	// OpenGL branch negates the source X component, which becomes Unreal Y.
	{
		FMOVINOscPointCloudAssembler Assembler;
		FMOVINOscPointCloudAssemblerConfig Config = UnitScaleConfig(8);
		Config.PositionScale = 100.0f;
		Config.bNegateSourceX = true;
		Assembler.Initialize(Config);

		TestTrue(TEXT("opengl chunk fed"), FeedChunk(Assembler, 0, 4, 0, 1, 4, 0));

		const TSharedPtr<FMOVINStagePointCloudStream, ESPMode::ThreadSafe> Stream = Assembler.GetStream();
		FMOVINStagePointCloudFrame* Frame = Stream->AcquireLatestReadyFrame();
		TestNotNull(TEXT("opengl frame published"), Frame);
		if (Frame != nullptr)
		{
			for (int32 Index = 0; Index < 4; ++Index)
			{
				const FVector4f Expected = ExpectedUnrealPoint(Index, 100.0f, true);
				TestTrue(FString::Printf(TEXT("opengl point %d"), Index),
					Frame->Points[Index].Equals(Expected, 0.001f));
			}
			Stream->ReleaseFrame(Frame);
		}
		Assembler.Shutdown();
	}

	// Offset is folded into the conversion loop.
	{
		FMOVINOscPointCloudAssembler Assembler;
		FMOVINOscPointCloudAssemblerConfig Config = UnitScaleConfig(8);
		Config.OffsetCm = FVector3f(10.0f, 20.0f, 30.0f);
		Assembler.Initialize(Config);

		TestTrue(TEXT("offset chunk fed"), FeedChunk(Assembler, 0, 2, 0, 1, 2, 0));

		const TSharedPtr<FMOVINStagePointCloudStream, ESPMode::ThreadSafe> Stream = Assembler.GetStream();
		FMOVINStagePointCloudFrame* Frame = Stream->AcquireLatestReadyFrame();
		TestNotNull(TEXT("offset frame published"), Frame);
		if (Frame != nullptr)
		{
			const FVector4f Expected = ExpectedUnrealPoint(1, 1.0f, false) + FVector4f(10.0f, 20.0f, 30.0f, 0.0f);
			TestTrue(TEXT("offset applied"), Frame->Points[1].Equals(Expected, 0.001f));
			Stream->ReleaseFrame(Frame);
		}

		// Bounds come out of the same pass, so they must cover both points exactly.
		Assembler.Shutdown();
	}

	return true;
}

bool FMOVINOscPointCloudReassemblyTest::RunTest(const FString& Parameters)
{
	constexpr int32 TotalPoints = 250;
	constexpr int32 MaxPointsPerChunk = 100;
	const TArray<FSenderChunk> Layout = BuildSenderChunkLayout(TotalPoints, MaxPointsPerChunk);
	const int32 NumChunks = Layout.Num();

	auto FeedLayoutChunk = [&](FMOVINOscPointCloudAssembler& Assembler, int32 Seq, int32 ChunkIndex)
	{
		const FSenderChunk& Chunk = Layout[ChunkIndex];
		return FeedChunk(Assembler, Seq, TotalPoints, ChunkIndex, NumChunks, Chunk.ChunkNumPoints, Chunk.ExpectedStart);
	};

	auto VerifyDensePoints = [&](FMOVINStagePointCloudFrame* Frame, const TArray<int32>& ExpectedGlobalIndices) -> bool
	{
		if (Frame == nullptr || Frame->PointCount != ExpectedGlobalIndices.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < ExpectedGlobalIndices.Num(); ++Index)
		{
			const FVector4f Expected = ExpectedUnrealPoint(ExpectedGlobalIndices[Index], 1.0f, false);
			if (!Frame->Points[Index].Equals(Expected, 0.001f))
			{
				return false;
			}
		}
		return true;
	};

	// In order, complete.
	{
		FMOVINOscPointCloudAssembler Assembler;
		Assembler.Initialize(UnitScaleConfig(TotalPoints));

		for (int32 ChunkIndex = 0; ChunkIndex < NumChunks; ++ChunkIndex)
		{
			TestTrue(TEXT("in order chunk fed"), FeedLayoutChunk(Assembler, 1, ChunkIndex));
		}

		FMOVINStagePointCloudFrame* Frame = Assembler.GetStream()->AcquireLatestReadyFrame();
		TestNotNull(TEXT("in order frame"), Frame);
		if (Frame != nullptr)
		{
			TestTrue(TEXT("in order complete"), Frame->bComplete);
			TestEqual(TEXT("in order point count"), Frame->PointCount, TotalPoints);
			TestTrue(TEXT("in order point 0"), Frame->Points[0].Equals(ExpectedUnrealPoint(0, 1.0f, false), 0.001f));
			TestTrue(TEXT("in order last point"),
				Frame->Points[TotalPoints - 1].Equals(ExpectedUnrealPoint(TotalPoints - 1, 1.0f, false), 0.001f));
			Assembler.GetStream()->ReleaseFrame(Frame);
		}
		Assembler.Shutdown();
	}

	// Reverse order, complete. The last chunk arriving first is exactly the case the offset rule
	// has to survive without knowing max_pts_per_chunk.
	{
		FMOVINOscPointCloudAssembler Assembler;
		Assembler.Initialize(UnitScaleConfig(TotalPoints));

		for (int32 ChunkIndex = NumChunks - 1; ChunkIndex >= 0; --ChunkIndex)
		{
			TestTrue(TEXT("reverse chunk fed"), FeedLayoutChunk(Assembler, 2, ChunkIndex));
		}

		FMOVINStagePointCloudFrame* Frame = Assembler.GetStream()->AcquireLatestReadyFrame();
		TestNotNull(TEXT("reverse frame"), Frame);
		if (Frame != nullptr)
		{
			TestTrue(TEXT("reverse complete"), Frame->bComplete);
			TestEqual(TEXT("reverse point count"), Frame->PointCount, TotalPoints);
			for (int32 Index = 0; Index < TotalPoints; Index += 37)
			{
				TestTrue(FString::Printf(TEXT("reverse point %d"), Index),
					Frame->Points[Index].Equals(ExpectedUnrealPoint(Index, 1.0f, false), 0.001f));
			}
			Assembler.GetStream()->ReleaseFrame(Frame);
		}
		Assembler.Shutdown();
	}

	// Tail loss: chunk 2 never arrives, so the received prefix is already dense and needs no move.
	{
		FMOVINOscPointCloudAssembler Assembler;
		Assembler.Initialize(UnitScaleConfig(TotalPoints));

		FeedLayoutChunk(Assembler, 3, 0);
		FeedLayoutChunk(Assembler, 3, 1);
		// Next sequence number forces the partial publish.
		FeedChunk(Assembler, 4, TotalPoints, 0, NumChunks, Layout[0].ChunkNumPoints, Layout[0].ExpectedStart);

		FMOVINStagePointCloudFrame* Frame = Assembler.GetStream()->AcquireLatestReadyFrame();
		TestNotNull(TEXT("tail loss frame"), Frame);
		if (Frame != nullptr)
		{
			TestFalse(TEXT("tail loss partial"), Frame->bComplete);
			TestEqual(TEXT("tail loss seq"), Frame->Seq, 3);
			TestEqual(TEXT("tail loss point count"), Frame->PointCount, 200);
			TestTrue(TEXT("tail loss last kept point"),
				Frame->Points[199].Equals(ExpectedUnrealPoint(199, 1.0f, false), 0.001f));
			Assembler.GetStream()->ReleaseFrame(Frame);
		}
		Assembler.Shutdown();
	}

	// Interior hole: chunk 1 is lost, so chunk 2's points must be compacted down onto chunk 0's.
	// Zero filling instead would pile fifty points onto the origin as a visible spike.
	{
		FMOVINOscPointCloudAssembler Assembler;
		Assembler.Initialize(UnitScaleConfig(TotalPoints));

		FeedLayoutChunk(Assembler, 5, 0);
		FeedLayoutChunk(Assembler, 5, 2);
		FeedChunk(Assembler, 6, TotalPoints, 0, NumChunks, Layout[0].ChunkNumPoints, Layout[0].ExpectedStart);

		FMOVINStagePointCloudFrame* Frame = Assembler.GetStream()->AcquireLatestReadyFrame();
		TestNotNull(TEXT("interior hole frame"), Frame);
		if (Frame != nullptr)
		{
			TestFalse(TEXT("interior hole partial"), Frame->bComplete);

			TArray<int32> Expected;
			for (int32 Index = 0; Index < 100; ++Index)
			{
				Expected.Add(Index);
			}
			for (int32 Index = 200; Index < 250; ++Index)
			{
				Expected.Add(Index);
			}
			TestTrue(TEXT("interior hole compacted"), VerifyDensePoints(Frame, Expected));
			Assembler.GetStream()->ReleaseFrame(Frame);
		}
		Assembler.Shutdown();
	}

	// Duplicate chunks must not double count towards completion.
	{
		FMOVINOscPointCloudAssembler Assembler;
		Assembler.Initialize(UnitScaleConfig(TotalPoints));

		FeedLayoutChunk(Assembler, 7, 0);
		FeedLayoutChunk(Assembler, 7, 0);
		FeedLayoutChunk(Assembler, 7, 0);
		TestNull(TEXT("duplicates do not complete a frame"), Assembler.GetStream()->AcquireLatestReadyFrame());

		FeedLayoutChunk(Assembler, 7, 1);
		FeedLayoutChunk(Assembler, 7, 2);
		FMOVINStagePointCloudFrame* Frame = Assembler.GetStream()->AcquireLatestReadyFrame();
		TestNotNull(TEXT("duplicate frame completes"), Frame);
		if (Frame != nullptr)
		{
			TestTrue(TEXT("duplicate frame complete"), Frame->bComplete);
			Assembler.GetStream()->ReleaseFrame(Frame);
		}
		Assembler.Shutdown();
	}

	// A small backwards sequence jump is a straggler and must be dropped, not treated as a restart.
	{
		FMOVINOscPointCloudAssembler Assembler;
		Assembler.Initialize(UnitScaleConfig(TotalPoints));

		for (int32 ChunkIndex = 0; ChunkIndex < NumChunks; ++ChunkIndex)
		{
			FeedLayoutChunk(Assembler, 100, ChunkIndex);
		}
		FMOVINStagePointCloudFrame* Frame = Assembler.GetStream()->AcquireLatestReadyFrame();
		TestNotNull(TEXT("frame before straggler"), Frame);
		if (Frame != nullptr)
		{
			Assembler.GetStream()->ReleaseFrame(Frame);
		}

		FeedLayoutChunk(Assembler, 95, 0);
		TestNull(TEXT("straggler publishes nothing"), Assembler.GetStream()->AcquireLatestReadyFrame());
		Assembler.Shutdown();
	}

	// A large backwards jump means the sender restarted and its counter went back to zero.
	{
		FMOVINOscPointCloudAssembler Assembler;
		Assembler.Initialize(UnitScaleConfig(TotalPoints));

		for (int32 ChunkIndex = 0; ChunkIndex < NumChunks; ++ChunkIndex)
		{
			FeedLayoutChunk(Assembler, 5000, ChunkIndex);
		}
		FMOVINStagePointCloudFrame* Frame = Assembler.GetStream()->AcquireLatestReadyFrame();
		if (Frame != nullptr)
		{
			Assembler.GetStream()->ReleaseFrame(Frame);
		}

		for (int32 ChunkIndex = 0; ChunkIndex < NumChunks; ++ChunkIndex)
		{
			FeedLayoutChunk(Assembler, 0, ChunkIndex);
		}
		FMOVINStagePointCloudFrame* Resynced = Assembler.GetStream()->AcquireLatestReadyFrame();
		TestNotNull(TEXT("resynced frame"), Resynced);
		if (Resynced != nullptr)
		{
			TestEqual(TEXT("resynced seq"), Resynced->Seq, 0);
			TestTrue(TEXT("resynced complete"), Resynced->bComplete);
			Assembler.GetStream()->ReleaseFrame(Resynced);
		}
		Assembler.Shutdown();
	}

	// MaxPoints cap: the frame still completes, but only the capped prefix is written.
	{
		FMOVINOscPointCloudAssembler Assembler;
		Assembler.Initialize(UnitScaleConfig(150));

		for (int32 ChunkIndex = 0; ChunkIndex < NumChunks; ++ChunkIndex)
		{
			FeedLayoutChunk(Assembler, 9, ChunkIndex);
		}

		FMOVINStagePointCloudFrame* Frame = Assembler.GetStream()->AcquireLatestReadyFrame();
		TestNotNull(TEXT("capped frame"), Frame);
		if (Frame != nullptr)
		{
			TestTrue(TEXT("capped frame complete"), Frame->bComplete);
			TestEqual(TEXT("capped point count"), Frame->PointCount, 150);
			TestEqual(TEXT("capped source total"), Frame->SourceTotalPoints, TotalPoints);
			TestTrue(TEXT("capped last point"),
				Frame->Points[149].Equals(ExpectedUnrealPoint(149, 1.0f, false), 0.001f));
			Assembler.GetStream()->ReleaseFrame(Frame);
		}
		Assembler.Shutdown();
	}

	// The consumer must not be handed the same frame twice.
	{
		FMOVINOscPointCloudAssembler Assembler;
		Assembler.Initialize(UnitScaleConfig(TotalPoints));

		for (int32 ChunkIndex = 0; ChunkIndex < NumChunks; ++ChunkIndex)
		{
			FeedLayoutChunk(Assembler, 11, ChunkIndex);
		}

		FMOVINStagePointCloudFrame* Frame = Assembler.GetStream()->AcquireLatestReadyFrame();
		TestNotNull(TEXT("first acquire"), Frame);
		TestNull(TEXT("second acquire returns null"), Assembler.GetStream()->AcquireLatestReadyFrame());
		if (Frame != nullptr)
		{
			Assembler.GetStream()->ReleaseFrame(Frame);
		}
		Assembler.Shutdown();
	}

	// Bounds are accumulated during conversion and must cover the whole frame.
	{
		FMOVINOscPointCloudAssembler Assembler;
		Assembler.Initialize(UnitScaleConfig(TotalPoints));

		for (int32 ChunkIndex = 0; ChunkIndex < NumChunks; ++ChunkIndex)
		{
			FeedLayoutChunk(Assembler, 13, ChunkIndex);
		}

		FMOVINStagePointCloudFrame* Frame = Assembler.GetStream()->AcquireLatestReadyFrame();
		TestNotNull(TEXT("bounds frame"), Frame);
		if (Frame != nullptr)
		{
			const FVector4f First = ExpectedUnrealPoint(0, 1.0f, false);
			const FVector4f Last = ExpectedUnrealPoint(TotalPoints - 1, 1.0f, false);
			TestTrue(TEXT("bounds min"), Frame->BoundsMin.Equals(FVector3f(First.X, First.Y, First.Z), 0.001f));
			TestTrue(TEXT("bounds max"), Frame->BoundsMax.Equals(FVector3f(Last.X, Last.Y, Last.Z), 0.001f));
			Assembler.GetStream()->ReleaseFrame(Frame);
		}
		Assembler.Shutdown();
	}

	return true;
}

bool FMOVINOscCapturedPointCloudTest::RunTest(const FString& Parameters)
{
	using namespace MOVINOscCapturedPointCloud;

	const FMOVINOscAddressMatcher Matcher = DefaultMatcher();

	// Datagrams the MOVIN Stage server actually emitted, spanning the observed size range. Pinning
	// against these rather than only against synthesised bytes is what catches a shared
	// misunderstanding between the parser and the tests.
	for (const FCapturedDatagram& Captured : Datagrams)
	{
		const FString Label = FString::Printf(TEXT("captured %d byte datagram"), Captured.NumBytes);

		int32 AddressBlockLen = 0;
		TestEqual(Label + TEXT(": classified"),
			FMOVINOscParser::ClassifyAddress(Captured.Bytes, Captured.NumBytes, Matcher, AddressBlockLen),
			EMOVINOscAddress::MovinPointCloud);
		TestEqual(Label + TEXT(": address block length"), AddressBlockLen, 20);

		FMOVINOscPointCloudChunkView View;
		const bool bParsed = FMOVINOscParser::ParsePointCloudChunk(Captured.Bytes, Captured.NumBytes, AddressBlockLen, View);
		TestTrue(Label + TEXT(": parsed"), bParsed);
		if (!bParsed)
		{
			continue;
		}

		TestEqual(Label + TEXT(": seq"), View.Seq, Captured.Seq);
		TestEqual(Label + TEXT(": total points"), View.TotalPoints, Captured.TotalPoints);
		TestEqual(Label + TEXT(": chunk index"), View.ChunkIndex, Captured.ChunkIndex);
		TestEqual(Label + TEXT(": num chunks"), View.NumChunks, Captured.NumChunks);
		TestEqual(Label + TEXT(": chunk point count"), View.ChunkNumPoints, Captured.ChunkNumPoints);
		TestEqual(Label + TEXT(": payload offset"),
			static_cast<int32>(View.PointData - Captured.Bytes),
			Captured.ExpectedArgsOffset + 20);

		// Every captured chunk must produce a valid destination offset, including the ragged final
		// chunk that carries a single point out of fifteen hundred.
		const int64 Offset = FMOVINOscPointCloudAssembler::ComputeChunkPointOffset(
			View.ChunkIndex, View.NumChunks, View.ChunkNumPoints, View.TotalPoints);
		TestTrue(Label + TEXT(": offset accepted"), Offset >= 0);
	}

	// One complete captured frame: the offset rule must tile [0, TotalPoints) with no gap or overlap.
	{
		TestEqual(TEXT("captured frame chunk count"),
			static_cast<int32>(UE_ARRAY_COUNT(CompleteFrameChunks)), CompleteFrameNumChunks);

		TArray<TPair<int32, int32>> Spans;
		for (const FCapturedChunkHeader& Chunk : CompleteFrameChunks)
		{
			const int64 Offset = FMOVINOscPointCloudAssembler::ComputeChunkPointOffset(
				Chunk.ChunkIndex, CompleteFrameNumChunks, Chunk.ChunkNumPoints, CompleteFrameTotalPoints);

			TestTrue(FString::Printf(TEXT("captured frame chunk %d offset accepted"), Chunk.ChunkIndex), Offset >= 0);
			if (Offset >= 0)
			{
				Spans.Add(TPair<int32, int32>(static_cast<int32>(Offset), static_cast<int32>(Offset) + Chunk.ChunkNumPoints));
			}
		}

		Spans.Sort([](const TPair<int32, int32>& A, const TPair<int32, int32>& B) { return A.Key < B.Key; });

		int32 Cursor = 0;
		for (const TPair<int32, int32>& Span : Spans)
		{
			TestEqual(FString::Printf(TEXT("captured frame span starts at %d"), Cursor), Span.Key, Cursor);
			Cursor = Span.Value;
		}
		TestEqual(TEXT("captured frame fully covered"), Cursor, CompleteFrameTotalPoints);
	}

	return true;
}

bool FMOVINOscUdpRoundTripTest::RunTest(const FString& Parameters)
{
	// Exercises the real receive path over loopback: socket, drain loop, classifier, chunk parser,
	// assembler and ring. The drain loop is the piece with no other coverage - it relies on Recv()
	// returning false as the "queue drained" signal, which only a real socket can demonstrate.
	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (SocketSubsystem == nullptr)
	{
		AddError(TEXT("No socket subsystem available."));
		return false;
	}

	FSocket* ReceiveSocket = FUdpSocketBuilder(TEXT("MOVINOscTestReceiver"))
		.AsNonBlocking()
		.AsReusable()
		.BoundToAddress(FIPv4Address::InternalLoopback)
		.BoundToPort(0)
		.WithReceiveBufferSize(4 * 1024 * 1024);

	if (ReceiveSocket == nullptr)
	{
		AddError(TEXT("Failed to bind the loopback receive socket."));
		return false;
	}

	const int32 BoundPort = ReceiveSocket->GetPortNo();

	FMOVINOscPointCloudAssembler Assembler;
	Assembler.Initialize(UnitScaleConfig(4096));

	const FMOVINOscAddressMatcher Matcher = DefaultMatcher();
	std::atomic<int32> MotionDatagrams{ 0 };
	std::atomic<int32> UnknownDatagrams{ 0 };

	{
		FMOVINOscReceiveThreadTuning Tuning;
		Tuning.MaxDatagramsPerWait = 1024;

		FMOVINOscUdpReceiveThread Receiver(ReceiveSocket, Tuning, TEXT("MOVINOscTestReceiveThread"));

		// Mirrors FMOVINStageLiveLinkSource::HandleDatagram's dispatch.
		Receiver.OnDatagram().BindLambda([&Assembler, &Matcher, &MotionDatagrams, &UnknownDatagrams](const uint8* Data, int32 NumBytes)
		{
			int32 AddressBlockLen = 0;
			switch (FMOVINOscParser::ClassifyAddress(Data, NumBytes, Matcher, AddressBlockLen))
			{
			case EMOVINOscAddress::MovinPointCloud:
			{
				FMOVINOscPointCloudChunkView View;
				if (FMOVINOscParser::ParsePointCloudChunk(Data, NumBytes, AddressBlockLen, View))
				{
					Assembler.HandleChunk(View);
				}
				else
				{
					Assembler.NoteMalformedChunk();
				}
				return;
			}

			case EMOVINOscAddress::VmcRootPos:
			case EMOVINOscAddress::VmcBonePos:
			{
				FMOVINOscMotionPacket Packet;
				if (FMOVINOscParser::ParsePacket(Data, NumBytes, Packet) == EMOVINOscPacketType::Motion)
				{
					++MotionDatagrams;
				}
				return;
			}

			default:
				++UnknownDatagrams;
				return;
			}
		});

		Receiver.Start();

		FSocket* SendSocket = FUdpSocketBuilder(TEXT("MOVINOscTestSender")).AsReusable();
		if (SendSocket == nullptr)
		{
			AddError(TEXT("Failed to create the sender socket."));
			Assembler.Shutdown();
			SocketSubsystem->DestroySocket(ReceiveSocket);
			return false;
		}

		TSharedRef<FInternetAddr> Destination = SocketSubsystem->CreateInternetAddr();
		Destination->SetIp(FIPv4Address::InternalLoopback.Value);
		Destination->SetPort(BoundPort);

		constexpr int32 TotalPoints = 1000;
		constexpr int32 MaxPointsPerChunk = 100;
		const TArray<FSenderChunk> Layout = BuildSenderChunkLayout(TotalPoints, MaxPointsPerChunk);

		// Interleave a motion datagram so the shared socket path is covered too.
		{
			TArray<uint8> Motion;
			AppendPaddedString(Motion, "/VMC/Ext/Root/Pos");
			AppendPaddedString(Motion, ",ssfffffffi");
			AppendPaddedString(Motion, "MOVIN_0");
			AppendPaddedString(Motion, "Root");
			for (float Value : { 1.0f, 2.0f, 3.0f, 0.0f, 0.0f, 0.0f, 1.0f })
			{
				MOVINOscTestUtil::AppendFloatBE(Motion, Value);
			}
			MOVINOscTestUtil::AppendInt32BE(Motion, 1);

			int32 Sent = 0;
			SendSocket->SendTo(Motion.GetData(), Motion.Num(), Sent, *Destination);
		}

		int32 DatagramsSent = 0;
		for (const FSenderChunk& Chunk : Layout)
		{
			const TArray<uint8> Bytes = BuildPointCloudMessage(
				DefaultPointCloudAddress, 1, TotalPoints, Chunk.ChunkIndex, Layout.Num(),
				Chunk.ChunkNumPoints, Chunk.ExpectedStart);

			int32 Sent = 0;
			if (SendSocket->SendTo(Bytes.GetData(), Bytes.Num(), Sent, *Destination) && Sent == Bytes.Num())
			{
				++DatagramsSent;
			}
		}

		TestEqual(TEXT("all chunks sent"), DatagramsSent, Layout.Num());

		// Poll rather than sleep a fixed amount: loopback is fast but the receive thread is
		// scheduled independently.
		FMOVINStagePointCloudFrame* Frame = nullptr;
		const double Deadline = FPlatformTime::Seconds() + 5.0;
		while (Frame == nullptr && FPlatformTime::Seconds() < Deadline)
		{
			Frame = Assembler.GetStream()->AcquireLatestReadyFrame();
			if (Frame == nullptr)
			{
				FPlatformProcess::Sleep(0.01f);
			}
		}

		TestNotNull(TEXT("frame received over loopback UDP"), Frame);
		if (Frame != nullptr)
		{
			TestTrue(TEXT("loopback frame complete"), Frame->bComplete);
			TestEqual(TEXT("loopback point count"), Frame->PointCount, TotalPoints);
			TestEqual(TEXT("loopback seq"), Frame->Seq, 1);
			for (int32 Index = 0; Index < TotalPoints; Index += 97)
			{
				TestTrue(FString::Printf(TEXT("loopback point %d"), Index),
					Frame->Points[Index].Equals(ExpectedUnrealPoint(Index, 1.0f, false), 0.001f));
			}
			Assembler.GetStream()->ReleaseFrame(Frame);
		}

		TestEqual(TEXT("motion datagram routed to the motion path"), MotionDatagrams.load(), 1);
		TestEqual(TEXT("no unknown addresses"), UnknownDatagrams.load(), 0);

		const FMOVINOscPointCloudStats Stats = Assembler.SnapshotStats();
		TestEqual(TEXT("no malformed chunks"), static_cast<int32>(Stats.MalformedChunks), 0);
		TestEqual(TEXT("no bad offsets"), static_cast<int32>(Stats.BadOffsetChunks), 0);

		// The drain loop must have terminated cleanly on EWOULDBLOCK rather than reporting errors.
		const FMOVINOscReceiveThreadStats ReceiveStats = Receiver.SnapshotStats();
		TestEqual(TEXT("no receive errors"), static_cast<int32>(ReceiveStats.RecvErrors), 0);
		TestEqual(TEXT("no drain overruns"), static_cast<int32>(ReceiveStats.DrainOverruns), 0);

		SocketSubsystem->DestroySocket(SendSocket);
	}

	Assembler.Shutdown();
	SocketSubsystem->DestroySocket(ReceiveSocket);
	return true;
}

bool FMOVINOscSourceSettingsPanelTest::RunTest(const FString& Parameters)
{
	// Drives exactly the path the Live Link details panel uses: GetSettingsClass to build the object,
	// InitializeSettings to populate it, OnSettingsChanged when the user edits a value. Without a
	// client the source never starts its socket, so this stays a pure configuration test.
	FMOVINStageLiveLinkConnectionSettings Defaults;
	TSharedRef<FMOVINStageLiveLinkSource> Source = MakeShared<FMOVINStageLiveLinkSource>(Defaults);

	const TSubclassOf<ULiveLinkSourceSettings> SettingsClass = Source->GetSettingsClass();
	TestTrue(TEXT("settings class is the MOVIN one"),
		SettingsClass.Get() == UMOVINStageLiveLinkSourceSettings::StaticClass());
	if (SettingsClass.Get() != UMOVINStageLiveLinkSourceSettings::StaticClass())
	{
		return false;
	}

	UMOVINStageLiveLinkSourceSettings* PanelSettings =
		NewObject<UMOVINStageLiveLinkSourceSettings>(GetTransientPackage(), SettingsClass.Get());
	TestNotNull(TEXT("settings object created"), PanelSettings);
	if (PanelSettings == nullptr)
	{
		return false;
	}

	// Point cloud ships disabled, so nothing should be streaming yet.
	TestFalse(TEXT("no stream before enabling"), Source->GetPointCloudStream().IsValid());

	Source->InitializeSettings(PanelSettings);

	// The panel must mirror what the source is actually running.
	TestEqual(TEXT("panel endpoint mirrors source"), PanelSettings->Endpoint, Defaults.Endpoint.ToString());
	TestEqual(TEXT("panel subject prefix defaults empty"), PanelSettings->SubjectPrefix, FString());
	TestEqual(TEXT("panel scale mirrors source"), PanelSettings->Scale, Defaults.PositionScale, 0.001f);
	TestFalse(TEXT("panel shows point cloud disabled"), PanelSettings->bEnablePointCloud);
	TestTrue(TEXT("connection string populated"), PanelSettings->ConnectionString.Contains(TEXT("PointCloud=false")));

	// Typing a prefix in the panel has to reach the connection string, or a preset saved right
	// afterwards loses it.
	PanelSettings->SubjectPrefix = TEXT("Stage_");
	{
		FPropertyChangedEvent Event(nullptr);
		Source->OnSettingsChanged(PanelSettings, Event);
	}
	TestTrue(TEXT("connection string carries subject prefix"),
		PanelSettings->ConnectionString.Contains(TEXT("SubjectPrefix=Stage_")));

	PanelSettings->SubjectPrefix.Reset();
	{
		FPropertyChangedEvent Event(nullptr);
		Source->OnSettingsChanged(PanelSettings, Event);
	}
	TestFalse(TEXT("clearing the prefix clears the connection string"),
		PanelSettings->ConnectionString.Contains(TEXT("SubjectPrefix=Stage_")));

	// Ticking the checkbox in the panel must actually bring the stream up.
	PanelSettings->bEnablePointCloud = true;
	PanelSettings->MaxPoints = 65536;
	{
		FPropertyChangedEvent Event(nullptr);
		Source->OnSettingsChanged(PanelSettings, Event);
	}

	TestTrue(TEXT("stream exists after enabling"), Source->GetPointCloudStream().IsValid());
	if (Source->GetPointCloudStream().IsValid())
	{
		TestEqual(TEXT("stream honours MaxPoints"), Source->GetPointCloudStream()->GetMaxPoints(), 65536);
	}
	TestTrue(TEXT("connection string updated"), PanelSettings->ConnectionString.Contains(TEXT("PointCloud=true")));
	TestTrue(TEXT("connection string carries MaxPoints"), PanelSettings->ConnectionString.Contains(TEXT("MaxPoints=65536")));

	// A non default coordinate mode must reach the assembler config.
	PanelSettings->PointCloudCoordinates = EMOVINOscPointCloudCoordinateMode::OpenGL;
	{
		FPropertyChangedEvent Event(nullptr);
		Source->OnSettingsChanged(PanelSettings, Event);
	}
	TestTrue(TEXT("connection string carries coordinate mode"),
		PanelSettings->ConnectionString.Contains(TEXT("PointCloudCoordinates=OpenGL")));

	// Unticking it must tear the stream back down.
	PanelSettings->bEnablePointCloud = false;
	{
		FPropertyChangedEvent Event(nullptr);
		Source->OnSettingsChanged(PanelSettings, Event);
	}
	TestFalse(TEXT("stream released after disabling"), Source->GetPointCloudStream().IsValid());

	// A preset restores through ConnectionString, so that path must work on its own.
	{
		FMOVINStageLiveLinkConnectionSettings PresetDefaults;
		TSharedRef<FMOVINStageLiveLinkSource> PresetSource = MakeShared<FMOVINStageLiveLinkSource>(PresetDefaults);

		UMOVINStageLiveLinkSourceSettings* PresetSettings =
			NewObject<UMOVINStageLiveLinkSourceSettings>(GetTransientPackage(), UMOVINStageLiveLinkSourceSettings::StaticClass());
		PresetSettings->ConnectionString = TEXT("Endpoint=0.0.0.0:11235;PointCloud=true;MaxPoints=32768");

		PresetSource->InitializeSettings(PresetSettings);

		TestTrue(TEXT("preset enabled the point cloud"), PresetSource->GetPointCloudStream().IsValid());
		TestTrue(TEXT("preset mirrored into the panel"), PresetSettings->bEnablePointCloud);
		TestEqual(TEXT("preset max points mirrored"), PresetSettings->MaxPoints, 32768);
	}

	return true;
}

bool FMOVINOscExclusiveBindTest::RunTest(const FString& Parameters)
{
	// The port protection in FMOVINStageLiveLinkSource::StartReceiver rests entirely on the receive
	// socket being built WITHOUT AsReusable. The in-process registry only sees this plugin's own
	// sources; what stops a second plugin - MOVIN LiveLink (Tracin), typically - from taking the
	// same port is the OS refusing the bind. With SO_REUSEADDR both sockets bind on Windows and
	// datagrams reach only one of them, silently. If that flag ever comes back, or a platform stops
	// honouring exclusive UDP binds, this test is what notices.
	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (SocketSubsystem == nullptr)
	{
		AddError(TEXT("No socket subsystem available."));
		return false;
	}

	// Both sockets use the same address, which is the case the plugin actually creates: two sources
	// with the same Endpoint string.
	FSocket* Holder = FUdpSocketBuilder(TEXT("MOVINOscExclusiveBindHolder"))
		.AsNonBlocking()
		.BoundToAddress(FIPv4Address::InternalLoopback)
		.BoundToPort(0);

	if (Holder == nullptr)
	{
		AddError(TEXT("Failed to bind the first loopback socket."));
		return false;
	}

	const int32 Port = Holder->GetPortNo();

	FSocket* Duplicate = FUdpSocketBuilder(TEXT("MOVINOscExclusiveBindDuplicate"))
		.AsNonBlocking()
		.BoundToAddress(FIPv4Address::InternalLoopback)
		.BoundToPort(Port);

	TestNull(TEXT("a second bind on a held port is refused without SO_REUSEADDR"), Duplicate);
	if (Duplicate != nullptr)
	{
		SocketSubsystem->DestroySocket(Duplicate);
	}

	// Releasing the port must free it again, or a source that stops and restarts - which is what
	// every settings edit does - could never reclaim its own port.
	SocketSubsystem->DestroySocket(Holder);

	FSocket* Reclaimed = FUdpSocketBuilder(TEXT("MOVINOscExclusiveBindReclaim"))
		.AsNonBlocking()
		.BoundToAddress(FIPv4Address::InternalLoopback)
		.BoundToPort(Port);

	TestNotNull(TEXT("the port is bindable again once released"), Reclaimed);
	if (Reclaimed != nullptr)
	{
		SocketSubsystem->DestroySocket(Reclaimed);
	}

	return true;
}

bool FMOVINOscSettingsRoundTripTest::RunTest(const FString& Parameters)
{
	using FSettings = FMOVINStageLiveLinkConnectionSettings;

	FSettings Original;
	FIPv4Endpoint::Parse(TEXT("127.0.0.1:11999"), Original.Endpoint);
	Original.SubjectPrefix = TEXT("Stage_");
	Original.PositionScale = 42.5f;
	Original.TimeoutSeconds = 2.25;
	Original.CoordinateSystem = FSettings::ECoordinateSystem::OpenGL;
	Original.FingerNameMode = FSettings::EFingerNameMode::Osc;
	Original.bApplyStreamRotationOffset = false;
	Original.RotationOffsetDegrees = FVector(1.5, -2.25, 3.125);
	Original.bPointCloudEnabled = true;
	Original.PointCloudMaxPoints = 123456;
	Original.PointCloudCoordinates = FSettings::EPointCloudCoordinateMode::Unity;
	Original.PointCloudScale = 7.5f;
	Original.PointCloudOffset = FVector(10.5, -20.25, 30.125);
	Original.PointCloudAddress = TEXT("/MOVIN/Cloud");
	Original.bPointCloudPartialPublish = false;
	Original.PointCloudSeqResyncThreshold = 250;
	Original.ReceiveBufferBytes = 16 * 1024 * 1024;
	Original.MaxDatagramsPerWait = 4096;

	const FSettings RoundTripped = FSettings::FromConnectionString(Original.ToConnectionString());

	TestEqual(TEXT("endpoint"), RoundTripped.Endpoint.ToString(), Original.Endpoint.ToString());
	TestEqual(TEXT("subject prefix"), RoundTripped.SubjectPrefix, Original.SubjectPrefix);
	TestEqual(TEXT("scale"), RoundTripped.PositionScale, Original.PositionScale, 0.001f);
	TestEqual(TEXT("timeout"), RoundTripped.TimeoutSeconds, Original.TimeoutSeconds, 0.001);
	TestTrue(TEXT("coordinates"), RoundTripped.CoordinateSystem == Original.CoordinateSystem);
	TestTrue(TEXT("finger names"), RoundTripped.FingerNameMode == Original.FingerNameMode);
	TestEqual(TEXT("apply rotation offset"), RoundTripped.bApplyStreamRotationOffset, Original.bApplyStreamRotationOffset);
	TestTrue(TEXT("rotation offset"), RoundTripped.RotationOffsetDegrees.Equals(Original.RotationOffsetDegrees, 0.001));
	TestEqual(TEXT("point cloud enabled"), RoundTripped.bPointCloudEnabled, Original.bPointCloudEnabled);
	TestEqual(TEXT("max points"), RoundTripped.PointCloudMaxPoints, Original.PointCloudMaxPoints);
	TestTrue(TEXT("point cloud coordinates"), RoundTripped.PointCloudCoordinates == Original.PointCloudCoordinates);
	TestEqual(TEXT("point cloud scale"), RoundTripped.PointCloudScale, Original.PointCloudScale, 0.001f);
	TestTrue(TEXT("point cloud offset"), RoundTripped.PointCloudOffset.Equals(Original.PointCloudOffset, 0.001));
	TestEqual(TEXT("point cloud address"), RoundTripped.PointCloudAddress, Original.PointCloudAddress);
	TestEqual(TEXT("partial publish"), RoundTripped.bPointCloudPartialPublish, Original.bPointCloudPartialPublish);
	TestEqual(TEXT("resync threshold"), RoundTripped.PointCloudSeqResyncThreshold, Original.PointCloudSeqResyncThreshold);
	TestEqual(TEXT("recv buffer bytes"), RoundTripped.ReceiveBufferBytes, Original.ReceiveBufferBytes);
	TestEqual(TEXT("max datagrams per wait"), RoundTripped.MaxDatagramsPerWait, Original.MaxDatagramsPerWait);

	// Defaults must survive a round trip too, including PointCloudScale's zero-means-inherit sentinel.
	const FSettings Defaults;
	const FSettings DefaultsRoundTripped = FSettings::FromConnectionString(Defaults.ToConnectionString());
	TestEqual(TEXT("default point cloud disabled"), DefaultsRoundTripped.bPointCloudEnabled, false);
	TestEqual(TEXT("default point cloud scale inherits"), DefaultsRoundTripped.PointCloudScale, 0.0f);
	TestEqual(TEXT("default effective point cloud scale"),
		DefaultsRoundTripped.GetEffectivePointCloudScale(), Defaults.PositionScale, 0.001f);
	TestTrue(TEXT("default point cloud coordinates inherit"),
		DefaultsRoundTripped.GetEffectivePointCloudCoordinateSystem() == Defaults.CoordinateSystem);
	TestEqual(TEXT("default max points"), DefaultsRoundTripped.PointCloudMaxPoints, Defaults.PointCloudMaxPoints);
	TestEqual(TEXT("default address"), DefaultsRoundTripped.PointCloudAddress, Defaults.PointCloudAddress);
	TestEqual(TEXT("default subject prefix empty"), DefaultsRoundTripped.SubjectPrefix, FString());

	// A preset saved before SubjectPrefix existed has no such key. It has to load as an empty prefix,
	// or every subject in that project silently changes name and its Anim Blueprints stop resolving.
	// It also still carries the retired Actors key, which must be tolerated rather than rejected.
	const FSettings Legacy = FSettings::FromConnectionString(
		TEXT("Endpoint=0.0.0.0:11235;Actors=MOVIN_0,MOVIN_1;Scale=100.000;Timeout=1.000")
		TEXT(";Coordinates=Unity;FingerNames=MovinManNotch;ApplyRotationOffset=true")
		TEXT(";RotationOffset=0.000,-90.000,-90.000;PointCloud=false;MaxPoints=331776")
		TEXT(";PointCloudCoordinates=Inherit;PointCloudScale=0.000"));

	TestEqual(TEXT("legacy prefix empty"), Legacy.SubjectPrefix, FString());
	TestEqual(TEXT("legacy endpoint"), Legacy.Endpoint.ToString(), FString(TEXT("0.0.0.0:11235")));
	TestEqual(TEXT("legacy scale"), Legacy.PositionScale, 100.0f, 0.001f);
	TestFalse(TEXT("retired Actors key is not written back"),
		Legacy.ToConnectionString().Contains(TEXT("Actors=")));

	// An explicitly empty prefix must also survive, so clearing the field in the panel sticks.
	FSettings Cleared = Original;
	Cleared.SubjectPrefix.Reset();
	TestEqual(TEXT("cleared prefix stays empty"),
		FSettings::FromConnectionString(Cleared.ToConnectionString()).SubjectPrefix, FString());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
