#ifndef __TRACYFRAMECPUSCANNER_HPP__
#define __TRACYFRAMECPUSCANNER_HPP__

#include "TracyBoundedScanCursor.hpp"

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

struct CpuFrameScanResult
{
    uint64_t inputZoneCount = 0;
    uint64_t validZoneCount = 0;
    uint64_t invalidZoneCount = 0;
    uint64_t unassignedZoneCount = 0;
    uint64_t completeFrameCount = 0;
    uint64_t incompleteFrameCount = 0;
    size_t maximumDepth = 0;
    size_t maximumBatchObserved = 0;
    uint64_t frameBoundaryProbeCount = 0;
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
    bool includeExactSignatures = true;
    bool includeLogicalSignatures = true;
    CpuLogicalSignatureMode logicalSignatureMode = CpuLogicalSignatureMode::FullPath;
    std::function<void( const std::string&, uint64_t, int64_t, int64_t )> completeFrameSink;
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
