#include "TracyWorkerTraceSource.hpp"

#include "TracyHash.hpp"
#include "TracyFileRead.hpp"
#include "TracyWorker.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace tracy::analysis
{
namespace
{

std::string Hex( uint64_t value )
{
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

std::string Safe( const char* value )
{
    return value ? value : "";
}

template<typename F>
void ForEachCpuZone( const Vector<short_ptr<ZoneEvent>>& zones, F&& callback )
{
    if( zones.is_magic() )
    {
        const auto& direct = reinterpret_cast<const Vector<ZoneEvent>&>( zones );
        for( const auto& zone : direct ) callback( &zone );
    }
    else
    {
        for( const auto& zone : zones ) callback( zone.get() );
    }
}

template<typename F>
void ForEachGpuZone( const Vector<short_ptr<GpuEvent>>& zones, F&& callback )
{
    if( zones.is_magic() )
    {
        const auto& direct = reinterpret_cast<const Vector<GpuEvent>&>( zones );
        for( const auto& zone : direct ) callback( &zone );
    }
    else
    {
        for( const auto& zone : zones ) callback( zone.get() );
    }
}

bool Intersects( int64_t begin, int64_t end, const ScanRange& range )
{
    return begin < range.endNs && end >= range.startNs;
}

uint8_t Expand5( uint16_t value )
{
    return uint8_t( ( value << 3 ) | ( value >> 2 ) );
}

uint8_t Expand6( uint16_t value )
{
    return uint8_t( ( value << 2 ) | ( value >> 4 ) );
}

void DecodeBc1( const uint8_t* input, uint32_t width, uint32_t height, std::vector<uint8_t>& output )
{
    output.assign( size_t( width ) * height * 4, 0 );
    const uint32_t blocksX = ( width + 3 ) / 4;
    const uint32_t blocksY = ( height + 3 ) / 4;
    for( uint32_t by = 0; by < blocksY; by++ )
    {
        for( uint32_t bx = 0; bx < blocksX; bx++ )
        {
            const uint8_t* block = input + ( size_t( by ) * blocksX + bx ) * 8;
            const uint16_t c0 = uint16_t( block[0] ) | uint16_t( block[1] ) << 8;
            const uint16_t c1 = uint16_t( block[2] ) | uint16_t( block[3] ) << 8;
            uint8_t colors[4][4] = {
                { Expand5( ( c0 >> 11 ) & 31 ), Expand6( ( c0 >> 5 ) & 63 ), Expand5( c0 & 31 ), 255 },
                { Expand5( ( c1 >> 11 ) & 31 ), Expand6( ( c1 >> 5 ) & 63 ), Expand5( c1 & 31 ), 255 },
                {}, {}
            };
            if( c0 > c1 )
            {
                for( size_t channel = 0; channel < 3; channel++ )
                {
                    colors[2][channel] = uint8_t( ( 2 * colors[0][channel] + colors[1][channel] ) / 3 );
                    colors[3][channel] = uint8_t( ( colors[0][channel] + 2 * colors[1][channel] ) / 3 );
                }
                colors[2][3] = colors[3][3] = 255;
            }
            else
            {
                for( size_t channel = 0; channel < 3; channel++ ) colors[2][channel] = uint8_t( ( colors[0][channel] + colors[1][channel] ) / 2 );
                colors[2][3] = 255;
                colors[3][0] = colors[3][1] = colors[3][2] = colors[3][3] = 0;
            }

            const uint32_t selectors = uint32_t( block[4] ) | uint32_t( block[5] ) << 8 | uint32_t( block[6] ) << 16 | uint32_t( block[7] ) << 24;
            for( uint32_t py = 0; py < 4; py++ )
            {
                for( uint32_t px = 0; px < 4; px++ )
                {
                    const uint32_t x = bx * 4 + px;
                    const uint32_t y = by * 4 + py;
                    if( x >= width || y >= height ) continue;
                    const uint32_t selector = ( selectors >> ( 2 * ( py * 4 + px ) ) ) & 3;
                    std::memcpy( output.data() + ( size_t( y ) * width + x ) * 4, colors[selector], 4 );
                }
            }
        }
    }
}

}

class WorkerTraceSource::Impl
{
public:
    struct CpuEntry
    {
        const ZoneEvent* zone;
        uint64_t thread;
        size_t index;
        std::optional<size_t> parent;
    };

    struct GpuEntry
    {
        const GpuEvent* zone;
        size_t context;
        uint64_t thread;
        size_t index;
        std::optional<size_t> parent;
    };

    explicit Impl( std::filesystem::path sourcePath )
        : path( std::move( sourcePath ) )
    {}

    std::string MakeRef( const char* kind, uint64_t id ) const
    {
        std::ostringstream out;
        out << "tracy:v1:" << fingerprint.substr( 0, 16 ) << ':' << kind << ':' << std::hex << id;
        return out.str();
    }

    SourceLocationDto SourceLocation( int16_t id ) const
    {
        const auto& source = worker->GetSourceLocation( id );
        return {
            MakeRef( "source", uint16_t( id ) ),
            Safe( source.name.active ? worker->GetString( source.name ) : nullptr ),
            Safe( worker->GetString( source.function ) ),
            Safe( worker->GetString( source.file ) ),
            source.line,
            source.color
        };
    }

    void IndexCpuVector( const Vector<short_ptr<ZoneEvent>>& zones, uint64_t thread, std::optional<size_t> parent )
    {
        ForEachCpuZone( zones, [&]( const ZoneEvent* zone ) {
            const size_t index = cpuZones.size();
            cpuZones.push_back( { zone, thread, index, parent } );
            cpuLookup.emplace( zone, index );
            if( zone->HasChildren() ) IndexCpuVector( worker->GetZoneChildren( zone->Child() ), thread, index );
        } );
    }

    void IndexGpuVector( const Vector<short_ptr<GpuEvent>>& zones, size_t context, uint64_t thread, std::optional<size_t> parent )
    {
        ForEachGpuZone( zones, [&]( const GpuEvent* zone ) {
            const size_t index = gpuZones.size();
            gpuZones.push_back( { zone, context, thread, index, parent } );
            gpuLookup.emplace( zone, index );
            if( zone->Child() >= 0 ) IndexGpuVector( worker->GetGpuChildren( zone->Child() ), context, thread, index );
        } );
    }

    void BuildIndexes()
    {
        std::vector<const ThreadData*> threads;
        for( const auto thread : worker->GetThreadData() ) threads.push_back( thread );
        std::sort( threads.begin(), threads.end(), []( const auto* lhs, const auto* rhs ) { return lhs->id < rhs->id; } );
        for( const auto* thread : threads ) IndexCpuVector( thread->timeline, thread->id, std::nullopt );

        const auto& contexts = worker->GetGpuData();
        for( size_t contextIndex = 0; contextIndex < contexts.size(); contextIndex++ )
        {
            const auto* context = contexts[contextIndex];
            std::vector<uint64_t> threadIds;
            for( const auto& [thread, data] : context->threadData ) threadIds.push_back( thread );
            std::sort( threadIds.begin(), threadIds.end() );
            for( const auto thread : threadIds ) IndexGpuVector( context->threadData.find( thread )->second.timeline, contextIndex, thread, std::nullopt );
        }

        std::vector<std::pair<std::string, uint64_t>> pools;
        for( const auto& [nameId, memory] : worker->GetMemNameMap() )
        {
            pools.emplace_back( nameId == 0 ? "Default allocator" : Safe( worker->GetString( nameId ) ), nameId );
        }
        std::sort( pools.begin(), pools.end(), []( const auto& lhs, const auto& rhs ) {
            return lhs.first != rhs.first ? lhs.first < rhs.first : lhs.second < rhs.second;
        } );
        for( size_t index = 0; index < pools.size(); index++ ) memoryPoolIndex.emplace( pools[index].second, index );
    }

    std::filesystem::path path;
    std::string fingerprint;
    std::unique_ptr<FileRead> file;
    std::unique_ptr<Worker> worker;
    std::vector<CpuEntry> cpuZones;
    std::vector<GpuEntry> gpuZones;
    std::unordered_map<const ZoneEvent*, size_t> cpuLookup;
    std::unordered_map<const GpuEvent*, size_t> gpuLookup;
    std::unordered_map<uint64_t, size_t> memoryPoolIndex;
    mutable std::mutex readMutex;
};

const char* ToString( TraceLoadErrorCode code )
{
    switch( code )
    {
    case TraceLoadErrorCode::NotFound: return "not_found";
    case TraceLoadErrorCode::OpenFailed: return "open_failed";
    case TraceLoadErrorCode::UnsupportedVersion: return "unsupported_version";
    case TraceLoadErrorCode::LegacyVersion: return "legacy_version";
    case TraceLoadErrorCode::Corrupt: return "corrupt";
    case TraceLoadErrorCode::ResourceLimit: return "resource_limit";
    case TraceLoadErrorCode::Internal: return "internal";
    }
    return "internal";
}

std::unique_ptr<WorkerTraceSource> WorkerTraceSource::Open( const std::filesystem::path& path, StateCallback stateCallback )
{
    auto impl = std::make_unique<Impl>( path );
    if( stateCallback ) stateCallback( TraceSourceState::Loading );
    if( !std::filesystem::exists( path ) ) throw TraceLoadError( TraceLoadErrorCode::NotFound, "trace file does not exist" );

    try
    {
        impl->fingerprint = Sha256File( path );
        impl->file.reset( FileRead::Open( path.string().c_str() ) );
        if( !impl->file ) throw TraceLoadError( TraceLoadErrorCode::OpenFailed, "unable to open trace file" );
        impl->worker = std::make_unique<Worker>( *impl->file, EventType::All, true, false );
        if( stateCallback ) stateCallback( TraceSourceState::Indexing );
        while( !impl->worker->IsBackgroundDone() ) std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
        impl->BuildIndexes();
        if( stateCallback ) stateCallback( TraceSourceState::Ready );
        return std::unique_ptr<WorkerTraceSource>( new WorkerTraceSource( std::move( impl ) ) );
    }
    catch( const UnsupportedVersion& error )
    {
        throw TraceLoadError( TraceLoadErrorCode::UnsupportedVersion, "trace was written by a newer Tracy version", error.version );
    }
    catch( const LegacyVersion& error )
    {
        throw TraceLoadError( TraceLoadErrorCode::LegacyVersion, "trace is older than the minimum supported version", error.version );
    }
    catch( const LoadFailure& error )
    {
        throw TraceLoadError( TraceLoadErrorCode::Corrupt, error.msg );
    }
    catch( const std::bad_alloc& )
    {
        throw TraceLoadError( TraceLoadErrorCode::ResourceLimit, "not enough memory to load trace" );
    }
    catch( const TraceLoadError& )
    {
        throw;
    }
    catch( const std::exception& error )
    {
        throw TraceLoadError( TraceLoadErrorCode::Internal, error.what() );
    }
}

WorkerTraceSource::WorkerTraceSource( std::unique_ptr<Impl> impl )
    : m_impl( std::move( impl ) )
{}

WorkerTraceSource::~WorkerTraceSource() = default;

const std::filesystem::path& WorkerTraceSource::Path() const { return m_impl->path; }
const std::string& WorkerTraceSource::Fingerprint() const { return m_impl->fingerprint; }

std::vector<FrameDto> WorkerTraceSource::GetFramesForSet( size_t frameSetIndex, size_t offset, size_t limit ) const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<FrameDto> result;
    auto& worker = *m_impl->worker;
    const auto& sets = worker.GetFrames();
    if( frameSetIndex >= sets.size() ) return result;
    const auto* set = sets[frameSetIndex];
    const size_t count = worker.GetFrameCount( *set );
    const size_t endIndex = std::min( count, offset + limit );
    result.reserve( endIndex > offset ? endIndex - offset : 0 );
    for( size_t frameIndex = offset; frameIndex < endIndex; frameIndex++ )
    {
        const int64_t begin = worker.GetFrameBegin( *set, frameIndex );
        const bool complete = frameIndex < worker.GetFullFrameCount( *set );
        FrameDto dto;
        dto.ref = m_impl->MakeRef( "frame", ( uint64_t( frameSetIndex ) << 32 ) | frameIndex );
        dto.frameSetRef = m_impl->MakeRef( "frame-set", frameSetIndex );
        dto.index = frameIndex;
        dto.beginNs = begin;
        if( complete ) dto.endNs = worker.GetFrameEnd( *set, frameIndex );
        dto.complete = complete;
        if( const auto* image = worker.GetFrameImage( *set, frameIndex ) )
        {
            const auto& images = worker.GetFrameImages();
            for( size_t imageIndex = 0; imageIndex < images.size(); imageIndex++ )
            {
                if( images[imageIndex].get() == image )
                {
                    dto.imageRef = m_impl->MakeRef( "frame-image", imageIndex );
                    break;
                }
            }
        }
        result.emplace_back( std::move( dto ) );
    }
    return result;
}

std::vector<int64_t> WorkerTraceSource::GetFrameDurations( size_t frameSetIndex ) const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<int64_t> result;
    auto& worker = *m_impl->worker;
    const auto& sets = worker.GetFrames();
    if( frameSetIndex >= sets.size() ) return result;
    const auto* set = sets[frameSetIndex];
    const size_t count = worker.GetFullFrameCount( *set );
    result.reserve( count );
    for( size_t index = 0; index < count; index++ ) result.emplace_back( worker.GetFrameTime( *set, index ) );
    return result;
}

