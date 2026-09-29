#pragma once

#include "LiveLinkSourceFactory.h"

#include "MOVINStageLiveLinkSourceFactory.generated.h"

UCLASS()
class MOVINSTAGELIVELINK_API UMOVINStageLiveLinkSourceFactory : public ULiveLinkSourceFactory
{
	GENERATED_BODY()

public:
	virtual FText GetSourceDisplayName() const override;
	virtual FText GetSourceTooltip() const override;
	virtual EMenuType GetMenuType() const override;
	virtual TSharedPtr<ILiveLinkSource> CreateSource(const FString& ConnectionString) const override;
};
