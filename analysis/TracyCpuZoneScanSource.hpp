#ifndef __TRACYCPUZONESCANSOURCE_HPP__
#define __TRACYCPUZONESCANSOURCE_HPP__

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tracy::analysis
{

// Optional interface of a trace source whose CPU zones can be read as compact
// records, by several threads at once. The records are the zones of the
// source's whole-capture ScanCpuZones, in the same order and with the same
// values, with table indexes in place of the DTO strings. Each thread's zones
// occupy one contiguous ordinal range.
struct CpuZoneScanLocation
{
    std::string ref;            // CpuZoneDto::sourceLocationRef
    std::string name;           // CpuZoneDto::name of a zone without a name of its own
    std::string function;
    std::string file;
    uint32_t line = 0;
};

struct CpuZoneScanThread
{
    std::string ref;            // CpuZoneDto::threadRef
    uint64_t first = 0;         // ordinal of the thread's first zone
    uint64_t count = 0;
    // Ascending ordinals of zones without a parent, a subset of them: places
    // where a scan of the thread can start with no zone open.
    std::vector<uint64_t> roots;
    // When known (else empty), for the zones from each of those zones up to
    // the next one: their earliest start and latest end (INT64_MAX when one
    // has no end).
    std::vector<int64_t> rootStarts;
    std::vector<int64_t> rootEnds;
};

struct CpuZoneScanRecord
{
    static constexpr uint64_t NoParent = UINT64_MAX;
    uint64_t ordinal = 0;
    uint64_t parent = NoParent;     // ordinal of the zone CpuZoneDto::parentRef names
    int64_t startNs = 0;
    int64_t endNs = 0;              // CpuZoneDto::endNs when hasEnd
    uint32_t location = 0;          // index into CpuZoneScanLocations()
    bool hasEnd = true;
    bool complete = true;           // CpuZoneDto::complete
    bool timingValid = true;        // CpuZoneDto::timingValid
    bool hasInvalidReason = false;  // CpuZoneDto::timingInvalidReason is set
    bool hasName = false;           // `name` replaces the location's name
    // Views valid until the reader's next Read.
    std::string_view invalidReason;
    std::string_view name;
};

class CpuZoneScanReader
{
public:
    virtual ~CpuZoneScanReader() = default;
    // Positions before zone `ordinal` of thread `thread` (an index into
    // CpuZoneScanThreads()).
    virtual void Seek( size_t thread, uint64_t ordinal ) = 0;
    // Replaces `records` with the next zones of the thread, at most `limit`;
    // none after its last zone.
    virtual void Read( std::vector<CpuZoneScanRecord>& records, size_t limit ) = 0;
};

class CpuZoneScanSource
{
public:
    virtual ~CpuZoneScanSource() = default;
    // False for a view whose underlying source has no compact records.
    virtual bool CpuZoneScanAvailable() const { return true; }
    virtual const std::vector<CpuZoneScanThread>& CpuZoneScanThreads() const = 0;
    virtual const std::vector<CpuZoneScanLocation>& CpuZoneScanLocations() const = 0;
    // Replaces `ref` with CpuZoneDto::ref of the zone with this ordinal.
    virtual void CpuZoneScanRef( uint64_t ordinal, std::string& ref ) const = 0;
    // A reader independent of every other; each may be used by its own thread.
    virtual std::unique_ptr<CpuZoneScanReader> OpenCpuZoneScanReader() const = 0;
};

}

#endif
