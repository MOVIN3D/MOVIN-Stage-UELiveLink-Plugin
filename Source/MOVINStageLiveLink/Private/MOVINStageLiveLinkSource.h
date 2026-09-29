#pragma once

#include "ILiveLinkSource.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "MOVINOscParser.h"
#include "MOVINOscPointCloudAssembler.h"
#include "MOVINOscUdpReceiveThread.h"

#include <atomic>

class FSocket;
struct FMOVINOscMotionPacket;

struct FMOVINStageLiveLinkConnectionSettings
{
	enum class ECoordinateSystem
	{
		Unity,
		OpenGL
	};

	enum class EFingerNameMode
	{
		Osc,
		MovinManNotch
	};

	/** Inherit keeps the cloud and the skeleton in the same space by construction. */
	enum class EPointCloudCoordinateMode
	{
		Inherit,
		Unity,
		OpenGL
	};

	FIPv4Endpoint Endpoint = FIPv4Endpoint(FIPv4Address::Any, 11235);

	/**
	 * Prepended to every published subject name. Empty by default, which reproduces the historical
	 * names exactly. Subjects are created for every actor name the stream carries - there is no
	 * actor list to configure - so this is the only thing that shapes a subject name.
	 */
	FString SubjectPrefix;

	float PositionScale = 100.0f;
	double TimeoutSeconds = 1.0;
	ECoordinateSystem CoordinateSystem = ECoordinateSystem::Unity;
	EFingerNameMode FingerNameMode = EFingerNameMode::MovinManNotch;
	bool bApplyStreamRotationOffset = true;
	FVector RotationOffsetDegrees = FVector(0.0, -90.0, -90.0);

	//~ Point cloud. Disabled by default so an existing setup behaves exactly as before.
	bool bPointCloudEnabled = false;
	int32 PointCloudMaxPoints = 331776;
	EPointCloudCoordinateMode PointCloudCoordinates = EPointCloudCoordinateMode::Inherit;

	/** Zero inherits PositionScale. */
	float PointCloudScale = 0.0f;

	FVector PointCloudOffset = FVector::ZeroVector;
	FString PointCloudAddress = TEXT("/MOVIN/PointCloud");
	bool bPointCloudPartialPublish = true;
	int32 PointCloudSeqResyncThreshold = 100;

	/** SO_RCVBUF request. Must absorb a whole point cloud frame burst. */
	int32 ReceiveBufferBytes = 32 * 1024 * 1024;

	int32 MaxDatagramsPerWait = 8192;

	static FMOVINStageLiveLinkConnectionSettings FromConnectionString(const FString& ConnectionString);
	FString ToConnectionString() const;

	/** Resolves PointCloudCoordinates against CoordinateSystem. */
	ECoordinateSystem GetEffectivePointCloudCoordinateSystem() const;

	/** Resolves PointCloudScale against PositionScale. */
	float GetEffectivePointCloudScale() const;
};

class FMOVINStageLiveLinkSource : public ILiveLinkSource, public TSharedFromThis<FMOVINStageLiveLinkSource>
{
public:
	explicit FMOVINStageLiveLinkSource(const FMOVINStageLiveLinkConnectionSettings& InSettings);
	virtual ~FMOVINStageLiveLinkSource() override;

	virtual void ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid) override;
	virtual void Update() override;
	virtual bool IsSourceStillValid() const override;
	virtual bool RequestSourceShutdown() override;
	virtual FText GetSourceType() const override;
	virtual FText GetSourceMachineName() const override;
	virtual FText GetSourceStatus() const override;
	virtual FText GetSourceToolTip() const override;

	/** The factory is a plain menu entry, so the Live Link details panel is where this is configured. */
	virtual TSubclassOf<ULiveLinkSourceSettings> GetSettingsClass() const override;
	virtual void InitializeSettings(ULiveLinkSourceSettings* Settings) override;
	virtual void OnSettingsChanged(ULiveLinkSourceSettings* Settings, const FPropertyChangedEvent& PropertyChangedEvent) override;

	/** Assembled point cloud frames, or null when point cloud reception is disabled. */
	TSharedPtr<FMOVINStagePointCloudStream, ESPMode::ThreadSafe> GetPointCloudStream() const;

