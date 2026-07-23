#include "TracyWorkerTraceSource.hpp"

#include "TracyHash.hpp"
#include "TracyFileHeader.hpp"
#include "TracyFileRead.hpp"
#include "TracyWorker.hpp"

#include <capstone.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
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

const char* CpuArchitectureName( CpuArchitecture value )
{
    switch( value )
    {
    case CpuArchX86: return "x86";
    case CpuArchX64: return "x86_64";
    case CpuArchArm32: return "arm";
    case CpuArchArm64: return "aarch64";
    case CpuArchUnknown: break;
    }
    return "unknown";
}

const char* GpuContextTypeName( GpuContextType value )
{
    switch( value )
    {
    case GpuContextType::Invalid: return "invalid";
    case GpuContextType::OpenGl: return "opengl";
    case GpuContextType::Vulkan: return "vulkan";
    case GpuContextType::OpenCL: return "opencl";
    case GpuContextType::Direct3D12: return "direct3d12";
    case GpuContextType::Direct3D11: return "direct3d11";
    case GpuContextType::Metal: return "metal";
    case GpuContextType::Custom: return "custom";
    case GpuContextType::CUDA: return "cuda";
    case GpuContextType::Rocprof: return "rocprof";
    }
    return "unknown";
}

const char* LockTypeName( LockType value )
{
    switch( value )
    {
    case LockType::Lockable: return "lockable";
    case LockType::SharedLockable: return "shared_lockable";
    }
    return "unknown";
}

const char* ContextSwitchReasonName( int8_t value )
{
    switch( value )
    {
    case ContextSwitchData::Wakeup: return "wakeup";
    case ContextSwitchData::Fiber: return "fiber";
    case ContextSwitchData::NoState: return "no_state";
    case ContextSwitchData::Win32_Executive: return "executive";
    case ContextSwitchData::Win32_FreePage: return "free_page";
    case ContextSwitchData::Win32_PageIn: return "page_in";
    case ContextSwitchData::Win32_PoolAllocation: return "pool_allocation";
    case ContextSwitchData::Win32_DelayExecution: return "delay_execution";
    case ContextSwitchData::Win32_Suspended: return "suspended";
    case ContextSwitchData::Win32_UserRequest: return "user_request";
    case ContextSwitchData::Win32_WrExecutive: return "wr_executive";
    case ContextSwitchData::Win32_WrFreePage: return "wr_free_page";
    case ContextSwitchData::Win32_WrPageIn: return "wr_page_in";
    case ContextSwitchData::Win32_WrPoolAllocation: return "wr_pool_allocation";
    case ContextSwitchData::Win32_WrDelayExecution: return "wr_delay_execution";
    case ContextSwitchData::Win32_WrSuspended: return "wr_suspended";
    case ContextSwitchData::Win32_WrUserRequest: return "wr_user_request";
    case ContextSwitchData::Win32_WrEventPair: return "wr_event_pair";
    case ContextSwitchData::Win32_WrQueue: return "wr_queue";
    case ContextSwitchData::Win32_WrLpcReceive: return "wr_lpc_receive";
    case ContextSwitchData::Win32_WrLpcReply: return "wr_lpc_reply";
    case ContextSwitchData::Win32_WrVirtualMemory: return "wr_virtual_memory";
    case ContextSwitchData::Win32_WrPageOut: return "wr_page_out";
    case ContextSwitchData::Win32_WrRendezvous: return "wr_rendezvous";
    case ContextSwitchData::Win32_WrKeyedEvent: return "wr_keyed_event";
    case ContextSwitchData::Win32_WrTerminated: return "wr_terminated";
    case ContextSwitchData::Win32_WrProcessInSwap: return "wr_process_in_swap";
    case ContextSwitchData::Win32_WrCpuRateControl: return "wr_cpu_rate_control";
    case ContextSwitchData::Win32_WrCalloutStack: return "wr_callout_stack";
    case ContextSwitchData::Win32_WrKernel: return "wr_kernel";
    case ContextSwitchData::Win32_WrResource: return "wr_resource";
    case ContextSwitchData::Win32_WrPushLock: return "wr_push_lock";
    case ContextSwitchData::Win32_WrMutex: return "wr_mutex";
    case ContextSwitchData::Win32_WrQuantumEnd: return "wr_quantum_end";
    case ContextSwitchData::Win32_WrDispatchInt: return "wr_dispatch_interrupt";
    case ContextSwitchData::Win32_WrPreempted: return "wr_preempted";
    case ContextSwitchData::Win32_WrYieldExecution: return "wr_yield_execution";
    case ContextSwitchData::Win32_WrFastMutex: return "wr_fast_mutex";
    case ContextSwitchData::Win32_WrGuardedMutex: return "wr_guarded_mutex";
    case ContextSwitchData::Win32_WrRundown: return "wr_rundown";
    case ContextSwitchData::Win32_WrAlertByThreadId: return "wr_alert_by_thread_id";
    case ContextSwitchData::Win32_WrDeferredPreempt: return "wr_deferred_preempt";
    case ContextSwitchData::Win32_WrPhysicalFault: return "wr_physical_fault";
    case ContextSwitchData::Win32_WrIoRing: return "wr_io_ring";
    case ContextSwitchData::Win32_WrMdlCache: return "wr_mdl_cache";
    case ContextSwitchData::Win32_WrRcu: return "wr_rcu";
    }
    return "unknown";
}

const char* ContextSwitchStateName( int8_t value )
{
    switch( value )
    {
    case 0: return "initialized";
    case 1: return "ready";
    case 2: return "running";
    case 3: return "standby";
    case 4: return "terminated";
    case 5: return "waiting";
    case 6: return "transition";
    case 7: return "deferred_ready";
    case 101: return "disk_sleep";
    case 102: return "idle";
    case 103: return "run_queue";
    case 104: return "sleeping";
    case 105: return "stopped";
    case 106: return "tracing_stop";
    case 107: return "paging";
    case 108: return "dead";
    case 109: return "zombie";
    case 110: return "parked";
    }
    return "unknown";
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
    return begin < range.endNs && end > range.startNs;
}

