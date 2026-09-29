#include "MOVINStageLiveLinkLog.h"

#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogMOVINStageLiveLink);

class FMOVINStageLiveLinkModule : public IModuleInterface
{
};

IMPLEMENT_MODULE(FMOVINStageLiveLinkModule, MOVINStageLiveLink)
