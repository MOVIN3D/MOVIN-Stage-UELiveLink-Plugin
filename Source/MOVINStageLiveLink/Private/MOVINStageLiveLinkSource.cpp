#include "MOVINStageLiveLinkSource.h"

#include "ILiveLinkClient.h"
#include "MOVINStagePointCloudRegistry.h"
#include "MOVINStageLiveLinkLog.h"
#include "MOVINStageLiveLinkSourceSettings.h"
#include "MOVINOscParser.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "Roles/LiveLinkAnimationTypes.h"
#include "Common/UdpSocketBuilder.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

FCriticalSection FMOVINStageLiveLinkSource::ActivePortsCriticalSection;
TSet<int32> FMOVINStageLiveLinkSource::ActivePorts;

namespace
{
	// Fallback flush for the legacy ",ssfffffff" stream, which carries no frame index. The current
	// stream flushes on frame index change instead, so this only guards a stalled partial frame.
	constexpr double PendingFrameFlushSeconds = 1.0 / 60.0;

	constexpr double RateSampleIntervalSeconds = 0.5;

	bool TryParseEndpoint(const FString& Text, FIPv4Endpoint& OutEndpoint)
	{
		if (Text.IsEmpty())
		{
			return false;
		}

		return FIPv4Endpoint::Parse(Text, OutEndpoint);
	}

	bool TryParseBool(const FString& Text, bool& OutValue)
	{
		if (Text.Equals(TEXT("true"), ESearchCase::IgnoreCase) ||
			Text.Equals(TEXT("1"), ESearchCase::IgnoreCase) ||
			Text.Equals(TEXT("yes"), ESearchCase::IgnoreCase))
		{
			OutValue = true;
			return true;
		}

		if (Text.Equals(TEXT("false"), ESearchCase::IgnoreCase) ||
			Text.Equals(TEXT("0"), ESearchCase::IgnoreCase) ||
			Text.Equals(TEXT("no"), ESearchCase::IgnoreCase))
		{
			OutValue = false;
			return true;
		}

		return false;
	}

	bool TryParseVector(const FString& Text, FVector& OutVector)
	{
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(","), true);
		if (Parts.Num() != 3)
		{
			return false;
		}

		for (FString& Part : Parts)
		{
			Part.TrimStartAndEndInline();
		}

		OutVector = FVector(
			FCString::Atod(*Parts[0]),
			FCString::Atod(*Parts[1]),
			FCString::Atod(*Parts[2]));
		return true;
	}

	const TCHAR* CoordinateSystemToString(FMOVINStageLiveLinkConnectionSettings::ECoordinateSystem CoordinateSystem)
	{
		switch (CoordinateSystem)
		{
		case FMOVINStageLiveLinkConnectionSettings::ECoordinateSystem::Unity:
			return TEXT("Unity");
		case FMOVINStageLiveLinkConnectionSettings::ECoordinateSystem::OpenGL:
		default:
			return TEXT("OpenGL");
		}
	}

	const TCHAR* FingerNameModeToString(FMOVINStageLiveLinkConnectionSettings::EFingerNameMode FingerNameMode)
	{
		switch (FingerNameMode)
		{
		case FMOVINStageLiveLinkConnectionSettings::EFingerNameMode::Osc:
			return TEXT("OSC");
		case FMOVINStageLiveLinkConnectionSettings::EFingerNameMode::MovinManNotch:
		default:
			return TEXT("MovinManNotch");
		}
	}

	const TCHAR* PointCloudCoordinateModeToString(FMOVINStageLiveLinkConnectionSettings::EPointCloudCoordinateMode Mode)
	{
		switch (Mode)
		{
		case FMOVINStageLiveLinkConnectionSettings::EPointCloudCoordinateMode::Unity:
			return TEXT("Unity");
		case FMOVINStageLiveLinkConnectionSettings::EPointCloudCoordinateMode::OpenGL:
			return TEXT("OpenGL");
		case FMOVINStageLiveLinkConnectionSettings::EPointCloudCoordinateMode::Inherit:
		default:
			return TEXT("Inherit");
		}
	}

	const TMap<FName, FName>& GetMovinManNotchFingerNameMap()
	{
		static const TMap<FName, FName> FingerNameMap = {
			{ TEXT("LeftIndexProximal"), TEXT("RightThumbProximal") },
			{ TEXT("LeftIndexIntermediate"), TEXT("RightThumbIntermediate") },
			{ TEXT("LeftIndexDistal"), TEXT("RightThumbDistal") },
			{ TEXT("LeftMiddleProximal"), TEXT("RightIndexProximal") },
			{ TEXT("LeftMiddleIntermediate"), TEXT("RightIndexIntermediate") },
			{ TEXT("LeftMiddleDistal"), TEXT("RightIndexDistal") },
			{ TEXT("LeftLittleProximal"), TEXT("RightMiddleProximal") },
			{ TEXT("LeftLittleIntermediate"), TEXT("RightMiddleIntermediate") },
			{ TEXT("LeftLittleDistal"), TEXT("RightMiddleDistal") },
			{ TEXT("LeftRingProximal"), TEXT("RightRingProximal") },
			{ TEXT("LeftRingIntermediate"), TEXT("RightRingIntermediate") },
			{ TEXT("LeftRingDistal"), TEXT("RightRingDistal") },
			{ TEXT("LeftThumbProximal"), TEXT("RightLittleProximal") },
			{ TEXT("LeftThumbIntermediate"), TEXT("RightLittleIntermediate") },
			{ TEXT("LeftThumbDistal"), TEXT("RightLittleDistal") },

			{ TEXT("RightIndexProximal"), TEXT("LeftThumbProximal") },
			{ TEXT("RightIndexIntermediate"), TEXT("LeftThumbIntermediate") },
			{ TEXT("RightIndexDistal"), TEXT("LeftThumbDistal") },
			{ TEXT("RightMiddleProximal"), TEXT("LeftIndexProximal") },
			{ TEXT("RightMiddleIntermediate"), TEXT("LeftIndexIntermediate") },
			{ TEXT("RightMiddleDistal"), TEXT("LeftIndexDistal") },
			{ TEXT("RightLittleProximal"), TEXT("LeftMiddleProximal") },
			{ TEXT("RightLittleIntermediate"), TEXT("LeftMiddleIntermediate") },
			{ TEXT("RightLittleDistal"), TEXT("LeftMiddleDistal") },
			{ TEXT("RightRingProximal"), TEXT("LeftRingProximal") },
			{ TEXT("RightRingIntermediate"), TEXT("LeftRingIntermediate") },
			{ TEXT("RightRingDistal"), TEXT("LeftRingDistal") },
			{ TEXT("RightThumbProximal"), TEXT("LeftLittleProximal") },
			{ TEXT("RightThumbIntermediate"), TEXT("LeftLittleIntermediate") },
			{ TEXT("RightThumbDistal"), TEXT("LeftLittleDistal") }
		};
		return FingerNameMap;
	}
}