BinaryResourceChunkDto BinaryChunk( std::string ref, const uint8_t* data, size_t totalBytes, size_t offset, size_t maxBytes )
{
    BinaryResourceChunkDto result;
    result.ref = std::move( ref );
    result.offset = offset;
    result.totalBytes = totalBytes;
    if( offset < totalBytes )
    {
        const auto size = std::min( maxBytes, totalBytes - offset );
        result.bytes.assign( data + offset, data + offset + size );
    }
    result.eof = offset >= totalBytes || result.bytes.size() >= totalBytes - offset;
    return result;
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

    std::optional<uint64_t> ParseRef( std::string_view ref, std::string_view kind ) const
    {
        const std::string prefix = "tracy:v1:" + fingerprint.substr( 0, 16 ) + ':' + std::string( kind ) + ':';
        if( !ref.starts_with( prefix ) ) return std::nullopt;
        uint64_t value = 0;
        const auto begin = ref.data() + prefix.size();
        const auto end = ref.data() + ref.size();
        const auto parsed = std::from_chars( begin, end, value, 16 );
        if( parsed.ec != std::errc() || parsed.ptr != end ) return std::nullopt;
        return value;
    }

    SourceLocationDto SourceLocation( int16_t id ) const
    {
        const auto& source = worker->GetSourceLocation( id );
        return {
            MakeRef( "source", uint16_t( id ) ),
            Safe( source.name.active ? worker->TryGetString( source.name ) : nullptr ),
            Safe( worker->TryGetString( source.function ) ),
            Safe( worker->TryGetString( source.file ) ),
            source.line,
            source.color,
            id,
            id < 0
        };
    }

    int64_t CpuChildTime( const ZoneEvent& zone ) const
    {
        int64_t time = 0;
        if( !zone.HasChildren() ) return time;
        ForEachCpuZone( worker->GetZoneChildren( zone.Child() ), [&]( const ZoneEvent* child ) {
            const auto end = worker->GetZoneEnd( *child );
            if( end >= child->Start() ) time += end - child->Start();
        } );
        return time;
    }

    int64_t GpuChildTime( const GpuEvent& zone ) const
    {
        int64_t time = 0;
        if( zone.Child() < 0 ) return time;
        ForEachGpuZone( worker->GetGpuChildren( zone.Child() ), [&]( const GpuEvent* child ) {
            const auto end = worker->GetZoneEnd( *child );
            if( end >= child->GpuStart() ) time += end - child->GpuStart();
        } );
        return time;
    }

    std::pair<std::optional<int64_t>, uint64_t> CpuRunningTime( uint64_t thread, const ZoneEvent& zone ) const
    {
        const auto* context = worker->GetContextSwitchData( thread );
        if( !context || !zone.IsEndValid() ) return { std::nullopt, 0 };
        int64_t running = 0;
        uint64_t regions = 0;
        const int64_t begin = zone.Start();
        const int64_t end = zone.End();
        for( const auto& event : context->v )
        {
            if( !event.IsEndValid() ) continue;
            if( event.End() <= begin ) continue;
            if( event.Start() >= end ) break;
            const auto overlapBegin = std::max( begin, event.Start() );
            const auto overlapEnd = std::min( end, event.End() );
            if( overlapEnd > overlapBegin )
            {
                running += overlapEnd - overlapBegin;
                regions++;
            }
        }
        return regions == 0 ? std::pair<std::optional<int64_t>, uint64_t> { std::nullopt, 0 } : std::pair<std::optional<int64_t>, uint64_t> { running, regions };
    }

    CpuZoneDto CpuDto( size_t index ) const
    {
        const auto& entry = cpuZones.at( index );
        const auto* zone = entry.zone;
        const auto& sourceData = worker->GetSourceLocation( zone->SrcLoc() );
        const auto source = SourceLocation( zone->SrcLoc() );
        CpuZoneDto dto;
        dto.ref = MakeRef( "cpu-zone", entry.index );
        dto.threadRef = MakeRef( "thread", entry.thread );
        dto.sourceLocationRef = source.ref;
        dto.extraIndex = zone->extra;
        const char* zoneName = nullptr;
        if( worker->HasZoneExtra( *zone ) )
        {
            if( worker->HasValidZoneExtra( *zone ) )
            {
                const auto& extra = worker->GetZoneExtra( *zone );
                dto.extraColor = extra.color.Val();
                if( extra.name.Active() )
                {
                    zoneName = worker->TryGetString( extra.name );
                    dto.extraName = Safe( zoneName );
                    dto.nameResolved = zoneName != nullptr;
                }
                if( extra.text.Active() ) dto.extraText = Safe( worker->TryGetString( extra.text ) );
            }
            else
            {
                dto.extraValid = false;
                dto.nameResolved = false;
            }
        }
        if( !zoneName )
        {
            zoneName = sourceData.name.active ? worker->TryGetString( sourceData.name ) : worker->TryGetString( sourceData.function );
            if( !zoneName ) dto.nameResolved = false;
        }
        dto.name = Safe( zoneName );
        dto.function = source.function;
        dto.file = source.file;
        dto.line = source.line;
        if( entry.parent ) dto.parentRef = MakeRef( "cpu-zone", *entry.parent );
        dto.startNs = zone->Start();
        dto.complete = zone->IsEndValid();
        if( dto.complete )
        {
            dto.endNs = zone->End();
            dto.selfTimeNs = std::max<int64_t>( 0, zone->End() - zone->Start() - CpuChildTime( *zone ) );
            const auto running = CpuRunningTime( entry.thread, *zone );
            dto.runningTimeNs = running.first;
            dto.runningRegions = running.second;
        }
        if( zone->HasChildren() ) dto.childCount = uint32_t( worker->GetZoneChildren( zone->Child() ).size() );
        if( worker->HasValidZoneExtra( *zone ) ) dto.callstack = worker->GetZoneExtra( *zone ).callstack.Val();
        if( dto.callstack != 0 ) dto.callstackRef = MakeRef( "callstack", dto.callstack );
        return dto;
    }

    GpuZoneDto GpuDto( size_t index ) const
    {
        const auto& entry = gpuZones.at( index );
        const auto* zone = entry.zone;
        const auto source = SourceLocation( zone->SrcLoc() );
        GpuZoneDto dto;
        dto.ref = MakeRef( "gpu-zone", entry.index );
        dto.contextRef = MakeRef( "gpu-context", entry.context );
        dto.threadRef = MakeRef( "thread", entry.thread );
        dto.sourceLocationRef = source.ref;
        dto.name = source.name.empty() ? source.function : source.name;
        dto.function = source.function;
        dto.file = source.file;
        dto.line = source.line;
        if( entry.parent ) dto.parentRef = MakeRef( "gpu-zone", *entry.parent );
        dto.gpuStartNs = zone->GpuStart();
        if( zone->GpuEnd() >= 0 )
        {
            dto.gpuEndNs = zone->GpuEnd();
            dto.selfTimeNs = std::max<int64_t>( 0, zone->GpuEnd() - zone->GpuStart() - GpuChildTime( *zone ) );
        }
        dto.cpuStartNs = zone->CpuStart();
        if( zone->CpuEnd() >= 0 ) dto.cpuEndNs = zone->CpuEnd();
        if( zone->Child() >= 0 ) dto.childCount = uint32_t( worker->GetGpuChildren( zone->Child() ).size() );
        dto.callstack = zone->callstack.Val();
        if( dto.callstack != 0 ) dto.callstackRef = MakeRef( "callstack", dto.callstack );
        dto.complete = zone->GpuEnd() >= 0 && zone->CpuEnd() >= 0;
        dto.queryId = zone->query_id;
        dto.queryIdAvailability.available = worker->GetTraceVersion() >= FileVersion( 0, 12, 4 );
        if( !dto.queryIdAvailability.available ) dto.queryIdAvailability.reason = "gpu query IDs were not persisted before Tracy 0.12.4";
        return dto;
    }

    std::optional<MemoryEventDto> MemoryDto( const MemoryEventKey& key ) const
    {
        const auto poolIndex = memoryPoolIndex.find( key.pool );
        if( poolIndex == memoryPoolIndex.end() ) return std::nullopt;
        const auto& memory = worker->GetMemoryNamed( key.pool );
        if( key.index >= memory.data.size() ) return std::nullopt;
        const auto& event = memory.data[key.index];
        MemoryEventDto dto;
        dto.ref = MakeRef( "memory-event", ( uint64_t( poolIndex->second ) << 40 ) | key.index );
        dto.poolRef = MakeRef( "memory-pool", poolIndex->second );
        const auto poolName = key.pool == 0 ? std::string( "Default allocator" ) : Safe( worker->GetString( key.pool ) );
        dto.address = IsGpuD3D12PoolName( poolName ) ? std::to_string( event.Ptr() ) : Hex( event.Ptr() ); dto.size = event.Size(); dto.allocationNs = event.TimeAlloc();
        if( event.TimeFree() >= 0 ) dto.freeNs = event.TimeFree();
        dto.allocationThreadRef = MakeRef( "thread", worker->DecompressThread( event.ThreadAlloc() ) );
        if( event.TimeFree() >= 0 ) dto.freeThreadRef = MakeRef( "thread", worker->DecompressThread( event.ThreadFree() ) );
        dto.allocationCallstack = event.CsAlloc(); dto.freeCallstack = event.csFree.Val(); dto.complete = event.TimeFree() >= 0;
        if( dto.allocationCallstack != 0 ) dto.allocationCallstackRef = MakeRef( "callstack", dto.allocationCallstack );
        if( dto.freeCallstack != 0 ) dto.freeCallstackRef = MakeRef( "callstack", dto.freeCallstack );
        if( const auto zone = FindCpuZoneAtTime( worker->DecompressThread( event.ThreadAlloc() ), event.TimeAlloc() ) ) dto.allocationZoneRef = MakeRef( "cpu-zone", *zone );
        if( event.TimeFree() >= 0 ) if( const auto zone = FindCpuZoneAtTime( worker->DecompressThread( event.ThreadFree() ), event.TimeFree() ) ) dto.freeZoneRef = MakeRef( "cpu-zone", *zone );
        return dto;
    }

    std::optional<size_t> FindCpuZoneAtTime( uint64_t thread, int64_t time ) const
    {
        const auto found = cpuZonesByThread.find( thread );
        if( found == cpuZonesByThread.end() ) return std::nullopt;
        const auto& indices = found->second;
        auto it = std::upper_bound( indices.begin(), indices.end(), time, [&]( int64_t value, size_t index ) { return value < cpuZones[index].zone->Start(); } );
        while( it != indices.begin() )
        {
            --it;
            const auto* zone = cpuZones[*it].zone;
            if( zone->Start() <= time && worker->GetZoneEnd( *zone ) >= time ) return *it;
        }
        return std::nullopt;
    }

    void IndexCpuVector( const Vector<short_ptr<ZoneEvent>>& zones, uint64_t thread, std::optional<size_t> parent )
    {
        ForEachCpuZone( zones, [&]( const ZoneEvent* zone ) {
            const size_t index = cpuZones.size();
            cpuZones.push_back( { zone, thread, index, parent } );
            cpuLookup.emplace( zone, index );
            cpuZonesByThread[thread].emplace_back( index );
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
        for( auto& [thread, indices] : cpuZonesByThread ) std::sort( indices.begin(), indices.end(), [&]( size_t lhs, size_t rhs ) {
            return cpuZones[lhs].zone->Start() != cpuZones[rhs].zone->Start() ? cpuZones[lhs].zone->Start() < cpuZones[rhs].zone->Start() : lhs < rhs;
        } );

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
        for( size_t index = 0; index < pools.size(); index++ )
        {
            memoryPoolIndex.emplace( pools[index].second, index );
            memoryPools.emplace_back( pools[index].second );
        }

        for( const auto& [sourcePath, block] : worker->GetSourceFileCache() ) sourceFiles.emplace_back( sourcePath );
        std::sort( sourceFiles.begin(), sourceFiles.end(), []( const auto* lhs, const auto* rhs ) { return std::strcmp( lhs, rhs ) < 0; } );

        for( const auto& [address, symbol] : worker->GetSymbolMap() ) symbols.emplace_back( address );
        std::sort( symbols.begin(), symbols.end() );
    }

    std::filesystem::path path;
    std::string fingerprint;
    std::unique_ptr<FileRead> file;
    std::unique_ptr<Worker> worker;
    std::vector<CpuEntry> cpuZones;
    std::vector<GpuEntry> gpuZones;
    std::unordered_map<const ZoneEvent*, size_t> cpuLookup;
    std::unordered_map<uint64_t, std::vector<size_t>> cpuZonesByThread;
    std::unordered_map<const GpuEvent*, size_t> gpuLookup;
    std::unordered_map<uint64_t, size_t> memoryPoolIndex;
    std::vector<uint64_t> memoryPools;
    std::vector<const char*> sourceFiles;
    std::vector<uint64_t> symbols;
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

WorkerLoadProgress WorkerTraceSource::GetLoadProgress()
{
    const auto& value = Worker::GetLoadProgress();
    static constexpr const char* stages[] = {
        "initialization", "locks", "messages", "zones", "gpu_zones", "plots", "memory", "callstacks", "frame_images", "context_switches", "context_switches_per_cpu",
        "thread_pid_map", "cpu_thread_data", "symbols", "symbol_code", "hardware_samples", "source_cache"
    };
    const auto stage = value.progress.load( std::memory_order_relaxed );
    return {
        stage < sizeof( stages ) / sizeof( stages[0] ) ? stages[stage] : "unknown",
        stage, value.total.load( std::memory_order_relaxed ),
        value.subProgress.load( std::memory_order_relaxed ), value.subTotal.load( std::memory_order_relaxed )
    };
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
        while( !impl->worker->IsBackgroundDone() ) std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
        if( stateCallback ) stateCallback( TraceSourceState::Indexing );
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
    catch( const NotTracyDump& )
    {
        throw TraceLoadError( TraceLoadErrorCode::Corrupt, "file does not contain a valid Tracy stream header" );
    }
    catch( const FileReadError& )
    {
        throw TraceLoadError( TraceLoadErrorCode::Corrupt, "trace ended before all declared records could be read" );
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

std::vector<SourceResourceDto> WorkerTraceSource::GetSourceResources() const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<SourceResourceDto> result;
    result.reserve( m_impl->sourceFiles.size() );
    for( size_t index = 0; index < m_impl->sourceFiles.size(); index++ )
    {
        const auto* path = m_impl->sourceFiles[index];
        const auto block = m_impl->worker->GetSourceFileFromCache( path );
        SourceResourceDto dto { index, m_impl->MakeRef( "source-file", index ), path, block.len };
        const auto pathLength = std::strlen( path );
        dto.pathBytes.assign( reinterpret_cast<const uint8_t*>( path ), reinterpret_cast<const uint8_t*>( path ) + pathLength );
        result.emplace_back( std::move( dto ) );
    }
    return result;
}

std::vector<SymbolResourceDto> WorkerTraceSource::GetSymbolResources() const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<SymbolResourceDto> result;
    for( const auto address : m_impl->symbols )
    {
        const auto* symbol = m_impl->worker->GetSymbolData( address );
        if( !symbol ) continue;
        uint32_t codeLength = 0;
        if( m_impl->worker->HasSymbolCode( address ) ) m_impl->worker->GetSymbolCode( address, codeLength );
        result.push_back( {
            address, m_impl->MakeRef( "symbol", address ), Safe( m_impl->worker->TryGetString( symbol->name ) ),
            Safe( m_impl->worker->TryGetString( symbol->file ) ), symbol->line, codeLength
        } );
    }
    return result;
}

std::vector<FrameImageMetadataDto> WorkerTraceSource::GetFrameImageResources() const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<FrameImageMetadataDto> result;
    const auto& images = m_impl->worker->GetFrameImages();
    result.reserve( images.size() );
    for( size_t index = 0; index < images.size(); index++ )
    {
        const auto* image = images[index].get();
        FrameImageMetadataDto dto;
        dto.id = index;
        dto.ref = m_impl->MakeRef( "frame-image", index );
        dto.width = image->w;
        dto.height = image->h;
        dto.flipped = image->flip != 0;
        dto.rawFrameIndex = image->frameRef;
        const auto* frameSet = m_impl->worker->GetFramesBase();
        if( frameSet && image->frameRef < m_impl->worker->GetFrameCount( *frameSet ) ) dto.frameRef = m_impl->MakeRef( "frame", image->frameRef );
        dto.rawBc1Bytes = uint64_t( image->w ) * image->h / 2;
        result.emplace_back( std::move( dto ) );
    }
    return result;
}

std::optional<CpuZoneDto> WorkerTraceSource::GetCpuZone( std::string_view ref ) const
{
    std::lock_guard lock( m_impl->readMutex );
    const auto index = m_impl->ParseRef( ref, "cpu-zone" );
    if( !index || *index >= m_impl->cpuZones.size() ) return std::nullopt;
    return m_impl->CpuDto( size_t( *index ) );
}

std::optional<GpuZoneDto> WorkerTraceSource::GetGpuZone( std::string_view ref ) const
{
    std::lock_guard lock( m_impl->readMutex );
    const auto index = m_impl->ParseRef( ref, "gpu-zone" );
    if( !index || *index >= m_impl->gpuZones.size() ) return std::nullopt;
    return m_impl->GpuDto( size_t( *index ) );
}

std::vector<CpuZoneDto> WorkerTraceSource::GetCpuZoneChildren( std::string_view ref, size_t offset, size_t limit ) const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<CpuZoneDto> result;
    const auto index = m_impl->ParseRef( ref, "cpu-zone" );
    if( !index || *index >= m_impl->cpuZones.size() ) return result;
    const auto* zone = m_impl->cpuZones[size_t( *index )].zone;
    if( !zone->HasChildren() ) return result;
    size_t position = 0;
    ForEachCpuZone( m_impl->worker->GetZoneChildren( zone->Child() ), [&]( const ZoneEvent* child ) {
        if( position++ < offset || result.size() >= limit ) return;
        const auto found = m_impl->cpuLookup.find( child );
        if( found != m_impl->cpuLookup.end() ) result.emplace_back( m_impl->CpuDto( found->second ) );
    } );
    return result;
}

std::vector<GpuZoneDto> WorkerTraceSource::GetGpuZoneChildren( std::string_view ref, size_t offset, size_t limit ) const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<GpuZoneDto> result;
    const auto index = m_impl->ParseRef( ref, "gpu-zone" );
    if( !index || *index >= m_impl->gpuZones.size() ) return result;
    const auto* zone = m_impl->gpuZones[size_t( *index )].zone;
    if( zone->Child() < 0 ) return result;
    size_t position = 0;
    ForEachGpuZone( m_impl->worker->GetGpuChildren( zone->Child() ), [&]( const GpuEvent* child ) {
        if( position++ < offset || result.size() >= limit ) return;
        const auto found = m_impl->gpuLookup.find( child );
        if( found != m_impl->gpuLookup.end() ) result.emplace_back( m_impl->GpuDto( found->second ) );
    } );
    return result;
}

