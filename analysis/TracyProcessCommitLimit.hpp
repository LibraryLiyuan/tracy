#ifndef TRACY_PROCESS_COMMIT_LIMIT_HPP
#define TRACY_PROCESS_COMMIT_LIMIT_HPP
#include <cstdint>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace tracy::analysis
{
// A process commit cap, not a resident-set measurement and not a per-domain
// allocation budget. The job has no kill-on-close flag; it affects this process
// and does not terminate any other process when the handle is released.
class ProcessCommitLimit
{
public:
    explicit ProcessCommitLimit(uint64_t bytes)
    {
        if(!bytes || bytes>32ull*1024*1024*1024) throw std::runtime_error("process_commit_limit_configuration");
#ifdef _WIN32
        m_job=CreateJobObjectW(nullptr,nullptr);
        if(!m_job) Fail("create");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        limits.ProcessMemoryLimit=SIZE_T(bytes);
        if(!SetInformationJobObject(m_job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))) Fail("configure");
        if(!AssignProcessToJobObject(m_job,GetCurrentProcess())) Fail("assign");
#else
        throw std::runtime_error("process_commit_hard_limit_unsupported");
#endif
    }
    ~ProcessCommitLimit()
    {
#ifdef _WIN32
        if(m_job) CloseHandle(m_job);
#endif
    }
    ProcessCommitLimit(const ProcessCommitLimit&)=delete;
    ProcessCommitLimit& operator=(const ProcessCommitLimit&)=delete;
private:
#ifdef _WIN32
    HANDLE m_job=nullptr;
    [[noreturn]] void Fail(const char* operation)
    {
        const auto error=GetLastError(); if(m_job) CloseHandle(m_job); m_job=nullptr;
        throw std::runtime_error(std::string("process_commit_limit_")+operation+":"+std::to_string(error));
    }
#endif
};
}
#endif