FMOVINStageLiveLinkConnectionSettings FMOVINStageLiveLinkConnectionSettings::FromConnectionString(const FString& ConnectionString)
{
	FMOVINStageLiveLinkConnectionSettings Result;
	if (ConnectionString.TrimStartAndEnd().IsEmpty())
	{
		return Result;
	}

	TArray<FString> Pairs;
	ConnectionString.ParseIntoArray(Pairs, TEXT(";"), true);
	for (const FString& Pair : Pairs)
	{
		FString Key;
		FString Value;
		if (!Pair.Split(TEXT("="), &Key, &Value))
		{
			continue;
		}

		Key.TrimStartAndEndInline();
		Value.TrimStartAndEndInline();

		if (Key.Equals(TEXT("Endpoint"), ESearchCase::IgnoreCase))
		{
			FIPv4Endpoint ParsedEndpoint;
			if (TryParseEndpoint(Value, ParsedEndpoint))
			{
				Result.Endpoint = ParsedEndpoint;
			}
		}
		else if (Key.Equals(TEXT("Actors"), ESearchCase::IgnoreCase))
		{
			// Written by versions that published a fixed actor list. Subjects are now created for
			// every actor name that arrives, so the key is accepted and ignored to keep old presets
			// loading.
		}
		else if (Key.Equals(TEXT("SubjectPrefix"), ESearchCase::IgnoreCase))
		{
			// Absent in strings written before the prefix existed, which is why it parses to empty.
			Result.SubjectPrefix = Value;
		}
		else if (Key.Equals(TEXT("Scale"), ESearchCase::IgnoreCase))
		{
			Result.PositionScale = FCString::Atof(*Value);
			if (Result.PositionScale <= 0.0f)
			{
				Result.PositionScale = 100.0f;
			}
		}
		else if (Key.Equals(TEXT("Timeout"), ESearchCase::IgnoreCase))
		{
			Result.TimeoutSeconds = FCString::Atod(*Value);
			if (Result.TimeoutSeconds <= 0.0)
			{
				Result.TimeoutSeconds = 1.0;
			}
		}
		else if (Key.Equals(TEXT("Coordinates"), ESearchCase::IgnoreCase) ||
			Key.Equals(TEXT("CoordinateSystem"), ESearchCase::IgnoreCase))
		{
			if (Value.Equals(TEXT("Unity"), ESearchCase::IgnoreCase))
			{
				Result.CoordinateSystem = ECoordinateSystem::Unity;
			}
			else if (Value.Equals(TEXT("OpenGL"), ESearchCase::IgnoreCase) ||
				Value.Equals(TEXT("GL"), ESearchCase::IgnoreCase))
			{
				Result.CoordinateSystem = ECoordinateSystem::OpenGL;
			}
		}
		else if (Key.Equals(TEXT("FingerNames"), ESearchCase::IgnoreCase) ||
			Key.Equals(TEXT("FingerNameMode"), ESearchCase::IgnoreCase))
		{
			// "VMC" is what presets saved before the OSC rename contain; kept as an alias so they load.
			if (Value.Equals(TEXT("OSC"), ESearchCase::IgnoreCase) ||
				Value.Equals(TEXT("VMC"), ESearchCase::IgnoreCase) ||
				Value.Equals(TEXT("Raw"), ESearchCase::IgnoreCase) ||
				Value.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			{
				Result.FingerNameMode = EFingerNameMode::Osc;
			}
			else if (Value.Equals(TEXT("MovinManNotch"), ESearchCase::IgnoreCase) ||
				Value.Equals(TEXT("MOVINManNotch"), ESearchCase::IgnoreCase) ||
				Value.Equals(TEXT("Notch"), ESearchCase::IgnoreCase))
			{
				Result.FingerNameMode = EFingerNameMode::MovinManNotch;
			}
		}
		else if (Key.Equals(TEXT("ApplyRotationOffset"), ESearchCase::IgnoreCase) ||
			Key.Equals(TEXT("ApplyStreamRotationOffset"), ESearchCase::IgnoreCase))
		{
			bool bParsedValue = false;
			if (TryParseBool(Value, bParsedValue))
			{
				Result.bApplyStreamRotationOffset = bParsedValue;
			}
		}
		else if (Key.Equals(TEXT("RotationOffset"), ESearchCase::IgnoreCase) ||
			Key.Equals(TEXT("StreamRotationOffset"), ESearchCase::IgnoreCase))
		{
			FVector ParsedOffset;
			if (TryParseVector(Value, ParsedOffset))
			{
				Result.RotationOffsetDegrees = ParsedOffset;
			}
		}
		else if (Key.Equals(TEXT("PointCloud"), ESearchCase::IgnoreCase))
		{
			bool bParsedValue = false;
			if (TryParseBool(Value, bParsedValue))
			{
				Result.bPointCloudEnabled = bParsedValue;
			}
		}
		else if (Key.Equals(TEXT("MaxPoints"), ESearchCase::IgnoreCase))
		{
			const int32 ParsedMaxPoints = FCString::Atoi(*Value);
			if (ParsedMaxPoints > 0)
			{
				Result.PointCloudMaxPoints = ParsedMaxPoints;
			}
		}
		else if (Key.Equals(TEXT("PointCloudCoordinates"), ESearchCase::IgnoreCase))
		{
			if (Value.Equals(TEXT("Unity"), ESearchCase::IgnoreCase))
			{
				Result.PointCloudCoordinates = EPointCloudCoordinateMode::Unity;
			}
			else if (Value.Equals(TEXT("OpenGL"), ESearchCase::IgnoreCase) ||
				Value.Equals(TEXT("GL"), ESearchCase::IgnoreCase))
			{
				Result.PointCloudCoordinates = EPointCloudCoordinateMode::OpenGL;
			}
			else if (Value.Equals(TEXT("Inherit"), ESearchCase::IgnoreCase))
			{
				Result.PointCloudCoordinates = EPointCloudCoordinateMode::Inherit;
			}
		}
		else if (Key.Equals(TEXT("PointCloudScale"), ESearchCase::IgnoreCase))
		{
			const float ParsedScale = FCString::Atof(*Value);
			Result.PointCloudScale = ParsedScale > 0.0f ? ParsedScale : 0.0f;
		}
		else if (Key.Equals(TEXT("PointCloudOffset"), ESearchCase::IgnoreCase))
		{
			FVector ParsedOffset;
			if (TryParseVector(Value, ParsedOffset))
			{
				Result.PointCloudOffset = ParsedOffset;
			}
		}
		else if (Key.Equals(TEXT("PointCloudAddress"), ESearchCase::IgnoreCase))
		{
			if (Value.StartsWith(TEXT("/")))
			{
				Result.PointCloudAddress = Value;
			}
		}
		else if (Key.Equals(TEXT("PointCloudPartialPublish"), ESearchCase::IgnoreCase))
		{
			bool bParsedValue = false;
			if (TryParseBool(Value, bParsedValue))
			{
				Result.bPointCloudPartialPublish = bParsedValue;
			}
		}
		else if (Key.Equals(TEXT("SeqResyncThreshold"), ESearchCase::IgnoreCase))
		{
			const int32 ParsedThreshold = FCString::Atoi(*Value);
			if (ParsedThreshold > 0)
			{
				Result.PointCloudSeqResyncThreshold = ParsedThreshold;
			}
		}
		else if (Key.Equals(TEXT("RecvBufferBytes"), ESearchCase::IgnoreCase))
		{
			const int32 ParsedBytes = FCString::Atoi(*Value);
			if (ParsedBytes > 0)
			{
				Result.ReceiveBufferBytes = ParsedBytes;
			}
		}
		else if (Key.Equals(TEXT("MaxDatagramsPerWait"), ESearchCase::IgnoreCase))
		{
			const int32 ParsedBudget = FCString::Atoi(*Value);
			if (ParsedBudget > 0)
			{
				Result.MaxDatagramsPerWait = ParsedBudget;
			}
		}
	}

	return Result;
}

FMOVINStageLiveLinkConnectionSettings::ECoordinateSystem FMOVINStageLiveLinkConnectionSettings::GetEffectivePointCloudCoordinateSystem() const
{
	switch (PointCloudCoordinates)
	{
	case EPointCloudCoordinateMode::Unity:
		return ECoordinateSystem::Unity;
	case EPointCloudCoordinateMode::OpenGL:
		return ECoordinateSystem::OpenGL;
	case EPointCloudCoordinateMode::Inherit:
	default:
		// Inheriting makes it impossible for the cloud and the skeleton to end up in mirrored spaces.
		return CoordinateSystem;
	}
}

float FMOVINStageLiveLinkConnectionSettings::GetEffectivePointCloudScale() const
{
	return PointCloudScale > 0.0f ? PointCloudScale : PositionScale;
}

FString FMOVINStageLiveLinkConnectionSettings::ToConnectionString() const
{
	// Every key parsed by FromConnectionString must be emitted here. The source factory uses
	// EMenuType::MenuEntry with no custom UI, so LiveLink presets round trip through this string -
	// a key missing here is silently lost on save and reload.
	return FString::Printf(
		TEXT("Endpoint=%s;SubjectPrefix=%s;Scale=%.3f;Timeout=%.3f;Coordinates=%s;FingerNames=%s")
		TEXT(";ApplyRotationOffset=%s;RotationOffset=%.3f,%.3f,%.3f")
		TEXT(";PointCloud=%s;MaxPoints=%d;PointCloudCoordinates=%s;PointCloudScale=%.3f")
		TEXT(";PointCloudOffset=%.3f,%.3f,%.3f;PointCloudAddress=%s;PointCloudPartialPublish=%s")
		TEXT(";SeqResyncThreshold=%d;RecvBufferBytes=%d;MaxDatagramsPerWait=%d"),
		*Endpoint.ToString(),
		*SubjectPrefix,
		PositionScale,
		TimeoutSeconds,
		CoordinateSystemToString(CoordinateSystem),
		FingerNameModeToString(FingerNameMode),
		bApplyStreamRotationOffset ? TEXT("true") : TEXT("false"),
		RotationOffsetDegrees.X,
		RotationOffsetDegrees.Y,
		RotationOffsetDegrees.Z,
		bPointCloudEnabled ? TEXT("true") : TEXT("false"),
		PointCloudMaxPoints,
		PointCloudCoordinateModeToString(PointCloudCoordinates),
		PointCloudScale,
		PointCloudOffset.X,
		PointCloudOffset.Y,
		PointCloudOffset.Z,
		*PointCloudAddress,
		bPointCloudPartialPublish ? TEXT("true") : TEXT("false"),
		PointCloudSeqResyncThreshold,
		ReceiveBufferBytes,
		MaxDatagramsPerWait);
}

FMOVINStageLiveLinkSource::FMOVINStageLiveLinkSource(const FMOVINStageLiveLinkConnectionSettings& InSettings)
	: Settings(InSettings)
{
	// No actor state is created up front: subjects appear as their first packets arrive.
	PointCloudMatcher = FMOVINOscAddressMatcher::Make(Settings.PointCloudAddress);
	InitializePointCloudAssembler();
}

void FMOVINStageLiveLinkSource::InitializePointCloudAssembler()
{
	if (!Settings.bPointCloudEnabled)
	{
		return;
	}

	FMOVINOscPointCloudAssemblerConfig AssemblerConfig;
	AssemblerConfig.MaxPoints = Settings.PointCloudMaxPoints;
	AssemblerConfig.SeqResyncThreshold = Settings.PointCloudSeqResyncThreshold;
	AssemblerConfig.bPartialPublish = Settings.bPointCloudPartialPublish;
	AssemblerConfig.PositionScale = Settings.GetEffectivePointCloudScale();
	AssemblerConfig.bNegateSourceX =
		Settings.GetEffectivePointCloudCoordinateSystem() == FMOVINStageLiveLinkConnectionSettings::ECoordinateSystem::OpenGL;
	AssemblerConfig.OffsetCm = FVector3f(Settings.PointCloudOffset);
	PointCloudAssembler.Initialize(AssemblerConfig);
}

void FMOVINStageLiveLinkSource::ApplyConnectionSettings(const FMOVINStageLiveLinkConnectionSettings& NewSettings)
{
	// StopReceiver joins the receive thread, so nothing is reading Settings or the assembler after
	// this returns. That is why the config is swapped here rather than guarded with a lock.
	//
	// The restart condition is "the source has a client and is not shutting down", not "the receiver
	// happens to be running right now". A start that failed - a port already owned by something else
	// being the usual reason - leaves the receiver stopped, and keying off that would mean the user
	// could fix the port in the panel and never get a retry.
	const bool bShouldRun = Client != nullptr && !bStopping.load();
	const FString PreviousSubjectPrefix = Settings.SubjectPrefix;
	StopReceiver();

	Settings = NewSettings;
	PointCloudMatcher = FMOVINOscAddressMatcher::Make(Settings.PointCloudAddress);

	InitializePointCloudAssembler();

	if (Client != nullptr)
	{
		// Actors already seen keep their state across an edit, so their motion resumes without a gap.
		// A prefix change renames every subject: retire the old names first, or they linger in the
		// panel as subjects that never receive a frame - the exact ambiguity a prefix is meant to remove.
		if (PreviousSubjectPrefix != Settings.SubjectPrefix)
		{
			RemoveSubjectsForNames(GetKnownActorNames(), PreviousSubjectPrefix);
		}
		PushStaticDataForSubjects();
	}

	if (bShouldRun)
	{
		StartReceiver();

		if (PointCloudAssembler.IsInitialized())
		{
			FMOVINStagePointCloudRegistry::Register(SourceGuid, PointCloudAssembler.GetStream());
		}
	}
}

TSubclassOf<ULiveLinkSourceSettings> FMOVINStageLiveLinkSource::GetSettingsClass() const
{
	return UMOVINStageLiveLinkSourceSettings::StaticClass();
}

void FMOVINStageLiveLinkSource::InitializeSettings(ULiveLinkSourceSettings* InSettings)
{
	UMOVINStageLiveLinkSourceSettings* MovinSettings = Cast<UMOVINStageLiveLinkSourceSettings>(InSettings);
	if (MovinSettings == nullptr)
	{
		return;
	}

	// A preset restores ConnectionString, so it wins on load. A freshly added source has an empty
	// string and keeps the defaults it was constructed with.
	if (!MovinSettings->ConnectionString.IsEmpty())
	{
		ApplyConnectionSettings(FMOVINStageLiveLinkConnectionSettings::FromConnectionString(MovinSettings->ConnectionString));
	}

	// Mirror the live configuration into the panel so what is shown is what is running.
	MovinSettings->Endpoint = Settings.Endpoint.ToString();
	MovinSettings->SubjectPrefix = Settings.SubjectPrefix;
	MovinSettings->Scale = Settings.PositionScale;
	MovinSettings->TimeoutSeconds = static_cast<float>(Settings.TimeoutSeconds);
	MovinSettings->Coordinates = Settings.CoordinateSystem == FMOVINStageLiveLinkConnectionSettings::ECoordinateSystem::OpenGL
		? EMOVINOscCoordinateSystem::OpenGL
		: EMOVINOscCoordinateSystem::Unity;
	MovinSettings->FingerNames = Settings.FingerNameMode == FMOVINStageLiveLinkConnectionSettings::EFingerNameMode::Osc
		? EMOVINOscFingerNameMode::Osc
		: EMOVINOscFingerNameMode::MovinManNotch;
	MovinSettings->bApplyRotationOffset = Settings.bApplyStreamRotationOffset;
	MovinSettings->RotationOffset = Settings.RotationOffsetDegrees;

	MovinSettings->bEnablePointCloud = Settings.bPointCloudEnabled;
	MovinSettings->MaxPoints = Settings.PointCloudMaxPoints;
	switch (Settings.PointCloudCoordinates)
	{
	case FMOVINStageLiveLinkConnectionSettings::EPointCloudCoordinateMode::Unity:
		MovinSettings->PointCloudCoordinates = EMOVINOscPointCloudCoordinateMode::Unity;
		break;
	case FMOVINStageLiveLinkConnectionSettings::EPointCloudCoordinateMode::OpenGL:
		MovinSettings->PointCloudCoordinates = EMOVINOscPointCloudCoordinateMode::OpenGL;
		break;
	default:
		MovinSettings->PointCloudCoordinates = EMOVINOscPointCloudCoordinateMode::Inherit;
		break;
	}
	MovinSettings->PointCloudScale = Settings.PointCloudScale;
	MovinSettings->PointCloudOffset = Settings.PointCloudOffset;
	MovinSettings->PointCloudAddress = Settings.PointCloudAddress;
	MovinSettings->bPointCloudPartialPublish = Settings.bPointCloudPartialPublish;
	MovinSettings->SeqResyncThreshold = Settings.PointCloudSeqResyncThreshold;
	MovinSettings->ReceiveBufferBytes = Settings.ReceiveBufferBytes;
	MovinSettings->MaxDatagramsPerWait = Settings.MaxDatagramsPerWait;

	MovinSettings->ConnectionString = Settings.ToConnectionString();
}

void FMOVINStageLiveLinkSource::OnSettingsChanged(ULiveLinkSourceSettings* InSettings, const FPropertyChangedEvent& PropertyChangedEvent)
{
	ILiveLinkSource::OnSettingsChanged(InSettings, PropertyChangedEvent);

	const UMOVINStageLiveLinkSourceSettings* MovinSettings = Cast<UMOVINStageLiveLinkSourceSettings>(InSettings);
	if (MovinSettings == nullptr)
	{
		return;
	}

	FMOVINStageLiveLinkConnectionSettings NewSettings;

	FIPv4Endpoint ParsedEndpoint;
	if (FIPv4Endpoint::Parse(MovinSettings->Endpoint, ParsedEndpoint))
	{
		NewSettings.Endpoint = ParsedEndpoint;
	}

	NewSettings.SubjectPrefix = MovinSettings->SubjectPrefix.TrimStartAndEnd();

	NewSettings.PositionScale = MovinSettings->Scale > 0.0f ? MovinSettings->Scale : 100.0f;
	NewSettings.TimeoutSeconds = MovinSettings->TimeoutSeconds > 0.0f ? MovinSettings->TimeoutSeconds : 1.0;
	NewSettings.CoordinateSystem = MovinSettings->Coordinates == EMOVINOscCoordinateSystem::OpenGL
		? FMOVINStageLiveLinkConnectionSettings::ECoordinateSystem::OpenGL
		: FMOVINStageLiveLinkConnectionSettings::ECoordinateSystem::Unity;
	NewSettings.FingerNameMode = MovinSettings->FingerNames == EMOVINOscFingerNameMode::Osc
		? FMOVINStageLiveLinkConnectionSettings::EFingerNameMode::Osc
		: FMOVINStageLiveLinkConnectionSettings::EFingerNameMode::MovinManNotch;
	NewSettings.bApplyStreamRotationOffset = MovinSettings->bApplyRotationOffset;
	NewSettings.RotationOffsetDegrees = MovinSettings->RotationOffset;

	NewSettings.bPointCloudEnabled = MovinSettings->bEnablePointCloud;
	NewSettings.PointCloudMaxPoints = FMath::Max(MovinSettings->MaxPoints, 1);
	switch (MovinSettings->PointCloudCoordinates)
	{
	case EMOVINOscPointCloudCoordinateMode::Unity:
		NewSettings.PointCloudCoordinates = FMOVINStageLiveLinkConnectionSettings::EPointCloudCoordinateMode::Unity;
		break;
	case EMOVINOscPointCloudCoordinateMode::OpenGL:
		NewSettings.PointCloudCoordinates = FMOVINStageLiveLinkConnectionSettings::EPointCloudCoordinateMode::OpenGL;
		break;
	default:
		NewSettings.PointCloudCoordinates = FMOVINStageLiveLinkConnectionSettings::EPointCloudCoordinateMode::Inherit;
		break;
	}
	NewSettings.PointCloudScale = FMath::Max(MovinSettings->PointCloudScale, 0.0f);
	NewSettings.PointCloudOffset = MovinSettings->PointCloudOffset;
	if (MovinSettings->PointCloudAddress.StartsWith(TEXT("/")))
	{
		NewSettings.PointCloudAddress = MovinSettings->PointCloudAddress;
	}
	NewSettings.bPointCloudPartialPublish = MovinSettings->bPointCloudPartialPublish;
	NewSettings.PointCloudSeqResyncThreshold = FMath::Max(MovinSettings->SeqResyncThreshold, 1);
	NewSettings.ReceiveBufferBytes = FMath::Max(MovinSettings->ReceiveBufferBytes, 64 * 1024);
	NewSettings.MaxDatagramsPerWait = FMath::Max(MovinSettings->MaxDatagramsPerWait, 1);

	ApplyConnectionSettings(NewSettings);

	// Keep the string in sync so a preset saved after this edit reproduces it.
	InSettings->ConnectionString = Settings.ToConnectionString();
}

FMOVINStageLiveLinkSource::~FMOVINStageLiveLinkSource()
{
	StopReceiver();
}

TSharedPtr<FMOVINStagePointCloudStream, ESPMode::ThreadSafe> FMOVINStageLiveLinkSource::GetPointCloudStream() const
{
	return PointCloudAssembler.GetStream();
}

void FMOVINStageLiveLinkSource::ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid)
{
	Client = InClient;
	SourceGuid = InSourceGuid;
	PushStaticDataForSubjects();
	StartReceiver();

	if (PointCloudAssembler.IsInitialized())
	{
		FMOVINStagePointCloudRegistry::Register(SourceGuid, PointCloudAssembler.GetStream());
	}
}

