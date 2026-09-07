#ifndef __TRACYBOUNDEDSCANCURSOR_HPP__
#define __TRACYBOUNDEDSCANCURSOR_HPP__

#include "TracyGpuScanTypes.hpp"
#include "TracyTraceSource.hpp"

#include <optional>
#include <stdexcept>
#include <variant>

namespace tracy::analysis
{

inline constexpr uint32_t NativeBoundedScanSchemaVersion = 1;
inline constexpr size_t NativeBoundedScanMaximumBatch = 65'536;

// Marker interface. Native scanners refuse a TraceSource that does not opt in,
// because several legacy TraceSource::Scan* defaults first materialize a full
// vector. Implementations must guarantee every scan method used below is truly
// bounded by the requested batch size (storage pages may be independently
// bounded and sliced).
class NativeBoundedTraceSource
{
public:
    virtual ~NativeBoundedTraceSource() = default;
    virtual uint32_t NativeBoundedScanVersion() const = 0;
};

// Normalized GPU scan projections are independent from storage backends.
// This contract prevents fallback to raw full-catalog reads.
class GpuCatalogBoundedScanSource
{
public:
    virtual ~GpuCatalogBoundedScanSource() = default;
    virtual uint64_t GetGpuCatalogResourceCountBounded() const = 0;
    virtual uint64_t GetGpuCatalogAllocationCountBounded() const = 0;
    virtual uint64_t GetGpuCatalogPassCountBounded() const = 0;
    virtual uint64_t GetGpuCatalogRangeCountBounded() const = 0;
    virtual std::vector<GpuAnalysisResourceSummary> ScanGpuCatalogResourcesBounded(
        size_t offset, size_t limit ) const = 0;
    virtual std::vector<GpuAllocationAnalysisRecord> ScanGpuCatalogAllocationsBounded(
        size_t offset, size_t limit ) const = 0;
    virtual std::vector<GpuPassWorkingSet> ScanGpuCatalogPassesBounded(
        size_t offset, size_t limit ) const = 0;
    virtual std::vector<GpuAnalysisRangeStoreEntry> ScanGpuCatalogRangesBounded(
        size_t offset, size_t limit ) const = 0;
};

enum class BoundedScanDomain : uint8_t
{
    Frame,
    CpuZone,
    GpuZone,
    Job,
    Relation,
    Memory,
    Sample,
    ContextSwitch,
    RuntimeDomain,
    ScriptFrame,
    ScriptStack,
    IoRequest,
    GfxDispatch,
    GfxEntity,
    GfxLink,
    Message,
    Plot,
    LockEvent,
    CpuUsage,
    Callsite,
    GpuResource,
    GpuAllocation,
    GpuPass,
    GpuRange
};

using BoundedScanRecords = std::variant<
    std::monostate,
    std::vector<FrameDto>,
    std::vector<CpuZoneDto>,
    std::vector<GpuZoneDto>,
    std::vector<JobDto>,
    std::vector<RelationDto>,
    std::vector<MemoryEventDto>,
    std::vector<SampleDto>,
    std::vector<ContextSwitchDto>,
    std::vector<RuntimeDomainStateDto>,
    std::vector<ScriptFrameDto>,
    std::vector<ScriptStackEventDto>,
    std::vector<IoRequestDto>,
    std::vector<GfxDispatchDto>,
    std::vector<GfxEntityDto>,
    std::vector<GfxLinkDto>,
    std::vector<MessageDto>,
    std::vector<PlotPointDto>,
    std::vector<LockEventDto>,
    std::vector<CpuUsagePointDto>,
    std::vector<CallsiteDto>,
    std::vector<GpuAnalysisResourceSummary>,
    std::vector<GpuAllocationAnalysisRecord>,
    std::vector<GpuPassWorkingSet>,
    std::vector<GpuAnalysisRangeStoreEntry>>;

struct BoundedScanCursor
{
    uint32_t schemaVersion = NativeBoundedScanSchemaVersion;
    BoundedScanDomain domain = BoundedScanDomain::Frame;
    TraceSourceKind sourceKind = TraceSourceKind::Snapshot;
    uint64_t sourceRevision = 0;
    int64_t startNs = std::numeric_limits<int64_t>::min();
    int64_t endNs = std::numeric_limits<int64_t>::max();
    uint64_t ordinal = 0;
    int64_t lastTimeNs = std::numeric_limits<int64_t>::min();
    std::string lastStableEntity;
    std::string resumeToken;
    bool complete = false;
};

struct BoundedScanRequest
{
    BoundedScanDomain domain = BoundedScanDomain::Frame;
    int64_t startNs = std::numeric_limits<int64_t>::min();
    int64_t endNs = std::numeric_limits<int64_t>::max();
    size_t limit = 4096;
    std::optional<BoundedScanCursor> cursor;
};

struct BoundedScanBatch
{
    BoundedScanCursor cursor;
    BoundedScanRecords records;
    size_t Count() const;
};

class BoundedScanError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

class BoundedTraceScanner
{
public:
    explicit BoundedTraceScanner( const TraceSource& source );
    BoundedScanBatch Read( const BoundedScanRequest& request ) const;

private:
    const TraceSource& m_source;
    const NativeBoundedTraceSource& m_bounded;
    const GpuCatalogBoundedScanSource* m_gpu = nullptr;
};

const char* BoundedScanDomainName( BoundedScanDomain domain );

}

#endif