MemoryFrameSnapshot WorkerTraceSource::GetMemoryFrameSnapshot( size_t frameSetIndex, size_t frameIndex, const std::vector<std::string>& poolRefs, bool allGpuD3D12Pools ) const
{
    std::lock_guard lock( m_impl->readMutex );
    const auto& frameSets = m_impl->worker->GetFrames();
    if( frameSetIndex >= frameSets.size() ) return {};
    const auto* frameSet = frameSets[frameSetIndex];
    if( frameIndex >= m_impl->worker->GetFullFrameCount( *frameSet ) ) return {};

    std::vector<uint64_t> pools;
    if( allGpuD3D12Pools )
    {
        for( const auto pool : m_impl->memoryPools )
        {
            const auto name = pool == 0 ? std::string( "Default allocator" ) : Safe( m_impl->worker->GetString( pool ) );
            if( IsGpuD3D12PoolName( name ) ) pools.emplace_back( pool );
        }
    }
    else if( !poolRefs.empty() )
    {
        for( const auto& ref : poolRefs )
        {
            const auto index = m_impl->ParseRef( ref, "memory-pool" );
            if( index && *index < m_impl->memoryPools.size() ) pools.emplace_back( m_impl->memoryPools[size_t( *index )] );
        }
    }
    else pools = m_impl->memoryPools;

    std::vector<MemoryEventInput> events;
    for( const auto pool : pools )
    {
        const auto& memory = m_impl->worker->GetMemoryNamed( pool );
        events.reserve( events.size() + memory.data.size() );
        for( size_t index = 0; index < memory.data.size(); index++ )
        {
            const auto& event = memory.data[index];
            MemoryEventInput input;
            input.key = { pool, index }; input.identifier = event.Ptr(); input.size = event.Size(); input.allocationNs = event.TimeAlloc();
            if( event.TimeFree() >= 0 ) input.freeNs = event.TimeFree();
            input.allocationThread = m_impl->worker->DecompressThread( event.ThreadAlloc() );
            if( event.TimeFree() >= 0 ) input.freeThread = m_impl->worker->DecompressThread( event.ThreadFree() );
            input.allocationCallstack = event.CsAlloc(); input.freeCallstack = event.csFree.Val();
            events.emplace_back( input );
        }
    }
    const auto begin = m_impl->worker->GetFrameBegin( *frameSet, frameIndex );
    const auto end = m_impl->worker->GetFrameEnd( *frameSet, frameIndex );
    constexpr int64_t BaselineWindow = 100 * 1000 * 1000;
    const bool possibleBaseline = m_impl->worker->IsOnDemand() && begin <= m_impl->worker->GetFirstTime() + BaselineWindow && end > m_impl->worker->GetFirstTime();
    return BuildMemoryFrameSnapshot( begin, end, pools, events, possibleBaseline );
}

std::optional<MemoryEventDto> WorkerTraceSource::GetMemoryEvent( const MemoryEventKey& key ) const
{
    std::lock_guard lock( m_impl->readMutex );
    return m_impl->MemoryDto( key );
}

std::optional<std::string> WorkerTraceSource::GetMemoryPoolRef( uint64_t internalPoolKey ) const
{
    std::lock_guard lock( m_impl->readMutex );
    const auto found = m_impl->memoryPoolIndex.find( internalPoolKey );
    if( found == m_impl->memoryPoolIndex.end() ) return std::nullopt;
    return m_impl->MakeRef( "memory-pool", found->second );
}

std::optional<std::string> WorkerTraceSource::GetCpuZoneRef( uint64_t internalZoneIndex ) const
{
    std::lock_guard lock( m_impl->readMutex );
    if( internalZoneIndex >= m_impl->cpuZones.size() ) return std::nullopt;
    return m_impl->MakeRef( "cpu-zone", internalZoneIndex );
}

std::optional<std::string> WorkerTraceSource::GetGpuZoneRef( uint64_t internalZoneIndex ) const
{
    std::lock_guard lock( m_impl->readMutex );
    if( internalZoneIndex >= m_impl->gpuZones.size() ) return std::nullopt;
    return m_impl->MakeRef( "gpu-zone", internalZoneIndex );
}

std::string WorkerTraceSource::MakeEntityRef( std::string_view kind, uint64_t id ) const
{
    std::lock_guard lock( m_impl->readMutex );
    const std::string value( kind );
    return m_impl->MakeRef( value.c_str(), id );
}

std::optional<uint64_t> WorkerTraceSource::ParseEntityRef( std::string_view ref, std::string_view kind ) const
{
    std::lock_guard lock( m_impl->readMutex );
    return m_impl->ParseRef( ref, kind );
}

