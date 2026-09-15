#include "TracyConversionResources.hpp"
#include "../analysis/TracyAnalysisIoPath.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif
namespace tracy::storage
{
namespace
{
[[noreturn]] void Refuse(const char* reason) { throw std::runtime_error(reason); }
void Peak(std::atomic<uint64_t>& peak,uint64_t value)
{ auto old=peak.load();while(value>old&&!peak.compare_exchange_weak(old,value)){} }
std::filesystem::path Key(const std::filesystem::path& path)
{ return std::filesystem::absolute(path).lexically_normal(); }
uint64_t PathMemory(const std::filesystem::path& p){return 512+4ull*p.native().size()*sizeof(std::filesystem::path::value_type);}
}
ConversionResources::ConversionResources(ResourceLimits limits,SampleProvider sample,std::function<uint64_t()> freeDisk,std::function<bool()> cancelled)
    :m_limits(limits),m_sample(std::move(sample)),m_freeDisk(std::move(freeDisk)),m_cancelProbe(std::move(cancelled))
{
    if(!limits.streamBytes || limits.streamBytes>UINT64_MAX/3 || !limits.clusterBytes ||
        !limits.maximumPrivateBytes || limits.maximumPrivateBytes>(32ull<<30) ||
        limits.targetPrivateBytes>limits.maximumPrivateBytes || !m_sample || !m_freeDisk)
        Refuse("conversion_resource_configuration");
    m_diskLimit=limits.streamBytes*3;
    if(limits.absoluteDiskLimit) m_diskLimit=std::min(m_diskLimit,limits.absoluteDiskLimit);
    if(limits.controlReserveBytes>m_diskLimit)Refuse("conversion_control_reserve");
    m_workspace=std::make_shared<analysis::AnalysisWorkspaceBudget>(limits.workspaceBytes,limits.workspaceBytes,
        [this](uint64_t bytes){if(!m_initializing)Check(bytes);});
    m_registryMemory=analysis::AnalysisWorkspaceReservation(m_workspace);
    // Fixed arena allowance for failure metadata, its path registries and the monitor.
    // This reservation survives budget refusal, so reporting needs no new lease.
    m_controlMemory=analysis::AnalysisWorkspaceReservation(m_workspace,2ull<<20);
    m_initializing=false;
}
ConversionResources::~ConversionResources() { StopMonitoring(); }
void ConversionResources::CheckFast() const
{
    if(m_cancelled.load(std::memory_order_acquire)) Refuse("conversion_cancelled");
    if(m_failed.load(std::memory_order_acquire)) Refuse(m_failure);
}
void ConversionResources::CheckSample(const ResourceSample& s,uint64_t next)
{
    Peak(m_peakPrivate,std::max(s.privateBytes,s.peakPrivateBytes));Peak(m_peakWorking,std::max(s.workingSetBytes,s.peakWorkingSetBytes));
    Peak(m_peakSystemCommit,s.systemCommit);
    const auto headroom=s.systemCommitLimit>s.systemCommit?s.systemCommitLimit-s.systemCommit:0;
    auto old=m_minCommitHeadroom.load();while(headroom<old&&!m_minCommitHeadroom.compare_exchange_weak(old,headroom)){}
    m_soft=s.privateBytes>=m_limits.targetPrivateBytes;
    if(s.privateBytes>m_limits.maximumPrivateBytes || next>m_limits.maximumPrivateBytes-s.privateBytes)
        Refuse("conversion_process_commit_limit");
    if(s.systemCommitLimit && (s.systemCommit>s.systemCommitLimit ||
        m_limits.commitReserveBytes>s.systemCommitLimit-s.systemCommit ||
        next>s.systemCommitLimit-s.systemCommit-m_limits.commitReserveBytes))
        Refuse("conversion_system_commit_headroom");
    if(s.physicalTotal && (s.physicalAvailable<std::max<uint64_t>(8ull<<30,s.physicalTotal*15/100) || s.physicalLoadPercent>=85))
        Refuse("conversion_physical_memory_pressure");
    if(s.physicalTotal && s.privateBytes>s.physicalTotal*60/100)
        Refuse("conversion_process_physical_ratio");
}
void ConversionResources::Check(uint64_t next)
{ if(m_cancelProbe&&m_cancelProbe())Cancel();CheckFast();CheckSample(m_sample(),next); }
void ConversionResources::Fail(const char* reason) noexcept
{
    try {
        std::lock_guard lock(m_failureLock);
        if(m_failed.load()) return;
        std::strncpy(m_failure,reason?reason:"conversion_unknown_failure",sizeof(m_failure)-1);
        m_failed.store(true,std::memory_order_release);
    } catch(...) { m_cancelled=true; }
}
const char* ConversionResources::Failure() const noexcept
{ return m_failed.load(std::memory_order_acquire)?m_failure:(m_cancelled.load()?"conversion_cancelled":""); }
void ConversionResources::StartMonitoring()
{
    if(m_monitor.joinable()) Refuse("conversion_monitor_already_started");
    m_stop=false;
    m_monitor=std::thread([this]{
        while(!m_stop.load()) {
            try{Check();}catch(const std::exception& e){Fail(e.what());}catch(...){Fail("conversion_monitor_failure");}
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}
void ConversionResources::StopMonitoring() noexcept
{ m_stop=true;try{if(m_monitor.joinable())m_monitor.join();}catch(...){} try{CheckSample(m_sample(),0);}catch(...){} }
void ConversionResources::BeforeFileGrowth(const std::filesystem::path& path,uint64_t size)
{Grow(path,size,false);}
void ConversionResources::BeforeControlGrowth(const std::filesystem::path& path,uint64_t size)
{Grow(path,size,true);}
void ConversionResources::Grow(const std::filesystem::path& path,uint64_t size,bool control)
{
    if(!control)CheckFast();
    const auto cluster=m_limits.clusterBytes;
    if(size>UINT64_MAX-(cluster-1)) Refuse("conversion_disk_size_overflow");
    const auto charge=std::max(cluster,((size+cluster-1)/cluster)*cluster);
    const auto key=Key(path);
    std::lock_guard lock(m_filesLock);
    const auto it=m_files.find(key);const auto previous=it==m_files.end()?0:it->second.bytes;
    if(it!=m_files.end()&&it->second.control!=control)Refuse("conversion_file_accounting_kind");
    if(charge<=previous) return;
    if(m_files.size()>=4096 && it==m_files.end()) Refuse("conversion_owned_file_limit");
    const auto delta=charge-previous;
    const auto reserve=control?0:(m_limits.controlReserveBytes>m_controlUsed?m_limits.controlReserveBytes-m_controlUsed:0);
    if(control && m_limits.controlReserveBytes && (m_controlUsed>m_limits.controlReserveBytes || delta>m_limits.controlReserveBytes-m_controlUsed))Refuse("conversion_control_budget");
    if(m_diskCurrent>m_diskLimit || reserve>m_diskLimit-m_diskCurrent || delta>m_diskLimit-m_diskCurrent-reserve)Refuse("conversion_disk_ratio_limit");
    const auto available=m_freeDisk();
    const auto freeReserve=control?0:m_limits.diskReserveBytes;
    if(available<freeReserve || delta>available-freeReserve) Refuse("conversion_disk_free_space");
    if(it==m_files.end()) {
        // Control metadata has a bounded reserve independent of a latched failure.
        // Its small registry allocation must remain possible during error reporting.
        if(control&&PathMemory(key)>(1ull<<20)-m_controlRegistryBytes)Refuse("conversion_control_registry_limit");
        if(!control)m_registryMemory.Resize(m_registryMemory.Bytes()+PathMemory(key));
        m_files.emplace(key,OwnedFile{charge,control,false});
        if(control)m_controlRegistryBytes+=PathMemory(key);
    }else it->second.bytes=charge;
    if(control)m_controlUsed+=delta;
    m_diskCurrent+=delta;m_diskPeak=std::max(m_diskPeak,m_diskCurrent);
}
bool ConversionResources::RemoveOwned(const std::filesystem::path& path) noexcept
{
    try {
        const auto key=Key(path);std::lock_guard lock(m_filesLock);
        const auto it=m_files.find(key);if(it==m_files.end()) return false;
        std::error_code error;std::filesystem::remove(analysis::AnalysisIoPath(key),error);
        if(error || std::filesystem::exists(analysis::AnalysisIoPath(key))) return false;
        m_diskCurrent-=it->second.bytes;
        if(it->second.control){m_controlUsed-=it->second.bytes;m_controlRegistryBytes-=PathMemory(key);}
        else m_registryMemory.Resize(m_registryMemory.Bytes()-PathMemory(key));
        m_files.erase(it);return true;
    }catch(...){return false;}
}
void ConversionResources::RenameOwned(const std::filesystem::path& from,const std::filesystem::path& to)
{
    Check();
    const auto a=Key(from);auto b=Key(to);std::lock_guard lock(m_filesLock);
    const auto it=m_files.find(a);if(it==m_files.end() || m_files.contains(b)) Refuse("conversion_rename_ownership");
    const auto oldMemory=PathMemory(a),newMemory=PathMemory(b);
    const bool control=it->second.control;
    if(control&&newMemory>oldMemory&&newMemory-oldMemory>(1ull<<20)-m_controlRegistryBytes)Refuse("conversion_control_registry_limit");
    if(!it->second.control && newMemory>oldMemory)m_registryMemory.Resize(m_registryMemory.Bytes()+newMemory-oldMemory);
#ifdef _WIN32
    if(!MoveFileExW(analysis::AnalysisIoPath(a).c_str(),analysis::AnalysisIoPath(b).c_str(),MOVEFILE_WRITE_THROUGH))Refuse("conversion_file_rename");
#else
    std::filesystem::rename(a,b);
#endif
    auto node=m_files.extract(it);node.key()=std::move(b);m_files.insert(std::move(node));
    if(control)m_controlRegistryBytes=m_controlRegistryBytes-oldMemory+newMemory;
    if(!control && oldMemory>newMemory)m_registryMemory.Resize(m_registryMemory.Bytes()-(oldMemory-newMemory));
}
void ConversionResources::ReplaceOwned(const std::filesystem::path& from,const std::filesystem::path& to,bool externalDestination)
{
    const auto a=Key(from);auto b=Key(to);std::lock_guard lock(m_filesLock);
    auto it=m_files.find(a),dest=m_files.find(b);
    if(it==m_files.end() || !it->second.control)Refuse("conversion_replace_ownership");
    if(dest==m_files.end() && std::filesystem::exists(analysis::AnalysisIoPath(b)) && !externalDestination)Refuse("conversion_existing_control_file");
    if(dest!=m_files.end()&&!dest->second.control)Refuse("conversion_replace_accounting_kind");
    const auto registryAfter=m_controlRegistryBytes-PathMemory(a)-(dest==m_files.end()?0:PathMemory(b))+PathMemory(b);
    if(registryAfter>(1ull<<20))Refuse("conversion_control_registry_limit");
#ifdef _WIN32
    if(!MoveFileExW(analysis::AnalysisIoPath(a).c_str(),analysis::AnalysisIoPath(b).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("conversion_control_publish:"+std::to_string(GetLastError()));
#else
    std::filesystem::rename(a,b);
#endif
    const bool keep=dest!=m_files.end()&&dest->second.keep;
    if(dest!=m_files.end()){m_diskCurrent-=dest->second.bytes;m_controlUsed-=dest->second.bytes;m_files.erase(dest);}
    auto node=m_files.extract(it);node.key()=std::move(b);node.mapped().keep=keep;m_files.insert(std::move(node));
    m_controlRegistryBytes=registryAfter;
}
void ConversionResources::KeepOwned(const std::filesystem::path& path)
{std::lock_guard lock(m_filesLock);const auto it=m_files.find(Key(path));if(it==m_files.end())Refuse("conversion_keep_ownership");it->second.keep=true;}
bool ConversionResources::CleanupOwned() noexcept
{
    try {
        std::vector<std::filesystem::path> paths;
        {std::lock_guard lock(m_filesLock);for(auto it=m_files.rbegin();it!=m_files.rend();++it)if(!it->second.keep)paths.push_back(it->first);}
        bool all=true;for(const auto& p:paths)if(!RemoveOwned(p))all=false;return all;
    }catch(...){return false;}
}
uint64_t ConversionResources::DiskCurrent() const { std::lock_guard lock(m_filesLock);return m_diskCurrent; }
std::vector<ConversionResources::FileSnapshot> ConversionResources::Files() const {
    std::lock_guard lock(m_filesLock);std::vector<FileSnapshot> result;result.reserve(m_files.size());
    for(const auto& [path,file]:m_files)result.push_back({path,file.bytes,file.keep});return result;
}
uint64_t ConversionResources::DiskPeak() const { std::lock_guard lock(m_filesLock);return m_diskPeak; }
ResourceSample ConversionResources::SystemSample()
{
    ResourceSample s;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX p{};p.cb=sizeof(p);
    MEMORYSTATUSEX m{};m.dwLength=sizeof(m);
    PERFORMANCE_INFORMATION i{};i.cb=sizeof(i);
    if(!GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&p),sizeof(p)) ||
        !GlobalMemoryStatusEx(&m) || !GetPerformanceInfo(&i,sizeof(i))) Refuse("conversion_memory_observation_failed");
    s.privateBytes=p.PrivateUsage;s.workingSetBytes=p.WorkingSetSize;
    // PeakPagefileUsage is the OS lifetime high-water mark of private commit.
    // https://learn.microsoft.com/windows/win32/api/psapi/ns-psapi-process_memory_counters_ex
    s.peakPrivateBytes=p.PeakPagefileUsage;s.peakWorkingSetBytes=p.PeakWorkingSetSize;
    s.physicalTotal=m.ullTotalPhys;s.physicalAvailable=m.ullAvailPhys;s.physicalLoadPercent=m.dwMemoryLoad;
    s.systemCommit=uint64_t(i.CommitTotal)*i.PageSize;s.systemCommitLimit=uint64_t(i.CommitLimit)*i.PageSize;
#else
    Refuse("conversion_memory_observation_unsupported");
#endif
    return s;
}
uint64_t ConversionResources::VolumeFree(const std::filesystem::path& path)
{
#ifdef _WIN32
    ULARGE_INTEGER available{},total{},free{};
    if(!GetDiskFreeSpaceExW(analysis::AnalysisIoPath(std::filesystem::absolute(path).parent_path()).c_str(),&available,&total,&free))Refuse("conversion_volume_free_query");
    return available.QuadPart;
#else
    return std::filesystem::space(std::filesystem::absolute(path).parent_path()).available;
#endif
}
std::filesystem::path ConversionResources::VolumeRoot(const std::filesystem::path& path)
{
#ifdef _WIN32
    wchar_t root[32768];
    if(!GetVolumePathNameW(analysis::AnalysisIoPath(std::filesystem::absolute(path).parent_path()).c_str(),root,32768))Refuse("conversion_volume_query");
    return root;
#else
    return std::filesystem::absolute(path).root_path();
#endif
}
uint64_t ConversionResources::VolumeCluster(const std::filesystem::path& path)
{
#ifdef _WIN32
    DWORD sectors=0,bytes=0,free=0,total=0;
    if(!GetDiskFreeSpaceW(VolumeRoot(path).c_str(),&sectors,&bytes,&free,&total)) Refuse("conversion_cluster_query");
    return uint64_t(sectors)*bytes;
#else
    return 4096;
#endif
}
}