TraceReadView WorkerTraceSource::AcquireReadView() const
{
    return { TraceSourceKind::Snapshot, TraceSourceState::Ready, 0, m_impl->worker->GetLastTime(), true };
}

std::vector<Capability> WorkerTraceSource::GetCapabilities() const
{
    const auto info = GetTraceInfo();
    const auto memoryPools = GetMemoryPools();
    const bool hasGpuMemory = std::any_of( memoryPools.begin(), memoryPools.end(), []( const auto& pool ) { return pool.gpuD3D12; } );
    auto capability = []( std::string domain, bool present, bool indexed, std::vector<std::string> methods, std::string reason = {} ) {
        return Capability { std::move( domain ), present, present, indexed && present, std::move( reason ), std::move( methods ) };
    };
    return {
        capability( "trace", true, true, { "trace.info", "trace.overview", "trace.counts", "trace.app_info", "trace.crash" } ),
        capability( "thread", info.counts.threads != 0, true, { "thread.list", "thread.get", "thread.statistics", "thread.timeline", "thread.migration" }, info.counts.threads ? "" : "trace contains no threads" ),
        capability( "cpu", info.counts.contextSwitches != 0, true, { "cpu.topology", "cpu.usage", "cpu.timeline" }, info.counts.contextSwitches ? "" : "trace contains no scheduling data" ),
        capability( "context_switch", info.counts.contextSwitches != 0, true, { "context_switch.range", "context_switch.thread", "context_switch.statistics" } ),
        capability( "frame", info.counts.frameSets != 0, true, { "frame.sets", "frame.list", "frame.get", "frame.statistics", "frame.outliers", "frame.range_mapping" } ),
        capability( "frame_image", info.counts.frameImages != 0, true, { "frame_image.list", "frame_image.metadata", "frame_image.resource" } ),
        capability( "zone.cpu", info.counts.cpuZones != 0, true, { "zone.cpu.search", "zone.cpu.get", "zone.cpu.tree", "zone.cpu.statistics", "zone.cpu.flamegraph" } ),
        capability( "zone.gpu", info.counts.gpuZones != 0, true, { "zone.gpu.contexts", "zone.gpu.search", "zone.gpu.get", "zone.gpu.statistics", "zone.gpu.flamegraph" } ),
        capability( "callstack", info.counts.callstackPayloads != 0, true, { "callstack.resolve", "callstack.frames", "callstack.parent", "callstack.batch" } ),
        capability( "sample", info.counts.samples != 0, true, { "sample.list", "sample.ghost_zones", "sample.symbol_statistics", "sample.flamegraph" } ),
        capability( "hardware_sample", info.counts.hardwareSamples != 0, true, { "hardware_sample.address", "hardware_sample.counts", "hardware_sample.capabilities" } ),
        capability( "symbol", info.counts.symbols != 0, true, { "symbol.search", "symbol.get", "symbol.address", "symbol.raw_code", "symbol.disassembly" } ),
        capability( "source", info.counts.sourceLocations != 0, true, { "source.locations", "source.statistics", "source.embedded", "source.lines" } ),
        capability( "memory", info.counts.memoryEvents != 0, true, { "memory.pools", "memory.events", "memory.active_at_time", "memory.frame_snapshot", "memory.diff", "memory.callstack_tree", "memory.leak_candidates" } ),
        capability( "memory.gpu", hasGpuMemory, true, { "memory.gpu.pools", "memory.gpu.allocations", "memory.gpu.request_scopes", "memory.gpu.pass_uses", "memory.gpu.attribution" } ),
        capability( "lock", info.counts.locks != 0, true, { "lock.list", "lock.get", "lock.timeline", "lock.contention_statistics" } ),
        capability( "plot", info.counts.plots != 0, true, { "plot.list", "plot.points", "plot.range", "plot.downsample", "plot.statistics" } ),
        capability( "message", info.counts.messages != 0, true, { "message.search", "message.get" } )
    };
}

