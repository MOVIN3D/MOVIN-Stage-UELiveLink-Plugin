#include "MOVINStagePointCloudComponent.h"

#include "Engine/Texture2D.h"
#include "MOVINStagePointCloudRegistry.h"
#include "MOVINStagePointCloudStream.h"
#include "RHITypes.h"

DEFINE_LOG_CATEGORY_STATIC(LogMOVINStagePointCloud, Log, All);

UMOVINStagePointCloudComponent::UMOVINStagePointCloudComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = true;

	// The tick group is deliberately left alone: UNiagaraComponent resolves it from its own
	// TickBehavior at activation and would overwrite anything set here.

	// LiveLink previewing happens in the editor viewport without PIE, and ActorComponent.cpp:1655
	// gates editor ticking on this flag.
	PrimaryComponentTick.bStartWithTickEnabled = true;
	bTickInEditor = true;

	// Required, not an optimisation choice. UNiagaraComponent disables its own component tick unless
	// the system runs in solo mode - in batched mode FNiagaraWorldManager ticks the system directly
	// and TickComponent is never called (NiagaraComponent.cpp:917-923). Since the texture upload runs
	// from TickComponent, without this the upload stops as soon as the system instance is created:
	// a couple of frames get through and then nothing, with no error anywhere.
	//
	// The documented cost is losing batching with other instances of the same system, which is
	// irrelevant here because a level has one point cloud component.
	SetForceSolo(true);

	for (TSharedPtr<std::atomic<bool>>& InFlight : StagingBufferInFlight)
	{
		InFlight = MakeShared<std::atomic<bool>>(false);
	}
}

void UMOVINStagePointCloudComponent::OnRegister()
{
	Super::OnRegister();

	// Re-assert solo for instances serialised before this was required; SetForceSolo is a no-op when
	// the flag already matches.
	SetForceSolo(true);

	EnsureResources();
	PushStaticParameters();
}

void UMOVINStagePointCloudComponent::OnUnregister()
{
	ReleaseResources();
	Super::OnUnregister();
}

bool UMOVINStagePointCloudComponent::IsStreamConnected() const
{
	return Stream.IsValid();
}

void UMOVINStagePointCloudComponent::EnsureResources()
{
	const int32 DesiredMaxPoints = FMath::Max(MaxPoints, FMOVINStagePointCloudStream::TextureWidth);
	if (PositionTexture != nullptr && AllocatedMaxPoints == DesiredMaxPoints)
	{
		return;
	}

	ReleaseResources();

	TextureWidth = FMOVINStagePointCloudStream::TextureWidth;
	TextureHeight = FMath::DivideAndRoundUp(DesiredMaxPoints, TextureWidth);
	AllocatedMaxPoints = DesiredMaxPoints;

	const int32 TexelCount = TextureWidth * TextureHeight;

	PositionTexture = UTexture2D::CreateTransient(TextureWidth, TextureHeight, PF_A32B32G32R32F, TEXT("MOVINStagePointCloudPositions"));
	if (PositionTexture == nullptr)
	{
		UE_LOG(LogMOVINStagePointCloud, Error,
			TEXT("Failed to create the %dx%d PF_A32B32G32R32F position texture."), TextureWidth, TextureHeight);
		return;
	}

	// UpdateTextureRegions refuses to do anything for a streamable texture, and crucially it does not
	// invoke the cleanup callback in that case - which would leak the region and wedge the staging
	// buffer in flight forever. NeverStream keeps it off the streaming path.
	PositionTexture->NeverStream = true;
	PositionTexture->SRGB = false;
	PositionTexture->Filter = TF_Nearest;
	PositionTexture->AddressX = TA_Clamp;
	PositionTexture->AddressY = TA_Clamp;
	PositionTexture->CompressionSettings = TC_HDR;
#if WITH_EDITORONLY_DATA
	PositionTexture->MipGenSettings = TMGS_NoMipmaps;
#endif

	// CreateTransient only creates the resource when seeded with data, so do it explicitly.
	PositionTexture->UpdateResource();

	for (TArray<FVector4f>& Staging : StagingBuffers)
	{
		Staging.SetNumZeroed(TexelCount);
	}
	NextStagingBuffer = 0;
	LastUploadedPointCount = 0;
	LastUploadedSeq = -1;
	LastPushedPointSize = -1.0f;
}

void UMOVINStagePointCloudComponent::ReleaseResources()
{
	// A staging buffer handed to the RHI thread must outlive the upload. Rather than block, hand the
	// buffers over to their cleanup callbacks by detaching them: allocate fresh flags so any pending
	// callback writes to an object nobody reads any more, and let the arrays be freed on the next
	// EnsureResources. Uploads only ever run while the component is registered, so at worst this
	// waits for one already enqueued command that holds its own shared flag.
	for (TSharedPtr<std::atomic<bool>>& InFlight : StagingBufferInFlight)
	{
		if (InFlight.IsValid() && InFlight->load(std::memory_order_acquire))
		{
			FlushRenderingCommands();
			break;
		}
	}

	PositionTexture = nullptr;
	for (TArray<FVector4f>& Staging : StagingBuffers)
	{
		Staging.Empty();
	}
	for (TSharedPtr<std::atomic<bool>>& InFlight : StagingBufferInFlight)
	{
		InFlight = MakeShared<std::atomic<bool>>(false);
	}

	AllocatedMaxPoints = 0;
	TextureWidth = 0;
	TextureHeight = 0;
	Stream.Reset();
}

bool UMOVINStagePointCloudComponent::ResolveStream()
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

