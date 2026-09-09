#include "TracyAnalysisProcessMemory.hpp"

#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace tracy::analysis
{
AnalysisProcessMemorySample ReadAnalysisProcessMemory()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb=sizeof(counters);
    if(!GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),sizeof(counters))) return {};
    return {true,uint64_t(counters.WorkingSetSize),uint64_t(counters.PrivateUsage)};
#elif defined(__linux__)
    std::ifstream input("/proc/self/smaps_rollup");
    AnalysisProcessMemorySample result;
    bool rss=false,privateDirty=false;
    std::string line;
    while(std::getline(input,line))
    {
        std::istringstream fields(line); std::string key,unit; uint64_t kib=0;
        if(!(fields>>key>>kib>>unit) || unit!="kB") continue;
        if(key=="Rss:") { result.residentBytes=kib*1024; rss=true; }
        if(key=="Private_Clean:" || key=="Private_Dirty:" || key=="Private_Hugetlb:" || key=="Swap:")
            result.privateBytes+=kib*1024;
        if(key=="Private_Dirty:") privateDirty=true;
    }
    result.available=rss && privateDirty; return result;
#elif defined(__APPLE__)
    task_vm_info_data_t info{}; mach_msg_type_number_t count=TASK_VM_INFO_COUNT;
    if(task_info(mach_task_self(),TASK_VM_INFO,reinterpret_cast<task_info_t>(&info),&count)!=KERN_SUCCESS) return {};
    return {true,uint64_t(info.resident_size),uint64_t(info.phys_footprint)};
#else
    return {}; // Fail closed instead of reporting an unmeasured zero.
#endif
}

AnalysisProcessMemoryGuard::AnalysisProcessMemoryGuard(AnalysisProcessMemoryOptions options)
    :m_options(std::move(options))
{
    if(!m_options.maximumBytes || m_options.maximumBytes>32ull*1024*1024*1024 ||
        m_options.interval<std::chrono::milliseconds(1) || m_options.interval>std::chrono::seconds(1) || !m_options.sample)
        throw std::runtime_error("analysis_process_memory_configuration");
    m_snapshot.maximumBytes=m_options.maximumBytes;
}
void AnalysisProcessMemoryGuard::Check(bool forceSample)
{
    std::lock_guard lock(m_mutex);
    if(!m_snapshot.error.empty()) throw std::runtime_error(m_snapshot.error);
    const auto now=std::chrono::steady_clock::now();
    if(!forceSample && now<m_nextSample) return;
    m_nextSample=now+m_options.interval;
    AnalysisProcessMemorySample sample;
    try { sample=m_options.sample(); } catch(...) { sample={}; }
    ++m_snapshot.samples;
    if(!sample.available) m_snapshot.error="analysis_process_memory_unavailable";
    else
    {
        m_snapshot.residentBytes=sample.residentBytes; m_snapshot.privateBytes=sample.privateBytes;
        m_snapshot.peakResidentBytes=std::max(m_snapshot.peakResidentBytes,sample.residentBytes);
        m_snapshot.peakPrivateBytes=std::max(m_snapshot.peakPrivateBytes,sample.privateBytes);
        if(sample.residentBytes>m_options.maximumBytes || sample.privateBytes>m_options.maximumBytes)
            m_snapshot.error="analysis_process_memory_budget";
    }
    if(!m_snapshot.error.empty()) throw std::runtime_error(m_snapshot.error);
}
AnalysisProcessMemorySnapshot AnalysisProcessMemoryGuard::Snapshot() const
{ std::lock_guard lock(m_mutex); return m_snapshot; }

std::jthread AnalysisProcessMemoryGuard::Monitor(std::stop_source stop)
{
    return std::jthread([this,stop=std::move(stop)](std::stop_token token) mutable {
        std::mutex mutex; std::condition_variable_any changed;
        while(!token.stop_requested() && !stop.stop_requested())
        {
            try { Check(); } catch(...) { stop.request_stop(); return; }
            std::unique_lock lock(mutex);
            changed.wait_for(lock,token,m_options.interval,[]{return false;});
        }
    });
}
}