TraceInfoDto WorkerTraceSource::GetTraceInfo() const
{
    std::lock_guard lock( m_impl->readMutex );
    auto& worker = *m_impl->worker;
    TraceInfoDto result;
    result.fingerprint = m_impl->fingerprint;
    result.captureName = worker.GetCaptureName();
    result.captureProgram = worker.GetCaptureProgram();
    result.hostInfo = worker.GetHostInfo();
    result.captureTime = worker.GetCaptureTime();
    result.executableTime = worker.GetExecutableTime();
    result.processId = worker.GetPid();
    result.traceVersion = worker.GetTraceVersion();
    result.resolution = worker.GetResolution();
    result.firstTimeNs = worker.GetFirstTime();
    result.lastTimeNs = worker.GetLastTime();
    result.loadTimeNs = worker.GetLoadTime();
    result.cpuId = worker.GetCpuId();
    result.cpuManufacturer = Safe( worker.GetCpuManufacturer() );
    const auto& crash = worker.GetCrashEvent();
    result.hasCrash = crash.thread != 0 || crash.time != 0 || crash.message != 0 || crash.callstack != 0;
    result.samplesInconsistent = worker.AreSamplesInconsistent();

    auto& counts = result.counts;
    counts.frameSets = worker.GetFrames().size();
    for( const auto* frames : worker.GetFrames() ) counts.frames += worker.GetFrameCount( *frames );
    counts.cpuZones = worker.GetZoneCount();
    counts.gpuZones = worker.GetGpuZoneCount();
    counts.threads = worker.GetThreadData().size();
    counts.locks = worker.GetLockCount();
    counts.plots = worker.GetPlotCount();
    counts.messages = worker.GetMessages().size();
    counts.memoryPools = worker.GetMemNameMap().size();
    for( const auto& [name, memory] : worker.GetMemNameMap() ) counts.memoryEvents += memory->data.size();
    counts.contextSwitches = worker.GetContextSwitchCount();
    counts.callstackPayloads = worker.GetCallstackPayloadCount();
    counts.callstackFrames = worker.GetCallstackFrameCount();
    counts.samples = worker.GetCallstackSampleCount();
    counts.hardwareSamples = worker.GetHwSampleCount();
    counts.symbols = worker.GetSymbolsCount();
    counts.symbolCodeBytes = worker.GetSymbolCodeSize();
    counts.sourceLocations = worker.GetSrcLocCount();
    counts.sourceCacheFiles = worker.GetSourceFileCacheCount();
    counts.sourceCacheBytes = worker.GetSourceFileCacheSize();
    counts.frameImages = worker.GetFrameImageCount();
    for( const auto& value : worker.GetAppInfo() ) result.appInfo.emplace_back( Safe( worker.GetString( value ) ) );
    return result;
}