GpuMemoryAttribution WorkerTraceSource::GetGpuMemoryAttribution() const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<GpuMemoryCpuZoneInput> cpuInputs;
    for( const auto& entry : m_impl->cpuZones )
    {
        const auto* zone = entry.zone;
        if( !zone->IsEndValid() ) continue;
        const auto& location = m_impl->worker->GetSourceLocation( zone->SrcLoc() );
        const auto marker = Safe( location.name.active ? m_impl->worker->TryGetString( location.name ) : m_impl->worker->TryGetString( location.function ) );
        if( marker != GpuMemoryRequestMarker && marker != GpuMemoryPassMarker ) continue;
        if( !m_impl->worker->HasValidZoneExtra( *zone ) ) continue;
        const auto& extra = m_impl->worker->GetZoneExtra( *zone );
        if( !extra.text.Active() ) continue;
        const auto* text = m_impl->worker->TryGetString( extra.text );
        if( !text ) continue;
        const auto* displayName = extra.name.Active() ? m_impl->worker->TryGetString( extra.name ) : nullptr;
        if( !displayName ) displayName = marker.c_str();
        cpuInputs.push_back( {
            entry.index, marker, Safe( displayName ), text,
            entry.thread, zone->Start(), m_impl->worker->GetZoneEnd( *zone )
        } );
    }

    std::vector<GpuMemoryGpuZoneInput> gpuInputs;
    gpuInputs.reserve( m_impl->gpuZones.size() );
    for( const auto& entry : m_impl->gpuZones )
    {
        const auto* zone = entry.zone;
        if( zone->GpuEnd() < 0 ) continue;
        const auto thread = zone->Thread() != 0 ? m_impl->worker->DecompressThread( zone->Thread() ) : entry.thread;
        const auto source = m_impl->SourceLocation( zone->SrcLoc() );
        gpuInputs.push_back( { entry.index, source.name.empty() ? source.function : source.name, thread, zone->CpuStart(), zone->GpuStart(), zone->GpuEnd() } );
    }

    std::vector<GpuMemoryAllocationInput> allocations;
    for( const auto pool : m_impl->memoryPools )
    {
        const auto name = pool == 0 ? std::string( "Default allocator" ) : Safe( m_impl->worker->GetString( pool ) );
        if( !IsGpuD3D12PoolName( name ) ) continue;
        const auto& memory = m_impl->worker->GetMemoryNamed( pool );
        for( size_t index = 0; index < memory.data.size(); index++ )
        {
            const auto& event = memory.data[index];
            if( event.Ptr() == 0 ) continue;
            allocations.push_back( { { pool, index }, event.Ptr(), event.Size(), m_impl->worker->DecompressThread( event.ThreadAlloc() ), event.TimeAlloc() } );
        }
    }
    return BuildGpuMemoryAttribution( cpuInputs, gpuInputs, allocations );
}

SourceTextDto WorkerTraceSource::ReadEmbeddedSource( size_t sourceId, size_t maxBytes ) const
{
    std::lock_guard lock( m_impl->readMutex );
    if( sourceId >= m_impl->sourceFiles.size() ) throw std::out_of_range( "source resource was not found" );
    const auto* path = m_impl->sourceFiles[sourceId];
    const auto block = m_impl->worker->GetSourceFileFromCache( path );
    const size_t size = std::min( size_t( block.len ), maxBytes );
    return { m_impl->MakeRef( "source-file", sourceId ), path, std::string( block.data, size ), true, size < block.len };
}

BinaryResourceChunkDto WorkerTraceSource::ReadEmbeddedSourceBytes( size_t sourceId, size_t offset, size_t maxBytes ) const
{
    std::lock_guard lock( m_impl->readMutex );
    if( sourceId >= m_impl->sourceFiles.size() ) throw std::out_of_range( "source resource was not found" );
    const auto block = m_impl->worker->GetSourceFileFromCache( m_impl->sourceFiles[sourceId] );
    return BinaryChunk(
        m_impl->MakeRef( "source-file", sourceId ),
        reinterpret_cast<const uint8_t*>( block.data ), block.len, offset, maxBytes );
}

SymbolCodeDto WorkerTraceSource::ReadSymbolCode( uint64_t symbolId, size_t maxBytes ) const
{
    std::lock_guard lock( m_impl->readMutex );
    if( !m_impl->worker->HasSymbolCode( symbolId ) ) throw std::out_of_range( "symbol code resource was not found" );
    uint32_t length = 0;
    const char* code = m_impl->worker->GetSymbolCode( symbolId, length );
    const size_t size = std::min( size_t( length ), maxBytes );
    SymbolCodeDto result { m_impl->MakeRef( "symbol", symbolId ), Hex( symbolId ), {}, size < length };
    result.bytes.assign( reinterpret_cast<const uint8_t*>( code ), reinterpret_cast<const uint8_t*>( code ) + size );
    return result;
}

BinaryResourceChunkDto WorkerTraceSource::ReadSymbolCodeBytes( uint64_t symbolId, size_t offset, size_t maxBytes ) const
{
    std::lock_guard lock( m_impl->readMutex );
    if( !m_impl->worker->HasSymbolCode( symbolId ) ) throw std::out_of_range( "symbol code resource was not found" );
    uint32_t length = 0;
    const auto* code = reinterpret_cast<const uint8_t*>( m_impl->worker->GetSymbolCode( symbolId, length ) );
    return BinaryChunk( m_impl->MakeRef( "symbol", symbolId ), code, length, offset, maxBytes );
}

FrameImageDto WorkerTraceSource::ReadFrameImage( size_t imageId, size_t maxBytes ) const
{
    std::lock_guard lock( m_impl->readMutex );
    const auto& images = m_impl->worker->GetFrameImages();
    if( imageId >= images.size() ) throw std::out_of_range( "frame image resource was not found" );
    const auto* image = images[imageId].get();
    const size_t outputSize = size_t( image->w ) * image->h * 4;
    if( image->w > 4096 || image->h > 4096 || outputSize > maxBytes ) throw std::runtime_error( "decoded frame image exceeds resource budget" );
    FrameImageDto result;
    result.ref = m_impl->MakeRef( "frame-image", imageId );
    result.width = image->w;
    result.height = image->h;
    result.flipped = image->flip != 0;
    DecodeBc1( reinterpret_cast<const uint8_t*>( m_impl->worker->UnpackFrameImage( *image ) ), image->w, image->h, result.rgba );
    return result;
}