void FMOVINStageLiveLinkSource::Update()
{
	if (Client == nullptr || bStopping)
	{
		return;
	}

	const double Now = FPlatformTime::Seconds();
	UpdateRateSamples(Now);

	FScopeLock Lock(&StateLock);
	for (TPair<FString, FActorState>& Pair : ActorStates)
	{
		FActorState& State = Pair.Value;
		if (State.bHasPendingFrame && Now - State.LastReceiveTime >= PendingFrameFlushSeconds)
		{
			PushFrameForActor(Pair.Key, State, Now);
		}
	}
}

void FMOVINStageLiveLinkSource::UpdateRateSamples(double Now)
{
	// Motion and point clouds run at different rates (60 and 20 fps), so a single packet counter
	// cannot tell which stream stalled. Sampling on the game thread also means the reported rate
	// decays to zero when a stream stops, which a producer side counter would not do.
	const uint64 CurrentMotionFrames = MotionFramesPushed.load(std::memory_order_relaxed);
	const uint64 CurrentPointCloudFrames = PointCloudAssembler.SnapshotStats().FramesPublished;

	if (RateSampleTime <= 0.0)
	{
		RateSampleTime = Now;
		RateSampleMotionFrames = CurrentMotionFrames;
		RateSamplePointCloudFrames = CurrentPointCloudFrames;
		return;
	}

	const double Elapsed = Now - RateSampleTime;
	if (Elapsed < RateSampleIntervalSeconds)
	{
		return;
	}

	MotionFps.store(static_cast<float>((CurrentMotionFrames - RateSampleMotionFrames) / Elapsed), std::memory_order_relaxed);
	PointCloudFps.store(static_cast<float>((CurrentPointCloudFrames - RateSamplePointCloudFrames) / Elapsed), std::memory_order_relaxed);

	RateSampleTime = Now;
	RateSampleMotionFrames = CurrentMotionFrames;
	RateSamplePointCloudFrames = CurrentPointCloudFrames;
}