std::vector<ThreadDto> WorkerTraceSource::GetThreads() const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<ThreadDto> result;
    auto& worker = *m_impl->worker;
    result.reserve( worker.GetThreadData().size() );
    for( const auto* thread : worker.GetThreadData() )
    {
        ThreadDto dto;
        dto.ref = m_impl->MakeRef( "thread", thread->id );
        dto.nativeId = thread->id;
        dto.processId = worker.GetPidFromTid( thread->id );
        dto.name = Safe( worker.GetThreadName( thread->id ) );
        dto.fiber = thread->isFiber != 0;
        dto.zoneCount = thread->count;
        dto.messageCount = thread->messages.size();
        dto.sampleCount = thread->samples.size();
        if( const auto* context = worker.GetContextSwitchData( thread->id ) ) dto.contextSwitchCount = context->v.size();
        const auto cpu = worker.GetCpuThreadData().find( thread->id );
        if( cpu != worker.GetCpuThreadData().end() )
        {
            dto.runningTimeNs = cpu->second.runningTime;
            dto.migrations = cpu->second.migrations;
        }
        result.emplace_back( std::move( dto ) );
    }
    std::sort( result.begin(), result.end(), []( const auto& lhs, const auto& rhs ) { return lhs.nativeId < rhs.nativeId; } );
    return result;
}

