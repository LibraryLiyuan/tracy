#ifndef __TRACYTRACESESSIONDERIVED_HPP__
#define __TRACYTRACESESSIONDERIVED_HPP__

#include "TracyGpuAnalysisSidecar.hpp"
#include "TracyTraceSessionCanonical.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>

namespace tracy::analysis
{

inline constexpr uint32_t TraceSessionDomainIndexSchemaVersion = 1;

struct TraceSessionDerivedStats
{
    uint64_t indexedRecords = 0;
    uint64_t indexedProtocolEvents = 0;
    uint64_t indexedProtocolFrames = 0;
    uint64_t indexedTransportRecords = 0;
    uint64_t indexBytes = 0;
    uint64_t indexFiles = 0;
    uint64_t gpuResources = 0;
    uint64_t gpuAllocations = 0;
    uint64_t gpuPasses = 0;
    std::array<uint64_t, size_t( TraceSessionProtocolDomain::Count )> domains {};
};

struct TraceSessionDerivedControl
{
    std::stop_token stopToken;
    std::function<void( float, const char* )> progress;
    uint64_t minimumFreeBytes = 64ull * 1024 * 1024 * 1024;
};

std::filesystem::path TraceSessionDomainIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest );
bool BuildTraceSessionMandatoryDerived( const std::filesystem::path& sessionRoot,
    TraceSessionManifest& manifest, const TraceSessionInventory& inventory,
    const TraceSessionDerivedControl& control, TraceSessionDerivedStats& stats,
    std::string& error );
bool AuditTraceSessionFinal( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, const TraceSessionInventory& inventory,
    TraceSessionDerivedStats& stats, std::string& error );

}

#endif
