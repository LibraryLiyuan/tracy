#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace tracy::stream
{
inline constexpr uint32_t EventStoreSchema = 17;
struct StoreOptions
{
    uint64_t cacheBytes = 64ull << 20;
    uint64_t resultBytes = 16ull << 20;
    uint64_t diskBytes = 64ull << 30;
    std::function<bool()> cancelled;
    std::function<void( uint64_t, uint64_t )> progress;
    // Reports the start of each long build or open phase by name.
    std::function<void( const char* )> stage;
    bool rebuild = false;
    // Open() hashes the store and its journal. When false, Open() accepts
    // them while each file keeps the size, write time and file id recorded at
    // build time, and VerifyContent() can hash them later.
    bool verifyContent = true;
    // The caller never uses an opened store from two threads at the same time.
    // Allows its SQLite connection to skip per-call locking.
    bool threadConfined = false;
};
struct StoreExportOptions
{
    uint8_t compressionStreams = 4;
    uint64_t fileBytes = 64ull << 30, temporaryBytes = 64ull << 30;
    // Page cache for the export staging tables (4-256 MiB); 0 uses the
    // store's cache budget.
    uint64_t cacheBytes = 0;
    bool overwrite = false;
    // Writes a skeleton snapshot: every domain except CPU and GPU zones, Job
    // schedules, configurations, dependencies and stages, graphics dispatches,
    // entities and links, and relations. Analysis tools load the skeleton for
    // the small domains and read the omitted ones from the store. Threads keep
    // their entries with empty timelines.
    bool skeleton = false;
    std::function<bool()> cancelled;
    std::function<void( const char*, uint64_t )> progress;
};
struct StoreExportResult
{
    uint64_t decodedBytes = 0, fileBytes = 0, revision = 0;
    // SHA256 of the decoded standard snapshot payload, not the compression envelope.
    std::string sha256;
};
struct StoreInfo
{
    uint32_t schema = EventStoreSchema;
    std::string session, sourceSha256, dataSha256, blockSha256, program;
    uint64_t blockBytes = 0;
    uint64_t sourceBytes = 0, validBytes = 0, revision = 0, events = 0, cpuZones = 0, frames = 0;
    uint64_t openZones = 0, unresolvedDefinitions = 0;
    uint64_t gpuZones = 0, incompleteGpuZones = 0;
    uint64_t samples = 0, pendingSamples = 0, contextRegions = 0, cpuRegions = 0;
    uint64_t messages = 0, plotPoints = 0, lockEvents = 0;
    uint64_t jobs = 0, ioRequests = 0, scriptEvents = 0;
    uint64_t allocations = 0, activeAllocations = 0;
    uint64_t gpuReferencePasses = 0, gpuReferenceUses = 0, gpuReferenceEnds = 0;
    uint64_t catalogGenerations = 0, catalogRecords = 0;
    uint64_t frameImages = 0, auxiliaryEvents = 0;
    uint64_t hardwareSamples = 0, hardwareAddresses = 0, symbols = 0, sourceFiles = 0;
    uint64_t fibers = 0;
    // Nodes of the whole-capture CPU call tree collected during ingestion;
    // zoneTree is false when it was not kept (see Analyze()).
    uint64_t zoneTreeNodes = 0;
    bool zoneTree = false;
    bool catalogPresent = false, catalogValid = false;
    int64_t firstNs = 0, lastNs = 0;
    bool cleanEnd = false;
    // True only after independently regenerated deterministic query transcript
    // and ordered handshakes/controls pass; legacy stores report false.
    bool transcriptVerified = false;
};
struct StoreCaptureMetadata
{
    bool available = false;
    double timerMultiplier = 1;
    int64_t initBegin = 0, initEnd = 0, samplingPeriod = 0;
    uint64_t resolution = 0, epoch = 0, executableTime = 0, pid = 0, frameOffset = 0;
    uint32_t cpuId = 0;
    uint8_t flags = 0, architecture = 0;
    std::array<char, 12> cpuManufacturer{};
    std::string program, host;
};
struct StoreCpuTopology
{
    uint32_t package = 0, die = 0, core = 0, thread = 0;
};
struct StoreThreadHint
{
    uint64_t event = 0, thread = 0;
    int32_t hint = 0;
};
struct StoreCrash
{
    uint64_t event = 0, thread = 0, message = 0, stack = 0;
    int64_t time = 0;
};
struct StoreExportPacket
{
    uint64_t event = 0, thread = 0, stack = 0;
    uint8_t type = 0, size = 0;
    std::array<uint8_t, 31> payload{};
};
struct StoreZone
{
    uint64_t id = 0, thread = 0, parent = 0, stack = 0, callsite = 0;
    int64_t start = 0;
    std::optional<int64_t> end;
    // Summed duration of the closed direct children; a closed zone's self
    // time is its duration minus childTime.
    int64_t childTime = 0;
    uint32_t depth = 0, line = 0, color = 0;
    std::optional<uint64_t> value;
    std::string name, function, file, text;
    bool textMore = false;
    uint64_t wireCallsite = 0;
    uint8_t provenance = 3, unavailableReason = 0;
};
struct StoreFiber
{
    uint64_t id = 0, key = 0, regions = 0, nameBytes = 0;
    int64_t running = 0;
    int32_t groupHint = 0;
    std::string name;
    bool nameMore = false, nameResolved = false;
};
struct StoreCallsite
{
    uint64_t id = 0, event = 0, thread = 0, source = 0, stack = 0;
    uint8_t domain = 0, flags = 0, provenance = 3, reason = 0;
};
struct StoreHardwareSample
{
    uint64_t id = 0, address = 0;
    int64_t time = 0;
    uint8_t kind = 0;
};
struct StoreHardwareAddress
{
    uint64_t id = 0, address = 0;
    std::array<uint64_t, 6> counts{};
};
enum class StoreCodeStatus : uint8_t
{
    NotRequested,
    Pending,
    Available,
    Unavailable
};
enum class StoreCodeKind : uint8_t
{
    Source,
    Symbol
};
struct StoreSourceResource
{
    uint64_t id = 0, bytes = 0, pathBytes = 0;
    StoreCodeStatus status = StoreCodeStatus::Pending;
    std::string path;
    bool pathMore = false;
};
struct StoreSymbolResource
{
    uint64_t id = 0, address = 0, size = 0, codeBytes = 0;
    uint32_t line = 0, callLine = 0;
    bool inlineFrame = false, textMore = false;
    StoreCodeStatus status = StoreCodeStatus::NotRequested;
    std::string name, file, image, callFile;
    std::array<uint64_t, 4> textBytes{}; // name, file, image, callFile
};
enum class StoreAuxiliaryKind : uint8_t
{
    Frame,
    GfxDispatch,
    GfxEntity,
    GfxLink,
    Relation,
    RuntimeState
};
struct StoreEntityKey
{
    uint8_t kind = 0; // JnEntityKind; the recorded type is part of the identity.
    uint64_t id = 0;
    bool operator==( const StoreEntityKey& other ) const = default;
};
struct StoreFrameIdentity
{
    uint64_t frame = 0, domainIndex = 0;
    uint8_t domain = 0, phase = 0, flags = 0;
};
struct StoreGfxDispatch
{
    uint64_t dispatch = 0, frame = 0;
    uint32_t expectedJobs = 0;
    uint8_t threadingMode = 0, flags = 0;
};
struct StoreGfxEntity
{
    uint64_t entity = 0, parent = 0;
    uint32_t gpuQuery = 0;
    uint8_t gpuContext = 0, kind = 0, flags = 0;
};
struct StoreGfxLink
{
    uint64_t source = 0, target = 0;
    uint8_t relation = 0, flags = 0;
};
struct StoreRelation
{
    uint64_t source = 0, target = 0;
    uint8_t sourceKind = 0, targetKind = 0, nameSpace = 0, relation = 0, flags = 0;
};
struct StoreRuntimeState
{
    uint64_t generation = 0, requestedFrame = 0;
    uint8_t domain = 0, requestedMode = 0, effectiveMode = 0, reason = 0, flags = 0;
};
struct StoreAuxiliaryRecord
{
    uint64_t id = 0, thread = 0;
    int64_t time = 0;
    StoreAuxiliaryKind kind = StoreAuxiliaryKind::Frame;
    std::variant<StoreFrameIdentity, StoreGfxDispatch, StoreGfxEntity, StoreGfxLink, StoreRelation, StoreRuntimeState> data;
};
std::vector<StoreEntityKey> AuxiliaryEntities( const StoreAuxiliaryRecord& record );
struct StoreFrameImage
{
    uint64_t id = 0, index = 0, frameIndex = 0, bc1Bytes = 0;
    uint16_t width = 0, height = 0;
    bool flipped = false;
    std::optional<uint64_t> frame;
    std::optional<int64_t> start, end;
};
struct StoreBinaryChunk
{
    uint64_t offset = 0, totalBytes = 0;
    std::vector<uint8_t> data;
    bool more = false;
};
struct StoreFrame
{
    uint64_t id = 0, set = 0;
    std::optional<uint64_t> image;
    int64_t start = 0;
    std::optional<int64_t> end;
    bool continuous = true;
};
struct StoreGpuZone
{
    uint64_t id = 0, context = 0, thread = 0, parent = 0, stack = 0, callsite = 0;
    int64_t cpuStart = 0;
    std::optional<int64_t> cpuEnd, gpuStart, gpuEnd;
    uint32_t depth = 0, line = 0, color = 0;
    uint16_t query = 0;
    uint8_t provenance = 3, unavailableReason = 0;
    uint64_t wireCallsite = 0;
    std::string name, function, file;
};
struct StoreMessage
{
    uint64_t id = 0, thread = 0, stack = 0;
    int64_t time = 0;
    uint32_t color = 0xFFFFFFFF;
    bool appInfo = false;
    std::string text;
};
struct StorePlot
{
    uint64_t id = 0, points = 0;
    std::string name;
    double min = 0, max = 0, sum = 0;
    uint8_t format = 0, kind = 0;
    uint32_t color = 0;
    bool steps = false, fill = true;
};
struct StorePlotPoint
{
    uint64_t id = 0, plot = 0;
    int64_t time = 0;
    double value = 0;
};
struct StoreLock
{
    uint64_t id = 0, events = 0, threads = 0;
    int64_t announce = 0;
    std::optional<int64_t> terminate;
    uint32_t line = 0;
    uint8_t type = 0;
    bool contended = false, valid = true;
    std::string name, function, file;
};
struct StoreLockEvent
{
    uint64_t id = 0, lock = 0, thread = 0, owner = 0, waitMask = 0, waitShared = 0, sharedMask = 0;
    int64_t time = 0;
    uint32_t count = 0, line = 0;
    uint8_t type = 0;
    std::string name, function, file;
    std::vector<uint64_t> waiters, sharedWaiters, sharedOwners;
};
struct StoreGpuContext
{
    uint64_t id = 0, sequence = 0, thread = 0, zones = 0, incomplete = 0, overflow = 0;
    double period = 1;
    uint8_t type = 0;
    bool calibrated = false;
    std::string name;
};
struct StoreGpuNote
{
    uint64_t id = 0;
    int64_t note = 0;
    std::string name;
};
struct StoreGpuAnnotation
{
    uint64_t id = 0, valueBits = 0;
    int64_t note = 0;
    uint16_t query = 0;
    std::string name;
};
struct StoreFrameSet
{
    uint64_t id = 0, frames = 0, completeFrames = 0;
    std::string name;
    bool continuous = true;
};
struct StoreSample
{
    uint64_t id = 0, thread = 0, stack = 0;
    int64_t time = 0;
    bool pending = false, contextSwitch = false, explicitContextSwitch = false;
};
struct StoreAllocation
{
    uint64_t id = 0, pool = 0, address = 0, size = 0, allocationThread = 0, freeThread = 0;
    uint64_t allocationStack = 0, freeStack = 0, allocationZone = 0, freeZone = 0, callsite = 0;
    int64_t allocated = 0;
    std::optional<int64_t> freed;
};
// The allocations of a pool live at a time: how many, their bytes, and all
// of them, the largest first, as rows without the zones enclosing their
// events.
struct StoreLiveAllocations
{
    uint64_t count = 0, bytes = 0;
    std::vector<StoreAllocation> largest;
};
struct StoreGpuReferencePass
{
    uint64_t id = 0, pass = 0, frame = 0, thread = 0;
    int64_t time = 0;
    uint32_t taxonomy = 0;
    uint8_t level = 0, flags = 0;
};
struct StoreGpuReference
{
    uint64_t id = 0, packet = 0, pass = 0, resource = 0, thread = 0;
    int64_t time = 0;
    uint32_t set = 0, mask = 0;
    uint16_t ordinal = 0;
    uint8_t flags = 0, encoding = 0;
};
struct StoreGpuReferenceEnd
{
    uint64_t id = 0, pass = 0, commandList = 0, thread = 0;
    int64_t time = 0;
    uint32_t total = 0;
    uint16_t dropped = 0;
    uint8_t flags = 0;
};
struct StoreCatalogGeneration
{
    uint64_t id = 0, event = 0, beginValue = 0, endValue = 0, records = 0, bytes = 0, unresolved = 0;
    int64_t begin = 0, end = 0;
    uint32_t sequence = 0, batches = 0;
    uint8_t state = 0, flags = 0;
    bool ended = false, valid = false;
};
struct StoreCatalogRecord
{
    uint64_t id = 0, generation = 0, object = 0;
    uint32_t sequence = 0, ordinal = 0;
    uint8_t kind = 0;
    std::optional<int64_t> time;
    std::vector<uint8_t> data;
};
struct StoreCatalogBatch
{
    uint64_t id = 0, generation = 0, transportChecksum = 0, storedChecksum = 0, firstRecord = 0;
    uint32_t sequence = 0, records = 0, bytes = 0;
    uint8_t kind = 0, encoding = 0, flags = 0;
    bool valid = true;
};
struct StoreCatalogResource
{
    uint64_t id = 0, generation = 0, resource = 0;
    int64_t begin = 0;
    std::optional<int64_t> end;
    std::string name;
    std::vector<uint8_t> descriptor;
    bool valid = false;
};
struct StoreMemoryPool
{
    uint64_t id = 0, event = 0, allocations = 0, active = 0, activeBytes = 0, frees = 0, low = 0, high = 0;
    std::string name;
};
struct StoreJob
{
    uint64_t id = 0, event = 0, packedHandle = 0, thread = 0, stack = 0, callsite = 0, originFrame = 0;
    int64_t schedule = 0, execution = 0, wait = 0, activeHelp = 0, spin = 0, sleep = 0;
    std::optional<int64_t> ready, queueEnter, firstRun, completed, dependencyLatency;
    uint32_t type = 0, count = 0, grain = 0, flow = 0, originSequence = 0, readyLane = 0, queueLane = 0, dispatches = 0, steals = 0, rangeSteals = 0, activeDispatches = 0, retries = 0, waitEnds = 0, continuations = 0;
    uint16_t expectedDependencies = 0, schema = 1;
    uint8_t kind = 0, flags = 0, readyFlags = 0, provenance = 3, reason = 0;
    bool orphan = true, captureBoundary = false, cancelled = false, incomplete = false;
    std::string name;
};
struct StoreJobStage
{
    uint64_t id = 0, job = 0, thread = 0, stack = 0, callsite = 0;
    int64_t time = 0;
    uint32_t span = 0, arg0 = 0, arg1 = 0;
    uint8_t stage = 0, flags = 0, provenance = 3, reason = 0;
};
struct StoreJobDependency
{
    uint64_t id = 0, job = 0, prerequisite = 0, handle = 0;
    uint8_t flags = 0;
};
struct StoreIoRequest
{
    uint64_t id = 0, event = 0, resource = 0, parent = 0, thread = 0, requested = 0, transferred = 0, stack = 0;
    int64_t queued = 0;
    std::optional<int64_t> start, end;
    uint32_t originSequence = 0, terminals = 0;
    uint8_t operation = 0, source = 0, priority = 0, subsystem = 0, flags = 0, configFlags = 0, parentKind = 0, status = 0;
    bool orphan = true, captureBoundary = false;
};
struct StoreIoStage
{
    uint64_t id = 0, request = 0, thread = 0, bytes = 0, stack = 0;
    int64_t time = 0;
    uint32_t detail = 0;
    uint8_t stage = 0, status = 0, flags = 0;
};
struct StoreScriptFrame
{
    uint64_t id = 0, thread = 0;
    uint32_t frame = 0, line = 0;
    uint8_t runtime = 0, flags = 0;
    std::string function, file;
};
struct StoreScriptEvent
{
    uint64_t id = 0, thread = 0, primary = 0, secondary = 0;
    int64_t time = 0;
    uint32_t value = 0;
    uint8_t runtime = 0, flags = 0, kind = 0;
    std::string text;
};
struct StoreContextRegion
{
    uint64_t id = 0, thread = 0;
    int64_t start = 0;
    std::optional<int64_t> end, wakeup;
    uint8_t cpu = 0, wakeupCpu = 0;
    int8_t reason = -1, state = -1;
    uint64_t relatedThread = 0;
};
struct StoreStackFrame
{
    uint64_t index = 0, address = 0, symbol = 0, symbolLength = 0;
    uint32_t line = 0;
    std::string name, file, image;
    bool inlineFrame = false;
};
// SQLite page cache, then the decoded event blocks. A block miss is one read
// of a block from the block file.
struct StoreCacheStats
{
    uint64_t bytes = 0, hits = 0, misses = 0;
    uint64_t blockBytes = 0, blockHits = 0, blockMisses = 0;
};
struct StoreOverview
{
    int64_t start = 0, end = 0, maximumFrameNs = 0;
    uint64_t frames = 0;
};
struct StoreParameter
{
    uint64_t event=0;
    uint32_t index=0;
    int32_t value=0;
    bool boolean=false;
    std::string name;
};
struct StoreTimelineOptions
{
    int64_t begin=0,end=1;
    uint32_t bins=128,depths=4;
    size_t maximumLanes=8;
    uint64_t afterLane=0; // Last lane ID plus one; zero includes GPU context 0.
    bool gpu=false;
    // Only this GPU context, or for CPU lanes, only this thread.
    std::optional<uint64_t> context;
};
struct StoreTimelineLane
{
    uint64_t id=0,events=0,unfinished=0,unplaced=0,invalid=0;
    uint32_t maximumDepth=0;
};
struct StoreTimelineBin
{
    uint64_t lane=0,events=0,closed=0,unfinished=0,invalid=0;
    int64_t begin=0,end=0;
    uint32_t depth=0;
};
struct StoreTimelineView
{
    std::vector<StoreTimelineLane> lanes;
    std::vector<StoreTimelineBin> cells;
    uint64_t events=0,nextLane=0;
    uint32_t bins=0,depths=0;
    bool more=false;
};
// A zone source as a timeline shows it; StoreTimelineZone::source numbers
// TimelineSources(), whose entry 0 stands for an unknown source.
struct StoreTimelineSource
{
    std::string name, function, file;
    uint32_t line = 0, color = 0;
};
// A CPU or GPU zone as a timeline draws it: no strings. An open zone ends at
// INT64_MAX. `color` is the zone's own color, 0 when it has none.
struct StoreTimelineZone
{
    uint64_t id = 0, thread = 0;
    int64_t start = 0, end = 0, childTime = 0;
    // A GPU zone's CPU interval, which recorded its commands; 0 when unknown.
    int64_t cpuStart = 0, cpuEnd = 0;
    uint32_t depth = 0, source = 0, color = 0;
    bool open = false;
};
struct StoreText
{
    uint64_t event = 0;
    std::string text;
};
struct StoreAnalysisOptions
{
    int64_t begin = INT64_MIN, end = INT64_MAX;
    bool gpu = false;
    // A whole-capture CPU analysis of a finished store merges the call tree
    // collected during ingestion. When true it aggregates every zone instead.
    bool events = false;
    std::optional<uint64_t> thread, context;
    uint64_t temporaryBytes = 64ull << 30;
    std::function<void( uint64_t, uint64_t )> progress;
};
struct StoreAnalysisSummary
{
    int64_t begin = INT64_MIN, end = INT64_MAX;
    uint64_t revision = 0, events = 0, closed = 0, unfinished = 0, invalid = 0;
    uint64_t sources = 0, nodes = 0, selfUnknown = 0;
    uint64_t workspaceBytes = 0;
    bool gpu = false;
    // Aggregated from the call tree collected during ingestion.
    bool callTree = false;
};
struct StoreAnalysisRow
{
    uint64_t id = 0, source = 0, parent = 0, thread = 0, context = 0, example = 0;
    uint64_t events = 0, closed = 0, unfinished = 0, invalid = 0, selfUnknown = 0, subtreeEvents = 0;
    int64_t inclusive = 0, self = 0, minimum = 0, maximum = 0;
    std::string name, function, file;
    uint32_t line = 0;
};
struct StoreAnalysisMatch
{
    uint64_t id = 0, thread = 0, context = 0, source = 0;
    std::optional<int64_t> start, end;
    std::string name, function, file;
    bool textMatched = false;
};
struct StoreFlameOptions
{
    uint64_t focus = 0;
    uint32_t depth = 8, pixels = 1024;
    size_t maximumNodes = 512;
    bool eventCounts = false;
};
struct StoreFlameBox
{
    StoreAnalysisRow row;
    double begin = 0, end = 0, weight = 0;
    uint32_t depth = 0;
    bool expandedWeight = false;
};
struct StoreFlameView
{
    std::vector<StoreFlameBox> boxes;
    uint64_t totalNodes = 0, omittedNodes = 0;
    double totalWeight = 0;
    uint32_t depth = 0;
    bool eventCounts = false;
};
template<class T>
struct StorePage
{
    std::vector<T> values;
    uint64_t next = 0;
    bool more = false;
};
class StoreAnalysis
{
public:
    ~StoreAnalysis();
    const StoreAnalysisSummary& Info() const;
    StorePage<StoreAnalysisRow> Statistics( uint64_t after = 0, size_t limit = 128 ) const;
    StorePage<StoreAnalysisRow> Flame( uint64_t parent = 0, uint64_t after = 0, size_t limit = 128 ) const;
    uint64_t FlameParent( uint64_t node ) const;
    StoreFlameView FlameView( const StoreFlameOptions& options = {} ) const;
    StorePage<StoreAnalysisMatch> Search( const std::string& text, bool caseSensitive = false, uint64_t after = 0, size_t limit = 128, std::optional<uint64_t> source = {} ) const;
private:
    StorePage<StoreAnalysisMatch> ZoneSearch( const std::string& text, bool caseSensitive, uint64_t after, size_t limit, std::optional<uint64_t> source ) const;
    friend class EventStore;
    struct Impl;
    explicit StoreAnalysis( std::unique_ptr<Impl> impl );
    std::unique_ptr<Impl> m_impl;
};
// CPU zones of one logical thread.
struct StoreZoneThread
{
    uint64_t thread = 0, zones = 0;
};
// One CPU zone as analysis tools read it. `end` is valid when !open; the
// source is the stored (kind,key) and the stack is the stored stack id. The
// text joins the zone's fragments in event order with newlines, each cut at
// its first NUL, as a standard snapshot stores it.
struct StoreZoneRecord
{
    uint64_t id = 0, parent = 0, stack = 0, callsite = 0, skey = 0, validation = 0;
    int64_t start = 0, end = 0, childTime = 0;
    uint32_t depth = 0, children = 0, color = 0;
    uint8_t skind = 0;
    bool open = false, hasColor = false, hasName = false, hasText = false;
    std::string name, text;
};
// The GPU zones of one (context, thread) timeline. `context` is the stored
// context id; a standard snapshot orders contexts by sequence and the threads
// of a context in ascending id order.
struct StoreGpuZoneGroup
{
    // `completed` counts the zones with a GPU end.
    uint64_t context = 0, thread = 0, zones = 0, completed = 0;
};
// One GPU zone as analysis tools read it. Unknown times are -1. `children`
// counts the direct children; `childTime` sums their GPU durations the way
// Tracy's Worker does, with a child's missing GPU end taken from its last
// descendant that has one.
struct StoreGpuZoneRecord
{
    uint64_t id = 0, parent = 0, stack = 0, callsite = 0, skey = 0;
    int64_t cpuStart = 0, cpuEnd = -1, gpuStart = -1, gpuEnd = -1, childTime = 0;
    uint32_t depth = 0, children = 0;
    uint16_t query = 0;
    uint8_t skind = 0;
};
// Reads the GPU zones of one timeline in preorder (id order) from any
// position, with the same restrictions as StoreZoneCursor.
class StoreGpuZoneCursor
{
public:
    ~StoreGpuZoneCursor();
    // Positions before zone `ordinal` of timeline `group` (an index into
    // EventStore::GpuZoneGroups()).
    void Seek( size_t group, uint64_t ordinal );
    bool Next( StoreGpuZoneRecord& zone );
    uint64_t Ordinal() const;
    struct Impl;

private:
    friend class EventStore;
    explicit StoreGpuZoneCursor( std::unique_ptr<Impl> impl );
    std::unique_ptr<Impl> m_impl;
};
// A Job id with the ids of its first and last schedule, config, dependency or
// stage record.
struct StoreJobKey
{
    uint64_t id = 0, first = 0, last = 0;
};
// The int16 source location number a standard snapshot of this store gives
// a stored (kind,key) source: static sources 1..N, dynamic sources negative.
struct StoreSourceNumber
{
    uint8_t kind = 0;
    uint64_t key = 0;
    int16_t number = 0;
};
// Reads the CPU zones of one logical thread in preorder (id order) from any
// position. It uses its store's connection and caches, so it must not
// outlive the store or run while another thread uses the store.
class StoreZoneCursor
{
public:
    ~StoreZoneCursor();
    // Positions before zone `ordinal` (0-based) of `thread`.
    void Seek( uint64_t thread, uint64_t ordinal );
    // Reads the next zone of the thread; false after its last zone.
    bool Next( StoreZoneRecord& zone );
    // Ordinal of the zone the next call to Next() reads.
    uint64_t Ordinal() const;
    struct Impl;

private:
    friend class EventStore;
    explicit StoreZoneCursor( std::unique_ptr<Impl> impl );
    std::unique_ptr<Impl> m_impl;
};
class EventStore
{
public:
    static std::filesystem::path Directory( const std::filesystem::path& journal );
    static StoreInfo Build( const std::filesystem::path& journal, const StoreOptions& options = {} );
    // Builds the store while the journal is still being recorded. Records are
    // decoded a few MiB behind the end of the journal; the build finishes once
    // the journal is complete, or once captureEnded() returns true and the
    // journal stops growing. The result equals Build() of the final journal.
    static StoreInfo BuildFollowing( const std::filesystem::path& journal, const std::function<bool()>& captureEnded, const StoreOptions& options = {} );
    static std::unique_ptr<EventStore> Open( const std::filesystem::path& journal, const StoreOptions& options = {} );
    // Another handle on the files this handle opened, for another thread. It
    // does not verify them again: this handle did when it opened them.
    std::unique_ptr<EventStore> OpenSibling( const StoreOptions& options = {} ) const;
    // Hashes the store files and the journal and compares them with the
    // manifest; throws on any difference.
    static void VerifyContent( const std::filesystem::path& journal, const StoreOptions& options = {} );
    // Schema of the store built for `journal` according to its manifest, or 0
    // when there is no readable manifest. Stores of any other schema than
    // EventStoreSchema must be rebuilt.
    static uint32_t StoredSchema( const std::filesystem::path& journal );
    ~EventStore();
    const StoreInfo& Info() const;
    StoreCacheStats CacheStats() const;
    StoreTimelineView TimelineOverview(const StoreTimelineOptions& options) const;
    // Timeline drawing. Every zone source with the names zone pages resolve.
    std::vector<StoreTimelineSource> TimelineSources() const;
    // Per thread, the CPU zones of the zone blocks overlapping [begin,end): an
    // upper bound of the zones that overlap it, from the block index only.
    std::vector<std::pair<uint64_t, uint64_t>> TimelineZoneCounts( int64_t begin, int64_t end ) const;
    // The CPU zones of `thread` overlapping [begin,end), in preorder; `name`
    // is the zone's own name, empty when it has none, valid during the call.
    // Stops when the visitor returns false.
    void VisitTimelineZones( uint64_t thread, int64_t begin, int64_t end,
        const std::function<bool( const StoreTimelineZone&, std::string_view name )>& visit ) const;
    // The same in two steps, so that several handles can decode the blocks
    // of one thread: its CPU zone blocks overlapping [begin,end) in zone
    // order, then the zones of one block overlapping [begin,end). The visit
    // returns false when the visitor stopped it.
    std::vector<uint64_t> TimelineBlocks( uint64_t thread, int64_t begin, int64_t end ) const;
    bool VisitTimelineBlockZones( uint64_t block, int64_t begin, int64_t end,
        const std::function<bool( const StoreTimelineZone&, std::string_view name )>& visit ) const;
    // The GPU zones of `context` whose GPU interval overlaps [begin,end), in
    // id order; with `rootsOnly`, only those without a parent, found without
    // the time index. Stops when the visitor returns false.
    void VisitTimelineGpuZones( uint64_t context, int64_t begin, int64_t end, bool rootsOnly,
        const std::function<bool( const StoreTimelineZone& )>& visit ) const;
    // The CPU zone blocks of `thread` overlapping [begin,end), in zone order:
    // each block's time span and the number of its zones at depth 0, from the
    // block index only. A block of nested zones only has none; the block of
    // their top-level zone spans them. Stops when the visitor returns false.
    void VisitTimelineBlocks( uint64_t thread, int64_t begin, int64_t end,
        const std::function<bool( int64_t start, int64_t end, uint64_t topLevelZones )>& visit ) const;
    // Thread names by thread id, every frame set, and every frame by set and start.
    std::vector<std::pair<uint64_t, std::string>> ThreadNames() const;
    std::vector<StoreFrameSet> FrameSets() const;
    std::vector<StoreFrame> AllFrames() const;
    // The result owns its disk workspace and outlives this source/revision handle.
    // Failed or cancelled work leaves previously returned analyses usable.
    std::unique_ptr<StoreAnalysis> Analyze( const StoreAnalysisOptions& options = {} ) const;
    StorePage<StoreZone> Zones( int64_t begin, int64_t end, std::optional<uint64_t> thread,
                                uint64_t after = 0, size_t limit = 256 ) const;
    std::optional<StoreZone> Zone( uint64_t id ) const;
    std::vector<uint64_t> Stack( uint64_t id ) const;
    StorePage<StoreStackFrame> StackFrames( uint64_t id, uint64_t after = 0, size_t limit = 128 ) const;
    StorePage<StoreText> Text( uint64_t id, uint64_t after = 0, size_t limit = 64 ) const;
    StorePage<StoreZone> Children( uint64_t id, uint64_t after = 0, size_t limit = 256 ) const;
    StorePage<StoreGpuContext> GpuContexts( uint64_t after = 0, size_t limit = 64 ) const;
    StorePage<StoreGpuZone> GpuZones( int64_t begin, int64_t end, std::optional<uint64_t> context = {}, uint64_t after = 0, size_t limit = 256 ) const;
    std::optional<StoreGpuZone> GpuZone( uint64_t id ) const;
    StorePage<StoreGpuZone> GpuChildren( uint64_t id, uint64_t after = 0, size_t limit = 256 ) const;
    StorePage<StoreGpuNote> GpuNotes( uint64_t context, uint64_t after = 0, size_t limit = 64 ) const;
    StorePage<StoreGpuAnnotation> GpuAnnotations( uint64_t context, std::optional<uint16_t> query = {}, uint64_t after = 0, size_t limit = 64 ) const;
    StorePage<StoreSample> Samples( int64_t begin, int64_t end, std::optional<uint64_t> thread = {}, uint64_t after = 0, size_t limit = 256, bool includePending = true ) const;
    std::optional<StoreSample> Sample( uint64_t id ) const;
    StorePage<StoreContextRegion> ContextRegions( int64_t begin, int64_t end, std::optional<uint64_t> thread = {}, uint64_t after = 0, size_t limit = 256 ) const;
    StorePage<StoreContextRegion> CpuRegions( int64_t begin, int64_t end, std::optional<uint8_t> cpu = {}, uint64_t after = 0, size_t limit = 256 ) const;
    StorePage<StoreMessage> Messages( int64_t begin, int64_t end, std::optional<uint64_t> thread = {}, uint64_t after = 0, size_t limit = 256, bool appInfo = false ) const;
    std::optional<StoreMessage> Message( uint64_t id ) const;
    StorePage<StorePlot> Plots( uint64_t after = 0, size_t limit = 64 ) const;
    StorePage<StoreParameter> Parameters( uint64_t after = 0, size_t limit = 64 ) const;
    StorePage<StorePlotPoint> PlotPoints( int64_t begin, int64_t end, std::optional<uint64_t> plot = {}, uint64_t after = 0, size_t limit = 512 ) const;
    StorePage<StoreLock> Locks( uint64_t after = 0, size_t limit = 64 ) const;
    StorePage<StoreLockEvent> LockEvents( int64_t begin, int64_t end, std::optional<uint64_t> lock = {}, uint64_t after = 0, size_t limit = 256 ) const;
    StorePage<StoreJob> Jobs( int64_t begin, int64_t end, uint64_t after = 0, size_t limit = 128 ) const;
    StorePage<StoreAllocation> Allocations( int64_t begin, int64_t end, std::optional<uint64_t> pool = {}, uint64_t after = 0, size_t limit = 256 ) const;
    std::optional<StoreAllocation> Allocation( uint64_t id ) const;
    // The allocations of `pool` live at `time`; at most the memory's
    // resources at one time, not the capture's.
    StoreLiveAllocations LiveAllocations( int64_t time, uint64_t pool ) const;
    StorePage<StoreMemoryPool> MemoryPools( uint64_t after = 0, size_t limit = 64 ) const;
    StorePage<StoreGpuReferencePass> GpuReferencePasses( int64_t begin, int64_t end, uint64_t after = 0, size_t limit = 128 ) const;
    StorePage<StoreGpuReference> GpuReferences( int64_t begin, int64_t end, std::optional<uint64_t> pass = {}, uint64_t after = 0, size_t limit = 256 ) const;
    StorePage<StoreGpuReferenceEnd> GpuReferenceEnds( int64_t begin, int64_t end, uint64_t after = 0, size_t limit = 128 ) const;
    StorePage<StoreCatalogGeneration> CatalogGenerations( uint64_t after = 0, size_t limit = 64 ) const;
    StorePage<StoreCatalogRecord> CatalogRecords( uint8_t kind, std::optional<uint64_t> generation = {}, uint64_t after = 0, size_t limit = 128 ) const;
    StorePage<StoreCatalogBatch> CatalogBatches( std::optional<uint64_t> generation = {}, uint64_t after = 0, size_t limit = 128 ) const;
    StorePage<StoreCatalogResource> CatalogResources( int64_t begin, int64_t end, std::optional<uint64_t> generation = {}, uint64_t after = 0, size_t limit = 128 ) const;
    std::optional<StoreJob> Job( uint64_t id ) const;
    StorePage<StoreJobStage> JobStages( uint64_t job, uint64_t after = 0, size_t limit = 128 ) const;
    StorePage<StoreJobDependency> JobDependencies( uint64_t job, uint64_t after = 0, size_t limit = 128 ) const;
    StorePage<uint32_t> JobLanes( uint64_t job, uint64_t after = 0, size_t limit = 128 ) const;
    StorePage<StoreIoRequest> IoRequests( int64_t begin, int64_t end, uint64_t after = 0, size_t limit = 128 ) const;
    std::optional<StoreIoRequest> IoRequest( uint64_t id ) const;
    StorePage<StoreIoStage> IoStages( uint64_t request, uint64_t after = 0, size_t limit = 128 ) const;
    StorePage<StoreScriptFrame> ScriptFrames( uint64_t after = 0, size_t limit = 128 ) const;
    StorePage<StoreScriptEvent> ScriptEvents( int64_t begin, int64_t end, uint64_t after = 0, size_t limit = 128 ) const;
    StorePage<StoreFrame> Frames( int64_t begin, int64_t end, uint64_t after = 0, size_t limit = 1024 ) const;
    StorePage<StoreFrameImage> FrameImages( uint64_t after = 0, size_t limit = 128 ) const;
    StorePage<StoreFrameImage> FrameImagesInRange( int64_t begin, int64_t end, uint64_t after = 0, size_t limit = 128 ) const;
    std::optional<StoreFrameImage> FrameImage( uint64_t id ) const;
    StoreBinaryChunk FrameImageBytes( uint64_t id, uint64_t offset, size_t maxBytes ) const;
    StorePage<StoreAuxiliaryRecord> AuxiliaryEvents( int64_t begin, int64_t end, std::optional<StoreAuxiliaryKind> kind = {}, std::optional<StoreEntityKey> entity = {}, uint64_t after = 0, size_t limit = 128 ) const;
    StorePage<StoreAuxiliaryRecord> RuntimeStatesAt( int64_t time, uint16_t afterDomain = 0, size_t limit = 64 ) const;
    StorePage<StoreHardwareSample> HardwareSamples( int64_t begin, int64_t end, std::optional<uint8_t> kind = {}, std::optional<uint64_t> address = {}, uint64_t after = 0, size_t limit = 256 ) const;
    StorePage<StoreHardwareAddress> HardwareAddresses( uint64_t after = 0, size_t limit = 128 ) const;
    StorePage<StoreSourceResource> SourceResources( uint64_t after = 0, size_t limit = 128 ) const;
    StorePage<StoreSymbolResource> SymbolResources( uint64_t after = 0, size_t limit = 128 ) const;
    StoreBinaryChunk CodeBytes( StoreCodeKind kind, uint64_t id, uint64_t offset, size_t maxBytes ) const;
    StoreBinaryChunk CodeTextBytes( StoreCodeKind kind, uint64_t id, uint8_t field, uint64_t offset, size_t maxBytes ) const;
    StorePage<StoreFiber> Fibers( uint64_t after = 0, size_t limit = 128 ) const;
    StoreBinaryChunk FiberNameBytes( uint64_t id, uint64_t offset, size_t maxBytes ) const;
    StorePage<StoreCallsite> Callsites( uint64_t after = 0, size_t limit = 128 ) const;
    // Export this pinned revision; validate with bounded reads before atomic publication.
    StoreExportResult ExportSnapshot( const std::filesystem::path& destination, const StoreExportOptions& options = {} ) const;
    StoreCaptureMetadata CaptureMetadata() const;
    StorePage<StoreCpuTopology> CpuTopology( uint64_t after = 0, size_t limit = 128 ) const;
    StorePage<StoreThreadHint> ThreadHints( uint64_t after = 0, size_t limit = 128 ) const;
    std::optional<StoreCrash> Crash() const;
    std::optional<StoreFrameSet> FrameSet( uint64_t id ) const;
    std::vector<StoreOverview> Overview( int64_t begin, int64_t end, size_t limit = 512 ) const;
    // Logical threads with CPU zones, in ascending id order, with their zone
    // counts. Tracy's Worker numbers the zones of a snapshot in this thread
    // order, each thread in preorder.
    std::vector<StoreZoneThread> ZoneThreads() const;
    // With `texts` false the cursor reads no zone text: records have none.
    std::unique_ptr<StoreZoneCursor> ZoneCursor( bool texts = true ) const;
    // Logical thread and preorder ordinal of CPU zone `id`.
    // Preorder ordinal of CPU zone `id` within its logical thread `thread`.
    std::optional<uint64_t> ZoneOrdinal( uint64_t thread, uint64_t id ) const;
    // Ordinal of the CPU zone of `thread` running at `time` by Tracy's rule:
    // among the zones that start at or before `time` and end at or after it,
    // the one with the latest start, the deeper on ties. A zone that never
    // closed ends with its last descendant.
    std::optional<uint64_t> ZoneAt( uint64_t thread, int64_t time ) const;
    // Source numbers of a standard snapshot of this store, for every stored
    // source in (kind,key) order.
    std::vector<StoreSourceNumber> SourceNumbers() const;
    // Stored stack of every callsite that has one, by callsite id.
    std::vector<std::pair<uint64_t, uint64_t>> CallsiteStacks() const;
    // Stored stack ids in ascending order; a snapshot numbers each stack by
    // its 1-based rank.
    std::vector<uint64_t> StackIds() const;
    // GPU zone timelines in snapshot order, built by one pass over the GPU
    // zones on first use.
    std::vector<StoreGpuZoneGroup> GpuZoneGroups() const;
    std::unique_ptr<StoreGpuZoneCursor> GpuZoneCursor() const;
    // GPU zones without a parent that have a GPU start and end, as (stored
    // context id, GPU start, GPU end), in id order.
    void VisitRootGpuZones( const std::function<void( uint64_t, int64_t, int64_t )>& visit ) const;
    // GPU zones with a GPU start whose GPU interval meets [start, end], found
    // through the GPU time index, as (id, stored context id, thread), in id order.
    std::vector<std::array<uint64_t, 3>> GpuZonesMeeting( int64_t start, int64_t end ) const;
    // Preorder ordinal of GPU zone `id` within timeline `group`.
    std::optional<uint64_t> GpuZoneOrdinal( size_t group, uint64_t id ) const;
    // Stored GPU context ids in snapshot order (by sequence).
    std::vector<uint64_t> GpuContextIds() const;
    // Raw JN packets of one export type in id order, the order of the
    // matching domain in a standard snapshot: the packet count, and up to
    // `limit` packets from ordinal `ordinal` on.
    uint64_t PacketCount( uint8_t type ) const;
    std::vector<StoreExportPacket> PacketsAt( uint8_t type, uint64_t ordinal, size_t limit ) const;
    // Visits the packets of one export type with ids in [from,to] in id
    // order until the visitor returns false.
    void VisitPackets( uint8_t type, uint64_t from, uint64_t to, const std::function<bool( const StoreExportPacket& )>& visit ) const;
    // Jobs in ascending id order: their number, the keys of up to `limit`
    // Jobs from ordinal `ordinal` on, and the key of one Job.
    uint64_t JobCount() const;
    std::vector<StoreJobKey> JobKeys( uint64_t ordinal, size_t limit ) const;
    std::optional<StoreJobKey> JobKey( uint64_t id ) const;
    // Jobs whose span from their first record (else schedule) to completion
    // (open when not completed) meets [start, end], found through the Job time
    // index, in id order.
    std::vector<StoreJobKey> JobKeysMeeting( int64_t start, int64_t end ) const;
    // Job type names as a standard snapshot resolves the raw name strings;
    // nothing for a name the snapshot has no string for.
    std::vector<std::pair<uint32_t, std::optional<std::string>>> JobTypeNames() const;

    struct Impl;

private:
    friend class LiveEventStore;
    friend class StoreAnalysis;
    explicit EventStore( std::unique_ptr<Impl> impl );
    StoreTimelineView CpuTimelineOverview( const StoreTimelineOptions& options, StoreTimelineView& view, size_t laneLimit ) const;
    std::unique_ptr<Impl> m_impl;
};
class LiveEventStore
{
public:
    static std::unique_ptr<LiveEventStore> Create( const std::filesystem::path& journal, const StoreOptions& options = {} );
    ~LiveEventStore();
    // One writer thread calls Update. Read-only snapshots own their SQLite
    // transaction and remain stable while later revisions are published.
    bool Update( uint64_t maxRecords = 64 );
    std::unique_ptr<EventStore> Snapshot( const StoreOptions& readOptions = {} ) const;
    uint64_t DecodedRecords() const;

private:
    struct Impl;
    explicit LiveEventStore( std::unique_ptr<Impl> impl );
    std::unique_ptr<Impl> m_impl;
};
}
