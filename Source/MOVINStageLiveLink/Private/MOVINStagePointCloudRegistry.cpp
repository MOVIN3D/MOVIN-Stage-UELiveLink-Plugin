#include "MOVINStagePointCloudRegistry.h"

#include "MOVINStagePointCloudStream.h"

namespace
{
	struct FRegistryEntry
	{
		FGuid SourceGuid;
		TSharedPtr<FMOVINStagePointCloudStream, ESPMode::ThreadSafe> Stream;
		uint64 Order = 0;
	};

	FCriticalSection GRegistryLock;
	TArray<FRegistryEntry> GEntries;
	uint64 GNextOrder = 1;
}

void FMOVINStagePointCloudRegistry::Register(
	const FGuid& SourceGuid,
	const TSharedPtr<FMOVINStagePointCloudStream, ESPMode::ThreadSafe>& Stream)
{
	if (!Stream.IsValid())
	{
		Unregister(SourceGuid);
		return;
	}

	FScopeLock Lock(&GRegistryLock);

	for (FRegistryEntry& Entry : GEntries)
	{
		if (Entry.SourceGuid == SourceGuid)
		{
			Entry.Stream = Stream;
			Entry.Order = GNextOrder++;
			return;
		}
	}

	GEntries.Add(FRegistryEntry{ SourceGuid, Stream, GNextOrder++ });
}

void FMOVINStagePointCloudRegistry::Unregister(const FGuid& SourceGuid)
{
	FScopeLock Lock(&GRegistryLock);
	GEntries.RemoveAll([&SourceGuid](const FRegistryEntry& Entry) { return Entry.SourceGuid == SourceGuid; });
}

TSharedPtr<FMOVINStagePointCloudStream, ESPMode::ThreadSafe> FMOVINStagePointCloudRegistry::GetStream(const FGuid& SourceGuid)
{
	FScopeLock Lock(&GRegistryLock);

	for (const FRegistryEntry& Entry : GEntries)
	{
		if (Entry.SourceGuid == SourceGuid)
		{
			return Entry.Stream;
		}
	}
	return nullptr;
}

TSharedPtr<FMOVINStagePointCloudStream, ESPMode::ThreadSafe> FMOVINStagePointCloudRegistry::GetMostRecentStream()
{
	FScopeLock Lock(&GRegistryLock);

	const FRegistryEntry* Newest = nullptr;
	for (const FRegistryEntry& Entry : GEntries)
	{
		if (Newest == nullptr || Entry.Order > Newest->Order)
		{
			Newest = &Entry;
		}
	}
	return Newest != nullptr ? Newest->Stream : nullptr;
}

TArray<FGuid> FMOVINStagePointCloudRegistry::GetRegisteredSources()
{
	FScopeLock Lock(&GRegistryLock);

	TArray<FGuid> Result;
	Result.Reserve(GEntries.Num());
	for (const FRegistryEntry& Entry : GEntries)
	{
		Result.Add(Entry.SourceGuid);
	}
	return Result;
}
