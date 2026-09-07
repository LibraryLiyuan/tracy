#ifndef __TRACYGPUJOBMANAGEDSCANNER_HPP__
#define __TRACYGPUJOBMANAGEDSCANNER_HPP__

#include "TracyBoundedScanCursor.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace tracy::analysis
{

enum class GpuQueueClass : uint8_t
{
    Direct,
    Compute,
    Copy,
    Unknown
};

enum class GpuFrameEvidence : uint8_t
{
    ExactFrameRelation,
    InferredFromChildPasses,
    TemporalCandidate,
    Unassigned
};

enum class ManagedProducerKind : uint8_t
{
    DirectSourceStack,
    UnityProfilerMarker
};

enum class GpuAnalysisTrack : uint8_t
{
    PhysicalQueueA,
    LogicalFrameB
};

struct GpuSignatureDefinition
{
    std::string signatureId;
    std::string parentSignatureId;
    std::string name;
    std::string sourceLocationRef;
    std::string function;
    std::string file;
    uint32_t line = 0;
    std::string contextRef;
    GpuQueueClass queueClass = GpuQueueClass::Unknown;
    std::string path;
    uint32_t depth = 0;
    bool logical = false;
};

struct GpuZoneScanFact
{
    std::string zoneRef;
    std::string signatureId;
    std::string logicalSignatureId;
    std::string parentZoneRef;
    std::string contextRef;
    GpuQueueClass queueClass = GpuQueueClass::Unknown;
    int64_t beginNs = 0;
    int64_t endNs = 0;
    int64_t inclusiveNs = 0;
    int64_t directChildUnionNs = 0;
    int64_t exclusiveNs = 0;
    uint32_t depth = 0;
    GpuFrameEvidence frameEvidence = GpuFrameEvidence::Unassigned;
    std::optional<uint64_t> frameId;
    std::optional<std::string> temporalFrameRef;
    bool exact = true;
    bool physicalTimingExact = true;
    uint64_t l0SegmentOrdinal = 0;
};

struct GpuTrackFact
{
    GpuAnalysisTrack track = GpuAnalysisTrack::PhysicalQueueA;
    std::string zoneRef;
    std::string signatureId;
    std::string contextRef;
    GpuQueueClass queueClass = GpuQueueClass::Unknown;
    int64_t inclusiveNs = 0;
    int64_t exclusiveNs = 0;
    GpuFrameEvidence frameEvidence = GpuFrameEvidence::Unassigned;
    std::optional<uint64_t> frameId;
    std::optional<std::string> temporalFrameRef;
    bool exact = true;
};

struct JobLifecycleFact
{
    std::string ref;
    uint64_t jobId = 0;
    uint64_t packedHandle = 0;
    uint32_t handleSlot = 0;
    uint32_t handleGeneration = 0;
    std::string name;
    uint32_t typeId = 0;
    int64_t scheduleNs = 0;
    std::optional<int64_t> readyNs;
    std::optional<int64_t> queueEnterNs;
    std::optional<int64_t> firstRunNs;
    std::optional<int64_t> completedNs;
    int64_t executionNs = 0;
    int64_t waitNs = 0;
    int64_t waitActiveHelpNs = 0;
    int64_t waitSpinYieldNs = 0;
    int64_t waitSleepNs = 0;
    uint64_t originFrameId = 0;
    uint32_t stageCount = 0;
    bool hasWaiter = false;
    bool hasContinuation = false;
    bool scheduleStackAvailable = false;
    uint32_t scheduleCallsiteId = 0;
    uint32_t scheduleCallstack = 0;
    std::string stackProvenance;
    std::string stackUnavailableReason;
    bool exact = true;
    std::vector<uint64_t> dependencyJobIds;
};

struct ManagedSourceStackFact
{
    uint64_t stackId = 0;
    uint8_t runtime = 0;
    uint32_t expectedDepth = 0;
    std::string marker;
    bool complete = false;
    std::string unavailableReason;
    std::vector<ScriptFrameDto> frames;
};

struct ManagedZoneFact
{
    uint64_t zoneId = 0;
    uint64_t stackId = 0;
    uint8_t runtime = 0;
    std::string threadRef;
    int64_t beginNs = 0;
    std::optional<int64_t> endNs;
    std::string marker;
    ManagedProducerKind producer = ManagedProducerKind::DirectSourceStack;
    bool stackAvailable = false;
    std::string stackProvenance;
    std::string stackUnavailableReason;
};

struct TypedRelationFact
{
    std::string ref;
    uint64_t sourceId = 0;
    uint64_t targetId = 0;
    int64_t timeNs = 0;
    uint8_t sourceKind = 0;
    uint8_t targetKind = 0;
    uint8_t relationNamespace = 0;
    uint8_t relation = 0;
    bool exact = true;
    bool ambiguous = false;
};

struct CrossDomainQualityFinding
{
    std::string code;
    std::string message;
    uint64_t count = 0;
    std::vector<std::string> representativeRefs;
};

struct GpuJobManagedScanResult
{
    size_t maximumBatchObserved = 0;
    size_t maximumGpuDepth = 0;
    uint64_t physicalL0SegmentCount = 0;
    uint64_t invalidGpuZoneCount = 0;
    uint64_t temporalGpuCandidateCount = 0;
    uint64_t unassignedGpuZoneCount = 0;
    uint64_t handleReuseCount = 0;
    uint64_t ambiguousRelationCount = 0;
    uint64_t gpuZoneCount = 0;
    uint64_t jobCount = 0;
    uint64_t managedZoneCount = 0;
    uint64_t relationCount = 0;
    bool qualityComplete = true;
    std::vector<GpuSignatureDefinition> gpuSignatures;
    std::vector<GpuZoneScanFact> gpuZones;
    std::vector<GpuTrackFact> gpuTracks;
    std::vector<JobLifecycleFact> jobs;
    std::vector<ManagedSourceStackFact> managedStacks;
    std::vector<ManagedZoneFact> managedZones;
    std::vector<TypedRelationFact> relations;
    std::vector<CrossDomainQualityFinding> qualityFindings;
};

enum class GpuLogicalSignatureMode : uint8_t
{
    FullPath,
    StableSite
};

struct GpuJobManagedScanOptions
{
    bool retainDetails = true;
    bool includeExactGpuSignatures = true;
    bool includeLogicalGpuSignatures = true;
    GpuLogicalSignatureMode logicalSignatureMode = GpuLogicalSignatureMode::FullPath;
    std::function<bool( const GpuZoneScanFact& )> gpuZoneSink;
    std::function<bool( const JobLifecycleFact& )> jobSink;
    std::function<bool( const ManagedZoneFact& )> managedZoneSink;
    std::function<bool( const TypedRelationFact& )> relationSink;
};

class GpuJobManagedScanner
{
public:
    explicit GpuJobManagedScanner( const TraceSource& source, size_t batchSize = 4096 );
    GpuJobManagedScanResult Scan( const GpuJobManagedScanOptions& options = {} ) const;

private:
    const TraceSource& m_source;
    size_t m_batchSize;
};

const char* GpuQueueClassName( GpuQueueClass value );
const char* GpuFrameEvidenceName( GpuFrameEvidence value );

}

#endif