bool FMOVINStageLiveLinkSource::IsSourceStillValid() const
{
	return !bStopping.load() && Socket != nullptr && bReceiverStarted.load();
}

bool FMOVINStageLiveLinkSource::RequestSourceShutdown()
{
	bStopping.store(true);
	StopReceiver();
	return true;
}

FText FMOVINStageLiveLinkSource::GetSourceType() const
{
	return NSLOCTEXT("MOVINStageLiveLink", "SourceType", "MOVIN Stage");
}

FText FMOVINStageLiveLinkSource::GetSourceMachineName() const
{
	// Product plus endpoint, so this one column identifies both which plugin owns the row and which
	// port it holds - the two things you need when two MOVIN sources are listed together.
	return FText::Format(
		NSLOCTEXT("MOVINStageLiveLink", "MachineName", "MOVIN Stage - {0}"),
		FText::FromString(Settings.Endpoint.ToString()));
}

FText FMOVINStageLiveLinkSource::GetSourceStatus() const
{
	if (bStopping.load())
	{
		return NSLOCTEXT("MOVINStageLiveLink", "StatusStopping", "Stopping");
	}
	if (bPortConflict.load())
	{
		// The only visible clue for the silent case: two LiveLink sources aimed at one port.
		return FText::Format(
			NSLOCTEXT("MOVINStageLiveLink", "StatusPortInUse", "Port {0} is in use - another LiveLink source may already own it"),
			FText::AsNumber(static_cast<int32>(Settings.Endpoint.Port), &FNumberFormattingOptions::DefaultNoGrouping()));
	}
	if (Socket == nullptr || !bReceiverStarted.load())
	{
		return NSLOCTEXT("MOVINStageLiveLink", "StatusSocketError", "Socket error");
	}

	const double LastPacket = Receiver.IsValid() ? Receiver->GetLastPacketTime() : 0.0;
	if (LastPacket <= 0.0)
	{
		return NSLOCTEXT("MOVINStageLiveLink", "StatusListening", "Listening");
	}
	if (FPlatformTime::Seconds() - LastPacket > Settings.TimeoutSeconds)
	{
		return NSLOCTEXT("MOVINStageLiveLink", "StatusNoData", "No motion");
	}
	return NSLOCTEXT("MOVINStageLiveLink", "StatusActive", "Active");
}