std::vector<FrameSetDto> WorkerTraceSource::GetFrameSets() const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<FrameSetDto> result;
    auto& worker = *m_impl->worker;
    const auto& frames = worker.GetFrames();
    result.reserve( frames.size() );
    for( size_t i = 0; i < frames.size(); i++ )
    {
        const auto* frameSet = frames[i];
        std::string name;
        if( frameSet->name == 0 ) name = "Frames";
        else if( frameSet->name >> 63 ) name = "Vsync " + std::to_string( uint32_t( frameSet->name ) );
        else name = Safe( worker.GetString( frameSet->name ) );
        result.push_back( { m_impl->MakeRef( "frame-set", i ), i, std::move( name ), frameSet->continuous != 0, worker.GetFrameCount( *frameSet ), worker.GetFullFrameCount( *frameSet ) } );
    }
    return result;
}

std::vector<GpuContextDto> WorkerTraceSource::GetGpuContexts() const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<GpuContextDto> result;
    auto& worker = *m_impl->worker;
    const auto& contexts = worker.GetGpuData();
    result.reserve( contexts.size() );
    for( size_t i = 0; i < contexts.size(); i++ )
    {
        const auto* context = contexts[i];
        result.push_back( {
            m_impl->MakeRef( "gpu-context", i ), i,
            context->name.Active() ? Safe( worker.GetString( context->name ) ) : "GPU context " + std::to_string( i ),
            m_impl->MakeRef( "thread", context->thread ), context->count, context->period,
            context->hasCalibration, uint8_t( context->type )
        } );
    }
    return result;
}

std::vector<MemoryPoolDto> WorkerTraceSource::GetMemoryPools() const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<MemoryPoolDto> result;
    auto& worker = *m_impl->worker;
    result.reserve( worker.GetMemNameMap().size() );
    for( const auto& [nameId, memory] : worker.GetMemNameMap() )
    {
        const std::string name = nameId == 0 ? "Default allocator" : Safe( worker.GetString( nameId ) );
        result.push_back( {
            m_impl->MakeRef( "memory-pool", m_impl->memoryPoolIndex.at( nameId ) ), nameId, name, memory->data.size(), memory->active.size(), memory->usage,
            memory->low == std::numeric_limits<uint64_t>::max() ? 0 : memory->low,
            memory->high == std::numeric_limits<uint64_t>::min() ? 0 : memory->high,
            name.rfind( "GPU D3D12 ", 0 ) == 0
        } );
    }
    std::sort( result.begin(), result.end(), []( const auto& lhs, const auto& rhs ) { return lhs.name < rhs.name; } );
    return result;
}

std::vector<PlotDto> WorkerTraceSource::GetPlotList() const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<PlotDto> result;
    auto& worker = *m_impl->worker;
    const auto& plots = worker.GetPlots();
    result.reserve( plots.size() );
    for( size_t i = 0; i < plots.size(); i++ )
    {
        const auto* plot = plots[i];
        result.push_back( { m_impl->MakeRef( "plot", i ), i, Safe( worker.GetString( plot->name ) ), uint8_t( plot->type ), uint8_t( plot->format ), plot->data.size(), plot->min, plot->max, plot->sum } );
    }
    return result;
}

std::vector<LockDto> WorkerTraceSource::GetLocks() const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<LockDto> result;
    auto& worker = *m_impl->worker;
    result.reserve( worker.GetLockMap().size() );
    for( const auto& [id, value] : worker.GetLockMap() )
    {
        const auto source = m_impl->SourceLocation( value->srcloc );
        const std::string name = value->customName.Active() ? Safe( worker.GetString( value->customName ) ) : source.name.empty() ? source.function : source.name;
        result.push_back( {
            m_impl->MakeRef( "lock", id ), id, name, source.ref, value->timeline.size(), value->threadList.size(), value->valid,
            value->isContended, value->timeAnnounce, value->timeTerminate < 0 ? std::nullopt : std::optional<int64_t>( value->timeTerminate )
        } );
    }
    std::sort( result.begin(), result.end(), []( const auto& lhs, const auto& rhs ) { return lhs.nativeId < rhs.nativeId; } );
    return result;
}