void UMOVINStagePointCloudComponent::PushStaticParameters()
{
	if (PositionTexture != nullptr)
	{
		SetVariableTexture(PositionTextureParameterName, PositionTexture);
		SetVariableInt(TextureWidthParameterName, TextureWidth);
		SetVariableInt(TextureHeightParameterName, TextureHeight);
	}

	if (!FMath::IsNearlyEqual(LastPushedPointSize, PointSize))
	{
		SetVariableFloat(PointSizeParameterName, PointSize);
		LastPushedPointSize = PointSize;
	}
}

void UMOVINStagePointCloudComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	EnsureResources();
	PushStaticParameters();

	if (!ResolveStream())
	{
		return;
	}

	// Null when nothing new arrived. The cloud updates at 20 fps while the game ticks far faster, so
	// re-uploading megabytes on every tick would waste PCIe bandwidth for no visible change. Holding
	// the previous frame is also the only sensible behaviour: LiDAR points carry no identity between
	// frames, so interpolating positions would be meaningless.
	FMOVINStagePointCloudFrame* Frame = Stream->AcquireLatestReadyFrame();
	if (Frame == nullptr)
	{
		return;
	}

	UploadFrame(*Frame);
	Stream->ReleaseFrame(Frame);

	// Frames are arriving but there is nothing to draw them with. Easy to miss otherwise, because
	// the receive side reports healthy statistics while the viewport stays empty.
	if (GetAsset() == nullptr && !bWarnedMissingAsset)
	{
		bWarnedMissingAsset = true;
		UE_LOG(LogMOVINStagePointCloud, Warning,
			TEXT("%s is receiving point cloud frames but has no Niagara system assigned, so nothing will render. ")
			TEXT("Assign a system that reads the '%s' texture user parameter. See the plugin README for the required setup."),
			*GetPathName(), *PositionTextureParameterName.ToString());
	}
}

void UMOVINStagePointCloudComponent::UploadFrame(const FMOVINStagePointCloudFrame& Frame)
{
	if (PositionTexture == nullptr || PositionTexture->GetResource() == nullptr)
	{
		return;
	}

	int32 BufferIndex = INDEX_NONE;
	for (int32 Attempt = 0; Attempt < UE_ARRAY_COUNT(StagingBuffers); ++Attempt)
	{
		const int32 Candidate = (NextStagingBuffer + Attempt) % UE_ARRAY_COUNT(StagingBuffers);
		if (!StagingBufferInFlight[Candidate]->load(std::memory_order_acquire))
		{
			BufferIndex = Candidate;
			break;
		}
	}

	if (BufferIndex == INDEX_NONE)
	{
		// Both uploads still in flight: skip this frame rather than overwrite data the GPU is reading.
		++SkippedUploadsNoStagingBuffer;
		return;
	}

	NextStagingBuffer = (BufferIndex + 1) % UE_ARRAY_COUNT(StagingBuffers);

	TArray<FVector4f>& Staging = StagingBuffers[BufferIndex];
	const int32 PointCount = FMath::Clamp(Frame.PointCount, 0, FMath::Min(Staging.Num(), Frame.Points.Num()));
	if (PointCount > 0)
	{
		FMemory::Memcpy(Staging.GetData(), Frame.Points.GetData(), static_cast<size_t>(PointCount) * sizeof(FVector4f));
	}

	// Full texture starting at (0,0). On D3D12 that takes the bLockFullTexture path, a write only
	// upload heap lock plus a CopyTextureRegion, with nothing waiting on the GPU.
	FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, TextureWidth, TextureHeight);

	// Shared so a callback that outlives the component writes to a live object rather than dangling.
	TSharedPtr<std::atomic<bool>> InFlight = StagingBufferInFlight[BufferIndex];
	InFlight->store(true, std::memory_order_release);

	PositionTexture->UpdateTextureRegions(
		0,
		1,
		Region,
		static_cast<uint32>(TextureWidth * sizeof(FVector4f)),
		static_cast<uint32>(sizeof(FVector4f)),
		reinterpret_cast<uint8*>(Staging.GetData()),
		[InFlight](uint8*, const FUpdateTextureRegion2D* Regions)
		{
			delete Regions;
			InFlight->store(false, std::memory_order_release);
		});

	LastUploadedPointCount = PointCount;
	LastUploadedSeq = Frame.Seq;

	SetVariableInt(ActiveCountParameterName, PointCount);

	// The frame's own extents, in the same space as the positions in the texture. Effects that need to
	// normalise a position - a height colour ramp, a sweeping scan plane - would otherwise have to
	// hardcode the stage dimensions and drift out of step with the data.
	//
	// Vector rather than Position: these are consumed as a numeric range, and mixing Niagara's
	// large-world Position type into that arithmetic just adds conversions.
	const FVector BoundsMin(Frame.BoundsMin.X, Frame.BoundsMin.Y, Frame.BoundsMin.Z);
	const FVector BoundsMax(Frame.BoundsMax.X, Frame.BoundsMax.Y, Frame.BoundsMax.Z);
	SetVariableVec3(BoundsMinParameterName, BoundsMin);
	SetVariableVec3(BoundsMaxParameterName, BoundsMax);

	// Bounds must be set explicitly: the emitter runs on the GPU so Niagara cannot derive them, and
	// without them the whole cloud is frustum culled as soon as the camera looks away from the origin.
	FBox LocalBounds(BoundsMin, BoundsMax);
	LocalBounds = LocalBounds.ExpandBy(FMath::Max(BoundsPaddingCm, 1.0f));
	SetSystemFixedBounds(LocalBounds);
}