FText FMOVINStageLiveLinkSource::GetSourceToolTip() const
{
	const FMOVINOscReceiveThreadStats ReceiveStats = Receiver.IsValid()
		? Receiver->SnapshotStats()
		: FMOVINOscReceiveThreadStats();

	int32 KnownActorCount = 0;
	{
		FScopeLock Lock(&StateLock);
		KnownActorCount = ActorStates.Num();
	}

	FString Text = FString::Printf(
		TEXT("%s\n")
		TEXT("Datagrams: %llu (%.1f MB total)\n")
		TEXT("Motion : %llu pkt | %llu frames | %.1f fps (expect 60) | %d actors"),
		*Settings.ToConnectionString(),
		ReceiveStats.Datagrams,
		static_cast<double>(ReceiveStats.Bytes) / (1024.0 * 1024.0),
		MotionPacketsAccepted.load(),
		MotionFramesPushed.load(),
		MotionFps.load(),
		KnownActorCount);

	if (Settings.bPointCloudEnabled)
	{
		const FMOVINOscPointCloudStats PointCloudStats = PointCloudAssembler.SnapshotStats();
		const TSharedPtr<FMOVINStagePointCloudStream, ESPMode::ThreadSafe> Stream = PointCloudAssembler.GetStream();

		const uint64 ChunksAndLost = PointCloudStats.ChunksReceived + PointCloudStats.ChunksLost;
		const double LossPercent = ChunksAndLost > 0
			? 100.0 * static_cast<double>(PointCloudStats.ChunksLost) / static_cast<double>(ChunksAndLost)
			: 0.0;

		Text += FString::Printf(
			TEXT("\nPCD    : %llu chunks | %.1f fps (expect 20) | %llu complete, %llu partial")
			TEXT("\nChunks : %llu lost (%.2f%%) | %llu dup | %llu late | %llu resync | %llu badOffset | %llu malformed")
			TEXT("\nPoints : %d / %d source (cap %d)"),
			PointCloudStats.ChunksReceived,
			PointCloudFps.load(),
			PointCloudStats.CompleteFrames,
			PointCloudStats.PartialFrames,
			PointCloudStats.ChunksLost,
			LossPercent,
			PointCloudStats.DuplicateChunks,
			PointCloudStats.LateChunks,
			PointCloudStats.Resyncs,
			PointCloudStats.BadOffsetChunks,
			PointCloudStats.MalformedChunks,
			PointCloudStats.LastPointCount,
			PointCloudStats.LastSourceTotalPoints,
			Settings.PointCloudMaxPoints);

		if (Stream.IsValid())
		{
			Text += FString::Printf(
				TEXT("\nFrames : %llu published, %llu consumed, %llu dropped, %llu starved"),
				Stream->GetFramesPublished(),
				Stream->GetFramesConsumed(),
				Stream->GetFramesDroppedBeforeDisplay(),
				Stream->GetWriteSlotStarvations());
		}
	}
	else
	{
		Text += FString::Printf(TEXT("\nPCD    : disabled (%llu chunks ignored)"), PointCloudPacketsIgnored.load());
	}

	Text += FString::Printf(
		TEXT("\nSocket : SO_RCVBUF %d granted | DrainOverruns %llu | RecvErrors %llu | Unknown %llu"),
		GrantedReceiveBufferBytes,
		ReceiveStats.DrainOverruns,
		ReceiveStats.RecvErrors,
		UnknownAddressPackets.load());

	return FText::FromString(Text);
}

bool FMOVINStageLiveLinkSource::TryRegisterPort(int32 Port)
{
	FScopeLock Lock(&ActivePortsCriticalSection);
	if (ActivePorts.Contains(Port))
	{
		return false;
	}

	ActivePorts.Add(Port);
	return true;
}

void FMOVINStageLiveLinkSource::UnregisterPort(int32 Port)
{
	FScopeLock Lock(&ActivePortsCriticalSection);
	ActivePorts.Remove(Port);
}