BinaryResourceChunkDto WorkerTraceSource::ReadFrameImageBc1( size_t imageId, size_t offset, size_t maxBytes ) const
{
    std::lock_guard lock( m_impl->readMutex );
    const auto& images = m_impl->worker->GetFrameImages();
    if( imageId >= images.size() ) throw std::out_of_range( "frame image resource was not found" );
    const auto* image = images[imageId].get();
    const size_t bytes = size_t( image->w ) * image->h / 2;
    const auto* bc1 = reinterpret_cast<const uint8_t*>( m_impl->worker->UnpackFrameImage( *image ) );
    return BinaryChunk( m_impl->MakeRef( "frame-image", imageId ), bc1, bytes, offset, maxBytes );
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
        if( reason.empty() ) reason = present ? "available in the persisted snapshot" : "data is absent from the persisted snapshot";
        return Capability { std::move( domain ), present, present, indexed && present, std::move( reason ), std::move( methods ) };
    };
    const bool hasCpu = !GetCpuTopology().empty() || info.counts.contextSwitches != 0;
    return {
        capability( "system", true, true, { "system.capabilities", "system.describe", "system.schema" } ),
        capability( "trace", true, true, { "trace.info", "trace.overview", "trace.counts", "trace.app_info", "trace.crash" } ),
        capability( "thread", info.counts.threads != 0, true, { "thread.list", "thread.get", "thread.statistics", "thread.timeline", "thread.migration" }, info.counts.threads ? "" : "trace contains no threads" ),
        capability( "cpu", hasCpu, true, { "cpu.topology", "cpu.usage", "cpu.timeline" }, hasCpu ? "" : "trace contains no CPU topology or scheduling data" ),
        capability( "context_switch", info.counts.contextSwitches != 0, true, { "context_switch.range", "context_switch.thread", "context_switch.statistics" } ),
        capability( "frame", info.counts.frameSets != 0, true, { "frame.sets", "frame.list", "frame.get", "frame.statistics", "frame.outliers", "frame.range_mapping" } ),
        capability( "frame_image", info.counts.frameImages != 0, true, { "frame_image.list", "frame_image.metadata", "frame_image.resource", "frame_image.raw" } ),
        capability( "timeline", info.counts.cpuZones != 0 || info.counts.gpuZones != 0 || info.counts.frames != 0 || info.counts.messages != 0 || info.counts.plots != 0 || info.counts.locks != 0 || info.counts.contextSwitches != 0, true, { "timeline.slice" } ),
        capability( "zone.cpu", info.counts.cpuZones != 0, true, { "zone.cpu.search", "zone.cpu.get", "zone.cpu.tree", "zone.cpu.statistics", "zone.cpu.flamegraph" } ),
        capability( "zone.gpu", info.counts.gpuZones != 0, true, { "zone.gpu.contexts", "zone.gpu.search", "zone.gpu.get", "zone.gpu.tree", "zone.gpu.statistics", "zone.gpu.flamegraph" } ),
        capability( "callstack", info.counts.callstackPayloads != 0 || info.counts.parentCallstackPayloads != 0, true, { "callstack.resolve", "callstack.frames", "callstack.parent", "callstack.batch" } ),
        capability( "sample", info.counts.samples != 0 || info.counts.contextSwitchSamples != 0 || info.counts.ghostZones != 0, true, { "sample.list", "sample.ghost_zones", "sample.symbol_statistics", "sample.flamegraph" } ),
        capability( "hardware_sample", info.counts.hardwareSamples != 0, true, { "hardware_sample.address", "hardware_sample.counts", "hardware_sample.events", "hardware_sample.capabilities" } ),
        capability( "symbol", info.counts.symbols != 0, true, { "symbol.search", "symbol.get", "symbol.address", "symbol.address_map", "symbol.raw_code", "symbol.disassembly" } ),
        capability( "source", info.counts.sourceLocations != 0 || info.counts.sourceCacheFiles != 0, true, { "source.locations", "source.statistics", "source.embedded", "source.lines", "source.raw" } ),
        capability( "memory", info.counts.memoryEvents != 0, true, { "memory.pools", "memory.events", "memory.get", "memory.active_at_time", "memory.frame_snapshot", "memory.diff", "memory.callstack_tree", "memory.leak_candidates" } ),
        capability( "memory.gpu", hasGpuMemory, true, { "memory.gpu.pools", "memory.gpu.allocations", "memory.gpu.request_scopes", "memory.gpu.pass_uses", "memory.gpu.attribution" } ),
        capability( "lock", info.counts.locks != 0, true, { "lock.list", "lock.get", "lock.timeline", "lock.contention_statistics" } ),
        capability( "plot", info.counts.plots != 0, true, { "plot.list", "plot.points", "plot.range", "plot.downsample", "plot.statistics" } ),
        capability( "message", info.counts.messages != 0, true, { "message.search", "message.get" } ),
        capability( "statistics", true, true, { "statistics.describe", "statistics.compute" } ),
        capability( "compare", true, true, { "compare.zones", "compare.frames", "compare.source" }, "requires a second ready trace session" ),
        capability( "validation", true, true, { "validation.run" } )
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
    result.cpuArchitecture = CpuArchitectureName( worker.GetCpuArch() );
    result.timerMultiplier = worker.GetTimerMultiplier();
    result.frameOffset = worker.GetFrameOffset();
    result.samplingPeriodNs = worker.GetSamplingPeriod();
    result.onDemand = worker.IsOnDemand();
    result.legacyQueueDelayAvailability.available = worker.GetTraceVersion() < FileVersion( 0, 12, 3 );
    if( result.legacyQueueDelayAvailability.available ) result.legacyQueueDelayNs = worker.GetLegacyQueueDelay();
    else result.legacyQueueDelayAvailability.reason = "legacy queue delay was removed from the trace format in Tracy 0.12.3";
    const auto& crash = worker.GetCrashEvent();
    result.hasCrash = crash.thread != 0 || crash.time != 0 || crash.message != 0 || crash.callstack != 0;
    result.samplesInconsistent = worker.AreSamplesInconsistent();

    auto& counts = result.counts;
    counts.frameSets = worker.GetFrames().size();
    for( const auto* frames : worker.GetFrames() ) counts.frames += worker.GetFrameCount( *frames );
    counts.cpuZones = worker.GetZoneCount();
    counts.gpuZones = worker.GetGpuZoneCount();
    {
        std::set<uint64_t> threads;
        for( const auto* thread : worker.GetThreadData() ) threads.emplace( thread->id );
        for( const auto& [thread, data] : worker.GetContextSwitchMap() ) threads.emplace( thread );
        for( const auto& [thread, data] : worker.GetCpuThreadData() ) threads.emplace( thread );
        for( const auto& [thread, process] : worker.GetTidToPidMap() ) threads.emplace( thread );
        for( const auto& [thread, name] : worker.GetThreadNameMap() ) threads.emplace( thread );
        for( const auto& [thread, names] : worker.GetExternalNameMap() ) threads.emplace( thread );
        counts.threads = threads.size();
    }
    counts.locks = worker.GetLockCount();
    counts.plots = worker.GetPlotCount();
    counts.messages = worker.GetMessages().size();
    counts.memoryPools = worker.GetMemNameMap().size();
    for( const auto& [name, memory] : worker.GetMemNameMap() ) counts.memoryEvents += memory->data.size();
    counts.contextSwitches = worker.GetContextSwitchCount();
    counts.callstackPayloads = worker.GetCallstackPayloadCount();
#ifndef TRACY_NO_STATISTICS
    counts.parentCallstackPayloads = worker.GetCallstackParentPayloadCount();
#endif
    counts.callstackFrames = worker.GetCallstackFrameCount();
#ifndef TRACY_NO_STATISTICS
    counts.parentCallstackFrames = worker.GetCallstackParentFrameCount();
#endif
    counts.samples = worker.GetCallstackSampleCount();
#ifndef TRACY_NO_STATISTICS
    counts.contextSwitchSamples = worker.GetContextSwitchSampleCount();
    for( const auto* thread : worker.GetThreadData() ) counts.kernelSamples += thread->kernelSampleCnt;
    counts.ghostZones = worker.GetGhostZonesCount();
    counts.childSampleSymbols = worker.GetChildSamplesCountSyms();
    counts.childSamples = worker.GetChildSamplesCountFull();
#endif
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
    std::map<uint64_t, const ThreadData*> threads;
    for( const auto* thread : worker.GetThreadData() ) threads.emplace( thread->id, thread );
    for( const auto& [thread, data] : worker.GetContextSwitchMap() ) threads.try_emplace( thread, nullptr );
    for( const auto& [thread, data] : worker.GetCpuThreadData() ) threads.try_emplace( thread, nullptr );
    for( const auto& [thread, process] : worker.GetTidToPidMap() ) threads.try_emplace( thread, nullptr );
    for( const auto& [thread, name] : worker.GetThreadNameMap() ) threads.try_emplace( thread, nullptr );
    for( const auto& [thread, names] : worker.GetExternalNameMap() ) threads.try_emplace( thread, nullptr );
    result.reserve( threads.size() );
    for( const auto& [threadId, thread] : threads )
    {
        ThreadDto dto;
        dto.ref = m_impl->MakeRef( "thread", threadId );
        dto.nativeId = threadId;
        dto.processId = worker.GetPidFromTid( threadId );
        dto.name = Safe( worker.GetThreadName( threadId ) );
        const auto localName = worker.GetThreadNameMap().find( threadId );
        if( localName != worker.GetThreadNameMap().end() ) dto.localName = Safe( localName->second );
        if( worker.HasExternalName( threadId ) )
        {
            const auto external = worker.GetExternalName( threadId );
            dto.externalProcessName = Safe( external.first );
            dto.externalThreadName = Safe( external.second );
        }
        if( thread )
        {
            dto.fiber = thread->isFiber != 0;
            dto.zoneCount = thread->count;
            dto.messageCount = thread->messages.size();
            dto.sampleCount = thread->samples.size();
            dto.kernelSampleCount = thread->kernelSampleCnt;
            dto.groupHintAvailability.available = worker.GetTraceVersion() >= FileVersion( 0, 11, 1 );
            if( dto.groupHintAvailability.available ) dto.groupHint = thread->groupHint;
            else dto.groupHintAvailability.reason = "thread group hint was not persisted before Tracy 0.11.1";
        }
        else
        {
            dto.groupHintAvailability.available = false;
            dto.groupHintAvailability.reason = "this native thread has no persisted Tracy thread record";
        }
        if( const auto* context = worker.GetContextSwitchData( threadId ) ) dto.contextSwitchCount = context->v.size();
        const auto cpu = worker.GetCpuThreadData().find( threadId );
        if( cpu != worker.GetCpuThreadData().end() )
        {
            dto.runningTimeNs = cpu->second.runningTime;
            dto.runningRegions = cpu->second.runningRegions;
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
        else name = Safe( worker.TryGetString( frameSet->name ) );
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
        GpuContextDto dto;
        dto.ref = m_impl->MakeRef( "gpu-context", i );
        dto.index = i;
        dto.name = context->name.Active() ? Safe( worker.TryGetString( context->name ) ) : "GPU context " + std::to_string( i );
        dto.threadRef = m_impl->MakeRef( "thread", context->thread );
        dto.zoneCount = context->count;
        dto.period = context->period;
        dto.calibrated = context->hasCalibration;
        dto.type = uint8_t( context->type );
        dto.typeName = GpuContextTypeName( context->type );
        dto.overflow = context->overflow;
        dto.notesAvailability.available = worker.GetTraceVersion() >= FileVersion( 0, 12, 4 );
        if( !dto.notesAvailability.available ) dto.notesAvailability.reason = "GPU note names, values, and query IDs were not persisted before Tracy 0.12.4";
        if( context->name.Active() ) dto.customName = Safe( worker.TryGetString( context->name ) );
        dto.noteNames.reserve( context->noteNames.size() );
        for( const auto& [time, name] : context->noteNames ) dto.noteNames.push_back( { time, Safe( worker.TryGetString( name ) ) } );
        std::sort( dto.noteNames.begin(), dto.noteNames.end(), []( const auto& lhs, const auto& rhs ) {
            return lhs.timeNs != rhs.timeNs ? lhs.timeNs < rhs.timeNs : lhs.name < rhs.name;
        } );
        for( const auto& [queryId, notes] : context->notes )
        {
            for( const auto& [time, value] : notes ) dto.notes.push_back( { queryId, time, value } );
        }
        std::sort( dto.notes.begin(), dto.notes.end(), []( const auto& lhs, const auto& rhs ) {
            return lhs.queryId != rhs.queryId ? lhs.queryId < rhs.queryId : lhs.timeNs < rhs.timeNs;
        } );
        result.emplace_back( std::move( dto ) );
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
        MemoryPoolDto dto;
        dto.ref = m_impl->MakeRef( "memory-pool", m_impl->memoryPoolIndex.at( nameId ) );
        dto.nativeNameId = nameId;
        dto.name = name;
        dto.eventCount = memory->data.size();
        dto.activeCount = memory->active.size();
        dto.activeBytes = memory->usage;
        dto.low = memory->low == std::numeric_limits<uint64_t>::max() ? 0 : memory->low;
        dto.high = memory->high == std::numeric_limits<uint64_t>::min() ? 0 : memory->high;
        dto.gpuD3D12 = IsGpuD3D12PoolName( name );
        dto.freeCount = memory->frees.size();
        dto.persistedUsageBytes = memory->usage;
        dto.storedNameId = memory->name;
        dto.storedName = memory->name == 0 ? "Default allocator" : Safe( worker.GetString( memory->name ) );
        result.emplace_back( std::move( dto ) );
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
        PlotDto dto;
        dto.ref = m_impl->MakeRef( "plot", i );
        dto.index = i;
        dto.name = Safe( worker.TryGetString( plot->name ) );
        dto.type = uint8_t( plot->type );
        dto.format = uint8_t( plot->format );
        dto.pointCount = plot->data.size();
        dto.min = plot->min;
        dto.max = plot->max;
        dto.sum = plot->sum;
        dto.showSteps = plot->showSteps != 0;
        dto.fill = plot->fill;
        dto.color = plot->color;
        result.emplace_back( std::move( dto ) );
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
        const std::string name = value->customName.Active() ? Safe( worker.TryGetString( value->customName ) ) : source.name.empty() ? source.function : source.name;
        LockDto dto;
        dto.ref = m_impl->MakeRef( "lock", id );
        dto.nativeId = id;
        dto.name = name;
        dto.sourceLocationRef = source.ref;
        dto.eventCount = value->timeline.size();
        dto.threadCount = value->threadList.size();
        dto.valid = value->valid;
        dto.contended = value->isContended;
        dto.announceNs = value->timeAnnounce;
        if( value->timeTerminate >= 0 ) dto.terminateNs = value->timeTerminate;
        dto.type = uint8_t( value->type );
        dto.typeName = LockTypeName( value->type );
        if( value->customName.Active() ) dto.customName = Safe( worker.TryGetString( value->customName ) );
        result.emplace_back( std::move( dto ) );
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
        result.emplace_back( m_impl->CpuDto( entry.index ) );
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
        result.emplace_back( m_impl->GpuDto( entry.index ) );
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
            if( auto dto = m_impl->MemoryDto( { poolId, eventIndex } ) ) result.emplace_back( std::move( *dto ) );
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
        MessageDto dto;
        dto.ref = m_impl->MakeRef( "message", index );
        dto.threadRef = m_impl->MakeRef( "thread", m_impl->worker->DecompressThread( message->thread ) );
        dto.timeNs = message->time;
        dto.text = Safe( m_impl->worker->TryGetString( message->ref ) );
        dto.color = message->color;
        dto.callstack = message->callstack.Val();
        if( dto.callstack != 0 ) dto.callstackRef = m_impl->MakeRef( "callstack", dto.callstack );
        result.emplace_back( std::move( dto ) );
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
    std::vector<uint64_t> threads;
    threads.reserve( worker.GetContextSwitchMap().size() );
    for( const auto& [thread, data] : worker.GetContextSwitchMap() ) threads.emplace_back( thread );
    std::sort( threads.begin(), threads.end() );
    uint64_t ordinal = 0;
    for( const auto thread : threads )
    {
        const auto* data = worker.GetContextSwitchData( thread );
        if( !data ) continue;
        for( size_t index = 0; index < data->v.size(); index++ )
        {
            const auto& event = data->v[index];
            const auto currentOrdinal = ordinal++;
            const int64_t end = event.IsEndValid() ? event.End() : event.Start();
            if( !Intersects( event.Start(), end, range ) ) continue;
            if( skipped++ < range.offset ) continue;
            result.emplace_back( m_impl->MakeRef( "context-switch", currentOrdinal ) );
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

CrashDto WorkerTraceSource::GetCrash() const
{
    std::lock_guard lock( m_impl->readMutex );
    const auto& crash = m_impl->worker->GetCrashEvent();
    CrashDto result;
    result.present = crash.thread != 0 || crash.time != 0 || crash.message != 0 || crash.callstack != 0;
    if( !result.present ) return result;
    result.threadRef = m_impl->MakeRef( "thread", crash.thread );
    result.timeNs = crash.time;
    result.message = crash.message == 0 ? "" : Safe( m_impl->worker->GetString( crash.message ) );
    result.callstack = crash.callstack;
    if( result.callstack != 0 ) result.callstackRef = m_impl->MakeRef( "callstack", result.callstack );
    return result;
}

std::vector<CpuTopologyDto> WorkerTraceSource::GetCpuTopology() const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<CpuTopologyDto> result;
    const auto dieAvailable = m_impl->worker->GetTraceVersion() >= FileVersion( 0, 11, 2 );
    for( const auto& [package, dies] : m_impl->worker->GetCpuTopology() )
    {
        for( const auto& [die, cores] : dies )
        {
            for( const auto& [core, cpus] : cores )
            {
                for( const auto cpu : cpus )
                {
                    CpuTopologyDto dto { cpu, package, die, core };
                    dto.dieAvailability.available = dieAvailable;
                    if( !dieAvailable ) dto.dieAvailability.reason = "CPU die IDs were not persisted before Tracy 0.11.2";
                    result.emplace_back( std::move( dto ) );
                }
            }
        }
    }
    std::sort( result.begin(), result.end(), []( const auto& lhs, const auto& rhs ) { return lhs.cpu < rhs.cpu; } );
    return result;
}

std::vector<CpuUsagePointDto> WorkerTraceSource::GetCpuUsage() const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<CpuUsagePointDto> result;
#ifndef TRACY_NO_STATISTICS
    if( !m_impl->worker->IsCpuUsageReady() ) return result;
    const auto& usage = m_impl->worker->GetCpuUsage();
    result.reserve( usage.size() );
    for( size_t index = 0; index < usage.size(); index++ )
    {
        const auto& point = usage[index];
        result.push_back( { m_impl->MakeRef( "cpu-usage", index ), point.Time(), point.Own(), point.Other() } );
    }
#endif
    return result;
}

std::vector<ContextSwitchDto> WorkerTraceSource::ScanContextSwitchEvents( const ScanRange& range ) const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<uint64_t> threads;
    for( const auto& [thread, data] : m_impl->worker->GetContextSwitchMap() ) threads.emplace_back( thread );
    std::sort( threads.begin(), threads.end() );

    std::vector<ContextSwitchDto> result;
    size_t skipped = 0;
    uint64_t ordinal = 0;
    for( const auto thread : threads )
    {
        const auto* data = m_impl->worker->GetContextSwitchData( thread );
        if( !data ) continue;
        for( const auto& event : data->v )
        {
            const auto currentOrdinal = ordinal++;
            const int64_t end = event.IsEndValid() ? event.End() : event.Start();
            if( !Intersects( event.Start(), end, range ) ) continue;
            if( skipped++ < range.offset ) continue;
            ContextSwitchDto dto;
            dto.ref = m_impl->MakeRef( "context-switch", currentOrdinal );
            dto.threadRef = m_impl->MakeRef( "thread", thread );
            dto.startNs = event.Start();
            if( event.IsEndValid() ) dto.endNs = event.End();
            if( event.WakeupVal() >= 0 ) dto.wakeupNs = event.WakeupVal();
            dto.cpu = event.Cpu();
            dto.wakeupCpu = event.WakeupCpu();
            dto.wakeupCpuAvailability.available = m_impl->worker->GetTraceVersion() >= FileVersion( 0, 11, 3 );
            if( !dto.wakeupCpuAvailability.available ) dto.wakeupCpuAvailability.reason = "context-switch wakeup CPU was not persisted before Tracy 0.11.3";
            dto.reason = int8_t( event.Reason() );
            dto.state = event.State();
            dto.complete = event.IsEndValid();
            dto.reasonName = ContextSwitchReasonName( dto.reason );
            dto.stateName = ContextSwitchStateName( dto.state );
            dto.relatedThreadIndex = event.Thread();
            if( dto.relatedThreadIndex != 0 ) dto.relatedThreadRef = m_impl->MakeRef( "thread", m_impl->worker->DecompressThread( dto.relatedThreadIndex ) );
            result.emplace_back( std::move( dto ) );
            if( result.size() >= range.limit ) return result;
        }
    }
    return result;
}

std::vector<CpuContextSwitchDto> WorkerTraceSource::ScanCpuContextSwitchEvents( const ScanRange& range ) const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<CpuContextSwitchDto> result;
    const auto* cpuData = m_impl->worker->GetCpuData();
    if( !cpuData ) return result;
    size_t skipped = 0;
    uint64_t ordinal = 0;
    for( int cpu = 0; cpu < m_impl->worker->GetCpuDataCpuCount(); cpu++ )
    {
        for( const auto& event : cpuData[cpu].cs )
        {
            const auto currentOrdinal = ordinal++;
            const auto complete = event.IsEndValid();
            const auto end = complete ? event.End() : event.Start();
            const auto intersects = complete ? Intersects( event.Start(), end, range ) : event.Start() >= range.startNs && event.Start() < range.endNs;
            if( !intersects ) continue;
            if( skipped++ < range.offset ) continue;
            CpuContextSwitchDto dto;
            dto.ref = m_impl->MakeRef( "cpu-context-switch", currentOrdinal );
            dto.cpu = uint32_t( cpu );
            dto.startNs = event.Start();
            if( complete ) dto.endNs = end;
            dto.rawThreadIndex = event.Thread();
            dto.threadRef = m_impl->MakeRef( "thread", m_impl->worker->DecompressThread( event.Thread() ) );
            dto.complete = complete;
            result.emplace_back( std::move( dto ) );
            if( result.size() >= range.limit ) return result;
        }
    }
    return result;
}

std::vector<SampleDto> WorkerTraceSource::ScanSampleEvents( const ScanRange& range ) const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<const ThreadData*> threads;
    for( const auto* thread : m_impl->worker->GetThreadData() ) threads.emplace_back( thread );
    std::sort( threads.begin(), threads.end(), []( const auto* lhs, const auto* rhs ) { return lhs->id < rhs->id; } );

    std::vector<SampleDto> result;
    size_t skipped = 0;
    uint64_t ordinal = 0;
    for( const auto* thread : threads )
    {
        for( const auto& sample : thread->samples )
        {
            const auto currentOrdinal = ordinal++;
            if( sample.time.Val() < range.startNs || sample.time.Val() >= range.endNs ) continue;
            if( skipped++ < range.offset ) continue;
            SampleDto dto;
            dto.ref = m_impl->MakeRef( "sample", currentOrdinal );
            dto.threadRef = m_impl->MakeRef( "thread", thread->id );
            dto.timeNs = sample.time.Val();
            dto.callstack = sample.callstack.Val();
            if( dto.callstack != 0 ) dto.callstackRef = m_impl->MakeRef( "callstack", dto.callstack );
            dto.kind = "sample";
            result.emplace_back( std::move( dto ) );
            if( result.size() >= range.limit ) return result;
        }
#ifndef TRACY_NO_STATISTICS
        for( const auto& sample : thread->ctxSwitchSamples )
        {
            const auto currentOrdinal = ordinal++;
            if( sample.time.Val() < range.startNs || sample.time.Val() >= range.endNs ) continue;
            if( skipped++ < range.offset ) continue;
            SampleDto dto;
            dto.ref = m_impl->MakeRef( "context-switch-sample", currentOrdinal );
            dto.threadRef = m_impl->MakeRef( "thread", thread->id );
            dto.timeNs = sample.time.Val();
            dto.callstack = sample.callstack.Val();
            if( dto.callstack != 0 ) dto.callstackRef = m_impl->MakeRef( "callstack", dto.callstack );
            dto.kind = "context_switch";
            result.emplace_back( std::move( dto ) );
            if( result.size() >= range.limit ) return result;
        }
#endif
    }
    return result;
}

