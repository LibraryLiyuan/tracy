#ifndef __TRACYTRACESESSIONJOBS_HPP__
#define __TRACYTRACESESSIONJOBS_HPP__

#include "TracyTraceSessionStore.hpp"
#include "TracyTraceSource.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tracy::analysis
{

inline constexpr uint32_t TraceSessionJobIndexSchemaVersion = 1;

struct TraceSessionJobStats
{
    uint64_t jobTypes = 0;
    uint64_t jobs = 0;
    uint64_t schedules = 0;
    uint64_t configs = 0;
    uint64_t dependencies = 0;
    uint64_t stages = 0;
    uint64_t fileBytes = 0;
};

class TraceSessionJobReader
{
public:
    static std::shared_ptr<TraceSessionJobReader> Open(
        const std::filesystem::path& sessionRoot, const TraceSessionManifest& manifest,
        std::string& error );

    const std::vector<JobDto>& Jobs() const { return m_jobs; }
    const TraceSessionJobStats& Stats() const { return m_stats; }

private:
    std::vector<JobDto> m_jobs;
    TraceSessionJobStats m_stats;
};

std::filesystem::path TraceSessionJobIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest );
bool BuildTraceSessionJobDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionJobStats& stats, std::string& error );

}

#endif
