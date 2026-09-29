#include "MOVINStagePointCloudLidarComponent.h"

#include "LidarPointCloud.h"
#include "MOVINStagePointCloudRegistry.h"
#include "MOVINStagePointCloudStream.h"

DEFINE_LOG_CATEGORY_STATIC(LogMOVINStagePointCloudLidar, Log, All);

UMOVINStagePointCloudLidarComponent::UMOVINStagePointCloudLidarComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	// LiveLink previewing happens in the editor viewport without PIE, and ActorComponent.cpp:1655
	// gates editor ticking on this flag.
	bTickInEditor = true;
}

void UMOVINStagePointCloudLidarComponent::OnRegister()
{
	Super::OnRegister();

	if (DynamicCloud == nullptr)
	{
		DynamicCloud = NewObject<ULidarPointCloud>(this, NAME_None, RF_Transient);

		// Without this the octree subdivides on every insert, which no per-frame stream can afford.
		DynamicCloud->SetOptimizedForDynamicData(true);
		DynamicCloud->Initialize(FBox(
			FVector(-MinimumHalfExtentCm),
			FVector(MinimumHalfExtentCm)));

		SetPointCloud(DynamicCloud);
	}

	ScratchPoints.Reserve(FMath::Max(MaxPoints, 1));
}

void UMOVINStagePointCloudLidarComponent::OnUnregister()
{
	Stream.Reset();
	Super::OnUnregister();
}

bool UMOVINStagePointCloudLidarComponent::IsStreamConnected() const
{
	return Stream.IsValid();
}

bool UMOVINStagePointCloudLidarComponent::ResolveStream()
{
	if (Stream.IsValid())
	{
		return true;
	}

	Stream = SourceGuidFilter.IsValid()
		? FMOVINStagePointCloudRegistry::GetStream(SourceGuidFilter)
		: FMOVINStagePointCloudRegistry::GetMostRecentStream();

	return Stream.IsValid();
}

void UMOVINStagePointCloudLidarComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (DynamicCloud == nullptr || !ResolveStream())
	{
		return;
	}

	// Null when nothing new arrived. Rebuilding the cloud every tick for 20 fps data would burn
	// several milliseconds of game thread time for no visible change. Holding the previous frame is
	// also the only sensible behaviour: LiDAR points carry no identity between frames, so
	// interpolating positions would be meaningless.
	FMOVINStagePointCloudFrame* Frame = Stream->AcquireLatestReadyFrame();
	if (Frame == nullptr)
	{
		return;
	}

	PushFrame(*Frame);
	Stream->ReleaseFrame(Frame);
}

void UMOVINStagePointCloudLidarComponent::PushFrame(const FMOVINStagePointCloudFrame& Frame)
{
	const int32 PointCount = FMath::Clamp(Frame.PointCount, 0, FMath::Min(MaxPoints, Frame.Points.Num()));

	ScratchPoints.SetNumUninitialized(PointCount, EAllowShrinking::No);
	for (int32 Index = 0; Index < PointCount; ++Index)
	{
		const FVector4f& Source = Frame.Points[Index];
		ScratchPoints[Index] = FLidarPointCloudPoint(FVector3f(Source.X, Source.Y, Source.Z));
	}

	// Keep the volume centred on the component origin. ULidarPointCloud::Initialize sets
	// LocationOffset to the box centre, so an off-centre box would silently shift every point.
	const float Padding = FMath::Max(BoundsPaddingCm, 0.0f);
	float HalfExtent = FMath::Max(MinimumHalfExtentCm, 1.0f);
	if (PointCount > 0)
	{
		const FVector3f& Min = Frame.BoundsMin;
		const FVector3f& Max = Frame.BoundsMax;
		HalfExtent = FMath::Max(HalfExtent, Min.GetAbsMax());
		HalfExtent = FMath::Max(HalfExtent, Max.GetAbsMax());
		HalfExtent += Padding;
	}

	// Initialize erases the existing points, which is exactly the per-frame reset we want. With
	// dynamic data optimisation there is no subdivision to tear down, so it stays cheap.
	DynamicCloud->Initialize(FBox(FVector(-HalfExtent), FVector(HalfExtent)));

	if (PointCount > 0)
	{
		DynamicCloud->InsertPoints(
			ScratchPoints.GetData(),
			PointCount,
			ELidarPointCloudDuplicateHandling::Ignore,
			/*bRefreshPointsBounds*/ true,
			FVector::ZeroVector);
	}

	DynamicCloud->RefreshRendering();

	LastFramePointCount = PointCount;
	LastFrameSeq = Frame.Seq;
}