std::vector<GhostZoneDto> WorkerTraceSource::ScanGhostZones( const ScanRange& range ) const
{
#ifdef TRACY_NO_STATISTICS
    (void)range;
    return {};
#else
    std::lock_guard lock( m_impl->readMutex );
    std::vector<const ThreadData*> threads;
    for( const auto* thread : m_impl->worker->GetThreadData() ) threads.emplace_back( thread );
    std::sort( threads.begin(), threads.end(), []( const auto* lhs, const auto* rhs ) { return lhs->id < rhs->id; } );

    std::vector<GhostZoneDto> result;
    size_t skipped = 0;
    uint64_t ordinal = 0;
    std::function<void( const Vector<GhostZone>&, const ThreadData&, uint32_t, std::optional<uint64_t> )> visit;
    visit = [&]( const Vector<GhostZone>& zones, const ThreadData& thread, uint32_t depth, std::optional<uint64_t> parentOrdinal ) {
        for( const auto& zone : zones )
        {
            const auto currentOrdinal = ordinal++;
            const bool intersects = Intersects( zone.start.Val(), zone.end.Val(), range );
            if( intersects && skipped++ >= range.offset && result.size() < range.limit )
            {
                const auto& key = m_impl->worker->GetGhostFrame( zone.frame );
                const auto* frame = m_impl->worker->GetCallstackFrame( key.frame );
                GhostZoneDto dto;
                dto.ref = m_impl->MakeRef( "ghost-zone", currentOrdinal );
                dto.threadRef = m_impl->MakeRef( "thread", thread.id );
                if( parentOrdinal ) dto.parentRef = m_impl->MakeRef( "ghost-zone", *parentOrdinal );
                dto.startNs = zone.start.Val();
                dto.endNs = zone.end.Val();
                dto.address = Hex( m_impl->worker->GetCanonicalPointer( key.frame ) );
                dto.depth = depth;
                dto.inlineFrame = frame && key.inlineFrame + 1 < frame->size;
                if( frame && key.inlineFrame < frame->size )
                {
                    const auto& symbol = frame->data[key.inlineFrame];
                    dto.name = Safe( m_impl->worker->TryGetString( symbol.name ) );
                    dto.file = Safe( m_impl->worker->TryGetString( symbol.file ) );
                    dto.line = symbol.line;
                }
                if( zone.child >= 0 ) dto.childCount = uint32_t( m_impl->worker->GetGhostChildren( zone.child ).size() );
                result.emplace_back( std::move( dto ) );
            }
            if( zone.child >= 0 ) visit( m_impl->worker->GetGhostChildren( zone.child ), thread, depth + 1, currentOrdinal );
        }
    };
    for( const auto* thread : threads )
    {
        visit( thread->ghostZones, *thread, 0, std::nullopt );
        if( result.size() >= range.limit ) break;
    }
    return result;
#endif
}

