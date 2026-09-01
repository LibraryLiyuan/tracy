#ifndef __TRACYTRACESESSIONLOCKS_HPP__
#define __TRACYTRACESESSIONLOCKS_HPP__

#include "TracyTraceSessionStore.hpp"
#include "TracyTraceSource.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tracy::analysis
{

inline constexpr uint32_t TraceSessionLockIndexSchemaVersion = 1;

struct TraceSessionLockStats
{
    uint64_t locks = 0;
    uint64_t events = 0;
    uint64_t announceEvents = 0;
    uint64_t terminateEvents = 0;
    uint64_t waitEvents = 0;
    uint64_t obtainEvents = 0;
    uint64_t releaseEvents = 0;
    uint64_t sharedWaitEvents = 0;
    uint64_t sharedObtainEvents = 0;
    uint64_t sharedReleaseEvents = 0;
    uint64_t nameEvents = 0;
    uint64_t markEvents = 0;
    uint64_t fileBytes = 0;
};

class TraceSessionLockReader
{
public:
    static std::shared_ptr<TraceSessionLockReader> Open(
        const std::filesystem::path& sessionRoot,
        const TraceSessionManifest& manifest, std::string& error );

    const TraceSessionLockStats& Stats() const { return m_stats; }
    std::vector<LockDto> Locks() const;
    std::vector<LockEventDto> Scan( const ScanRange& range ) const;

private:
    struct Impl;
    explicit TraceSessionLockReader( std::shared_ptr<Impl> impl );
    std::shared_ptr<Impl> m_impl;
    TraceSessionLockStats m_stats;
};

std::filesystem::path TraceSessionLockIndexRoot(
    const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest );
bool BuildTraceSessionLockDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionLockStats& stats,
    std::string& error );
bool AuditTraceSessionLockDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionLockStats& stats,
    std::string& error );

}

#endif
