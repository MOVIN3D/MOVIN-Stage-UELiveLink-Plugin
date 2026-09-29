#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

/**
 * Everything this module logs goes here rather than to LogTemp.
 *
 * A project can run this plugin alongside the MOVIN LiveLink (Tracin) plugin, which logs under
 * LogMOVINLiveLink. Anyone diagnosing "which source stopped receiving" filters the Output Log by
 * category, so a bind failure buried in LogTemp is a bind failure nobody finds.
 */
MOVINSTAGELIVELINK_API DECLARE_LOG_CATEGORY_EXTERN(LogMOVINStageLiveLink, Log, All);