std::vector<HardwareSampleDto> WorkerTraceSource::GetHardwareSamples() const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<HardwareSampleDto> result;
    result.reserve( m_impl->worker->GetHwSamples().size() );
    for( const auto& [address, data] : m_impl->worker->GetHwSamples() )
    {
        result.push_back( {
            m_impl->MakeRef( "hardware-sample", address ), Hex( address ),
            data.cycles.size(), data.retired.size(), data.cacheRef.size(), data.cacheMiss.size(), data.branchRetired.size(), data.branchMiss.size()
        } );
    }
    std::sort( result.begin(), result.end(), []( const auto& lhs, const auto& rhs ) { return lhs.address < rhs.address; } );
    return result;
}

std::vector<HardwareSampleEventDto> WorkerTraceSource::GetHardwareSampleEvents( uint64_t address, std::string_view kind, size_t offset, size_t limit ) const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<HardwareSampleEventDto> result;
    const auto found = m_impl->worker->GetHwSamples().find( address );
    if( found == m_impl->worker->GetHwSamples().end() || limit == 0 ) return result;

    size_t skipped = 0;
    const auto append = [&]( std::string_view eventKind, const auto& events ) {
        if( kind != "all" && kind != eventKind ) return;
        for( size_t index = 0; index < events.size(); index++ )
        {
            if( skipped++ < offset ) continue;
            if( result.size() >= limit ) return;
            HardwareSampleEventDto dto;
            dto.ref = m_impl->MakeRef( "hardware-sample", address ) + ':' + std::string( eventKind ) + ':' + std::to_string( index );
            dto.address = Hex( address );
            dto.kind = eventKind;
            dto.eventIndex = index;
            dto.timeNs = events[index].Val();
            result.emplace_back( std::move( dto ) );
        }
    };

    const auto& samples = found->second;
    append( "cycles", samples.cycles );
    append( "retired", samples.retired );
    append( "cache_references", samples.cacheRef );
    append( "cache_misses", samples.cacheMiss );
    append( "branch_retired", samples.branchRetired );
    append( "branch_misses", samples.branchMiss );
    return result;
}

std::vector<LockEventDto> WorkerTraceSource::ScanLockEvents( const ScanRange& range ) const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<std::pair<uint32_t, const LockMap*>> locks;
    for( const auto& [id, value] : m_impl->worker->GetLockMap() ) locks.emplace_back( id, value );
    std::sort( locks.begin(), locks.end(), []( const auto& lhs, const auto& rhs ) { return lhs.first < rhs.first; } );
    const auto typeName = []( LockEvent::Type type ) {
        switch( type )
        {
        case LockEvent::Type::Wait: return "wait";
        case LockEvent::Type::Obtain: return "obtain";
        case LockEvent::Type::Release: return "release";
        case LockEvent::Type::WaitShared: return "wait_shared";
        case LockEvent::Type::ObtainShared: return "obtain_shared";
        case LockEvent::Type::ReleaseShared: return "release_shared";
        }
        return "unknown";
    };

    std::vector<LockEventDto> result;
    size_t skipped = 0;
    uint64_t ordinal = 0;
    for( const auto& [lockId, lock] : locks )
    {
        for( const auto& item : lock->timeline )
        {
            const auto currentOrdinal = ordinal++;
            const auto* event = item.ptr.get();
            if( event->Time() < range.startNs || event->Time() >= range.endNs ) continue;
            if( skipped++ < range.offset ) continue;
            LockEventDto dto;
            dto.ref = m_impl->MakeRef( "lock-event", currentOrdinal );
            dto.lockRef = m_impl->MakeRef( "lock", lockId );
            dto.timeNs = event->Time();
            if( event->thread < lock->threadList.size() ) dto.threadRef = m_impl->MakeRef( "thread", lock->threadList[event->thread] );
            dto.type = typeName( event->type );
            if( item.lockCount != 0 && item.lockingThread < lock->threadList.size() ) dto.ownerThreadRef = m_impl->MakeRef( "thread", lock->threadList[item.lockingThread] );
            dto.lockCount = item.lockCount;
            dto.sourceLocationRef = m_impl->SourceLocation( event->SrcLoc() ).ref;
            for( size_t bit = 0; bit < lock->threadList.size() && bit < 64; bit++ )
            {
                if( item.waitList & ( uint64_t( 1 ) << bit ) ) dto.waiterThreadRefs.emplace_back( m_impl->MakeRef( "thread", lock->threadList[bit] ) );
            }
            result.emplace_back( std::move( dto ) );
            if( result.size() >= range.limit ) return result;
        }
    }
    return result;
}

std::vector<SymbolDto> WorkerTraceSource::GetSymbols() const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<SymbolDto> result;
    result.reserve( m_impl->symbols.size() );
    for( const auto address : m_impl->symbols )
    {
        const auto* symbol = m_impl->worker->GetSymbolData( address );
        if( !symbol ) continue;
        SymbolDto dto;
        dto.ref = m_impl->MakeRef( "symbol", address );
        dto.address = Hex( address );
        dto.name = Safe( m_impl->worker->TryGetString( symbol->name ) );
        dto.file = Safe( m_impl->worker->TryGetString( symbol->file ) );
        dto.line = symbol->line;
        if( symbol->imageName.Active() ) dto.imageName = Safe( m_impl->worker->TryGetString( symbol->imageName ) );
        if( symbol->callFile.Active() ) dto.callFile = Safe( m_impl->worker->TryGetString( symbol->callFile ) );
        dto.callLine = symbol->callLine;
        dto.inlineFrame = symbol->isInline != 0;
        dto.size = uint32_t( symbol->size.Val() );
#ifndef TRACY_NO_STATISTICS
        if( const auto* stats = m_impl->worker->GetSymbolStats( address ) )
        {
            dto.inclusiveSamples = stats->incl;
            dto.exclusiveSamples = stats->excl;
        }
        if( const auto* children = m_impl->worker->GetChildSamples( address ) ) dto.childSamples = children->size();
#endif
        dto.hasCode = m_impl->worker->HasSymbolCode( address );
        result.emplace_back( std::move( dto ) );
    }
    return result;
}

