#include "Modules/ModuleManager.h"
#include "Trace/Trace.h"

class FSkiPresentationModule final : public FDefaultGameModuleImpl
{
public:
    virtual void StartupModule() override
    {
        FDefaultGameModuleImpl::StartupModule();
#if UE_BUILD_SHIPPING
        // Unreal Trace otherwise opens a process-wide TCP control listener on port 1985.
        // The offline Mountain contract permits no process-owned sockets.
        UE::Trace::Shutdown();
#endif
    }
};

IMPLEMENT_PRIMARY_GAME_MODULE(FSkiPresentationModule, SkiPresentation, "SkiAreaDesignChallenge")