void FMOVINStageLiveLinkSource::StartReceiver()
{
	if (bReceiverStarted.load())
	{
		return;
	}

	const int32 Port = Settings.Endpoint.Port;
	if (!TryRegisterPort(Port))
	{
		bPortConflict.store(true);
		UE_LOG(LogMOVINStageLiveLink, Error,
			TEXT("UDP port %d is already claimed by another MOVIN Stage source. Give each source its own port."),
			Port);
		return;
	}
	RegisteredPort = Port;
	bPortConflict.store(false);

	// Deliberately not AsReusable(). SO_REUSEADDR lets a second UDP socket bind the same
	// address and port on Windows, after which datagrams reach only one of them with no error
	// anywhere - the failure mode when this plugin and the MOVIN LiveLink (Tracin) plugin are
	// pointed at the same port. Without it the OS refuses the duplicate bind and the panel says so.
	// UDP has no TIME_WAIT, so nothing here needs the flag to survive a restart.
	Socket = FUdpSocketBuilder(TEXT("MOVIN Stage LiveLink UDP Receiver"))
		.AsNonBlocking()
		.BoundToEndpoint(Settings.Endpoint)
		.WithReceiveBufferSize(Settings.ReceiveBufferBytes);

	if (Socket == nullptr)
	{
		// Almost always the port: another process, or another LiveLink plugin, already owns it.
		bPortConflict.store(true);
		UnregisterPort(RegisteredPort);
		RegisteredPort = 0;
		UE_LOG(LogMOVINStageLiveLink, Error,
			TEXT("Failed to bind UDP endpoint %s. Another application or LiveLink source may already own port %d."),
			*Settings.Endpoint.ToString(), Port);
		return;
	}

	// FUdpSocketBuilder discards the granted size, and the kernel routinely grants less than asked.
	// A point cloud frame arrives as one multi megabyte burst, so knowing the real figure matters.
	if (!Socket->SetReceiveBufferSize(Settings.ReceiveBufferBytes, GrantedReceiveBufferBytes))
	{
		GrantedReceiveBufferBytes = 0;
	}
	if (GrantedReceiveBufferBytes > 0 && GrantedReceiveBufferBytes < Settings.ReceiveBufferBytes)
	{
		UE_LOG(LogMOVINStageLiveLink, Warning,
			TEXT("Requested SO_RCVBUF %d bytes but was granted %d. Large point cloud frames may be dropped by the kernel."),
			Settings.ReceiveBufferBytes, GrantedReceiveBufferBytes);
	}

	FMOVINOscReceiveThreadTuning Tuning;
	Tuning.MaxDatagramsPerWait = Settings.MaxDatagramsPerWait;

	Receiver = MakeUnique<FMOVINOscUdpReceiveThread>(Socket, Tuning, TEXT("MOVINStageLiveLinkReceiver"));
	Receiver->OnDatagram().BindRaw(this, &FMOVINStageLiveLinkSource::HandleDatagram);
	Receiver->Start();
	bReceiverStarted.store(true);
}

void FMOVINStageLiveLinkSource::StopReceiver()
{
	bReceiverStarted.store(false);
	bPortConflict.store(false);

	FMOVINStagePointCloudRegistry::Unregister(SourceGuid);

	// Destroying the receiver joins its thread, so the assembler is quiescent afterwards.
	Receiver.Reset();

	if (Socket != nullptr)
	{
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
	}

	// After the socket is gone, so a source that stops and restarts on the same port can reclaim it.
	if (RegisteredPort != 0)
	{
		UnregisterPort(RegisteredPort);
		RegisteredPort = 0;
	}

	PointCloudAssembler.Shutdown();
}

void FMOVINStageLiveLinkSource::HandleDatagram(const uint8* Data, int32 NumBytes)
{
	if (bStopping.load() || Data == nullptr || NumBytes <= 0)
	{
		return;
	}

	int32 AddressBlockLen = 0;
	const EMOVINOscAddress Address = FMOVINOscParser::ClassifyAddress(Data, NumBytes, PointCloudMatcher, AddressBlockLen);

	switch (Address)
	{
	case EMOVINOscAddress::MovinPointCloud:
	{
		if (!Settings.bPointCloudEnabled)
		{
			++PointCloudPacketsIgnored;
			return;
		}

		FMOVINOscPointCloudChunkView ChunkView;
		if (FMOVINOscParser::ParsePointCloudChunk(Data, NumBytes, AddressBlockLen, ChunkView))
		{
			PointCloudAssembler.HandleChunk(ChunkView);
		}
		else
		{
			PointCloudAssembler.NoteMalformedChunk();
		}
		return;
	}

	case EMOVINOscAddress::VmcRootPos:
	case EMOVINOscAddress::VmcBonePos:
	{
		// Unchanged slow path. Motion is a small fraction of the packet rate, so the FString work
		// ParsePacket does is irrelevant here - and leaving it alone keeps the existing tests valid.
		FMOVINOscMotionPacket Packet;
		if (FMOVINOscParser::ParsePacket(Data, NumBytes, Packet) == EMOVINOscPacketType::Motion)
		{
			HandleMotionPacket(Packet);
		}
		return;
	}

	default:
		++UnknownAddressPackets;
		return;
	}
}

void FMOVINStageLiveLinkSource::HandleMotionPacket(const FMOVINOscMotionPacket& Packet)
{
	if (Client == nullptr || Packet.ActorName.IsEmpty())
	{
		return;
	}

	const FName BoneName = NormalizeIncomingBoneName(Packet.BoneName, Settings.FingerNameMode);
	const int32* BoneIndexPtr = GetBoneNameToIndex().Find(BoneName);
	if (BoneIndexPtr == nullptr)
	{
		return;
	}

	const double Now = FPlatformTime::Seconds();

	FScopeLock Lock(&StateLock);
	FActorState* StatePtr = ActorStates.Find(Packet.ActorName);
	if (StatePtr == nullptr)
	{
		// First packet from this actor: it becomes a subject on the spot. There is no configured
		// actor list, so whatever the stage sends - one performer or twenty - simply appears.
		if (ActorStates.Num() >= MaxDynamicActors)
		{
			if (!bWarnedActorCap)
			{
				bWarnedActorCap = true;
				UE_LOG(LogMOVINStageLiveLink, Warning,
					TEXT("Ignoring actor '%s': this source already tracks %d actors, the hard cap. ")
					TEXT("Either the sender is minting actor names or MaxDynamicActors needs raising."),
					*Packet.ActorName, MaxDynamicActors);
			}
			return;
		}

		StatePtr = &ActorStates.Add(Packet.ActorName);
		StatePtr->Transforms.Init(FTransform::Identity, GetBoneNames().Num());
		StatePtr->SourceTransforms.Init(FTransform::Identity, GetBoneNames().Num());

		// Static data must precede the first frame, and both come from this thread in order.
		PushStaticDataForActor(Packet.ActorName);

		UE_LOG(LogMOVINStageLiveLink, Log,
			TEXT("New actor '%s' published as Live Link subject '%s'."),
			*Packet.ActorName, *MakeSubjectName(Packet.ActorName).ToString());
	}

	FActorState& State = *StatePtr;
	++MotionPacketsAccepted;

	if (Packet.bHasFrameIndex)
	{
		if (State.bHasPendingFrame && State.bHasFrameIndex && State.FrameIndex != Packet.FrameIndex)
		{
			PushFrameForActor(Packet.ActorName, State, Now);
		}
		State.bHasFrameIndex = true;
		State.FrameIndex = Packet.FrameIndex;
	}
	else if (Packet.bIsRoot && State.bHasPendingFrame)
	{
		PushFrameForActor(Packet.ActorName, State, Now);
		State.bHasFrameIndex = false;
	}

	State.SourceTransforms[*BoneIndexPtr] = MakeSourceTransform(Packet.Position, Packet.Rotation);
	State.bHasPendingFrame = true;
	State.LastReceiveTime = Now;
}

