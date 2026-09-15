#ifndef TRACY_ALLOCATION_DIAGNOSTICS_HPP
#define TRACY_ALLOCATION_DIAGNOSTICS_HPP
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <new.h>
#endif
namespace tracy {
struct AllocationContext {
    const char* site="CRT allocation";uintptr_t owner=0;
    uint64_t oldSize=0,oldCapacity=0,elementBytes=0;
};
struct AllocationFailureFrame {uintptr_t address=0,moduleBase=0;char module[260]{};};
struct AllocationFailureRecord {
    bool ready=false;const char* site="CRT allocation";const char* role="unspecified";
    uintptr_t owner=0;uint64_t oldSize=0,oldCapacity=0,elementBytes=0,requestedBytes=0;
    uint64_t privateBytes=0,systemCommitBytes=0,systemCommitLimitBytes=0,tickMilliseconds=0,eventOrdinal=0,completedFrames=0;
    uint32_t threadId=0,frameCount=0;int32_t queueType=-1;
    AllocationFailureFrame frames[24]{};
};
inline std::atomic<bool> g_allocationDiagnosticsEnabled{false};
inline std::atomic<unsigned> g_allocationFailureState{0};
inline AllocationFailureRecord g_allocationFailure;
inline thread_local const AllocationContext* g_allocationContext=nullptr;
inline thread_local const char* g_allocationRole="unspecified";
inline thread_local int32_t g_allocationQueueType=-1;
inline thread_local uint64_t g_allocationEventOrdinal=0,g_allocationCompletedFrames=0;
inline bool AllocationDiagnosticsEnabled() noexcept {return g_allocationDiagnosticsEnabled.load(std::memory_order_relaxed);}
class ScopedAllocationContext {
    AllocationContext m_context;const AllocationContext* m_previous=nullptr;bool m_active;
public:
    ScopedAllocationContext(const char* site,const void* owner,uint64_t size,uint64_t capacity,uint64_t width) noexcept
        :m_context{site,reinterpret_cast<uintptr_t>(owner),size,capacity,width},m_active(AllocationDiagnosticsEnabled()) {
        if(m_active){m_previous=g_allocationContext;g_allocationContext=&m_context;}
    }
    ~ScopedAllocationContext(){if(m_active)g_allocationContext=m_previous;}
};
inline AllocationFailureRecord GetAllocationFailure() noexcept {
    if(g_allocationFailureState.load(std::memory_order_acquire)!=2)return {};
    return g_allocationFailure;
}
#ifdef _WIN32
// Called at the failed allocation, before exception unwinding. No retry, heap
// allocation, file IO, symbol lookup or container traversal is performed here.
inline int __cdecl CaptureAllocationFailure(size_t bytes) noexcept {
    if(!AllocationDiagnosticsEnabled())return 0;
    unsigned expected=0;if(!g_allocationFailureState.compare_exchange_strong(expected,1))return 0;
    const auto savedError=GetLastError();auto& r=g_allocationFailure;
    r.requestedBytes=bytes;r.threadId=GetCurrentThreadId();r.tickMilliseconds=GetTickCount64();
    r.role=g_allocationRole;r.queueType=g_allocationQueueType;r.eventOrdinal=g_allocationEventOrdinal;r.completedFrames=g_allocationCompletedFrames;
    if(g_allocationContext){const auto& c=*g_allocationContext;r.site=c.site;r.owner=c.owner;r.oldSize=c.oldSize;r.oldCapacity=c.oldCapacity;r.elementBytes=c.elementBytes;}
    PROCESS_MEMORY_COUNTERS_EX p{};p.cb=sizeof(p);
    if(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&p),sizeof(p)))r.privateBytes=p.PrivateUsage;
    PERFORMANCE_INFORMATION info{};info.cb=sizeof(info);
    if(GetPerformanceInfo(&info,sizeof(info))){r.systemCommitBytes=uint64_t(info.CommitTotal)*info.PageSize;r.systemCommitLimitBytes=uint64_t(info.CommitLimit)*info.PageSize;}
    void* frames[24]{};r.frameCount=CaptureStackBackTrace(1,24,frames,nullptr);
    for(uint32_t i=0;i<r.frameCount;++i){auto& f=r.frames[i];f.address=reinterpret_cast<uintptr_t>(frames[i]);MEMORY_BASIC_INFORMATION memory{};
        if(VirtualQuery(frames[i],&memory,sizeof(memory))){f.moduleBase=reinterpret_cast<uintptr_t>(memory.AllocationBase);GetModuleFileNameA(reinterpret_cast<HMODULE>(memory.AllocationBase),f.module,sizeof(f.module)-1);}}
    r.ready=true;g_allocationFailureState.store(2,std::memory_order_release);SetLastError(savedError);return 0;
}
#endif
class AllocationDiagnostics {
#ifdef _WIN32
    _PNH m_previousHandler=nullptr;int m_previousMode=0;
#endif
public:
    AllocationDiagnostics(){
#ifdef _WIN32
        if(AllocationDiagnosticsEnabled())throw std::runtime_error("allocation_diagnostics_already_active");
        void* warmup[2];CaptureStackBackTrace(0,2,warmup,nullptr);
        g_allocationFailure={};g_allocationFailureState=0;
        g_allocationRole="main";
        g_allocationDiagnosticsEnabled=true;
        m_previousHandler=_set_new_handler(CaptureAllocationFailure);m_previousMode=_set_new_mode(1);
#else
        throw std::runtime_error("allocation_diagnostics_unsupported");
#endif
    }
    ~AllocationDiagnostics(){
#ifdef _WIN32
        _set_new_mode(m_previousMode);_set_new_handler(m_previousHandler);g_allocationDiagnosticsEnabled=false;
#endif
    }
    AllocationDiagnostics(const AllocationDiagnostics&)=delete;
    AllocationDiagnostics& operator=(const AllocationDiagnostics&)=delete;
};
}
#endif
