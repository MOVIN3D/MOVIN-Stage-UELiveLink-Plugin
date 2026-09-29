#if WITH_DEV_AUTOMATION_TESTS

#include "MOVINOscParser.h"
#include "MOVINOscTestHelpers.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMOVINOscParserNoFrameTest, "MOVIN.Stage.Parser.NoFrame", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMOVINOscParserWithFrameTest, "MOVIN.Stage.Parser.WithFrame", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMOVINOscParserPointCloudTest, "MOVIN.Stage.Parser.PointCloudIgnored", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
	using MOVINOscTestUtil::AppendFloatBE;
	using MOVINOscTestUtil::AppendInt32BE;
	using MOVINOscTestUtil::AppendPaddedString;

	TArray<uint8> BuildMotionMessage(const ANSICHAR* TypeTags, bool bWithFrame)
	{
		TArray<uint8> Bytes;
		AppendPaddedString(Bytes, "/VMC/Ext/Root/Pos");
		AppendPaddedString(Bytes, TypeTags);
		AppendPaddedString(Bytes, "MOVIN_1");
		AppendPaddedString(Bytes, "Root");
		for (float Value : {1.0f, 2.0f, 3.0f, 0.0f, 0.0f, 0.0f, 1.0f})
		{
			AppendFloatBE(Bytes, Value);
		}
		if (bWithFrame)
		{
			AppendInt32BE(Bytes, 42);
		}
		return Bytes;
	}
}

bool FMOVINOscParserNoFrameTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Bytes = BuildMotionMessage(",ssfffffff", false);
	FMOVINOscMotionPacket Packet;
	TestEqual(TEXT("packet type"), FMOVINOscParser::ParsePacket(Bytes.GetData(), Bytes.Num(), Packet), EMOVINOscPacketType::Motion);
	TestEqual(TEXT("actor"), Packet.ActorName, FString(TEXT("MOVIN_1")));
	TestEqual(TEXT("bone"), Packet.BoneName, FString(TEXT("Root")));
	TestFalse(TEXT("frame index absent"), Packet.bHasFrameIndex);
	TestEqual(TEXT("position z"), Packet.Position.Z, 3.0f);
	return true;
}

bool FMOVINOscParserWithFrameTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Bytes = BuildMotionMessage(",ssfffffffi", true);
	FMOVINOscMotionPacket Packet;
	TestEqual(TEXT("packet type"), FMOVINOscParser::ParsePacket(Bytes.GetData(), Bytes.Num(), Packet), EMOVINOscPacketType::Motion);
	TestTrue(TEXT("frame index present"), Packet.bHasFrameIndex);
	TestEqual(TEXT("frame index"), Packet.FrameIndex, 42);
	return true;
}

bool FMOVINOscParserPointCloudTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Bytes;
	AppendPaddedString(Bytes, "/MOVIN/PointCloud");
	AppendPaddedString(Bytes, ",iiiii");
	FMOVINOscMotionPacket Packet;
	TestEqual(TEXT("packet type"), FMOVINOscParser::ParsePacket(Bytes.GetData(), Bytes.Num(), Packet), EMOVINOscPacketType::PointCloud);
	return true;
}

#endif