FName FMOVINStageLiveLinkSource::MakeSubjectName(const FString& ActorName) const
{
	return Settings.SubjectPrefix.IsEmpty()
		? FName(*ActorName)
		: FName(*(Settings.SubjectPrefix + ActorName));
}

void FMOVINStageLiveLinkSource::RemoveSubjectsForNames(const TArray<FString>& ActorNames, const FString& SubjectPrefix)
{
	if (Client == nullptr)
	{
		return;
	}

	for (const FString& ActorName : ActorNames)
	{
		const FName PreviousSubject = SubjectPrefix.IsEmpty()
			? FName(*ActorName)
			: FName(*(SubjectPrefix + ActorName));

		// A name the current prefix still produces is about to be re-pushed; tearing it down first
		// would blank the character for a frame.
		if (MakeSubjectName(ActorName) == PreviousSubject)
		{
			continue;
		}

		Client->RemoveSubject_AnyThread(FLiveLinkSubjectKey(SourceGuid, PreviousSubject));
	}
}

TArray<FString> FMOVINStageLiveLinkSource::GetKnownActorNames() const
{
	FScopeLock Lock(&StateLock);
	TArray<FString> Names;
	ActorStates.GenerateKeyArray(Names);
	return Names;
}

void FMOVINStageLiveLinkSource::PushStaticDataForActor(const FString& ActorName)
{
	if (Client == nullptr)
	{
		return;
	}

	FLiveLinkSkeletonStaticData StaticData;
	StaticData.SetBoneNames(GetBoneNames());
	StaticData.SetBoneParents(GetBoneParents());

	FLiveLinkStaticDataStruct StaticDataStruct(FLiveLinkSkeletonStaticData::StaticStruct());
	StaticDataStruct.InitializeWith(FLiveLinkSkeletonStaticData::StaticStruct(), &StaticData);

	Client->PushSubjectStaticData_AnyThread(
		FLiveLinkSubjectKey(SourceGuid, MakeSubjectName(ActorName)),
		ULiveLinkAnimationRole::StaticClass(),
		MoveTemp(StaticDataStruct));
}

void FMOVINStageLiveLinkSource::PushStaticDataForSubjects()
{
	for (const FString& ActorName : GetKnownActorNames())
	{
		PushStaticDataForActor(ActorName);
	}
}

void FMOVINStageLiveLinkSource::PushFrameForActor(const FString& ActorName, FActorState& State, double PushTime)
{
	if (Client == nullptr || State.SourceTransforms.Num() != GetBoneNames().Num())
	{
		return;
	}

	FLiveLinkAnimationFrameData FrameData;
	State.Transforms = BuildLiveLinkTransforms(State.SourceTransforms, Settings);
	FrameData.Transforms = State.Transforms;
	FrameData.WorldTime = FLiveLinkWorldTime(PushTime, 0.0);
	if (State.bHasFrameIndex)
	{
		FrameData.MetaData.SceneTime.Time = FFrameTime(State.FrameIndex);
	}

	FLiveLinkFrameDataStruct FrameDataStruct(FLiveLinkAnimationFrameData::StaticStruct());
	FrameDataStruct.InitializeWith(FLiveLinkAnimationFrameData::StaticStruct(), &FrameData);

	Client->PushSubjectFrameData_AnyThread(
		FLiveLinkSubjectKey(SourceGuid, MakeSubjectName(ActorName)),
		MoveTemp(FrameDataStruct));

	State.bHasPendingFrame = false;
	State.LastPushTime = PushTime;
	++State.FramesPushed;
	++MotionFramesPushed;
}

const TArray<FName>& FMOVINStageLiveLinkSource::GetBoneNames()
{
	static const TArray<FName> BoneNames = {
		TEXT("Root"), TEXT("Hips"), TEXT("Spine"), TEXT("Chest"), TEXT("Neck"), TEXT("Head"),
		TEXT("LeftShoulder"), TEXT("LeftUpperArm"), TEXT("LeftLowerArm"), TEXT("LeftHand"),
		TEXT("RightShoulder"), TEXT("RightUpperArm"), TEXT("RightLowerArm"), TEXT("RightHand"),
		TEXT("LeftUpperLeg"), TEXT("LeftLowerLeg"), TEXT("LeftFoot"), TEXT("LeftToes"),
		TEXT("RightUpperLeg"), TEXT("RightLowerLeg"), TEXT("RightFoot"), TEXT("RightToes")
	};
	return BoneNames;
}

const TArray<int32>& FMOVINStageLiveLinkSource::GetBoneParents()
{
	static const TArray<int32> BoneParents = {
		-1, 0, 1, 2, 3, 4,
		3, 6, 7, 8,
		3, 10, 11, 12,
		1, 14, 15, 16,
		1, 18, 19, 20
	};
	return BoneParents;
}

const TMap<FName, int32>& FMOVINStageLiveLinkSource::GetBoneNameToIndex()
{
	static const TMap<FName, int32> BoneNameToIndex = []()
	{
		TMap<FName, int32> Map;
		const TArray<FName>& Names = GetBoneNames();
		for (int32 Index = 0; Index < Names.Num(); ++Index)
		{
			Map.Add(Names[Index], Index);
		}
		return Map;
	}();
	return BoneNameToIndex;
}

FName FMOVINStageLiveLinkSource::NormalizeIncomingBoneName(const FString& BoneName, FMOVINStageLiveLinkConnectionSettings::EFingerNameMode FingerNameMode)
{
	const FName IncomingName(*BoneName);
	if (FingerNameMode == FMOVINStageLiveLinkConnectionSettings::EFingerNameMode::MovinManNotch)
	{
		if (const FName* RemappedName = GetMovinManNotchFingerNameMap().Find(IncomingName))
		{
			return *RemappedName;
		}
	}

	return IncomingName;
}

FTransform FMOVINStageLiveLinkSource::MakeSourceTransform(const FVector3f& SourcePosition, const FQuat4f& SourceRotation)
{
	FQuat Rotation(
		static_cast<double>(SourceRotation.X),
		static_cast<double>(SourceRotation.Y),
		static_cast<double>(SourceRotation.Z),
		static_cast<double>(SourceRotation.W));
	Rotation.Normalize();

	return FTransform(
		Rotation,
		FVector(
			static_cast<double>(SourcePosition.X),
			static_cast<double>(SourcePosition.Y),
			static_cast<double>(SourcePosition.Z)),
		FVector::OneVector);
}

TArray<FTransform> FMOVINStageLiveLinkSource::BuildLiveLinkTransforms(const TArray<FTransform>& SourceTransforms, const FMOVINStageLiveLinkConnectionSettings& Settings)
{
	TArray<FTransform> WorkingTransforms = SourceTransforms;
	if (Settings.CoordinateSystem == FMOVINStageLiveLinkConnectionSettings::ECoordinateSystem::Unity &&
		Settings.bApplyStreamRotationOffset)
	{
		ApplyStreamRotationOffset(WorkingTransforms, Settings.RotationOffsetDegrees);
	}

	TArray<FTransform> Result;
	Result.Reserve(WorkingTransforms.Num());
	for (const FTransform& SourceTransform : WorkingTransforms)
	{
		Result.Add(ConvertSourceTransform(SourceTransform, Settings.PositionScale, Settings.CoordinateSystem));
	}

	return Result;
}

