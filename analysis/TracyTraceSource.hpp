#ifndef __TRACYTRACESOURCE_HPP__
#define __TRACYTRACESOURCE_HPP__

#include "TracyMemoryAnalysis.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tracy::analysis
{

enum class TraceSourceKind : uint8_t
{
    Snapshot,
    Segment
};

enum class TraceSourceState : uint8_t
{
    Queued,
    Loading,
    Indexing,
    Ready,
    Failed,
    Closing,
    Closed
};

struct TraceReadView
{
    TraceSourceKind sourceKind = TraceSourceKind::Snapshot;
    TraceSourceState state = TraceSourceState::Ready;
    uint64_t revision = 0;
    int64_t watermarkNs = 0;
    bool complete = true;
};

struct ScanRange
{
    int64_t startNs = std::numeric_limits<int64_t>::min();
    int64_t endNs = std::numeric_limits<int64_t>::max();
    size_t offset = 0;
    size_t limit = 100;
};

struct Capability
{
    std::string domain;
    bool present = false;
    bool queryable = false;
    bool indexed = false;
    std::string reason;
    std::vector<std::string> methods;
};

struct FieldAvailabilityDto
{
    bool available = true;
    std::string reason;
};

struct TraceCountsDto
{
    uint64_t frames = 0;
    uint64_t frameSets = 0;
    uint64_t cpuZones = 0;
    uint64_t gpuZones = 0;
    uint64_t threads = 0;
    uint64_t locks = 0;
    uint64_t plots = 0;
    uint64_t messages = 0;
    uint64_t memoryEvents = 0;
    uint64_t memoryPools = 0;
    uint64_t contextSwitches = 0;
    uint64_t callstackPayloads = 0;
    uint64_t parentCallstackPayloads = 0;
    uint64_t callstackFrames = 0;
    uint64_t parentCallstackFrames = 0;
    uint64_t samples = 0;
    uint64_t contextSwitchSamples = 0;
    uint64_t kernelSamples = 0;
    uint64_t ghostZones = 0;
    uint64_t childSampleSymbols = 0;
    uint64_t childSamples = 0;
    uint64_t hardwareSamples = 0;
    uint64_t symbols = 0;
    uint64_t symbolCodeBytes = 0;
    uint64_t sourceLocations = 0;
    uint64_t sourceCacheFiles = 0;
    uint64_t sourceCacheBytes = 0;
    uint64_t frameImages = 0;
    uint64_t jobTypes = 0;
    uint64_t jobs = 0;
    uint64_t jobDependencies = 0;
    uint64_t jobStages = 0;
    uint64_t gfxDispatches = 0;
    uint64_t gfxEntities = 0;
    uint64_t gfxLinks = 0;
    uint64_t correlatedFrameEvents = 0;
};

struct TraceInfoDto
{
    std::string fingerprint;
    std::string captureName;
    std::string captureProgram;
    std::string hostInfo;
    uint64_t captureTime = 0;
    uint64_t executableTime = 0;
    uint64_t processId = 0;
    int traceVersion = 0;
    int64_t resolution = 0;
    int64_t firstTimeNs = 0;
    int64_t lastTimeNs = 0;
    int64_t loadTimeNs = 0;
    uint32_t cpuId = 0;
    std::string cpuManufacturer;
    std::string cpuArchitecture;
    bool hasCrash = false;
    bool samplesInconsistent = false;
    TraceCountsDto counts;
    std::vector<std::string> appInfo;
    double timerMultiplier = 1;
    uint64_t frameOffset = 0;
    int64_t samplingPeriodNs = 0;
    bool onDemand = false;
    std::optional<int64_t> legacyQueueDelayNs;
    FieldAvailabilityDto legacyQueueDelayAvailability;
};

struct ThreadDto
{
    std::string ref;
    uint64_t nativeId = 0;
    uint64_t processId = 0;
    std::string name;
    bool fiber = false;
    uint64_t zoneCount = 0;
    uint64_t messageCount = 0;
    uint64_t sampleCount = 0;
    uint64_t contextSwitchCount = 0;
    int64_t runningTimeNs = 0;
    uint32_t migrations = 0;
    std::optional<std::string> externalProcessName;
    std::optional<std::string> externalThreadName;
    std::optional<uint64_t> kernelSampleCount;
    std::optional<int32_t> groupHint;
    std::optional<uint32_t> runningRegions;
    std::optional<std::string> localName;
    FieldAvailabilityDto groupHintAvailability;
};

struct FrameSetDto
{
    std::string ref;
    size_t index = 0;
    std::string name;
    bool continuous = false;
    size_t frameCount = 0;
    size_t completeFrameCount = 0;
};

struct GpuContextDto
{
    std::string ref;
    size_t index = 0;
    std::string name;
    std::string threadRef;
    uint64_t zoneCount = 0;
    double period = 0;
    bool calibrated = false;
    uint8_t type = 0;
    std::string typeName;
    uint64_t overflow = 0;

    struct NoteName
    {
        int64_t timeNs = 0;
        std::string name;
    };

    struct Note
    {
        uint16_t queryId = 0;
        int64_t timeNs = 0;
        double value = 0;
    };

    std::vector<NoteName> noteNames;
    std::vector<Note> notes;
    std::optional<std::string> customName;
    FieldAvailabilityDto notesAvailability;
};

struct MemoryPoolDto
{
    std::string ref;
    uint64_t nativeNameId = 0;
    std::string name;
    uint64_t eventCount = 0;
    uint64_t activeCount = 0;
    uint64_t activeBytes = 0;
    uint64_t low = 0;
    uint64_t high = 0;
    bool gpuD3D12 = false;
    uint64_t freeCount = 0;
    uint64_t persistedUsageBytes = 0;
    uint64_t storedNameId = 0;
    std::string storedName;
};

struct PlotDto
{
    std::string ref;
    size_t index = 0;
    std::string name;
    uint8_t type = 0;
    uint8_t format = 0;
    uint64_t pointCount = 0;
    double min = 0;
    double max = 0;
    double sum = 0;
    bool showSteps = false;
    uint8_t fill = 0;
    uint32_t color = 0;
};

struct LockDto
{
    std::string ref;
    uint32_t nativeId = 0;
    std::string name;
    std::string sourceLocationRef;
    uint64_t eventCount = 0;
    uint64_t threadCount = 0;
    bool valid = false;
    bool contended = false;
    int64_t announceNs = 0;
    std::optional<int64_t> terminateNs;
    uint8_t type = 0;
    std::string typeName;
    std::optional<std::string> customName;
};

struct SourceLocationDto
{
    std::string ref;
    std::string name;
    std::string function;
    std::string file;
    uint32_t line = 0;
    uint32_t color = 0;
    int32_t nativeId = 0;
    bool dynamic = false;
};

struct FrameDto
{
    std::string ref;
    std::string frameSetRef;
    size_t index = 0;
    int64_t beginNs = 0;
    std::optional<int64_t> endNs;
    std::optional<std::string> imageRef;
    bool complete = true;
};

struct CpuZoneDto
{
    std::string ref;
    std::string threadRef;
    std::string sourceLocationRef;
    std::string name;
    std::string function;
    std::string file;
    uint32_t line = 0;
    std::optional<std::string> parentRef;
    int64_t startNs = 0;
    std::optional<int64_t> endNs;
    uint32_t callstack = 0;
    std::optional<std::string> callstackRef;
    uint32_t childCount = 0;
    std::optional<int64_t> selfTimeNs;
    std::optional<int64_t> runningTimeNs;
    uint64_t runningRegions = 0;
    bool complete = true;
    bool nameResolved = true;
    uint32_t extraIndex = 0;
    bool extraValid = true;
    std::optional<std::string> extraName;
    std::optional<std::string> extraText;
    uint32_t extraColor = 0;
};

struct GpuZoneDto
{
    std::string ref;
    std::string contextRef;
    std::string threadRef;
    std::string sourceLocationRef;
    std::string name;
    std::string function;
    std::string file;
    uint32_t line = 0;
    std::optional<std::string> parentRef;
    int64_t gpuStartNs = 0;
    std::optional<int64_t> gpuEndNs;
    int64_t cpuStartNs = 0;
    std::optional<int64_t> cpuEndNs;
    uint32_t callstack = 0;
    std::optional<std::string> callstackRef;
    uint32_t childCount = 0;
    std::optional<int64_t> selfTimeNs;
    bool complete = true;
    uint16_t queryId = 0;
    FieldAvailabilityDto queryIdAvailability;
};

struct CrashDto
{
    bool present = false;
    std::string threadRef;
    std::optional<int64_t> timeNs;
    std::string message;
    uint32_t callstack = 0;
    std::optional<std::string> callstackRef;
};

struct CpuTopologyDto
{
    uint32_t cpu = 0;
    uint32_t package = 0;
    uint32_t die = 0;
    uint32_t core = 0;
    FieldAvailabilityDto dieAvailability;
};

struct CpuUsagePointDto
{
    std::string ref;
    int64_t timeNs = 0;
    uint8_t own = 0;
    uint8_t other = 0;
};

struct ContextSwitchDto
{
    std::string ref;
    std::string threadRef;
    int64_t startNs = 0;
    std::optional<int64_t> endNs;
    std::optional<int64_t> wakeupNs;
    uint8_t cpu = 0;
    uint8_t wakeupCpu = 0;
    int8_t reason = 0;
    int8_t state = 0;
    bool complete = true;
    std::string reasonName;
    std::string stateName;
    uint16_t relatedThreadIndex = 0;
    std::optional<std::string> relatedThreadRef;
    FieldAvailabilityDto wakeupCpuAvailability;
};

struct CpuContextSwitchDto
{
    std::string ref;
    uint32_t cpu = 0;
    int64_t startNs = 0;
    std::optional<int64_t> endNs;
    uint16_t rawThreadIndex = 0;
    std::string threadRef;
    bool complete = true;
};

struct SampleDto
{
    std::string ref;
    std::string threadRef;
    int64_t timeNs = 0;
    uint32_t callstack = 0;
    std::optional<std::string> callstackRef;
    std::string kind = "sample";
};

struct GhostZoneDto
{
    std::string ref;
    std::string threadRef;
    std::optional<std::string> parentRef;
    int64_t startNs = 0;
    int64_t endNs = 0;
    std::string name;
    std::string file;
    uint32_t line = 0;
    std::string address;
    uint32_t depth = 0;
    uint32_t childCount = 0;
    bool inlineFrame = false;
};

struct HardwareSampleDto
{
    std::string ref;
    std::string address;
    uint64_t cycles = 0;
    uint64_t retired = 0;
    uint64_t cacheReferences = 0;
    uint64_t cacheMisses = 0;
    uint64_t branchRetired = 0;
    uint64_t branchMisses = 0;
};

struct HardwareSampleEventDto
{
    std::string ref;
    std::string address;
    std::string kind;
    uint64_t eventIndex = 0;
    int64_t timeNs = 0;
};

struct LockEventDto
{
    std::string ref;
    std::string lockRef;
    int64_t timeNs = 0;
    std::string threadRef;
    std::string type;
    std::optional<std::string> ownerThreadRef;
    uint32_t lockCount = 0;
    std::vector<std::string> waiterThreadRefs;
    std::string sourceLocationRef;
};

struct SymbolDto
{
    std::string ref;
    std::string address;
    std::string name;
    std::string file;
    uint32_t line = 0;
    uint64_t size = 0;
    uint32_t inclusiveSamples = 0;
    uint32_t exclusiveSamples = 0;
    uint64_t childSamples = 0;
    bool hasCode = false;
    std::optional<std::string> imageName;
    std::optional<std::string> callFile;
    uint32_t callLine = 0;
    bool inlineFrame = false;
};

struct SymbolAddressMappingDto
{
    std::string ref;
    std::string address;
    std::string symbolRef;
    std::string symbolAddress;
    uint32_t offset = 0;
    bool inlineMapping = false;
};

struct MemoryEventDto
{
    std::string ref;
    std::string poolRef;
    std::string address;
    uint64_t size = 0;
    int64_t allocationNs = 0;
    std::optional<int64_t> freeNs;
    std::string allocationThreadRef;
    std::optional<std::string> freeThreadRef;
    uint32_t allocationCallstack = 0;
    uint32_t freeCallstack = 0;
    std::optional<std::string> allocationCallstackRef;
    std::optional<std::string> freeCallstackRef;
    std::optional<std::string> allocationZoneRef;
    std::optional<std::string> freeZoneRef;
    bool complete = true;
};

struct MessageDto
{
    std::string ref;
    std::string threadRef;
    int64_t timeNs = 0;
    std::string text;
    uint32_t color = 0;
    uint32_t callstack = 0;
    std::optional<std::string> callstackRef;
};

struct PlotPointDto
{
    std::string ref;
    std::string plotRef;
    int64_t timeNs = 0;
    double value = 0;
};

struct JobDependencyDto
{
    uint64_t prerequisiteJobId = 0;
    uint64_t prerequisiteHandle = 0;
    uint8_t flags = 0;
};

struct JobStageDto
{
    int64_t timeNs = 0;
    std::string threadRef;
    uint32_t spanId = 0;
    uint32_t arg0 = 0;
    uint32_t arg1 = 0;
    uint8_t stage = 0;
    uint8_t flags = 0;
};

struct JobDto
{
    std::string ref;
    uint64_t jobId = 0;
    uint64_t packedHandle = 0;
    std::string name;
    uint32_t typeId = 0;
    uint8_t kind = 0;
    uint8_t flags = 0;
    int64_t scheduleNs = 0;
    std::string scheduleThreadRef;
    uint32_t count = 0;
    uint32_t grainSize = 0;
    uint32_t unityFlowId = 0;
    uint32_t originFrameSequence = 0;
    uint64_t originFrameId = 0;
    uint32_t scheduleCallstack = 0;
    uint16_t expectedDependencyCount = 0;
    std::optional<int64_t> firstRunNs;
    std::optional<int64_t> completedNs;
    int64_t executionNs = 0;
    int64_t waitActiveHelpNs = 0;
    int64_t waitSpinYieldNs = 0;
    int64_t waitSleepNs = 0;
    bool cancelled = false;
    bool incomplete = false;
    bool orphan = false;
    bool truncated = false;
    std::vector<JobDependencyDto> dependencies;
    std::vector<JobStageDto> stages;
};

struct GfxDispatchDto
{
    std::string ref;
    uint64_t dispatchId = 0;
    uint64_t frameIndex = 0;
    int64_t timeNs = 0;
    std::string threadRef;
    uint32_t expectedJobs = 0;
    uint8_t threadingMode = 0;
    uint8_t flags = 0;
};

struct GfxEntityDto
{
    std::string ref;
    uint64_t entityId = 0;
    uint64_t parentId = 0;
    int64_t timeNs = 0;
    std::string threadRef;
    uint32_t gpuQueryId = 0;
    uint8_t gpuContext = 0;
    uint8_t kind = 0;
    uint8_t flags = 0;
};

struct GfxLinkDto
{
    std::string ref;
    uint64_t sourceId = 0;
    uint64_t targetId = 0;
    int64_t timeNs = 0;
    std::string threadRef;
    uint8_t relation = 0;
    uint8_t flags = 0;
};

struct CorrelatedFrameEventDto
{
    std::string ref;
    uint64_t frameId = 0;
    uint64_t domainIndex = 0;
    int64_t timeNs = 0;
    std::string threadRef;
    uint8_t domain = 0;
    uint8_t phase = 0;
    uint8_t flags = 0;
};

struct CallstackFrameDto
{
    std::string ref;
    std::string name;
    std::string file;
    uint32_t line = 0;
    std::string address;
    std::string symbolAddress;
    bool inlineFrame = false;
    uint32_t callstack = 0;
    size_t depth = 0;
    std::optional<std::string> imageName;
};

struct SourceTextDto
{
    std::string ref;
    std::string path;
    std::string text;
    bool embedded = false;
    bool truncated = false;
};

struct SymbolCodeDto
{
    std::string ref;
    std::string address;
    std::vector<uint8_t> bytes;
    bool truncated = false;
};

struct DisassemblyInstructionDto
{
    std::string ref;
    std::string address;
    std::string bytes;
    std::string mnemonic;
    std::string operands;
    uint32_t size = 0;
};

struct FrameImageDto
{
    std::string ref;
    uint32_t width = 0;
    uint32_t height = 0;
    bool flipped = false;
    std::vector<uint8_t> rgba;
};

struct SourceResourceDto
{
    size_t id = 0;
    std::string ref;
    std::string path;
    uint64_t bytes = 0;
    std::vector<uint8_t> pathBytes;
};

struct SymbolResourceDto
{
    uint64_t id = 0;
    std::string ref;
    std::string name;
    std::string file;
    uint32_t line = 0;
    uint64_t codeBytes = 0;
};

struct FrameImageMetadataDto
{
    size_t id = 0;
    std::string ref;
    uint32_t width = 0;
    uint32_t height = 0;
    bool flipped = false;
    uint32_t rawFrameIndex = 0;
    std::optional<std::string> frameRef;
    uint64_t rawBc1Bytes = 0;
};

struct BinaryResourceChunkDto
{
    std::string ref;
    uint64_t offset = 0;
    uint64_t totalBytes = 0;
    std::vector<uint8_t> bytes;
    bool eof = true;
};

class TraceSource
{
public:
    virtual ~TraceSource() = default;

    virtual std::vector<Capability> GetCapabilities() const = 0;
    virtual TraceReadView AcquireReadView() const = 0;
    virtual TraceInfoDto GetTraceInfo() const = 0;
    virtual std::vector<ThreadDto> GetThreads() const = 0;
    virtual std::vector<FrameSetDto> GetFrameSets() const = 0;
    virtual std::vector<GpuContextDto> GetGpuContexts() const = 0;
    virtual std::vector<MemoryPoolDto> GetMemoryPools() const = 0;
    virtual std::vector<PlotDto> GetPlotList() const = 0;
    virtual std::vector<LockDto> GetLocks() const = 0;

    virtual std::vector<CpuZoneDto> ScanCpuZones( const ScanRange& range ) const = 0;
    virtual std::vector<GpuZoneDto> ScanGpuZones( const ScanRange& range ) const = 0;
    virtual std::vector<FrameDto> ScanFrames( const ScanRange& range ) const = 0;
    virtual std::vector<MemoryEventDto> ScanMemoryEvents( const ScanRange& range ) const = 0;
    virtual std::vector<MessageDto> ScanMessages( const ScanRange& range ) const = 0;
    virtual std::vector<PlotPointDto> ScanPlots( const ScanRange& range ) const = 0;

    virtual std::vector<std::string> ScanLocks( const ScanRange& range ) const = 0;
    virtual std::vector<std::string> ScanContextSwitches( const ScanRange& range ) const = 0;
    virtual std::vector<std::string> ScanSamples( const ScanRange& range ) const = 0;

    virtual std::vector<JobDto> GetJobs() const { return {}; }
    virtual std::vector<GfxDispatchDto> GetGfxDispatches() const { return {}; }
    virtual std::vector<GfxEntityDto> GetGfxEntities() const { return {}; }
    virtual std::vector<GfxLinkDto> GetGfxLinks() const { return {}; }
    virtual std::vector<CorrelatedFrameEventDto> GetCorrelatedFrameEvents() const { return {}; }

    virtual CrashDto GetCrash() const { return {}; }
    virtual std::vector<CpuTopologyDto> GetCpuTopology() const { return {}; }
    virtual std::vector<CpuUsagePointDto> GetCpuUsage() const { return {}; }
    virtual std::vector<ContextSwitchDto> ScanContextSwitchEvents( const ScanRange& ) const { return {}; }
    virtual std::vector<CpuContextSwitchDto> ScanCpuContextSwitchEvents( const ScanRange& ) const { return {}; }
    virtual std::vector<SampleDto> ScanSampleEvents( const ScanRange& ) const { return {}; }
    virtual std::vector<GhostZoneDto> ScanGhostZones( const ScanRange& ) const { return {}; }
    virtual std::vector<HardwareSampleDto> GetHardwareSamples() const { return {}; }
    virtual std::vector<HardwareSampleEventDto> GetHardwareSampleEvents( uint64_t, std::string_view, size_t, size_t ) const { return {}; }
    virtual std::vector<LockEventDto> ScanLockEvents( const ScanRange& ) const { return {}; }
    virtual std::vector<SymbolDto> GetSymbols() const { return {}; }
    virtual std::vector<SymbolAddressMappingDto> GetSymbolAddressMappings( size_t, size_t ) const { return {}; }
    virtual std::optional<SymbolAddressMappingDto> ResolveSymbolAddress( uint64_t ) const { return std::nullopt; }
    virtual std::vector<SourceLocationDto> GetSourceLocations() const { return {}; }

    virtual std::vector<CallstackFrameDto> ResolveCallstacks( const std::vector<uint32_t>& callstacks, size_t maxDepth ) const = 0;
    virtual std::vector<CallstackFrameDto> ResolveParentCallstacks( const std::vector<uint32_t>&, size_t ) const { return {}; }
    virtual std::vector<SourceTextDto> ResolveSources( const std::vector<std::string>& sourceRefs, size_t maxBytes ) const = 0;
    virtual std::vector<SymbolCodeDto> ResolveSymbols( const std::vector<std::string>& symbolRefs, size_t maxBytes ) const = 0;
    virtual std::vector<FrameImageDto> ResolveFrameImages( const std::vector<std::string>& imageRefs, size_t maxBytes ) const = 0;

    // Stable query-oriented batch helpers. Storage adapters implement these
    // without exposing Worker containers, addresses, or GUI types.
    virtual std::vector<FrameDto> GetFramesForSet( size_t frameSetIndex, size_t offset, size_t limit ) const = 0;
    virtual std::vector<int64_t> GetFrameDurations( size_t frameSetIndex ) const = 0;
    virtual std::vector<SourceResourceDto> GetSourceResources() const = 0;
    virtual std::vector<SymbolResourceDto> GetSymbolResources() const = 0;
    virtual std::vector<FrameImageMetadataDto> GetFrameImageResources() const = 0;
    virtual std::optional<CpuZoneDto> GetCpuZone( std::string_view ref ) const = 0;
    virtual std::optional<GpuZoneDto> GetGpuZone( std::string_view ref ) const = 0;
    virtual std::vector<CpuZoneDto> GetCpuZoneChildren( std::string_view ref, size_t offset, size_t limit ) const = 0;
    virtual std::vector<GpuZoneDto> GetGpuZoneChildren( std::string_view ref, size_t offset, size_t limit ) const = 0;
    virtual MemoryFrameSnapshot GetMemoryFrameSnapshot( size_t frameSetIndex, size_t frameIndex, const std::vector<std::string>& poolRefs, bool allGpuD3D12Pools ) const = 0;
    virtual std::optional<MemoryEventDto> GetMemoryEvent( const MemoryEventKey& key ) const = 0;
    virtual std::optional<std::string> GetMemoryPoolRef( uint64_t internalPoolKey ) const = 0;
    virtual std::optional<std::string> GetCpuZoneRef( uint64_t internalZoneIndex ) const = 0;
    virtual std::optional<std::string> GetGpuZoneRef( uint64_t internalZoneIndex ) const = 0;
    virtual std::string MakeEntityRef( std::string_view kind, uint64_t id ) const = 0;
    virtual std::optional<uint64_t> ParseEntityRef( std::string_view ref, std::string_view kind ) const = 0;
    virtual GpuMemoryAttribution GetGpuMemoryAttribution() const = 0;
    virtual SourceTextDto ReadEmbeddedSource( size_t sourceId, size_t maxBytes ) const = 0;
    virtual BinaryResourceChunkDto ReadEmbeddedSourceBytes( size_t sourceId, size_t offset, size_t maxBytes ) const = 0;
    virtual SymbolCodeDto ReadSymbolCode( uint64_t symbolId, size_t maxBytes ) const = 0;
    virtual BinaryResourceChunkDto ReadSymbolCodeBytes( uint64_t symbolId, size_t offset, size_t maxBytes ) const = 0;
    virtual std::vector<DisassemblyInstructionDto> DisassembleSymbol( std::string_view symbolRef, size_t maxBytes, size_t maxInstructions ) const = 0;
    virtual FrameImageDto ReadFrameImage( size_t imageId, size_t maxBytes ) const = 0;
    virtual BinaryResourceChunkDto ReadFrameImageBc1( size_t imageId, size_t offset, size_t maxBytes ) const = 0;
};

const char* ToString( TraceSourceKind value );
const char* ToString( TraceSourceState value );

}

#endif
