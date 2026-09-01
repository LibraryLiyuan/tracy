#ifndef __TRACYTRACESESSIONPLOTS_HPP__
#define __TRACYTRACESESSIONPLOTS_HPP__

#include "TracyTraceSessionStore.hpp"
#include "TracyTraceSource.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tracy::analysis
{

inline constexpr uint32_t TraceSessionPlotIndexSchemaVersion = 1;

struct TraceSessionPlotStats
{
    uint64_t plots = 0;
    uint64_t points = 0;
    uint64_t dataEvents = 0;
    uint64_t configEvents = 0;
    uint64_t nameEvents = 0;
    uint64_t fileBytes = 0;
};

class TraceSessionPlotReader
{
public:
    static std::shared_ptr<TraceSessionPlotReader> Open(
        const std::filesystem::path& sessionRoot, const TraceSessionManifest& manifest,
        std::string& error );

    const TraceSessionPlotStats& Stats() const { return m_stats; }
    const std::vector<PlotDto>& Plots() const { return m_plots; }
    std::vector<PlotPointDto> Scan( const ScanRange& range ) const;

private:
    struct Impl;
    explicit TraceSessionPlotReader( std::shared_ptr<Impl> impl );
    std::shared_ptr<Impl> m_impl;
    TraceSessionPlotStats m_stats;
    std::vector<PlotDto> m_plots;
};

std::filesystem::path TraceSessionPlotIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest );
bool BuildTraceSessionPlotDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionPlotStats& stats, std::string& error );
bool AuditTraceSessionPlotDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionPlotStats& stats, std::string& error );

}

#endif
