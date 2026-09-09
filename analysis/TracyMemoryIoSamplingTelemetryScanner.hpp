#ifndef __TRACYMEMORYIOSAMPLINGTELEMETRYSCANNER_HPP__
#define __TRACYMEMORYIOSAMPLINGTELEMETRYSCANNER_HPP__

#include "TracyBoundedScanCursor.hpp"
#include "TracyAnalysisWorkspaceBudget.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace tracy::analysis
{

enum class CpuMemoryClassification : uint8_t
{
    Capacity,
    Growth,
    Churn,
    LeakCandidate,
    Transient,
    AccountingGap
};

enum class GpuCatalogState : uint8_t
{
    Complete,
    Invalid,
    Disabled,
    Absent
};

enum class IoLifecycleState : uint8_t
{
    Complete,
    Cancelled,
    Error,
    Orphan,
    Incomplete
};

enum class TelemetryEvidenceClass : uint8_t
{
    MeasuredSelfCost,
    ObservedSystemPressure,
    EstimatedAttribution,
    RequiresABValidation
};

enum class TotalCaptureOverheadState : uint8_t
{
    NotMeasuredSingleTrace
};

struct CpuMemoryPoolFact
{
    std::string poolRef;
    std::string name;
    uint64_t eventCount = 0;
    uint64_t freeCount = 0;
    uint64_t openBoundaryCount = 0;
    uint64_t aliveAtEndCount = 0;
    uint64_t addressReuseCount = 0;
    uint64_t accountingGapCount = 0;
    uint64_t transientCount = 0;
    uint64_t leakCandidateCount = 0;
    uint64_t totalAllocatedBytes = 0;
    uint64_t totalFreedBytes = 0;
    uint64_t peakLiveBytes = 0;
    uint64_t endLiveBytes = 0;
    bool hasCapacity = false;
    bool hasGrowth = false;
    bool hasChurn = false;
    bool hasLeakCandidate = false;
    bool hasTransient = false;
    bool hasAccountingGap = false;
    std::vector<CpuMemoryClassification> classifications;
};

struct GpuMemoryFact
{
    GpuCatalogState catalogState = GpuCatalogState::Absent;
    std::string catalogReason;
    bool physicalFactsAvailable = false;
    bool resourceFactsAvailable = false;
    uint64_t allocationCount = 0;
    uint64_t resourceCount = 0;
    uint64_t passCount = 0;
    uint64_t rangeCount = 0;
    uint64_t sharedAllocationCount = 0;
    uint64_t aliasResourceCount = 0;
    uint64_t invalidAllocationCount = 0;
    uint64_t invalidResourceCount = 0;
    uint64_t physicalBytes = 0;
    // Concurrent live root-allocation peak, never sum of historical creates.
    bool physicalPeakExact = true;
    uint64_t physicalPeakTimeNs = 0;
    uint64_t residentBytes = 0;
    uint64_t ownedPhysicalBytes = 0;
    uint64_t logicalCapacityBytes = 0;
    uint64_t rangeEvidenceBytes = 0;
    uint64_t maximumDirectWorkingSetBytes = 0;
    uint64_t maximumInclusiveWorkingSetBytes = 0;
    std::optional<uint64_t> dxgiUsageBytes;
    std::optional<uint64_t> untrackedBytes;
};

struct IoRequestFact
{
    std::string ref;
    uint64_t requestId = 0;
    IoLifecycleState state = IoLifecycleState::Incomplete;
    std::optional<int64_t> queueLatencyNs;
    std::optional<int64_t> executionNs;
    std::optional<int64_t> totalNs;
    uint64_t requestedBytes = 0;
    uint64_t transferredBytes = 0;
    uint32_t stageCount = 0;
    bool exact = true;
};

struct IoSummaryFact
{
    uint64_t completeCount = 0;
    uint64_t cancelledCount = 0;
    uint64_t errorCount = 0;
    uint64_t orphanCount = 0;
    uint64_t incompleteCount = 0;
    uint64_t requestedBytes = 0;
    uint64_t transferredBytes = 0;
};

struct SamplingLeafFact
{
    std::string threadRole;
    std::string sampleKind;
    uint32_t callstack = 0;
    std::string leafFunction;
    std::string leafFile;
    uint32_t leafLine = 0;
    uint64_t sampleCount = 0;
    bool resolved = false;
};

struct SamplingSummaryFact
{
    bool available = false;
    std::string unavailableReason;
    uint64_t totalSamples = 0;
    uint64_t unresolvedSamples = 0;
    std::vector<SamplingLeafFact> leaves;
};

struct SchedulingRoleFact
{
    std::string threadRole;
    uint64_t intervalCount = 0;
    uint64_t incompleteCount = 0;
    uint64_t waitCount = 0;
    uint64_t preemptCount = 0;
    int64_t runningNs = 0;
    int64_t readyWaitNs = 0;
};

struct SchedulingSummaryFact
{
    bool available = false;
    std::string unavailableReason;
    std::vector<SchedulingRoleFact> roles;
};

struct TelemetryProducerFact
{
    uint64_t producerId = 0;
    std::string key;
    std::string sourceMode;
    std::string state;
    uint64_t observed = 0;
    uint64_t eventCount = 0;
    uint64_t eventBytes = 0;
    uint64_t cpuTimeNs = 0;
    uint64_t dropped = 0;
    uint64_t filtered = 0;
    uint64_t sampledOut = 0;
    uint64_t overflow = 0;
    uint64_t mismatch = 0;
    uint64_t unresolved = 0;
    uint64_t tailTruncated = 0;
    uint64_t degrade = 0;
    bool complete = true;
};

struct TelemetryCostFact
{
    bool producerQualityPresent = false;
    bool producerQualityComplete = false;
    uint64_t totalCpuTimeNs = 0;
    uint64_t totalEventCount = 0;
    uint64_t totalEventBytes = 0;
    uint64_t totalDropped = 0;
    uint64_t totalOverflow = 0;
    uint64_t frameImageCount = 0;
    TotalCaptureOverheadState totalCaptureOverhead = TotalCaptureOverheadState::NotMeasuredSingleTrace;
    bool requiresAbValidation = true;
    std::string workerCostUnavailableReason = "tracy_worker_cpu_cost_not_persisted_in_single_trace";
    std::string frameImageCostUnavailableReason = "frame_image_cpu_gpu_cost_not_persisted_in_single_trace";
    std::string diskWriteUnavailableReason = "capture_write_cost_not_persisted_in_single_trace";
    std::vector<TelemetryEvidenceClass> evidenceClasses;
    std::vector<TelemetryProducerFact> producers;
};

struct MemoryIoSamplingQualityFinding
{
    std::string code;
    std::string message;
    uint64_t count = 0;
    std::vector<std::string> representativeRefs;
};

struct MemoryIoSamplingTelemetryScanResult
{
    // Declared first so the lease outlives all returned allocations.
    AnalysisWorkspaceReservation outputWorkspace;
    MemoryIoSamplingTelemetryScanResult() = default;
    MemoryIoSamplingTelemetryScanResult( MemoryIoSamplingTelemetryScanResult&& ) = default;
    MemoryIoSamplingTelemetryScanResult& operator=( MemoryIoSamplingTelemetryScanResult&& ) = delete;
    size_t maximumBatchObserved = 0;
    bool qualityComplete = true;
    // Exact source records consumed by each bounded scan.  Keep these counts
    // separate from the compact facts below: pools, unique allocations,
    // resource summaries and scheduling roles are outputs, not input records.
    uint64_t inputMemoryEventCount = 0;
    uint64_t inputGpuAllocationCount = 0;
    uint64_t inputGpuResourceCount = 0;
    uint64_t inputGpuPassCount = 0;
    uint64_t inputGpuRangeCount = 0;
    uint64_t inputIoRequestCount = 0;
    uint64_t inputSampleCount = 0;
    uint64_t inputContextSwitchCount = 0;
    uint64_t inputTelemetryRecordCount = 0;
    std::vector<CpuMemoryPoolFact> cpuMemoryPools;
    GpuMemoryFact gpuMemory;
    std::vector<IoRequestFact> ioRequests;
    IoSummaryFact ioSummary;
    SamplingSummaryFact sampling;
    SchedulingSummaryFact scheduling;
    TelemetryCostFact telemetry;
    std::vector<MemoryIoSamplingQualityFinding> qualityFindings;
};

class MemoryIoSamplingTelemetryScanner
{
public:
    explicit MemoryIoSamplingTelemetryScanner( const TraceSource& source,
        size_t batchSize = 4096, int64_t transientLifetimeNs = 1'000'000,
        int64_t leakCandidateAgeNs = 10'000'000'000 );
    MemoryIoSamplingTelemetryScanResult Scan( const std::function<bool()>& cancelled = {} ) const;
    // Same source audit and capacity/quality facts; omit IO, sample-leaf and
    // scheduling detail vectors unused by the default Query analysis path.
    MemoryIoSamplingTelemetryScanResult ScanSummary( const std::function<bool()>& cancelled,
        std::shared_ptr<AnalysisWorkspaceBudget> workspace ) const;

private:
    MemoryIoSamplingTelemetryScanResult ScanImpl( const std::function<bool()>& cancelled,
        bool retainDetails, std::shared_ptr<AnalysisWorkspaceBudget> workspace ) const;
    const TraceSource& m_source;
    size_t m_batchSize;
    int64_t m_transientLifetimeNs;
    int64_t m_leakCandidateAgeNs;
};

const char* CpuMemoryClassificationName( CpuMemoryClassification value );
const char* GpuCatalogStateName( GpuCatalogState value );
const char* IoLifecycleStateName( IoLifecycleState value );
const char* TelemetryEvidenceClassName( TelemetryEvidenceClass value );

}

#endif