std::vector<CpuZoneDto> WorkerTraceSource::ScanCpuZones( const ScanRange& range ) const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<CpuZoneDto> result;
    size_t skipped = 0;
    for( const auto& entry : m_impl->cpuZones )
    {
        const auto* zone = entry.zone;
        const int64_t end = zone->IsEndValid() ? zone->End() : zone->Start();
        if( !Intersects( zone->Start(), end, range ) ) continue;
        if( skipped++ < range.offset ) continue;
        const auto source = m_impl->SourceLocation( zone->SrcLoc() );
        CpuZoneDto dto;
        dto.ref = m_impl->MakeRef( "cpu-zone", entry.index );
        dto.threadRef = m_impl->MakeRef( "thread", entry.thread );
        dto.sourceLocationRef = source.ref;
        dto.name = Safe( m_impl->worker->GetZoneName( *zone ) );
        dto.function = source.function;
        dto.file = source.file;
        dto.line = source.line;
        if( entry.parent ) dto.parentRef = m_impl->MakeRef( "cpu-zone", *entry.parent );
        dto.startNs = zone->Start();
        if( zone->IsEndValid() ) dto.endNs = zone->End();
        dto.complete = zone->IsEndValid();
        if( m_impl->worker->HasZoneExtra( *zone ) ) dto.callstack = m_impl->worker->GetZoneExtra( *zone ).callstack.Val();
        result.emplace_back( std::move( dto ) );
        if( result.size() >= range.limit ) break;
    }
    return result;
}

std::vector<GpuZoneDto> WorkerTraceSource::ScanGpuZones( const ScanRange& range ) const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<GpuZoneDto> result;
    size_t skipped = 0;
    for( const auto& entry : m_impl->gpuZones )
    {
        const auto* zone = entry.zone;
        const int64_t end = zone->GpuEnd() >= 0 ? zone->GpuEnd() : zone->GpuStart();
        if( !Intersects( zone->GpuStart(), end, range ) ) continue;
        if( skipped++ < range.offset ) continue;
        const auto source = m_impl->SourceLocation( zone->SrcLoc() );
        GpuZoneDto dto;
        dto.ref = m_impl->MakeRef( "gpu-zone", entry.index );
        dto.contextRef = m_impl->MakeRef( "gpu-context", entry.context );
        dto.threadRef = m_impl->MakeRef( "thread", entry.thread );
        dto.sourceLocationRef = source.ref;
        dto.name = Safe( m_impl->worker->GetZoneName( *zone ) );
        dto.function = source.function;
        dto.file = source.file;
        dto.line = source.line;
        if( entry.parent ) dto.parentRef = m_impl->MakeRef( "gpu-zone", *entry.parent );
        dto.gpuStartNs = zone->GpuStart();
        if( zone->GpuEnd() >= 0 ) dto.gpuEndNs = zone->GpuEnd();
        dto.cpuStartNs = zone->CpuStart();
        if( zone->CpuEnd() >= 0 ) dto.cpuEndNs = zone->CpuEnd();
        dto.callstack = zone->callstack.Val();
        dto.complete = zone->GpuEnd() >= 0 && zone->CpuEnd() >= 0;
        result.emplace_back( std::move( dto ) );
        if( result.size() >= range.limit ) break;
    }
    return result;
}

std::vector<FrameDto> WorkerTraceSource::ScanFrames( const ScanRange& range ) const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<FrameDto> result;
    auto& worker = *m_impl->worker;
    size_t skipped = 0;
    const auto& sets = worker.GetFrames();
    for( size_t setIndex = 0; setIndex < sets.size(); setIndex++ )
    {
        const auto* set = sets[setIndex];
        for( size_t frameIndex = 0; frameIndex < worker.GetFrameCount( *set ); frameIndex++ )
        {
            const int64_t begin = worker.GetFrameBegin( *set, frameIndex );
            const bool complete = frameIndex < worker.GetFullFrameCount( *set );
            const int64_t end = complete ? worker.GetFrameEnd( *set, frameIndex ) : begin;
            if( !Intersects( begin, end, range ) ) continue;
            if( skipped++ < range.offset ) continue;
            FrameDto dto;
            dto.ref = m_impl->MakeRef( "frame", ( uint64_t( setIndex ) << 32 ) | frameIndex );
            dto.frameSetRef = m_impl->MakeRef( "frame-set", setIndex );
            dto.index = frameIndex;
            dto.beginNs = begin;
            if( complete ) dto.endNs = end;
            dto.complete = complete;
            if( const auto* image = worker.GetFrameImage( *set, frameIndex ) )
            {
                const auto& images = worker.GetFrameImages();
                for( size_t imageIndex = 0; imageIndex < images.size(); imageIndex++ )
                {
                    if( images[imageIndex].get() == image )
                    {
                        dto.imageRef = m_impl->MakeRef( "frame-image", imageIndex );
                        break;
                    }
                }
            }
            result.emplace_back( std::move( dto ) );
            if( result.size() >= range.limit ) return result;
        }
    }
    return result;
}

