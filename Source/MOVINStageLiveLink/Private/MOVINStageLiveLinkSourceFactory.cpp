#include "MOVINStageLiveLinkSourceFactory.h"

#include "MOVINStageLiveLinkSource.h"

FText UMOVINStageLiveLinkSourceFactory::GetSourceDisplayName() const
{
	// Names the product, not the transport. A project can also have the MOVIN LiveLink (Tracin)
	// plugin installed, whose entry reads "MOVIN Live Source" - "MOVIN OSC" gave the user no
	// way to tell which one belonged to MOVIN Stage.
	return NSLOCTEXT("MOVINStageLiveLink", "FactoryDisplayName", "MOVIN Stage (OSC)");
}

FText UMOVINStageLiveLinkSourceFactory::GetSourceTooltip() const
{
	return NSLOCTEXT("MOVINStageLiveLink", "FactoryTooltip", "Receive MOVIN Stage motion and point cloud over a single UDP port (OSC).");
}

ULiveLinkSourceFactory::EMenuType UMOVINStageLiveLinkSourceFactory::GetMenuType() const
{
	return EMenuType::MenuEntry;
}

TSharedPtr<ILiveLinkSource> UMOVINStageLiveLinkSourceFactory::CreateSource(const FString& ConnectionString) const
{
	const FMOVINStageLiveLinkConnectionSettings Settings =
		FMOVINStageLiveLinkConnectionSettings::FromConnectionString(ConnectionString);
	return MakeShared<FMOVINStageLiveLinkSource>(Settings);
}