std::vector<SymbolAddressMappingDto> WorkerTraceSource::GetSymbolAddressMappings( size_t offset, size_t limit ) const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<std::pair<uint64_t, uint64_t>> mappings;
    mappings.reserve( m_impl->worker->GetCodeSymbolMap().size() );
    for( const auto& value : m_impl->worker->GetCodeSymbolMap() ) mappings.emplace_back( value.first, value.second );
    std::sort( mappings.begin(), mappings.end() );

    std::vector<SymbolAddressMappingDto> result;
    const auto begin = std::min( offset, mappings.size() );
    const auto end = begin + std::min( limit, mappings.size() - begin );
    result.reserve( end - begin );
    for( size_t index = begin; index < end; index++ )
    {
        const auto [address, symbol] = mappings[index];
        const auto* symbolData = m_impl->worker->GetSymbolData( symbol );
        result.push_back( {
            m_impl->MakeRef( "symbol-address", address ), Hex( address ), m_impl->MakeRef( "symbol", symbol ),
            Hex( symbol ), address >= symbol ? uint32_t( std::min<uint64_t>( address - symbol, std::numeric_limits<uint32_t>::max() ) ) : 0,
            symbolData && symbolData->isInline != 0
        } );
    }
    return result;
}

std::optional<SymbolAddressMappingDto> WorkerTraceSource::ResolveSymbolAddress( uint64_t address ) const
{
    std::lock_guard lock( m_impl->readMutex );
    auto symbol = m_impl->worker->GetInlineSymbolForAddress( address );
    uint32_t offset = 0;
    if( symbol == 0 ) symbol = m_impl->worker->GetSymbolForAddress( address, offset );
    else if( address >= symbol ) offset = uint32_t( std::min<uint64_t>( address - symbol, std::numeric_limits<uint32_t>::max() ) );
    if( symbol == 0 ) return std::nullopt;
    const auto* symbolData = m_impl->worker->GetSymbolData( symbol );
    if( !symbolData ) return std::nullopt;
    return SymbolAddressMappingDto {
        m_impl->MakeRef( "symbol-address", address ), Hex( address ), m_impl->MakeRef( "symbol", symbol ),
        Hex( symbol ), offset, symbolData->isInline != 0
    };
}

std::vector<SourceLocationDto> WorkerTraceSource::GetSourceLocations() const
{
    std::lock_guard lock( m_impl->readMutex );
    std::vector<SourceLocationDto> result;
    result.reserve( m_impl->worker->GetSrcLocCount() );
    // Index zero is Tracy's static-source sentinel. GetSourceLocation( 0 ) assumes a
    // real expanded entry and therefore must never be used for enumeration.
    for( size_t index = 1; index < m_impl->worker->GetStaticSourceLocationCount(); index++ ) result.emplace_back( m_impl->SourceLocation( int16_t( index ) ) );
    for( size_t index = 0; index < m_impl->worker->GetDynamicSourceLocationCount(); index++ ) result.emplace_back( m_impl->SourceLocation( -int16_t( index + 1 ) ) );
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
                result.push_back( { m_impl->MakeRef( "callstack-frame", ( uint64_t( callstack ) << 32 ) | depth ), "", "", 0, Hex( worker.GetCanonicalPointer( entry ) ), "0x0", false, callstack, depth } );
                depth++;
                continue;
            }
            std::optional<std::string> imageName;
            if( frameData->imageName.Active() ) imageName = Safe( worker.TryGetString( frameData->imageName ) );
            for( uint8_t frameIndex = 0; frameIndex < frameData->size && depth < maxDepth; frameIndex++ )
            {
                const auto& frame = frameData->data[frameIndex];
                result.push_back( {
                    m_impl->MakeRef( "callstack-frame", ( uint64_t( callstack ) << 32 ) | depth ),
                    Safe( worker.TryGetString( frame.name ) ), Safe( worker.TryGetString( frame.file ) ), frame.line,
                    Hex( worker.GetCanonicalPointer( entry ) ), Hex( frame.symAddr ), frameIndex + 1 != frameData->size, callstack, depth, imageName
                } );
                depth++;
            }
        }
    }
    return result;
}

std::vector<CallstackFrameDto> WorkerTraceSource::ResolveParentCallstacks( const std::vector<uint32_t>& callstacks, size_t maxDepth ) const
{
#ifdef TRACY_NO_STATISTICS
    (void)callstacks;
    (void)maxDepth;
    return {};
#else
    std::lock_guard lock( m_impl->readMutex );
    std::vector<CallstackFrameDto> result;
    auto& worker = *m_impl->worker;
    for( const auto callstack : callstacks )
    {
        if( callstack >= worker.GetCallstackParentPayloadCount() ) continue;
        const auto& entries = worker.GetParentCallstack( callstack );
        size_t depth = 0;
        for( const auto& entry : entries )
        {
            if( depth >= maxDepth ) break;
            const auto* frameData = entry.custom ? worker.GetParentCallstackFrame( entry ) : worker.GetCallstackFrame( entry );
            if( !frameData )
            {
                result.push_back( { m_impl->MakeRef( "parent-callstack-frame", ( uint64_t( callstack ) << 32 ) | depth ), "", "", 0, Hex( worker.GetCanonicalPointer( entry ) ), "0x0", false, callstack, depth } );
                depth++;
                continue;
            }
            std::optional<std::string> imageName;
            if( frameData->imageName.Active() ) imageName = Safe( worker.TryGetString( frameData->imageName ) );
            for( uint8_t frameIndex = 0; frameIndex < frameData->size && depth < maxDepth; frameIndex++ )
            {
                const auto& frame = frameData->data[frameIndex];
                result.push_back( {
                    m_impl->MakeRef( "parent-callstack-frame", ( uint64_t( callstack ) << 32 ) | depth ),
                    Safe( worker.TryGetString( frame.name ) ), Safe( worker.TryGetString( frame.file ) ), frame.line,
                    Hex( worker.GetCanonicalPointer( entry ) ), Hex( frame.symAddr ), frameIndex + 1 != frameData->size, callstack, depth, imageName
                } );
                depth++;
            }
        }
    }
    return result;
#endif
}

std::vector<DisassemblyInstructionDto> WorkerTraceSource::DisassembleSymbol( std::string_view symbolRef, size_t maxBytes, size_t maxInstructions ) const
{
    std::lock_guard lock( m_impl->readMutex );
    const auto symbol = m_impl->ParseRef( symbolRef, "symbol" );
    if( !symbol || !m_impl->worker->HasSymbolCode( *symbol ) ) return {};

    csh handle = 0;
    cs_err status = CS_ERR_ARCH;
    switch( m_impl->worker->GetCpuArch() )
    {
    case CpuArchX86: status = cs_open( CS_ARCH_X86, CS_MODE_32, &handle ); break;
    case CpuArchX64: status = cs_open( CS_ARCH_X86, CS_MODE_64, &handle ); break;
    case CpuArchArm32: status = cs_open( CS_ARCH_ARM, CS_MODE_ARM, &handle ); break;
    case CpuArchArm64: status = cs_open( CS_ARCH_AARCH64, CS_MODE_ARM, &handle ); break;
    case CpuArchUnknown: return {};
    }
    if( status != CS_ERR_OK ) return {};

    uint32_t persistedBytes = 0;
    const auto* code = reinterpret_cast<const uint8_t*>( m_impl->worker->GetSymbolCode( *symbol, persistedBytes ) );
    const auto bytes = std::min( size_t( persistedBytes ), maxBytes );
    cs_insn* instructions = nullptr;
    const auto count = cs_disasm( handle, code, bytes, *symbol, maxInstructions, &instructions );
    std::vector<DisassemblyInstructionDto> result;
    result.reserve( count );
    for( size_t index = 0; index < count; index++ )
    {
        const auto& instruction = instructions[index];
        std::ostringstream encoded;
        for( size_t byte = 0; byte < instruction.size; byte++ ) encoded << std::hex << std::setw( 2 ) << std::setfill( '0' ) << unsigned( instruction.bytes[byte] );
        result.push_back( {
            m_impl->MakeRef( "instruction", instruction.address ), Hex( instruction.address ), encoded.str(),
            instruction.mnemonic, instruction.op_str, instruction.size
        } );
    }
    if( instructions ) cs_free( instructions, count );
    cs_close( &handle );
    return result;
}

std::vector<SourceTextDto> WorkerTraceSource::ResolveSources( const std::vector<std::string>& sourceRefs, size_t maxBytes ) const
{
    std::vector<SourceTextDto> result;
    const auto resources = GetSourceResources();
    for( const auto& resource : resources )
    {
        if( std::find( sourceRefs.begin(), sourceRefs.end(), resource.ref ) != sourceRefs.end() ) result.emplace_back( ReadEmbeddedSource( resource.id, maxBytes ) );
    }
    return result;
}

std::vector<SymbolCodeDto> WorkerTraceSource::ResolveSymbols( const std::vector<std::string>& symbolRefs, size_t maxBytes ) const
{
    std::vector<SymbolCodeDto> result;
    const auto resources = GetSymbolResources();
    for( const auto& resource : resources )
    {
        if( resource.codeBytes != 0 && std::find( symbolRefs.begin(), symbolRefs.end(), resource.ref ) != symbolRefs.end() ) result.emplace_back( ReadSymbolCode( resource.id, maxBytes ) );
    }
    return result;
}

std::vector<FrameImageDto> WorkerTraceSource::ResolveFrameImages( const std::vector<std::string>& imageRefs, size_t maxBytes ) const
{
    std::vector<FrameImageDto> result;
    const auto resources = GetFrameImageResources();
    for( const auto& resource : resources )
    {
        if( std::find( imageRefs.begin(), imageRefs.end(), resource.ref ) != imageRefs.end() ) result.emplace_back( ReadFrameImage( resource.id, maxBytes ) );
    }
    return result;
}

}