std::vector<MemoryEventDto> WorkerTraceSource::ScanMemoryEvents( const ScanRange& range ) const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<std::pair<uint64_t, const MemData*>> pools;
    for( const auto& value : m_impl->worker->GetMemNameMap() ) pools.emplace_back( value.first, value.second );
    std::sort( pools.begin(), pools.end(), [&]( const auto& lhs, const auto& rhs ) {
        return m_impl->memoryPoolIndex.at( lhs.first ) < m_impl->memoryPoolIndex.at( rhs.first );
    } );

    std::vector<MemoryEventDto> result;
    size_t skipped = 0;
    for( size_t poolIndex = 0; poolIndex < pools.size(); poolIndex++ )
    {
        const auto [poolId, memory] = pools[poolIndex];
        for( size_t eventIndex = 0; eventIndex < memory->data.size(); eventIndex++ )
        {
            const auto& event = memory->data[eventIndex];
            const int64_t end = event.TimeFree() >= 0 ? event.TimeFree() : m_impl->worker->GetLastTime();
            if( !Intersects( event.TimeAlloc(), end, range ) ) continue;
            if( skipped++ < range.offset ) continue;
            MemoryEventDto dto;
            dto.ref = m_impl->MakeRef( "memory-event", ( uint64_t( poolIndex ) << 40 ) | eventIndex );
            dto.poolRef = m_impl->MakeRef( "memory-pool", m_impl->memoryPoolIndex.at( poolId ) );
            dto.address = Hex( event.Ptr() );
            dto.size = event.Size();
            dto.allocationNs = event.TimeAlloc();
            if( event.TimeFree() >= 0 ) dto.freeNs = event.TimeFree();
            dto.allocationThreadRef = m_impl->MakeRef( "thread", m_impl->worker->DecompressThread( event.ThreadAlloc() ) );
            if( event.TimeFree() >= 0 ) dto.freeThreadRef = m_impl->MakeRef( "thread", m_impl->worker->DecompressThread( event.ThreadFree() ) );
            dto.allocationCallstack = event.CsAlloc();
            dto.freeCallstack = event.csFree.Val();
            dto.complete = event.TimeFree() >= 0;
            result.emplace_back( std::move( dto ) );
            if( result.size() >= range.limit ) return result;
        }
    }
    return result;
}

std::vector<MessageDto> WorkerTraceSource::ScanMessages( const ScanRange& range ) const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<MessageDto> result;
    size_t skipped = 0;
    const auto& messages = m_impl->worker->GetMessages();
    for( size_t index = 0; index < messages.size(); index++ )
    {
        const auto* message = messages[index].get();
        if( message->time < range.startNs || message->time >= range.endNs ) continue;
        if( skipped++ < range.offset ) continue;
        result.push_back( {
            m_impl->MakeRef( "message", index ),
            m_impl->MakeRef( "thread", m_impl->worker->DecompressThread( message->thread ) ),
            message->time, Safe( m_impl->worker->GetString( message->ref ) ), message->color, message->callstack.Val()
        } );
        if( result.size() >= range.limit ) break;
    }
    return result;
}

std::vector<PlotPointDto> WorkerTraceSource::ScanPlots( const ScanRange& range ) const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<PlotPointDto> result;
    size_t skipped = 0;
    const auto& plots = m_impl->worker->GetPlots();
    for( size_t plotIndex = 0; plotIndex < plots.size(); plotIndex++ )
    {
        const auto* plot = plots[plotIndex];
        for( size_t pointIndex = 0; pointIndex < plot->data.size(); pointIndex++ )
        {
            const auto& point = plot->data[pointIndex];
            if( point.time.Val() < range.startNs || point.time.Val() >= range.endNs ) continue;
            if( skipped++ < range.offset ) continue;
            result.push_back( {
                m_impl->MakeRef( "plot-point", ( uint64_t( plotIndex ) << 40 ) | pointIndex ),
                m_impl->MakeRef( "plot", plotIndex ), point.time.Val(), point.val
            } );
            if( result.size() >= range.limit ) return result;
        }
    }
    return result;
}

std::vector<std::string> WorkerTraceSource::ScanLocks( const ScanRange& range ) const
{
    std::vector<std::string> result;
    const auto locks = GetLocks();
    for( size_t index = range.offset; index < locks.size() && result.size() < range.limit; index++ ) result.emplace_back( locks[index].ref );
    return result;
}

std::vector<std::string> WorkerTraceSource::ScanContextSwitches( const ScanRange& range ) const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<std::string> result;
    size_t skipped = 0;
    auto& worker = *m_impl->worker;
    for( const auto* thread : worker.GetThreadData() )
    {
        const auto* data = worker.GetContextSwitchData( thread->id );
        if( !data ) continue;
        for( size_t index = 0; index < data->v.size(); index++ )
        {
            const auto& event = data->v[index];
            const int64_t end = event.IsEndValid() ? event.End() : event.Start();
            if( !Intersects( event.Start(), end, range ) ) continue;
            if( skipped++ < range.offset ) continue;
            result.emplace_back( m_impl->MakeRef( "context-switch", ( thread->id << 24 ) ^ index ) );
            if( result.size() >= range.limit ) return result;
        }
    }
    return result;
}