void FMOVINStageLiveLinkSource::ApplyStreamRotationOffset(TArray<FTransform>& InOutSourceTransforms, const FVector& OffsetDegrees)
{
	const TArray<int32>& Parents = GetBoneParents();
	if (InOutSourceTransforms.Num() != Parents.Num())
	{
		return;
	}

	const FQuat RotationOffset = MakeUnityEulerQuaternion(OffsetDegrees);

	TArray<FVector> GlobalPositions;
	TArray<FQuat> GlobalRotations;
	TArray<FVector> OffsetGlobalPositions;
	TArray<FQuat> OffsetGlobalRotations;
	GlobalPositions.SetNum(InOutSourceTransforms.Num());
	GlobalRotations.SetNum(InOutSourceTransforms.Num());
	OffsetGlobalPositions.SetNum(InOutSourceTransforms.Num());
	OffsetGlobalRotations.SetNum(InOutSourceTransforms.Num());

	for (int32 Index = 0; Index < InOutSourceTransforms.Num(); ++Index)
	{
		const int32 ParentIndex = Parents[Index];
		const FVector LocalPosition = InOutSourceTransforms[Index].GetTranslation();
		FQuat LocalRotation = InOutSourceTransforms[Index].GetRotation();
		LocalRotation.Normalize();

		if (ParentIndex < 0)
		{
			GlobalPositions[Index] = LocalPosition;
			GlobalRotations[Index] = LocalRotation;
		}
		else
		{
			GlobalPositions[Index] = GlobalPositions[ParentIndex] + GlobalRotations[ParentIndex].RotateVector(LocalPosition);
			GlobalRotations[Index] = (GlobalRotations[ParentIndex] * LocalRotation).GetNormalized();
		}

		// Mirrors CharacterStreaming.ApplyRotationalOffset:
		// globalFrame = globalFrame * Matrix4x4.Rotate(Quaternion.Euler(offset)).
		OffsetGlobalPositions[Index] = GlobalPositions[Index];
		OffsetGlobalRotations[Index] = (GlobalRotations[Index] * RotationOffset).GetNormalized();
	}

	for (int32 Index = 0; Index < InOutSourceTransforms.Num(); ++Index)
	{
		const int32 ParentIndex = Parents[Index];
		FVector LocalPosition = OffsetGlobalPositions[Index];
		FQuat LocalRotation = OffsetGlobalRotations[Index];

		if (ParentIndex >= 0)
		{
			const FQuat InverseParentRotation = OffsetGlobalRotations[ParentIndex].Inverse();
			LocalPosition = InverseParentRotation.RotateVector(OffsetGlobalPositions[Index] - OffsetGlobalPositions[ParentIndex]);
			LocalRotation = (InverseParentRotation * OffsetGlobalRotations[Index]).GetNormalized();
		}

		InOutSourceTransforms[Index].SetTranslation(LocalPosition);
		InOutSourceTransforms[Index].SetRotation(LocalRotation);
	}
}

FQuat FMOVINStageLiveLinkSource::MakeUnityEulerQuaternion(const FVector& EulerDegrees)
{
	const FQuat X(FVector::ForwardVector, FMath::DegreesToRadians(EulerDegrees.X));
	const FQuat Y(FVector::RightVector, FMath::DegreesToRadians(EulerDegrees.Y));
	const FQuat Z(FVector::UpVector, FMath::DegreesToRadians(EulerDegrees.Z));

	// Unity's Quaternion.Euler uses Z-X-Y application order.
	FQuat Result = Y * X * Z;
	Result.Normalize();
	return Result;
}

FTransform FMOVINStageLiveLinkSource::ConvertOscTransform(const FVector3f& SourcePosition, const FQuat4f& SourceRotation, float Scale, FMOVINStageLiveLinkConnectionSettings::ECoordinateSystem CoordinateSystem)
{
	if (CoordinateSystem == FMOVINStageLiveLinkConnectionSettings::ECoordinateSystem::Unity)
	{
		// Unity frame (sender convention): +X right, +Y up, +Z forward.
		// Unreal: +X forward, +Y right, +Z up.
		const FVector Position(
			static_cast<double>(SourcePosition.Z) * Scale,
			static_cast<double>(SourcePosition.X) * Scale,
			static_cast<double>(SourcePosition.Y) * Scale);
		return FTransform(ConvertUnityRotationToUnreal(SourceRotation), Position, FVector::OneVector);
	}

	// OpenGL-style stream used by the MOVIN Python viewer:
	// +X left, +Y up, +Z forward. Unreal remains +X forward, +Y right, +Z up.
	const FVector Position(
		static_cast<double>(SourcePosition.Z) * Scale,
		static_cast<double>(-SourcePosition.X) * Scale,
		static_cast<double>(SourcePosition.Y) * Scale);
	return FTransform(ConvertOpenGlRotationToUnreal(SourceRotation), Position, FVector::OneVector);
}

FTransform FMOVINStageLiveLinkSource::ConvertSourceTransform(const FTransform& SourceTransform, float Scale, FMOVINStageLiveLinkConnectionSettings::ECoordinateSystem CoordinateSystem)
{
	const FVector SourcePosition = SourceTransform.GetTranslation();
	const FQuat SourceRotation = SourceTransform.GetRotation();

	if (CoordinateSystem == FMOVINStageLiveLinkConnectionSettings::ECoordinateSystem::Unity)
	{
		const FVector Position(
			SourcePosition.Z * Scale,
			SourcePosition.X * Scale,
			SourcePosition.Y * Scale);
		return FTransform(ConvertUnityRotationToUnreal(SourceRotation), Position, FVector::OneVector);
	}

	const FVector Position(
		SourcePosition.Z * Scale,
		-SourcePosition.X * Scale,
		SourcePosition.Y * Scale);
	return FTransform(ConvertOpenGlRotationToUnreal(SourceRotation), Position, FVector::OneVector);
}

FQuat FMOVINStageLiveLinkSource::ConvertUnityRotationToUnreal(const FQuat4f& UnityRotation)
{
	FQuat4f Q = UnityRotation;
	if (!Q.IsNormalized())
	{
		Q.Normalize();
	}

	FQuat Result(static_cast<double>(Q.Z), static_cast<double>(Q.X), static_cast<double>(Q.Y), static_cast<double>(Q.W));
	Result.Normalize();
	return Result;
}

FQuat FMOVINStageLiveLinkSource::ConvertUnityRotationToUnreal(const FQuat& UnityRotation)
{
	FQuat Q = UnityRotation;
	Q.Normalize();

	FQuat Result(Q.Z, Q.X, Q.Y, Q.W);
	Result.Normalize();
	return Result;
}

FQuat FMOVINStageLiveLinkSource::ConvertOpenGlRotationToUnreal(const FQuat4f& OpenGlRotation)
{
	FQuat4f Q = OpenGlRotation;
	if (!Q.IsNormalized())
	{
		Q.Normalize();
	}

	FQuat Result(static_cast<double>(-Q.Z), static_cast<double>(Q.X), static_cast<double>(-Q.Y), static_cast<double>(Q.W));
	Result.Normalize();
	return Result;
}

FQuat FMOVINStageLiveLinkSource::ConvertOpenGlRotationToUnreal(const FQuat& OpenGlRotation)
{
	FQuat Q = OpenGlRotation;
	Q.Normalize();

	FQuat Result(-Q.Z, Q.X, -Q.Y, Q.W);
	Result.Normalize();
	return Result;
}
