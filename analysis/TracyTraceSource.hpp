#ifndef __TRACYTRACESOURCE_HPP__
#define __TRACYTRACESOURCE_HPP__

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
    uint64_t callstackFrames = 0;
    uint64_t samples = 0;
    uint64_t hardwareSamples = 0;
    uint64_t symbols = 0;
    uint64_t symbolCodeBytes = 0;
    uint64_t sourceLocations = 0;
    uint64_t sourceCacheFiles = 0;
    uint64_t sourceCacheBytes = 0;
    uint64_t frameImages = 0;
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
    bool hasCrash = false;
    bool samplesInconsistent = false;
    TraceCountsDto counts;
    std::vector<std::string> appInfo;
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
};

struct SourceLocationDto
{
    std::string ref;
    std::string name;
    std::string function;
    std::string file;
    uint32_t line = 0;
    uint32_t color = 0;
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
    bool complete = true;
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
    bool complete = true;
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
};

struct PlotPointDto
{
    std::string ref;
    std::string plotRef;
    int64_t timeNs = 0;
    double value = 0;
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
    uint32_t frameRef = 0;
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

    virtual std::vector<CallstackFrameDto> ResolveCallstacks( const std::vector<uint32_t>& callstacks, size_t maxDepth ) const = 0;
    virtual std::vector<SourceTextDto> ResolveSources( const std::vector<std::string>& sourceRefs, size_t maxBytes ) const = 0;
    virtual std::vector<SymbolCodeDto> ResolveSymbols( const std::vector<std::string>& symbolRefs, size_t maxBytes ) const = 0;
    virtual std::vector<FrameImageDto> ResolveFrameImages( const std::vector<std::string>& imageRefs, size_t maxBytes ) const = 0;
};

const char* ToString( TraceSourceKind value );
const char* ToString( TraceSourceState value );

}

#endif