private:
	struct FActorState
	{
		TArray<FTransform> Transforms;
		TArray<FTransform> SourceTransforms;
		bool bHasPendingFrame = false;
		bool bHasFrameIndex = false;
		int32 FrameIndex = 0;
		double LastReceiveTime = 0.0;
		double LastPushTime = 0.0;
		uint64 FramesPushed = 0;
	};

	void StartReceiver();
	void StopReceiver();

	/**
	 * Ports claimed by live sources in this process.
	 *
	 * The socket is bound without SO_REUSEADDR, so the OS already refuses a second bind - including
	 * one from another plugin, which is the case this cannot see. This registry exists to catch the
	 * duplicate before the bind is attempted, so the panel can say "port in use" rather than the
	 * generic socket error, and so two MOVIN Stage sources on one port fail predictably.
	 */
	static bool TryRegisterPort(int32 Port);
	static void UnregisterPort(int32 Port);

	/** Subject name published for an actor. Empty prefix yields the actor name unchanged. */
	FName MakeSubjectName(const FString& ActorName) const;

	/** Removes the subjects these actors were published under with the given prefix. */
	void RemoveSubjectsForNames(const TArray<FString>& ActorNames, const FString& SubjectPrefix);

	/** Actor names seen so far. Takes StateLock. */
	TArray<FString> GetKnownActorNames() const;

	/**
	 * Swaps in a new configuration. The receive thread reads Settings and PointCloudMatcher directly,
	 * so this stops the receiver before mutating them rather than trying to synchronize.
	 */
	void ApplyConnectionSettings(const FMOVINStageLiveLinkConnectionSettings& NewSettings);

	void InitializePointCloudAssembler();
	void HandleDatagram(const uint8* Data, int32 NumBytes);
	void HandleMotionPacket(const FMOVINOscMotionPacket& Packet);

	/** Publishes the skeleton for one actor. Called on first sight of an actor and after a prefix change. */
	void PushStaticDataForActor(const FString& ActorName);

	/** Re-publishes the skeleton for every actor seen so far. */
	void PushStaticDataForSubjects();
	void PushFrameForActor(const FString& ActorName, FActorState& State, double PushTime);
	void UpdateRateSamples(double Now);

	static const TArray<FName>& GetBoneNames();
	static const TArray<int32>& GetBoneParents();
	static const TMap<FName, int32>& GetBoneNameToIndex();
	static FName NormalizeIncomingBoneName(const FString& BoneName, FMOVINStageLiveLinkConnectionSettings::EFingerNameMode FingerNameMode);
	static FTransform MakeSourceTransform(const FVector3f& SourcePosition, const FQuat4f& SourceRotation);
	static TArray<FTransform> BuildLiveLinkTransforms(const TArray<FTransform>& SourceTransforms, const FMOVINStageLiveLinkConnectionSettings& Settings);
	static void ApplyStreamRotationOffset(TArray<FTransform>& InOutSourceTransforms, const FVector& OffsetDegrees);
	static FQuat MakeUnityEulerQuaternion(const FVector& EulerDegrees);
	static FTransform ConvertOscTransform(const FVector3f& SourcePosition, const FQuat4f& SourceRotation, float Scale, FMOVINStageLiveLinkConnectionSettings::ECoordinateSystem CoordinateSystem);
	static FTransform ConvertSourceTransform(const FTransform& SourceTransform, float Scale, FMOVINStageLiveLinkConnectionSettings::ECoordinateSystem CoordinateSystem);
	static FQuat ConvertUnityRotationToUnreal(const FQuat4f& UnityRotation);
	static FQuat ConvertUnityRotationToUnreal(const FQuat& UnityRotation);
	static FQuat ConvertOpenGlRotationToUnreal(const FQuat4f& OpenGlRotation);
	static FQuat ConvertOpenGlRotationToUnreal(const FQuat& OpenGlRotation);

private:
	FMOVINStageLiveLinkConnectionSettings Settings;
	ILiveLinkClient* Client = nullptr;
	FGuid SourceGuid;
	FSocket* Socket = nullptr;
	TUniquePtr<FMOVINOscUdpReceiveThread> Receiver;
	mutable FCriticalSection StateLock;

	/**
	 * One entry per actor name seen on the wire. Subjects are created on first sight rather than
	 * from a configured list, so a stage can put any number of performers through one source.
	 */
	TMap<FString, FActorState> ActorStates;

	/**
	 * Guard against a misbehaving sender minting a new actor name per packet, which would otherwise
	 * create subjects without bound. Far above any real stage; raise it if a production needs more.
	 */
	static constexpr int32 MaxDynamicActors = 64;

	/** Written under StateLock. */
	bool bWarnedActorCap = false;

	/** Precompiled point cloud address, matched by raw bytes on the receive thread. */
	FMOVINOscAddressMatcher PointCloudMatcher;

	/** Receive thread owned once the thread is running. */
	FMOVINOscPointCloudAssembler PointCloudAssembler;

	/** SO_RCVBUF the socket subsystem actually granted. */
	int32 GrantedReceiveBufferBytes = 0;

	std::atomic<uint64> MotionPacketsAccepted = 0;
	std::atomic<uint64> MotionFramesPushed = 0;
	std::atomic<uint64> PointCloudPacketsIgnored = 0;
	std::atomic<uint64> UnknownAddressPackets = 0;
	std::atomic<bool> bStopping = false;
	std::atomic<bool> bReceiverStarted = false;

	/** Set when the last start attempt lost the port. Separates "in use" from a generic socket error. */
	std::atomic<bool> bPortConflict = false;

	/** The port this source currently owns in ActivePorts, or 0 when it owns none. */
	int32 RegisteredPort = 0;

	//~ Rate sampling, written by Update() on the game thread.
	std::atomic<float> MotionFps = 0.0f;
	std::atomic<float> PointCloudFps = 0.0f;
	double RateSampleTime = 0.0;
	uint64 RateSampleMotionFrames = 0;
	uint64 RateSamplePointCloudFrames = 0;

	static FCriticalSection ActivePortsCriticalSection;
	static TSet<int32> ActivePorts;
};
