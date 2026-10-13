#ifndef __TRACYFRAMECPUSCANNER_HPP__
#define __TRACYFRAMECPUSCANNER_HPP__

#include "TracyBoundedScanCursor.hpp"
#include "TracyAnalysisWorkspaceBudget.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace tracy::analysis
{

enum class CpuWorkClass : uint8_t
{
    ActiveWork,
    Wait,
    IntentionalPacing,
    Unknown
};

struct CpuSignatureDefinition
{
    std::string signatureId;
    std::string parentSignatureId;
    std::string name;
    std::string sourceLocationRef;
    std::string function;
    std::string file;
    uint32_t line = 0;
    std::string threadRef;
    std::string threadRole;
    std::string path;
    uint32_t depth = 0;
    CpuWorkClass workClass = CpuWorkClass::Unknown;
    bool logical = false;
};

struct CpuSignatureFrameRun
{
    std::string signatureId;
    std::string frameSetRef;
    std::string frameRef;
    size_t frameIndex = 0;
    int64_t inclusiveNs = 0;
    int64_t directChildUnionNs = 0;
    int64_t exclusiveNs = 0;
    uint64_t occurrenceCount = 0;
    bool exact = true;
    std::vector<std::string> representativeEventRefs;
    CpuWorkClass workClass = CpuWorkClass::Unknown;
};

struct CpuSignatureDenominator
{
    std::string signatureId;
    std::string frameSetRef;
    uint64_t completeFrameDenominator = 0;
    uint64_t whenPresentDenominator = 0;
};

struct CpuScanQualityFinding
{
    std::string code;
    std::string message;
    uint64_t count = 0;
    std::vector<std::string> representativeRefs;
};

// Exact denominator facts are retained per FrameSet so downstream domains do
// not accidentally divide Player work by unrelated Editor/Render frame sets.
// The aggregate completeFrameCount below remains useful for scanner telemetry,
// but it is never a valid cross-domain per-frame denominator by itself.
struct CpuFrameSetDenominator
{
    std::string frameSetRef;
    std::string name;
    bool continuous = false;
    uint64_t completeFrameCount = 0;
};

struct CpuFrameScanTimings
{
    uint64_t totalNs = 0;
    uint64_t frameReadNs = 0;
    uint64_t zoneReadNs = 0;
    uint64_t zoneProcessNs = 0;
    uint64_t frameReadCalls = 0;
    uint64_t zoneReadCalls = 0;
    uint64_t inputZones = 0;
    uint64_t frameBoundaryProbes = 0;
    uint64_t logicalSignatureHashes = 0;
    uint64_t emittedRuns = 0;
    uint64_t sinkSamples = 0;
    uint64_t sinkSampleNs = 0;
};

struct CpuFrameScanResult
{
    AnalysisWorkspaceReservation outputWorkspace;
    explicit CpuFrameScanResult( std::shared_ptr<AnalysisWorkspaceBudget> workspace = {} )
        : outputWorkspace( std::move( workspace ), 2048 ) {}
    CpuFrameScanResult( CpuFrameScanResult&& ) = default;
    CpuFrameScanResult& operator=( CpuFrameScanResult&& ) = delete;
    uint64_t inputZoneCount = 0;
    uint64_t validZoneCount = 0;
    uint64_t invalidZoneCount = 0;
    uint64_t unassignedZoneCount = 0;
    uint64_t completeFrameCount = 0;
    uint64_t incompleteFrameCount = 0;
    size_t maximumDepth = 0;
    size_t maximumBatchObserved = 0;
    uint64_t frameBoundaryProbeCount = 0;
    uint64_t exactSignatureHashCount = 0;
    uint64_t logicalSignatureHashCount = 0;
    CpuFrameScanTimings timings;
    bool qualityComplete = true;
    std::vector<CpuSignatureDefinition> signatures;
    std::vector<CpuSignatureFrameRun> runs;
    std::vector<CpuSignatureDenominator> denominators;
    std::vector<CpuFrameSetDenominator> frameSetDenominators;
    std::vector<CpuScanQualityFinding> qualityFindings;
};

using CpuSignatureFrameSink = std::function<bool( const CpuSignatureFrameRun& )>;

// Non-owning hot-path view used by the native policy scanner. All strings are
// valid only for the duration of the callback; ordinals are stable for one
// ScanView invocation and avoid repeated string registration and copying.
struct CpuSignatureFrameRunView
{
    std::string_view signatureId;
    std::string_view frameSetRef;
    std::string_view frameRef;
    uint32_t signatureOrdinal = 0;
    uint32_t frameSetOrdinal = 0;
    size_t frameIndex = 0;
    int64_t inclusiveNs = 0;
    int64_t directChildUnionNs = 0;
    int64_t exclusiveNs = 0;
    uint64_t occurrenceCount = 0;
    bool exact = true;
    std::string_view representativeEventRef;
    CpuWorkClass workClass = CpuWorkClass::Unknown;
};

using CpuSignatureFrameViewSink = std::function<bool( const CpuSignatureFrameRunView& )>;

enum class CpuLogicalSignatureMode : uint8_t
{
    FullPath,
    StableSite
};

struct CpuFrameScanOptions
{
    std::function<bool()> cancelled;
    bool collectTiming = false;
    bool useFrameCursor = true;
    bool includeExactSignatures = true;
    bool includeLogicalSignatures = true;
    CpuLogicalSignatureMode logicalSignatureMode = CpuLogicalSignatureMode::FullPath;
    // Policy scans may collapse per-occurrence extra names (for example GPU
    // resource tags) onto their static function/site while retaining the full
    // parent path. Raw Zone DTOs and names remain available from TraceSource.
    bool canonicalizeLogicalDynamicNames = false;
    bool includeLogicalThreadRole = false;
    std::function<void( const std::string&, uint64_t, int64_t, int64_t )> completeFrameSink;
    // When supplied, emit each exact/logical definition once before its first
    // frame run, and do not retain full definitions in the result. The view is
    // valid only during the callback; false cancels the scan explicitly.
    std::function<bool( const CpuSignatureDefinition& )> definitionSink;
    // Throttling belongs to the caller. The scanner reports once per bounded
    // source page and once after closing the final thread stacks.
    std::function<void( uint64_t inputZones, uint64_t emittedRuns )> progress;
    // Receives a cumulative batch-level snapshot before progress. A bounded
    // prefix probe can retain timings even when the following page cancels.
    std::function<void( const CpuFrameScanTimings& )> timingProgress;
    // Shared analysis-owned metadata, signatures, active stacks and returned
    // result storage. Source pages are charged once returned; constructing a
    // native source DTO is not protected by an absolute allocator limit.
    std::shared_ptr<AnalysisWorkspaceBudget> workspace;
    // When non-zero, a scan without logical signatures of a source that offers
    // compact zone records (CpuZoneScanSource) processes them on this many
    // threads and replays their effects in scan order: sinks receive the same
    // calls in the same order, and the result is the same.
    size_t zoneWorkers = 0;
    // A worker's unit of work: at least this many zones of one thread, ending
    // before a zone without a parent (or at the thread's end).
    size_t zoneChunk = 32768;
    // When false, ScanView passes no representative event ref (an empty view)
    // to its sink, which then need not be formatted.
    bool runRefs = true;
    // A frame-window evidence sink needs the native runs and quality audit,
    // but not retained per-signature whole-capture denominator strings.
    bool retainDenominators = true;
};

class ExactFrameCpuScanner
{
public:
    explicit ExactFrameCpuScanner( const TraceSource& source, size_t batchSize = 4096 );
    CpuFrameScanResult Scan( const CpuSignatureFrameSink& sink = {},
        const CpuFrameScanOptions& options = {} ) const;
    CpuFrameScanResult ScanView( const CpuSignatureFrameViewSink& sink,
        const CpuFrameScanOptions& options = {} ) const;

private:
    CpuFrameScanResult ScanInternal( const CpuSignatureFrameSink* sink,
        const CpuSignatureFrameViewSink* viewSink,
        const CpuFrameScanOptions& options ) const;
    const TraceSource& m_source;
    size_t m_batchSize;
};

const char* CpuWorkClassName( CpuWorkClass value );

}

#endif
