#ifndef TRACY_OFFLINE_DOMAINS_HPP
#define TRACY_OFFLINE_DOMAINS_HPP
#include "TracyJnData.hpp"
#include <cstddef>
#include <stdexcept>
#define TRACY_OFFLINE_APPEND_DOMAINS(X) \
 X(JobSchedule,jobSchedules,JnJobScheduleData) \
 X(JobConfig,jobConfigs,JnJobConfigData) \
 X(JobDependency,jobDependencies,JnJobDependencyData) \
 X(GfxDispatch,gfxDispatches,JnGfxDispatchData) \
 X(Frame,frames,JnFrameData) \
 X(IoRequest,ioRequests,JnIoRequestData) \
 X(IoConfig,ioConfigs,JnIoConfigData) \
 X(IoStage,ioStages,JnIoStageData) \
 X(RuntimeDomainState,runtimeDomainStates,JnRuntimeDomainStateData) \
 X(ScriptFrame,scriptFrames,JnScriptFrameData) \
 X(ScriptStack,scriptStacks,JnScriptStackData) \
 X(Callsite,callsites,JnCallsiteData)
namespace tracy {
enum class OfflineAppendDomain {
#define X(name,field,type) name,
 TRACY_OFFLINE_APPEND_DOMAINS(X)
#undef X
 Count
};
inline constexpr size_t OfflineAppendDomainCount=size_t(OfflineAppendDomain::Count);
struct OfflineAppendDescriptor {const char* name;uint32_t width;};
inline constexpr OfflineAppendDescriptor OfflineAppendDescriptors[]={
#define X(name,field,type) {#field,sizeof(type)},
 TRACY_OFFLINE_APPEND_DOMAINS(X)
#undef X
};
inline const OfflineAppendDescriptor& OfflineAppendInfo(OfflineAppendDomain domain){auto i=size_t(domain);if(i>=OfflineAppendDomainCount)throw std::runtime_error("offline_append_domain");return OfflineAppendDescriptors[i];}
}
#endif
