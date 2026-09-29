#pragma once

#include "CoreMinimal.h"

class FMOVINStagePointCloudStream;

/**
 * Side channel lookup so a renderer can find the point cloud stream owned by a LiveLink source
 * without the LiveLink module knowing anything about rendering.
 *
 * Registration happens when a source starts receiving and is cleared on shutdown. Consumers
 * normally want GetMostRecentStream(); the GUID keyed lookup exists for the multi source case.
 */
class MOVINSTAGELIVELINK_API FMOVINStagePointCloudRegistry
{
public:
	static void Register(const FGuid& SourceGuid, const TSharedPtr<FMOVINStagePointCloudStream, ESPMode::ThreadSafe>& Stream);
	static void Unregister(const FGuid& SourceGuid);

	static TSharedPtr<FMOVINStagePointCloudStream, ESPMode::ThreadSafe> GetStream(const FGuid& SourceGuid);

	/** The most recently registered stream, or null when no source is streaming point clouds. */
	static TSharedPtr<FMOVINStagePointCloudStream, ESPMode::ThreadSafe> GetMostRecentStream();

	static TArray<FGuid> GetRegisteredSources();
};