std::vector<std::string> WorkerTraceSource::ScanSamples( const ScanRange& range ) const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<std::string> result;
    size_t skipped = 0;
    for( const auto* thread : m_impl->worker->GetThreadData() )
    {
        for( size_t index = 0; index < thread->samples.size(); index++ )
        {
            const auto& sample = thread->samples[index];
            if( sample.time.Val() < range.startNs || sample.time.Val() >= range.endNs ) continue;
            if( skipped++ < range.offset ) continue;
            result.emplace_back( m_impl->MakeRef( "sample", ( thread->id << 24 ) ^ index ) );
            if( result.size() >= range.limit ) return result;
        }
    }
    return result;
}

std::vector<CallstackFrameDto> WorkerTraceSource::ResolveCallstacks( const std::vector<uint32_t>& callstacks, size_t maxDepth ) const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<CallstackFrameDto> result;
    auto& worker = *m_impl->worker;
    for( const auto callstack : callstacks )
    {
        if( callstack == 0 || callstack > worker.GetCallstackPayloadCount() ) continue;
        const auto& entries = worker.GetCallstack( callstack );
        size_t depth = 0;
        for( const auto& entry : entries )
        {
            if( depth >= maxDepth ) break;
            const auto* frameData = worker.GetCallstackFrame( entry );
            if( !frameData )
            {
                result.push_back( { m_impl->MakeRef( "callstack-frame", ( uint64_t( callstack ) << 32 ) | depth++ ), "", "", 0, Hex( worker.GetCanonicalPointer( entry ) ), "0x0", false } );
                continue;
            }
            for( uint8_t frameIndex = 0; frameIndex < frameData->size && depth < maxDepth; frameIndex++ )
            {
                const auto& frame = frameData->data[frameIndex];
                result.push_back( {
                    m_impl->MakeRef( "callstack-frame", ( uint64_t( callstack ) << 32 ) | depth ),
                    Safe( worker.GetString( frame.name ) ), Safe( worker.GetString( frame.file ) ), frame.line,
                    Hex( worker.GetCanonicalPointer( entry ) ), Hex( frame.symAddr ), frameIndex + 1 != frameData->size
                } );
                depth++;
            }
        }
    }
    return result;
}

std::vector<SourceTextDto> WorkerTraceSource::ResolveSources( const std::vector<std::string>& sourceRefs, size_t maxBytes ) const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<SourceTextDto> result;
    for( const auto& [path, block] : m_impl->worker->GetSourceFileCache() )
    {
        const auto ref = m_impl->MakeRef( "source-file", std::hash<std::string_view>{}( path ) );
        if( std::find( sourceRefs.begin(), sourceRefs.end(), ref ) == sourceRefs.end() ) continue;
        const size_t size = std::min( size_t( block.len ), maxBytes );
        result.push_back( { ref, path, std::string( block.data, size ), true, size < block.len } );
    }
    return result;
}

std::vector<SymbolCodeDto> WorkerTraceSource::ResolveSymbols( const std::vector<std::string>& symbolRefs, size_t maxBytes ) const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<SymbolCodeDto> result;
    for( const auto& [address, symbol] : m_impl->worker->GetSymbolMap() )
    {
        const auto ref = m_impl->MakeRef( "symbol", address );
        if( std::find( symbolRefs.begin(), symbolRefs.end(), ref ) == symbolRefs.end() || !m_impl->worker->HasSymbolCode( address ) ) continue;
        uint32_t length = 0;
        const char* code = m_impl->worker->GetSymbolCode( address, length );
        const size_t size = std::min( size_t( length ), maxBytes );
        SymbolCodeDto dto { ref, Hex( address ), {}, size < length };
        dto.bytes.assign( reinterpret_cast<const uint8_t*>( code ), reinterpret_cast<const uint8_t*>( code ) + size );
        result.emplace_back( std::move( dto ) );
    }
    return result;
}

std::vector<FrameImageDto> WorkerTraceSource::ResolveFrameImages( const std::vector<std::string>& imageRefs, size_t maxBytes ) const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<FrameImageDto> result;
    const auto& images = m_impl->worker->GetFrameImages();
    for( size_t index = 0; index < images.size(); index++ )
    {
        const auto ref = m_impl->MakeRef( "frame-image", index );
        if( std::find( imageRefs.begin(), imageRefs.end(), ref ) == imageRefs.end() ) continue;
        const auto* image = images[index].get();
        const size_t outputSize = size_t( image->w ) * image->h * 4;
        if( outputSize > maxBytes ) throw std::runtime_error( "decoded frame image exceeds resource budget" );
        FrameImageDto dto;
        dto.ref = ref;
        dto.width = image->w;
        dto.height = image->h;
        dto.flipped = image->flip != 0;
        DecodeBc1( reinterpret_cast<const uint8_t*>( m_impl->worker->UnpackFrameImage( *image ) ), image->w, image->h, dto.rgba );
        result.emplace_back( std::move( dto ) );
    }
    return result;
}

}
