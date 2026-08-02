#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <inttypes.h>
#include <limits>

#include "TracyImGui.hpp"
#include "TracyMouse.hpp"
#include "TracyPrint.hpp"
#include "TracyView.hpp"
#include "tracy_pdqsort.h"
#include "../Fonts.hpp"

namespace tracy
{

constexpr size_t ChunkBits = 10;
constexpr size_t PageBits = 10;
constexpr size_t PageSize = 1 << PageBits;
constexpr size_t PageChunkBits = ChunkBits + PageBits;
constexpr size_t PageChunkSize = 1 << PageChunkBits;

uint32_t MemDecayColor[256] = {
    0x0, 0xFF077F07, 0xFF078007, 0xFF078207, 0xFF078307, 0xFF078507, 0xFF078707, 0xFF078807,
    0xFF078A07, 0xFF078B07, 0xFF078D07, 0xFF078F07, 0xFF079007, 0xFF089208, 0xFF089308, 0xFF089508,
    0xFF089708, 0xFF089808, 0xFF089A08, 0xFF089B08, 0xFF089D08, 0xFF089F08, 0xFF08A008, 0xFF08A208,
    0xFF09A309, 0xFF09A509, 0xFF09A709, 0xFF09A809, 0xFF09AA09, 0xFF09AB09, 0xFF09AD09, 0xFF09AF09,
    0xFF09B009, 0xFF09B209, 0xFF09B309, 0xFF09B509, 0xFF0AB70A, 0xFF0AB80A, 0xFF0ABA0A, 0xFF0ABB0A,
    0xFF0ABD0A, 0xFF0ABF0A, 0xFF0AC00A, 0xFF0AC20A, 0xFF0AC30A, 0xFF0AC50A, 0xFF0AC70A, 0xFF0BC80B,
    0xFF0BCA0B, 0xFF0BCB0B, 0xFF0BCD0B, 0xFF0BCF0B, 0xFF0BD00B, 0xFF0BD20B, 0xFF0BD30B, 0xFF0BD50B,
    0xFF0BD70B, 0xFF0BD80B, 0xFF0BDA0B, 0xFF0CDB0C, 0xFF0CDD0C, 0xFF0CDF0C, 0xFF0CE00C, 0xFF0CE20C,
    0xFF0CE30C, 0xFF0CE50C, 0xFF0CE70C, 0xFF0CE80C, 0xFF0CEA0C, 0xFF0CEB0C, 0xFF0DED0D, 0xFF0DEF0D,
    0xFF0DF00D, 0xFF0DF20D, 0xFF0DF30D, 0xFF0DF50D, 0xFF0DF70D, 0xFF0DF80D, 0xFF0DFA0D, 0xFF0DFB0D,
    0xFF0DFD0D, 0xFF0EFF0E, 0xFF0EFF0E, 0xFF0EFF0E, 0xFF0EFF0E, 0xFF0EFF0E, 0xFF0EFF0E, 0xFF0EFF0E,
    0xFF0EFF0E, 0xFF0EFF0E, 0xFF0EFF0E, 0xFF0EFF0E, 0xFF0EFF0E, 0xFF0FFF0F, 0xFF0FFF0F, 0xFF0FFF0F,
    0xFF0FFF0F, 0xFF0FFF0F, 0xFF0FFF0F, 0xFF0FFF0F, 0xFF0FFF0F, 0xFF0FFF0F, 0xFF0FFF0F, 0xFF0FFF0F,
    0xFF10FF10, 0xFF10FF10, 0xFF10FF10, 0xFF10FF10, 0xFF10FF10, 0xFF10FF10, 0xFF10FF10, 0xFF10FF10,
    0xFF10FF10, 0xFF10FF10, 0xFF10FF10, 0xFF10FF10, 0xFF11FF11, 0xFF11FF11, 0xFF11FF11, 0xFF11FF11,
    0xFF11FF11, 0xFF11FF11, 0xFF11FF11, 0xFF11FF11, 0xFF11FF11, 0xFF11FF11, 0xFF11FF11, 0xFF12FF12,
    0x0, 0xFF1212FF, 0xFF1111FF, 0xFF1111FF, 0xFF1111FF, 0xFF1111FF, 0xFF1111FF, 0xFF1111FF,
    0xFF1111FF, 0xFF1111FF, 0xFF1111FF, 0xFF1111FF, 0xFF1111FF, 0xFF1010FF, 0xFF1010FF, 0xFF1010FF,
    0xFF1010FF, 0xFF1010FF, 0xFF1010FF, 0xFF1010FF, 0xFF1010FF, 0xFF1010FF, 0xFF1010FF, 0xFF1010FF,
    0xFF1010FF, 0xFF0F0FFF, 0xFF0F0FFF, 0xFF0F0FFF, 0xFF0F0FFF, 0xFF0F0FFF, 0xFF0F0FFF, 0xFF0F0FFF,
    0xFF0F0FFF, 0xFF0F0FFF, 0xFF0F0FFF, 0xFF0F0FFF, 0xFF0E0EFF, 0xFF0E0EFF, 0xFF0E0EFF, 0xFF0E0EFF,
    0xFF0E0EFF, 0xFF0E0EFF, 0xFF0E0EFF, 0xFF0E0EFF, 0xFF0E0EFF, 0xFF0E0EFF, 0xFF0E0EFF, 0xFF0E0EFF,
    0xFF0D0DFD, 0xFF0D0DFB, 0xFF0D0DFA, 0xFF0D0DF8, 0xFF0D0DF7, 0xFF0D0DF5, 0xFF0D0DF3, 0xFF0D0DF2,
    0xFF0D0DF0, 0xFF0D0DEF, 0xFF0D0DED, 0xFF0C0CEB, 0xFF0C0CEA, 0xFF0C0CE8, 0xFF0C0CE7, 0xFF0C0CE5,
    0xFF0C0CE3, 0xFF0C0CE2, 0xFF0C0CE0, 0xFF0C0CDF, 0xFF0C0CDD, 0xFF0C0CDB, 0xFF0B0BDA, 0xFF0B0BD8,
    0xFF0B0BD7, 0xFF0B0BD5, 0xFF0B0BD3, 0xFF0B0BD2, 0xFF0B0BD0, 0xFF0B0BCF, 0xFF0B0BCD, 0xFF0B0BCB,
    0xFF0B0BCA, 0xFF0B0BC8, 0xFF0A0AC7, 0xFF0A0AC5, 0xFF0A0AC3, 0xFF0A0AC2, 0xFF0A0AC0, 0xFF0A0ABF,
    0xFF0A0ABD, 0xFF0A0ABB, 0xFF0A0ABA, 0xFF0A0AB8, 0xFF0A0AB7, 0xFF0909B5, 0xFF0909B3, 0xFF0909B2,
    0xFF0909B0, 0xFF0909AF, 0xFF0909AD, 0xFF0909AB, 0xFF0909AA, 0xFF0909A8, 0xFF0909A7, 0xFF0909A5,
    0xFF0909A3, 0xFF0808A2, 0xFF0808A0, 0xFF08089F, 0xFF08089D, 0xFF08089B, 0xFF08089A, 0xFF080898,
    0xFF080897, 0xFF080895, 0xFF080893, 0xFF080892, 0xFF070790, 0xFF07078F, 0xFF07078D, 0xFF07078B,
    0xFF07078A, 0xFF070788, 0xFF070787, 0xFF070785, 0xFF070783, 0xFF070782, 0xFF070780, 0xFF07077F,
};

struct MemoryPage
{
    uint64_t page;
    int8_t data[PageSize];
};

static tracy_force_inline MemoryPage& GetPage( unordered_flat_map<uint64_t, MemoryPage>& memmap, uint64_t page )
{
    auto it = memmap.find( page );
    if( it == memmap.end() )
    {
        it = memmap.emplace( page, MemoryPage { page, {} } ).first;
    }
    return it->second;
}

static tracy_force_inline void FillPages( unordered_flat_map<uint64_t, MemoryPage>& memmap, uint64_t c0, uint64_t c1, int8_t val )
{
    auto p0 = c0 >> PageBits;
    const auto p1 = c1 >> PageBits;

    if( p0 == p1 )
    {
        const auto a0 = c0 & ( PageSize - 1 );
        const auto a1 = c1 & ( PageSize - 1 );

        auto& page = GetPage( memmap, p0 );
        if( a0 == a1 )
        {
            page.data[a0] = val;
        }
        else
        {
            memset( page.data + a0, val, a1 - a0 + 1 );
        }
    }
    else
    {
        {
            const auto a0 = c0 & ( PageSize - 1 );
            auto& page = GetPage( memmap, p0 );
            memset( page.data + a0, val, PageSize - a0 );
        }
        while( ++p0 < p1 )
        {
            auto& page = GetPage( memmap, p0 );
            memset( page.data, val, PageSize );
        }
        {
            const auto a1 = c1 & ( PageSize - 1 );
            auto& page = GetPage( memmap, p1 );
            memset( page.data, val, a1 + 1 );
        }
    }
}

std::vector<MemoryPage> View::GetMemoryPages() const
{
    std::vector<MemoryPage> ret;

    static unordered_flat_map<uint64_t, MemoryPage> memmap;

    const auto& mem = m_worker.GetMemoryNamed( m_memInfo.pool );
    const auto memlow = mem.low;

    if( m_memInfo.range.active )
    {
        auto it = std::lower_bound( mem.data.begin(), mem.data.end(), m_memInfo.range.min, []( const auto& lhs, const auto& rhs ) { return lhs.TimeAlloc() < rhs; } );
        if( it != mem.data.end() )
        {
            auto end = std::lower_bound( mem.data.begin(), mem.data.end(), m_memInfo.range.max, []( const auto& lhs, const auto& rhs ) { return lhs.TimeAlloc() < rhs; } );
            while( it != end )
            {
                auto& alloc = *it++;

                const auto a0 = alloc.Ptr() - memlow;
                const auto a1 = a0 + alloc.Size();
                int8_t val = alloc.TimeFree() < 0 ?
                    int8_t( std::max( int64_t( 1 ), 127 - ( ( m_memInfo.range.max - alloc.TimeAlloc() ) >> 24 ) ) ) :
                    ( alloc.TimeFree() > m_memInfo.range.max ?
                        int8_t( std::max( int64_t( 1 ), 127 - ( ( m_memInfo.range.max - alloc.TimeAlloc() ) >> 24 ) ) ) :
                        int8_t( -std::max( int64_t( 1 ), 127 - ( ( m_memInfo.range.max - alloc.TimeFree() ) >> 24 ) ) ) );

                const auto c0 = a0 >> ChunkBits;
                const auto c1 = a1 >> ChunkBits;

                FillPages( memmap, c0, c1, val );
            }
        }
    }
    else
    {
        const auto lastTime = m_worker.GetLastTime();
        for( auto& alloc : mem.data )
        {
            const auto a0 = alloc.Ptr() - memlow;
            const auto a1 = a0 + alloc.Size();
            const int8_t val = alloc.TimeFree() < 0 ?
                int8_t( std::max( int64_t( 1 ), 127 - ( ( lastTime - std::min( lastTime, alloc.TimeAlloc() ) ) >> 24 ) ) ) :
                int8_t( -std::max( int64_t( 1 ), 127 - ( ( lastTime - std::min( lastTime, alloc.TimeFree() ) ) >> 24 ) ) );

            const auto c0 = a0 >> ChunkBits;
            const auto c1 = a1 >> ChunkBits;

            FillPages( memmap, c0, c1, val );
        }
    }

    std::vector<unordered_flat_map<uint64_t, MemoryPage>::const_iterator> itmap;
    itmap.reserve( memmap.size() );
    ret.reserve( memmap.size() );
    for( auto it = memmap.begin(); it != memmap.end(); ++it ) itmap.emplace_back( it );
    pdqsort_branchless( itmap.begin(), itmap.end(), []( const auto& lhs, const auto& rhs ) { return lhs->second.page < rhs->second.page; } );
    for( auto& v : itmap ) ret.emplace_back( v->second );

    memmap.clear();
    return ret;
}

bool View::IsGpuD3D12MemoryPool( uint64_t pool ) const
{
    if( pool == 0 ) return false;
    const auto name = m_worker.GetString( pool );
    return name && analysis::IsGpuD3D12PoolName( name );
}

bool View::IsMemoryFramePlot( const PlotData& plot ) const
{
    if( plot.type == PlotType::Memory ) return true;
    if( plot.format != PlotValueFormatting::Memory || plot.name == 0 ) return false;
    const auto name = m_worker.GetString( plot.name );
    return name && ( strncmp( name, "GPU ", 4 ) == 0 || strncmp( name, "D3D12MA ", 8 ) == 0 );
}

const char* View::GetMemoryPoolName( uint64_t pool ) const
{
    return pool == 0 ? "Default allocator" : m_worker.GetString( pool );
}

std::vector<uint64_t> View::GetMemoryFramePools() const
{
    std::vector<uint64_t> pools;
    const auto& memNameMap = m_worker.GetMemNameMap();
    if( m_memInfo.frame.scope == MemoryFrameScope::SinglePool )
    {
        if( memNameMap.find( m_memInfo.pool ) != memNameMap.end() ) pools.emplace_back( m_memInfo.pool );
        return pools;
    }

    for( const auto& v : memNameMap )
    {
        if( IsGpuD3D12MemoryPool( v.first ) ) pools.emplace_back( v.first );
    }
    std::sort( pools.begin(), pools.end(), [this]( uint64_t lhs, uint64_t rhs ) {
        return strcmp( GetMemoryPoolName( lhs ), GetMemoryPoolName( rhs ) ) < 0;
    } );
    return pools;
}

size_t View::GetMemoryFrameCount( const FrameData& frameSet ) const
{
    if( frameSet.frames.empty() ) return 0;
    return m_worker.GetFullFrameCount( frameSet );
}

View::MemoryFrameMapping View::FindMemoryFrameAtTime( const FrameData& frameSet, int64_t time, int& frameIndex ) const
{
    frameIndex = -1;
    const auto count = GetMemoryFrameCount( frameSet );
    if( count == 0 ) return MemoryFrameMapping::None;

    const auto first = m_worker.GetFrameBegin( frameSet, 0 );
    const auto last = m_worker.GetFrameEnd( frameSet, count - 1 );
    if( time < first || time >= last ) return MemoryFrameMapping::OutsideRange;

    const auto begin = frameSet.frames.begin();
    const auto end = begin + count;
    auto it = std::upper_bound( begin, end, time, []( int64_t value, const auto& frame ) { return value < frame.start; } );
    if( it == begin ) return MemoryFrameMapping::OutsideRange;
    --it;

    const auto idx = int( std::distance( begin, it ) );
    const auto frameBegin = m_worker.GetFrameBegin( frameSet, idx );
    const auto frameEnd = m_worker.GetFrameEnd( frameSet, idx );
    if( frameBegin <= time && time < frameEnd )
    {
        frameIndex = idx;
        return MemoryFrameMapping::Valid;
    }
    return MemoryFrameMapping::BetweenFrames;
}

bool View::SelectMemoryFrame( const FrameData* frameSet, int frameIndex )
{
    if( !frameSet ) return false;
    const auto count = GetMemoryFrameCount( *frameSet );
    if( frameIndex < 0 || size_t( frameIndex ) >= count ) return false;

    auto& selection = m_memInfo.frame;
    selection.active = true;
    selection.frameSet = frameSet;
    selection.frameIndex = frameIndex;
    selection.frameNumberInput = GetFrameNumber( *frameSet, frameIndex );
    selection.mapping = MemoryFrameMapping::Valid;
    selection.dirty = true;
    return true;
}

void View::SelectMemoryFrameAtTime( int64_t time )
{
    auto& selection = m_memInfo.frame;
    const FrameData* frameSet = selection.frameSet;
    if( !frameSet ) frameSet = m_frames ? m_frames : m_worker.GetFramesBase();
    selection.frameSet = frameSet;
    selection.frameIndex = -1;
    selection.mapping = frameSet ? FindMemoryFrameAtTime( *frameSet, time, selection.frameIndex ) : MemoryFrameMapping::None;
    if( selection.mapping == MemoryFrameMapping::Valid )
    {
        selection.frameNumberInput = GetFrameNumber( *frameSet, selection.frameIndex );
    }
    selection.dirty = true;
}

void View::InspectMemoryPlot( const PlotData& plot, size_t item )
{
    if( item >= plot.data.size() || !IsMemoryFramePlot( plot ) ) return;

    const auto& plotItem = plot.data[item];
    auto& selection = m_memInfo.frame;
    selection.active = true;
    selection.triggerActive = true;
    selection.triggerNamedMemory = plot.type == PlotType::Memory;
    selection.triggerTime = plotItem.time.Val();
    selection.triggerPlot = plot.name;
    selection.triggerValue = plotItem.val;
    selection.triggerChange = item == 0 ? 0 : plotItem.val - plot.data[item-1].val;
    selection.tab = MemoryFrameTab::ActiveAtEnd;
    selection.forceTabSelection = true;

    if( plot.type == PlotType::Memory )
    {
        m_memInfo.pool = plot.name;
        m_memInfo.showAllocList = false;
        selection.scope = MemoryFrameScope::SinglePool;
    }
    else
    {
        selection.scope = MemoryFrameScope::AllGpuD3D12Pools;
        const auto pools = GetMemoryFramePools();
        if( !pools.empty() ) m_memInfo.pool = pools.front();
    }

    if( selection.syncFromPlot || selection.frameIndex < 0 ) SelectMemoryFrameAtTime( selection.triggerTime );
    selection.dirty = true;
    m_memInfo.show = true;
    m_memInfo.focus = true;
}

bool View::MemoryFrameSnapshotNeedsRebuild() const
{
    const auto& selection = m_memInfo.frame;
    const auto& snapshot = m_memInfo.frameSnapshot;
    if( selection.dirty ) return true;
    if( selection.mapping != MemoryFrameMapping::Valid ) return false;
    if( !snapshot.valid ) return true;

    const auto pools = GetMemoryFramePools();
    if( pools.size() != m_memInfo.frameSnapshotStamps.size() ) return true;
    for( size_t i=0; i<pools.size(); i++ )
    {
        if( pools[i] != m_memInfo.frameSnapshotStamps[i].pool ) return true;
        const auto& mem = m_worker.GetMemoryNamed( pools[i] );
        if( mem.data.size() != m_memInfo.frameSnapshotStamps[i].allocations || mem.frees.size() != m_memInfo.frameSnapshotStamps[i].frees ) return true;
    }

    const auto frameSet = selection.frameSet;
    if( !frameSet || selection.frameIndex < 0 ) return true;
    return snapshot.begin != m_worker.GetFrameBegin( *frameSet, selection.frameIndex ) || snapshot.end != m_worker.GetFrameEnd( *frameSet, selection.frameIndex );
}

void View::RebuildMemoryFrameSnapshot()
{
    auto& selection = m_memInfo.frame;
    auto& snapshot = m_memInfo.frameSnapshot;
    snapshot = MemoryFrameSnapshot {};
    m_memInfo.frameSnapshotStamps.clear();
    selection.dirty = false;

    if( selection.mapping != MemoryFrameMapping::Valid || !selection.frameSet || selection.frameIndex < 0 ) return;
    const auto count = GetMemoryFrameCount( *selection.frameSet );
    if( size_t( selection.frameIndex ) >= count ) return;

    const auto begin = m_worker.GetFrameBegin( *selection.frameSet, selection.frameIndex );
    const auto end = m_worker.GetFrameEnd( *selection.frameSet, selection.frameIndex );
    if( end <= begin ) return;
    bool possibleCaptureBaseline = false;
    if( m_worker.IsOnDemand() )
    {
        constexpr int64_t BaselineWindow = 100 * 1000 * 1000;
        const auto firstTime = m_worker.GetFirstTime();
        possibleCaptureBaseline = begin <= firstTime + BaselineWindow && end > firstTime;
    }

    const auto pools = GetMemoryFramePools();
    std::vector<analysis::MemoryEventInput> events;
    for( const auto pool : pools )
    {
        const auto& mem = m_worker.GetMemoryNamed( pool );
        m_memInfo.frameSnapshotStamps.emplace_back( MemoryFramePoolStamp { pool, mem.data.size(), mem.frees.size() } );
        events.reserve( events.size() + mem.data.size() );
        for( size_t index=0; index<mem.data.size(); index++ )
        {
            const auto& event = mem.data[index];
            analysis::MemoryEventInput input;
            input.key = { pool, index };
            input.identifier = event.Ptr();
            input.size = event.Size();
            input.allocationNs = event.TimeAlloc();
            if( event.TimeFree() >= 0 ) input.freeNs = event.TimeFree();
            input.allocationThread = m_worker.DecompressThread( event.ThreadAlloc() );
            if( event.TimeFree() >= 0 ) input.freeThread = m_worker.DecompressThread( event.ThreadFree() );
            input.allocationCallstack = event.CsAlloc();
            input.freeCallstack = event.csFree.Val();
            events.emplace_back( input );
        }
    }
    snapshot = analysis::BuildMemoryFrameSnapshot( begin, end, pools, events, possibleCaptureBaseline );
}

void View::DrawMemoryIdentifier( uint64_t pool, const MemEvent& event ) const
{
    if( IsGpuD3D12MemoryPool( pool ) )
    {
        ImGui::Text( "%" PRIu64, event.Ptr() );
    }
    else
    {
        ImGui::Text( "0x%" PRIx64, event.Ptr() );
    }
}

size_t View::GetGpuMemoryAllocationCount() const
{
    size_t count = 0;
    for( const auto& v : m_worker.GetMemNameMap() )
    {
        if( IsGpuD3D12MemoryPool( v.first ) ) count += v.second->data.size();
    }
    return count;
}

bool View::GpuMemoryAttributionNeedsRebuild() const
{
    const auto& cache = m_memInfo.gpuAttribution;
    return !cache.ready ||
        cache.pendingCpuZones ||
        cache.pendingGpuZones ||
        cache.zoneCount != m_worker.GetZoneCount() ||
        cache.gpuZoneCount != m_worker.GetGpuZoneCount() ||
        cache.allocationCount != GetGpuMemoryAllocationCount() ||
        cache.gpuZonesReady != m_worker.AreGpuSourceLocationZonesReady();
}

void View::EnsureGpuMemoryAttribution()
{
    if( !GpuMemoryAttributionNeedsRebuild() ) return;
    auto& cache = m_memInfo.gpuAttribution;
    const auto now = ImGui::GetTime();
    if( now < cache.nextRebuildTime ) return;
    cache.nextRebuildTime = now + 0.5;
    RebuildGpuMemoryAttribution();
}

void View::RebuildGpuMemoryAttribution()
{
    auto& cache = m_memInfo.gpuAttribution;
    const auto nextRebuildTime = cache.nextRebuildTime;
    const auto selectedPassId = cache.selectedPassId;
    cache = GpuMemoryAttributionCache {};
    cache.nextRebuildTime = nextRebuildTime;
    cache.selectedPassId = selectedPassId;
    cache.zoneCount = m_worker.GetZoneCount();
    cache.gpuZoneCount = m_worker.GetGpuZoneCount();
    cache.allocationCount = GetGpuMemoryAllocationCount();
    cache.gpuZonesReady = m_worker.AreGpuSourceLocationZonesReady();

    if( !m_worker.AreSourceLocationZonesReady() )
    {
        cache.ready = false;
        return;
    }

    std::vector<analysis::GpuMemoryCpuZoneInput> cpuInputs;
    std::vector<const ZoneEvent*> cpuZonePointers;
    for( const auto& sourceEntry : m_worker.GetSourceLocationZones() )
    {
        const auto& sourceLocation = m_worker.GetSourceLocation( sourceEntry.first );
        const auto markerNamePtr = m_worker.GetZoneName( sourceLocation );
        const std::string markerName = markerNamePtr ? markerNamePtr : "";
        if( markerName != analysis::GpuMemoryRequestMarker && markerName != analysis::GpuMemoryPassMarker ) continue;
        for( const auto& zoneThread : sourceEntry.second.zones )
        {
            const auto zone = zoneThread.Zone();
            if( !zone ) continue;
            if( zone->End() < 0 ) { cache.pendingCpuZones = true; continue; }
            if( !m_worker.HasZoneExtra( *zone ) ) continue;
            const auto& extra = m_worker.GetZoneExtra( *zone );
            if( !extra.text.Active() ) continue;
            const auto text = m_worker.GetString( extra.text );
            if( !text ) continue;
            const auto zoneName = m_worker.GetZoneName( *zone );
            const auto index = cpuZonePointers.size();
            cpuZonePointers.emplace_back( zone );
            cpuInputs.push_back( {
                index, markerName, zoneName ? zoneName : "", text,
                m_worker.DecompressThread( zoneThread.Thread() ), zone->Start(), m_worker.GetZoneEnd( *zone )
            } );
        }
    }

    std::vector<analysis::GpuMemoryGpuZoneInput> gpuInputs;
    std::vector<const GpuEvent*> gpuZonePointers;
    if( cache.gpuZonesReady )
    {
        for( const auto& sourceEntry : m_worker.GetGpuSourceLocationZones() )
        {
            for( const auto& zoneThread : sourceEntry.second.zones )
            {
                const auto zone = zoneThread.Zone();
                if( !zone ) continue;
                if( zone->GpuEnd() < 0 ) { cache.pendingGpuZones = true; continue; }
                const auto name = m_worker.GetZoneName( *zone );
                if( !name ) continue;
                const auto thread = zone->Thread() != 0 ? m_worker.DecompressThread( zone->Thread() ) : m_worker.DecompressThread( zoneThread.Thread() );
                const auto index = gpuZonePointers.size();
                gpuZonePointers.emplace_back( zone );
                gpuInputs.push_back( { index, name, thread, zone->CpuStart(), zone->GpuStart(), zone->GpuEnd() } );
            }
        }
    }

    std::vector<analysis::GpuMemoryAllocationInput> allocationInputs;
    for( const auto& memoryEntry : m_worker.GetMemNameMap() )
    {
        const auto pool = memoryEntry.first;
        if( !IsGpuD3D12MemoryPool( pool ) ) continue;
        const auto* poolName = m_worker.GetString( pool );
        const auto& memory = *memoryEntry.second;
        for( size_t index=0; index<memory.data.size(); index++ )
        {
            const auto& event = memory.data[index];
            if( event.Ptr() == 0 ) continue;
            allocationInputs.push_back( { { pool, index }, event.Ptr(), event.Size(),
                m_worker.DecompressThread( event.ThreadAlloc() ), event.TimeAlloc(),
                event.TimeFree() >= 0 ? std::optional<int64_t>( event.TimeFree() ) : std::nullopt,
                event.CsAlloc(), event.csFree.Val(), poolName ? poolName : "" } );
        }
    }

    const auto attribution = analysis::BuildGpuMemoryAttribution( cpuInputs, gpuInputs, allocationInputs );
    cache.protocolPresent = attribution.protocolPresent;
    cache.requestScopes.reserve( attribution.requestScopes.size() );
    for( size_t index=0; index<attribution.requestScopes.size(); index++ )
    {
        GpuMemoryRequestScope scope;
        static_cast<analysis::GpuMemoryRequestScope&>( scope ) = attribution.requestScopes[index];
        if( scope.cpuZoneIndex < cpuZonePointers.size() ) scope.zone = cpuZonePointers[scope.cpuZoneIndex];
        const auto scopeIndex = cache.requestScopes.size();
        cache.requestScopes.emplace_back( std::move( scope ) );
        if( cache.requestScopes.back().labelId != 0 ) cache.requestScopeByLabel[cache.requestScopes.back().labelId] = scopeIndex;
    }

    cache.passes.reserve( attribution.passes.size() );
    for( size_t index=0; index<attribution.passes.size(); index++ )
    {
        GpuMemoryPass pass;
        static_cast<analysis::GpuMemoryPass&>( pass ) = attribution.passes[index];
        if( pass.cpuZoneIndex < cpuZonePointers.size() ) pass.relationZone = cpuZonePointers[pass.cpuZoneIndex];
        if( pass.gpuZoneIndex && *pass.gpuZoneIndex < gpuZonePointers.size() ) pass.gpuZone = gpuZonePointers[*pass.gpuZoneIndex];
        pass.gpuPairAmbiguous = pass.gpuPairing == analysis::GpuZonePairing::Ambiguous;
        const auto passIndex = cache.passes.size();
        cache.passById[pass.passId] = passIndex;
        for( const auto& use : pass.uses ) cache.passesByAllocation[use.allocationId].emplace_back( passIndex );
        cache.passes.emplace_back( std::move( pass ) );
    }
    for( const auto& allocation : attribution.allocations )
    {
        cache.allocationById[allocation.allocation.allocationId] = MemoryEventRef { allocation.allocation.key.pool, allocation.allocation.key.index };
        if( allocation.requestLabelId ) cache.requestLabelByAllocation[allocation.allocation.allocationId] = *allocation.requestLabelId;
    }
    if( cache.selectedPassId != 0 && cache.passById.find( cache.selectedPassId ) == cache.passById.end() ) cache.selectedPassId = 0;
    cache.ready = true;
    return;
}

std::string View::FormatGpuMemoryUsage( uint32_t usageMask ) const
{
    return analysis::FormatGpuMemoryUsage( usageMask );
}

bool View::DrawGpuMemoryPassLink( const GpuMemoryPass& pass, int& widgetId )
{
    ImGui::PushID( widgetId++ );
    const bool selected = pass.gpuZone ? m_gpuInfoWindow == pass.gpuZone : m_zoneInfoWindow == pass.relationZone;
    const bool clicked = ImGui::Selectable( pass.name.c_str(), selected );
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    if( clicked )
    {
        if( pass.gpuZone ) ShowZoneInfo( *pass.gpuZone, pass.gpuThread );
        else if( pass.relationZone ) ShowZoneInfo( *pass.relationZone );
    }
    if( hovered )
    {
        if( pass.gpuZone )
        {
            m_gpuHighlight = pass.gpuZone;
            ZoneTooltip( *pass.gpuZone );
            if( IsMouseClicked( 2 ) ) ZoomToZone( *pass.gpuZone );
        }
        else if( pass.relationZone )
        {
            m_zoneHighlight = pass.relationZone;
            ZoneTooltip( *pass.relationZone );
            if( IsMouseClicked( 2 ) ) ZoomToZone( *pass.relationZone );
        }
    }
    return clicked;
}

void View::DrawGpuMemoryPassesForSelectedFrame()
{
    EnsureGpuMemoryAttribution();
    auto& cache = m_memInfo.gpuAttribution;
    if( !cache.ready )
    {
        TextDisabledUnformatted( "Pass attribution is being indexed..." );
        return;
    }
    if( !cache.protocolPresent )
    {
        TextColoredUnformatted( ImVec4( 1.f, 0.8f, 0.2f, 1.f ), "No GTMEM1 pass relations in this capture. Use a pass-attribution Godot build with allocation/full D3D12 memory mode." );
        return;
    }

    const auto& snapshot = m_memInfo.frameSnapshot;
    std::vector<size_t> framePasses;
    framePasses.reserve( cache.passes.size() );
    unordered_flat_map<uint64_t, uint32_t> framePassCountByLabel;
    for( size_t i=0; i<cache.passes.size(); i++ )
    {
        const auto& pass = cache.passes[i];
        if( pass.start >= snapshot.begin && pass.start < snapshot.end )
        {
            framePasses.emplace_back( i );
            if( pass.labelId != 0 ) framePassCountByLabel[pass.labelId]++;
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted( ICON_FA_DIAGRAM_PROJECT " Passes in frame" );
    if( framePasses.empty() )
    {
        TextDisabledUnformatted( "No GTMEM1 pass relation zone starts in the selected frame." );
        return;
    }

    unordered_flat_set<uint64_t> uniqueAllocations;
    uint64_t uniqueBytes = 0;
    uint64_t relationCount = 0;
    uint64_t untrackedReferences = 0;
    uint32_t linkedAllocations = 0;
    uint32_t gpuPairs = 0;
    uint32_t ambiguousGpuPairs = 0;
    uint32_t incompletePasses = 0;
    uint32_t truncatedPasses = 0;
    for( const auto passIndex : framePasses )
    {
        const auto& pass = cache.passes[passIndex];
        if( pass.gpuZone ) gpuPairs++;
        if( pass.gpuPairAmbiguous ) ambiguousGpuPairs++;
        if( !pass.complete ) incompletePasses++;
        if( pass.truncated ) truncatedPasses++;
        relationCount += pass.uses.size();
        untrackedReferences += pass.untrackedReferences;
        for( const auto& use : pass.uses )
        {
            if( !uniqueAllocations.emplace( use.allocationId ).second ) continue;
            const auto allocation = cache.allocationById.find( use.allocationId );
            if( allocation != cache.allocationById.end() )
            {
                const auto& ref = allocation->second;
                uniqueBytes += m_worker.GetMemoryNamed( ref.pool ).data[ref.index].Size();
                linkedAllocations++;
            }
        }
    }

    ImGui::Text( "%s pass(es) | %s/%s linked allocation(s) | %s relation(s) | %s unique referenced bytes",
        RealToString( framePasses.size() ), RealToString( linkedAllocations ), RealToString( uniqueAllocations.size() ), RealToString( relationCount ), MemSizeToString( uniqueBytes ) );
    ImGui::Text( "%s uniquely paired GPU zone(s) | %s CPU fallback(s) | %s ambiguous | %s untracked reference(s) | %s truncated pass(es)",
        RealToString( gpuPairs ), RealToString( framePasses.size() - gpuPairs - ambiguousGpuPairs ), RealToString( ambiguousGpuPairs ), RealToString( untrackedReferences ), RealToString( truncatedPasses ) );
    if( incompletePasses != 0 )
    {
        TextColoredUnformatted( ImVec4( 1.f, 0.4f, 0.3f, 1.f ), "One or more relation payloads are incomplete; affected rows are marked below." );
    }
    TextDisabledUnformatted( "Referenced bytes are a per-pass resource set, not exclusive ownership. Do not sum pass rows; use the unique union above." );
    TextDisabledUnformatted( "History begins when the capture connects. Request origins and earlier pass uses for replayed baseline allocations may be unavailable." );

    const auto height = ImGui::GetTextLineHeightWithSpacing() * std::min<size_t>( framePasses.size() + 2, 16 );
    const auto flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY;
    if( !ImGui::BeginTable( "##gpuMemoryFramePasses", 14, flags, ImVec2( 0, height ) ) ) return;
    ImGui::TableSetupScrollFreeze( 1, 1 );
    ImGui::TableSetupColumn( "Pass", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHide, 280 );
    ImGui::TableSetupColumn( "Level", ImGuiTableColumnFlags_WidthFixed, 55 );
    ImGui::TableSetupColumn( "Operations", ImGuiTableColumnFlags_WidthFixed, 110 );
    ImGui::TableSetupColumn( "Resources", ImGuiTableColumnFlags_WidthFixed, 100 );
    ImGui::TableSetupColumn( "Textures", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 150 );
    ImGui::TableSetupColumn( "Buffers", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 150 );
    ImGui::TableSetupColumn( "Read-only bytes", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 115 );
    ImGui::TableSetupColumn( "Write/RW bytes", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 115 );
    ImGui::TableSetupColumn( "Referenced bytes", ImGuiTableColumnFlags_WidthFixed, 120 );
    ImGui::TableSetupColumn( "First-use bytes", ImGuiTableColumnFlags_WidthFixed, 110 );
    ImGui::TableSetupColumn( "Request allocations / bytes", ImGuiTableColumnFlags_WidthFixed, 175 );
    ImGui::TableSetupColumn( "GPU duration", ImGuiTableColumnFlags_WidthFixed, 110 );
    ImGui::TableSetupColumn( "Untracked refs", ImGuiTableColumnFlags_WidthFixed, 100 );
    ImGui::TableSetupColumn( "Quality", ImGuiTableColumnFlags_WidthFixed, 130 );
    ImGui::TableHeadersRow();

    int widgetId = 0;
    for( const auto passIndex : framePasses )
    {
        const auto& pass = cache.passes[passIndex];
        uint64_t referencedBytes = 0;
        uint64_t firstUseBytes = 0;
        uint64_t requestScopeBytes = 0;
        uint64_t textureBytes = 0;
        uint64_t bufferBytes = 0;
        uint64_t readOnlyBytes = 0;
        uint64_t writeBytes = 0;
        uint32_t textureCount = 0;
        uint32_t bufferCount = 0;
        uint32_t requestScopeAllocations = 0;
        for( const auto& use : pass.uses )
        {
            const auto allocation = cache.allocationById.find( use.allocationId );
            if( allocation == cache.allocationById.end() ) continue;
            const auto& ref = allocation->second;
            const auto size = m_worker.GetMemoryNamed( ref.pool ).data[ref.index].Size();
            referencedBytes += size;
            if( use.kind == 'T' )
            {
                textureCount++;
                textureBytes += size;
            }
            else if( use.kind == 'B' )
            {
                bufferCount++;
                bufferBytes += size;
            }
            if( ( use.usageMask & ( 1u << 1 ) ) != 0 ) writeBytes += size;
            else if( ( use.usageMask & ( 1u << 0 ) ) != 0 ) readOnlyBytes += size;

            const auto history = cache.passesByAllocation.find( use.allocationId );
            if( history != cache.passesByAllocation.end() && !history->second.empty() && history->second.front() == passIndex ) firstUseBytes += size;
            const auto request = cache.requestLabelByAllocation.find( use.allocationId );
            if( request != cache.requestLabelByAllocation.end() && request->second == pass.labelId )
            {
                requestScopeAllocations++;
                requestScopeBytes += size;
            }
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if( DrawGpuMemoryPassLink( pass, widgetId ) ) cache.selectedPassId = pass.passId;
        ImGui::TableNextColumn();
        ImGui::TextUnformatted( RealToString( pass.level ) );
        ImGui::TableNextColumn();
        ImGui::TextUnformatted( pass.operations.empty() ? "-" : pass.operations.c_str() );
        ImGui::TableNextColumn();
        if( pass.truncated ) ImGui::Text( "%s / %s", RealToString( pass.emittedUseCount ), RealToString( pass.totalUseCount ) );
        else ImGui::TextUnformatted( RealToString( pass.uses.size() ) );
        ImGui::TableNextColumn();
        ImGui::Text( "%s / %s", RealToString( textureCount ), MemSizeToString( textureBytes ) );
        ImGui::TableNextColumn();
        ImGui::Text( "%s / %s", RealToString( bufferCount ), MemSizeToString( bufferBytes ) );
        ImGui::TableNextColumn();
        ImGui::TextUnformatted( MemSizeToString( readOnlyBytes ) );
        ImGui::TableNextColumn();
        ImGui::TextUnformatted( MemSizeToString( writeBytes ) );
        ImGui::TableNextColumn();
        ImGui::TextUnformatted( MemSizeToString( referencedBytes ) );
        ImGui::TableNextColumn();
        ImGui::TextUnformatted( MemSizeToString( firstUseBytes ) );
        ImGui::TableNextColumn();
        if( requestScopeAllocations == 0 ) TextDisabledUnformatted( "-" );
        else if( framePassCountByLabel[pass.labelId] > 1 ) ImGui::Text( "%s / %s (shared)", RealToString( requestScopeAllocations ), MemSizeToString( requestScopeBytes ) );
        else ImGui::Text( "%s / %s", RealToString( requestScopeAllocations ), MemSizeToString( requestScopeBytes ) );
        ImGui::TableNextColumn();
        if( pass.gpuZone && pass.gpuZone->GpuEnd() >= 0 ) ImGui::TextUnformatted( TimeToString( pass.gpuZone->GpuEnd() - pass.gpuZone->GpuStart() ) );
        else if( pass.gpuPairAmbiguous ) TextColoredUnformatted( ImVec4( 1.f, 0.8f, 0.2f, 1.f ), "Ambiguous" );
        else TextDisabledUnformatted( "CPU relation" );
        ImGui::TableNextColumn();
        ImGui::TextUnformatted( RealToString( pass.untrackedReferences ) );
        ImGui::TableNextColumn();
        if( !pass.complete ) TextColoredUnformatted( ImVec4( 1.f, 0.3f, 0.2f, 1.f ), "Incomplete" );
        else if( pass.truncated ) ImGui::Text( "Truncated (%s)", RealToString( pass.droppedUses ) );
        else if( pass.gpuPairAmbiguous ) TextColoredUnformatted( ImVec4( 1.f, 0.8f, 0.2f, 1.f ), "Exact; GPU ambiguous" );
        else TextColoredUnformatted( ImVec4( 0.5f, 1.f, 0.5f, 1.f ), "Exact relation" );
    }
    ImGui::EndTable();

    if( cache.selectedPassId != 0 )
    {
        const auto selected = cache.passById.find( cache.selectedPassId );
        if( selected == cache.passById.end() || cache.passes[selected->second].start < snapshot.begin || cache.passes[selected->second].start >= snapshot.end )
        {
            cache.selectedPassId = 0;
        }
        else
        {
            DrawGpuMemoryPassDetails( cache.passes[selected->second], widgetId );
        }
    }
}

void View::DrawGpuMemoryPassDetails( const GpuMemoryPass& pass, int& widgetId )
{
    auto& cache = m_memInfo.gpuAttribution;
    const auto& selection = m_memInfo.frame;
    const auto& snapshot = m_memInfo.frameSnapshot;

    uint64_t referencedBytes = 0;
    uint64_t firstUseBytes = 0;
    for( const auto& use : pass.uses )
    {
        const auto allocation = cache.allocationById.find( use.allocationId );
        if( allocation == cache.allocationById.end() ) continue;
        const auto& ref = allocation->second;
        const auto size = m_worker.GetMemoryNamed( ref.pool ).data[ref.index].Size();
        referencedBytes += size;
        const auto history = cache.passesByAllocation.find( use.allocationId );
        if( history != cache.passesByAllocation.end() && !history->second.empty() && cache.passes[history->second.front()].passId == pass.passId ) firstUseBytes += size;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text( ICON_FA_DIAGRAM_PROJECT " Pass details: %s", pass.name.c_str() );
    ImGui::SameLine();
    if( ImGui::SmallButton( ICON_FA_XMARK " Close##gpuMemoryPassDetails" ) )
    {
        cache.selectedPassId = 0;
        return;
    }
    ImGui::Text( "Pass ID: %" PRIu64 " | Request label: %" PRIu64 " | Godot frame: %" PRIu64 " | Level: %d | Operations: %s",
        pass.passId, pass.labelId, pass.frame, pass.level, pass.operations.empty() ? "-" : pass.operations.c_str() );
    if( pass.gpuZone && pass.gpuZone->GpuEnd() >= 0 )
    {
        ImGui::Text( "GPU duration: %s | Tracked resources: %s | Referenced bytes: %s | First-use bytes: %s",
            TimeToString( pass.gpuZone->GpuEnd() - pass.gpuZone->GpuStart() ), RealToString( pass.uses.size() ), MemSizeToString( referencedBytes ), MemSizeToString( firstUseBytes ) );
    }
    else
    {
        ImGui::Text( "GPU duration: %s | Tracked resources: %s | Referenced bytes: %s | First-use bytes: %s",
            pass.gpuPairAmbiguous ? "Ambiguous GPU link" : "CPU relation fallback", RealToString( pass.uses.size() ), MemSizeToString( referencedBytes ), MemSizeToString( firstUseBytes ) );
    }
    ImGui::Text( "Untracked references: %s | Emitted/total resources: %s/%s | Payload: %s",
        RealToString( pass.untrackedReferences ), RealToString( pass.emittedUseCount ), RealToString( pass.totalUseCount ), pass.complete ? ( pass.truncated ? "Complete, truncated" : "Complete" ) : "Incomplete" );
    TextDisabledUnformatted( "Select an allocation ID below to open its normal Memory allocation detail window." );

    const auto height = ImGui::GetTextLineHeightWithSpacing() * std::min<size_t>( pass.uses.size() + 2, 16 );
    const auto flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY;
    if( !ImGui::BeginTable( "##gpuMemoryPassResources", 12, flags, ImVec2( 0, height ) ) ) return;
    ImGui::TableSetupScrollFreeze( 2, 1 );
    ImGui::TableSetupColumn( "Logical allocation ID", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHide, 150 );
    ImGui::TableSetupColumn( "Pool", ImGuiTableColumnFlags_WidthFixed, 240 );
    ImGui::TableSetupColumn( "Kind", ImGuiTableColumnFlags_WidthFixed, 70 );
    ImGui::TableSetupColumn( "Size", ImGuiTableColumnFlags_WidthFixed, 100 );
    ImGui::TableSetupColumn( "State in frame", ImGuiTableColumnFlags_WidthFixed, 120 );
    ImGui::TableSetupColumn( "Usage", ImGuiTableColumnFlags_WidthFixed, 230 );
    ImGui::TableSetupColumn( "Alloc frame", ImGuiTableColumnFlags_WidthFixed, 90 );
    ImGui::TableSetupColumn( "Free frame", ImGuiTableColumnFlags_WidthFixed, 90 );
    ImGui::TableSetupColumn( "Request scope", ImGuiTableColumnFlags_WidthFixed, 250 );
    ImGui::TableSetupColumn( "First use", ImGuiTableColumnFlags_WidthFixed, 250 );
    ImGui::TableSetupColumn( "Last use", ImGuiTableColumnFlags_WidthFixed, 250 );
    ImGui::TableSetupColumn( "Alloc call stack", ImGuiTableColumnFlags_WidthFixed, 110 );
    ImGui::TableHeadersRow();

    ImGuiListClipper clipper;
    clipper.Begin( int( pass.uses.size() ) );
    while( clipper.Step() )
    {
        for( int row=clipper.DisplayStart; row<clipper.DisplayEnd; row++ )
        {
            const auto& use = pass.uses[row];
            const auto allocation = cache.allocationById.find( use.allocationId );
            const bool mapped = allocation != cache.allocationById.end();
            const MemoryEventRef ref = mapped ? allocation->second : MemoryEventRef {};
            const MemEvent* event = mapped ? &m_worker.GetMemoryNamed( ref.pool ).data[ref.index] : nullptr;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID( widgetId++ );
            if( mapped )
            {
                if( ImGui::Selectable( RealToString( use.allocationId ), m_memoryAllocInfoPool == ref.pool && m_memoryAllocInfoWindow == int64_t( ref.index ) ) )
                {
                    m_memoryAllocInfoPool = ref.pool;
                    m_memoryAllocInfoWindow = int64_t( ref.index );
                }
            }
            else
            {
                ImGui::TextUnformatted( RealToString( use.allocationId ) );
            }
            ImGui::PopID();

            ImGui::TableNextColumn();
            if( mapped ) ImGui::TextUnformatted( GetMemoryPoolName( ref.pool ) );
            else TextDisabledUnformatted( "Unmapped" );

            ImGui::TableNextColumn();
            const char* kind = use.kind == 'T' ? "Texture" : use.kind == 'B' ? "Buffer" : use.kind == 'A' ? "AS" : "Unknown";
            ImGui::TextUnformatted( kind );

            ImGui::TableNextColumn();
            if( event ) ImGui::TextUnformatted( MemSizeToString( event->Size() ) );
            else TextDisabledUnformatted( "-" );

            ImGui::TableNextColumn();
            if( event )
            {
                const auto ta = event->TimeAlloc();
                const auto tf = event->TimeFree();
                if( ta >= snapshot.end ) TextDisabledUnformatted( "Not allocated yet" );
                else if( tf >= 0 && tf < snapshot.begin ) TextDisabledUnformatted( "Freed before frame" );
                else if( ta >= snapshot.begin && tf >= 0 && tf < snapshot.end ) ImGui::TextUnformatted( "Transient" );
                else if( ta >= snapshot.begin ) ImGui::TextUnformatted( "Allocated in frame" );
                else if( tf >= snapshot.begin && tf < snapshot.end ) ImGui::TextUnformatted( "Freed in frame" );
                else TextColoredUnformatted( ImVec4( 0.6f, 1.f, 0.6f, 1.f ), "Active" );
            }
            else TextDisabledUnformatted( "-" );

            ImGui::TableNextColumn();
            const auto usage = FormatGpuMemoryUsage( use.usageMask );
            ImGui::TextUnformatted( usage.c_str() );

            int allocFrame = -1;
            int freeFrame = -1;
            const bool hasAllocFrame = event && selection.frameSet && FindMemoryFrameAtTime( *selection.frameSet, event->TimeAlloc(), allocFrame ) == MemoryFrameMapping::Valid;
            const bool hasFreeFrame = event && event->TimeFree() >= 0 && selection.frameSet && FindMemoryFrameAtTime( *selection.frameSet, event->TimeFree(), freeFrame ) == MemoryFrameMapping::Valid;
            ImGui::TableNextColumn();
            if( hasAllocFrame ) ImGui::TextUnformatted( RealToString( GetFrameNumber( *selection.frameSet, allocFrame ) ) );
            else TextDisabledUnformatted( "-" );
            ImGui::TableNextColumn();
            if( event && event->TimeFree() < 0 ) TextColoredUnformatted( ImVec4( 0.6f, 1.f, 0.6f, 1.f ), "Active" );
            else if( hasFreeFrame ) ImGui::TextUnformatted( RealToString( GetFrameNumber( *selection.frameSet, freeFrame ) ) );
            else TextDisabledUnformatted( "-" );

            const GpuMemoryRequestScope* requestScope = nullptr;
            const auto requestLabel = cache.requestLabelByAllocation.find( use.allocationId );
            if( requestLabel != cache.requestLabelByAllocation.end() )
            {
                const auto scope = cache.requestScopeByLabel.find( requestLabel->second );
                if( scope != cache.requestScopeByLabel.end() ) requestScope = &cache.requestScopes[scope->second];
            }
            ImGui::TableNextColumn();
            if( requestScope )
            {
                ImGui::PushID( widgetId++ );
                const bool selected = ImGui::Selectable( requestScope->name.c_str(), m_zoneInfoWindow == requestScope->zone );
                const bool hovered = ImGui::IsItemHovered();
                ImGui::PopID();
                if( selected ) ShowZoneInfo( *requestScope->zone );
                if( hovered )
                {
                    m_zoneHighlight = requestScope->zone;
                    ZoneTooltip( *requestScope->zone );
                    if( IsMouseClicked( 2 ) ) ZoomToZone( *requestScope->zone );
                }
            }
            else TextDisabledUnformatted( "Unknown / baseline" );

            const auto history = cache.passesByAllocation.find( use.allocationId );
            const bool hasHistory = history != cache.passesByAllocation.end() && !history->second.empty();
            ImGui::TableNextColumn();
            if( hasHistory ) DrawGpuMemoryPassLink( cache.passes[history->second.front()], widgetId );
            else TextDisabledUnformatted( "-" );
            ImGui::TableNextColumn();
            if( hasHistory ) DrawGpuMemoryPassLink( cache.passes[history->second.back()], widgetId );
            else TextDisabledUnformatted( "-" );

            ImGui::TableNextColumn();
            if( event && event->CsAlloc() != 0 ) SmallCallstackButton( "alloc", event->CsAlloc(), widgetId );
            else TextDisabledUnformatted( "-" );
        }
    }
    ImGui::EndTable();
}

void View::DrawGpuMemoryAllocationAttribution( uint64_t allocationId, int& widgetId )
{
    EnsureGpuMemoryAttribution();
    const auto& cache = m_memInfo.gpuAttribution;
    ImGui::Separator();
    ImGui::TextUnformatted( ICON_FA_DIAGRAM_PROJECT " GPU pass attribution" );
    if( !cache.ready )
    {
        TextDisabledUnformatted( "Pass attribution is being indexed..." );
        return;
    }
    if( !cache.protocolPresent )
    {
        TextColoredUnformatted( ImVec4( 1.f, 0.8f, 0.2f, 1.f ), "No GTMEM1 relation metadata is present in this capture." );
        return;
    }

    const GpuMemoryRequestScope* requestScope = nullptr;
    const auto requestLabel = cache.requestLabelByAllocation.find( allocationId );
    if( requestLabel != cache.requestLabelByAllocation.end() )
    {
        const auto scope = cache.requestScopeByLabel.find( requestLabel->second );
        if( scope != cache.requestScopeByLabel.end() ) requestScope = &cache.requestScopes[scope->second];
    }

    ImGui::TextUnformatted( "Request scope:" );
    ImGui::SameLine();
    if( requestScope )
    {
        ImGui::PushID( widgetId++ );
        const bool selected = ImGui::Selectable( requestScope->name.c_str(), m_zoneInfoWindow == requestScope->zone, 0, ImVec2( 0, 0 ) );
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if( selected ) ShowZoneInfo( *requestScope->zone );
        if( hovered )
        {
            m_zoneHighlight = requestScope->zone;
            ZoneTooltip( *requestScope->zone );
            if( IsMouseClicked( 2 ) ) ZoomToZone( *requestScope->zone );
        }
    }
    else
    {
        TextDisabledUnformatted( "Unknown (pre-existing, replayed, or allocated outside a labeled request scope)" );
    }

    const auto history = cache.passesByAllocation.find( allocationId );
    if( history == cache.passesByAllocation.end() || history->second.empty() )
    {
        TextDisabledUnformatted( "No captured pass references this allocation." );
        return;
    }

    ImGui::Text( "Pass count: %s", RealToString( history->second.size() ) );
    const auto& firstPass = cache.passes[history->second.front()];
    const auto& lastPass = cache.passes[history->second.back()];
    ImGui::Text( "First use: %s", firstPass.name.c_str() );
    ImGui::Text( "Last use: %s", lastPass.name.c_str() );
    if( requestScope && requestScope->labelId != firstPass.labelId )
    {
        TextDisabledUnformatted( "The request scope and first-use pass differ; this is expected for persistent or prepared resources." );
    }

    const auto height = ImGui::GetTextLineHeightWithSpacing() * std::min<size_t>( history->second.size() + 2, 14 );
    const auto flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY;
    if( !ImGui::BeginTable( "##gpuMemoryAllocationPassHistory", 6, flags, ImVec2( 680, height ) ) ) return;
    ImGui::TableSetupScrollFreeze( 1, 1 );
    ImGui::TableSetupColumn( "Pass", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHide, 280 );
    ImGui::TableSetupColumn( "Frame", ImGuiTableColumnFlags_WidthFixed, 75 );
    ImGui::TableSetupColumn( "Operations", ImGuiTableColumnFlags_WidthFixed, 100 );
    ImGui::TableSetupColumn( "Access", ImGuiTableColumnFlags_WidthFixed, 220 );
    ImGui::TableSetupColumn( "GPU duration", ImGuiTableColumnFlags_WidthFixed, 100 );
    ImGui::TableSetupColumn( "Relation", ImGuiTableColumnFlags_WidthFixed, 105 );
    ImGui::TableHeadersRow();

    for( const auto passIndex : history->second )
    {
        const auto& pass = cache.passes[passIndex];
        uint32_t usageMask = 0;
        for( const auto& use : pass.uses )
        {
            if( use.allocationId == allocationId )
            {
                usageMask = use.usageMask;
                break;
            }
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        DrawGpuMemoryPassLink( pass, widgetId );
        ImGui::TableNextColumn();
        ImGui::TextUnformatted( RealToString( pass.frame ) );
        ImGui::TableNextColumn();
        ImGui::TextUnformatted( pass.operations.empty() ? "-" : pass.operations.c_str() );
        ImGui::TableNextColumn();
        const auto usage = FormatGpuMemoryUsage( usageMask );
        ImGui::TextUnformatted( usage.c_str() );
        ImGui::TableNextColumn();
        if( pass.gpuZone && pass.gpuZone->GpuEnd() >= 0 ) ImGui::TextUnformatted( TimeToString( pass.gpuZone->GpuEnd() - pass.gpuZone->GpuStart() ) );
        else if( pass.gpuPairAmbiguous ) TextColoredUnformatted( ImVec4( 1.f, 0.8f, 0.2f, 1.f ), "Ambiguous" );
        else TextDisabledUnformatted( "-" );
        ImGui::TableNextColumn();
        if( !pass.complete ) TextColoredUnformatted( ImVec4( 1.f, 0.3f, 0.2f, 1.f ), "Incomplete" );
        else if( pass.gpuPairAmbiguous ) TextColoredUnformatted( ImVec4( 1.f, 0.8f, 0.2f, 1.f ), "Ambiguous" );
        else ImGui::TextUnformatted( pass.gpuZone ? "GPU paired" : "CPU fallback" );
    }
    ImGui::EndTable();
}

void View::DrawMemoryFrameSummary()
{
    const auto& snapshot = m_memInfo.frameSnapshot;
    const auto& total = snapshot.total;
    if( ImGui::BeginTable( "##memoryFrameSummary", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp ) )
    {
        ImGui::TableSetupColumn( "State" );
        ImGui::TableSetupColumn( "Bytes" );
        ImGui::TableSetupColumn( "Allocations" );
        ImGui::TableHeadersRow();

        auto row = []( const char* name, uint64_t bytes, uint64_t count, const char* prefix ) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted( name );
            ImGui::TableNextColumn();
            ImGui::Text( "%s%s", prefix, MemSizeToString( bytes ) );
            ImGui::TableNextColumn();
            ImGui::Text( "%s%s", prefix, RealToString( count ) );
        };

        row( "Frame start", total.startBytes, total.startCount, "" );
        row( "Allocated in frame", total.allocatedBytes, total.allocatedCount, "+" );
        row( "Freed in frame", total.freedBytes, total.freedCount, "-" );
        row( "Frame end", total.endBytes, total.endCount, "" );

        const bool bytesPositive = total.endBytes >= total.startBytes;
        const bool countPositive = total.endCount >= total.startCount;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted( "Net change" );
        ImGui::TableNextColumn();
        ImGui::Text( "%c%s", bytesPositive ? '+' : '-', MemSizeToString( bytesPositive ? total.endBytes - total.startBytes : total.startBytes - total.endBytes ) );
        ImGui::TableNextColumn();
        ImGui::Text( "%c%s", countPositive ? '+' : '-', RealToString( countPositive ? total.endCount - total.startCount : total.startCount - total.endCount ) );

        row( "Frame peak", total.peakBytes, total.peakCount, "" );
        ImGui::EndTable();
    }

    if( snapshot.pools.size() > 1 && ImGui::TreeNode( "Per-pool frame summary" ) )
    {
        const auto height = ImGui::GetTextLineHeightWithSpacing() * std::min<size_t>( snapshot.pools.size() + 2, 12 );
        if( ImGui::BeginTable( "##memoryFramePoolSummary", 10, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY, ImVec2( 0, height ) ) )
        {
            ImGui::TableSetupColumn( "Pool", ImGuiTableColumnFlags_WidthFixed, 260 );
            ImGui::TableSetupColumn( "Start" );
            ImGui::TableSetupColumn( "Allocated" );
            ImGui::TableSetupColumn( "Freed" );
            ImGui::TableSetupColumn( "End" );
            ImGui::TableSetupColumn( "Net" );
            ImGui::TableSetupColumn( "Peak" );
            ImGui::TableSetupColumn( "Start count" );
            ImGui::TableSetupColumn( "End count" );
            ImGui::TableSetupColumn( "Peak count" );
            ImGui::TableHeadersRow();
            for( const auto& summary : snapshot.pools )
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted( GetMemoryPoolName( summary.pool ) );
                ImGui::TableNextColumn();
                ImGui::TextUnformatted( MemSizeToString( summary.startBytes ) );
                ImGui::TableNextColumn();
                ImGui::Text( "+%s", MemSizeToString( summary.allocatedBytes ) );
                ImGui::TableNextColumn();
                ImGui::Text( "-%s", MemSizeToString( summary.freedBytes ) );
                ImGui::TableNextColumn();
                ImGui::TextUnformatted( MemSizeToString( summary.endBytes ) );
                ImGui::TableNextColumn();
                if( summary.endBytes >= summary.startBytes ) ImGui::Text( "+%s", MemSizeToString( summary.endBytes - summary.startBytes ) );
                else ImGui::Text( "-%s", MemSizeToString( summary.startBytes - summary.endBytes ) );
                ImGui::TableNextColumn();
                ImGui::TextUnformatted( MemSizeToString( summary.peakBytes ) );
                ImGui::TableNextColumn();
                ImGui::TextUnformatted( RealToString( summary.startCount ) );
                ImGui::TableNextColumn();
                ImGui::TextUnformatted( RealToString( summary.endCount ) );
                ImGui::TableNextColumn();
                ImGui::TextUnformatted( RealToString( summary.peakCount ) );
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
}

void View::DrawMemoryFrameInspector()
{
    auto& selection = m_memInfo.frame;
    ImGui::Separator();
    ImGui::TextUnformatted( ICON_FA_FILM " Frame memory inspector" );
    ImGui::SameLine();
    if( ImGui::Checkbox( "Enabled##memoryFrame", &selection.active ) && selection.active )
    {
        if( !selection.frameSet ) selection.frameSet = m_frames ? m_frames : m_worker.GetFramesBase();
        const auto time = m_vd.zvStart + ( m_vd.zvEnd - m_vd.zvStart ) / 2;
        SelectMemoryFrameAtTime( time );
        if( selection.mapping != MemoryFrameMapping::Valid && selection.frameSet && GetMemoryFrameCount( *selection.frameSet ) != 0 ) SelectMemoryFrame( selection.frameSet, 0 );
    }
    if( !selection.active ) return;

    const bool gpuScope = selection.scope == MemoryFrameScope::AllGpuD3D12Pools || IsGpuD3D12MemoryPool( m_memInfo.pool );
    if( gpuScope ) EnsureGpuMemoryAttribution();

    const auto gpuPools = [&]() {
        const auto oldScope = selection.scope;
        selection.scope = MemoryFrameScope::AllGpuD3D12Pools;
        auto pools = GetMemoryFramePools();
        selection.scope = oldScope;
        return pools;
    }();

    TextDisabledUnformatted( "Scope:" );
    ImGui::SameLine();
    const auto scopeName = selection.scope == MemoryFrameScope::AllGpuD3D12Pools ? "All GPU D3D12 pools" : GetMemoryPoolName( m_memInfo.pool );
    ImGui::SetNextItemWidth( 300 * GetScale() );
    if( ImGui::BeginCombo( "##memoryFrameScope", scopeName ) )
    {
        if( !gpuPools.empty() && ImGui::Selectable( "All GPU D3D12 pools", selection.scope == MemoryFrameScope::AllGpuD3D12Pools ) )
        {
            selection.scope = MemoryFrameScope::AllGpuD3D12Pools;
            selection.dirty = true;
        }

        std::vector<uint64_t> pools;
        for( const auto& v : m_worker.GetMemNameMap() ) pools.emplace_back( v.first );
        std::sort( pools.begin(), pools.end(), [this]( uint64_t lhs, uint64_t rhs ) { return strcmp( GetMemoryPoolName( lhs ), GetMemoryPoolName( rhs ) ) < 0; } );
        for( const auto pool : pools )
        {
            const bool selected = selection.scope == MemoryFrameScope::SinglePool && m_memInfo.pool == pool;
            if( ImGui::Selectable( GetMemoryPoolName( pool ), selected ) )
            {
                m_memInfo.pool = pool;
                selection.scope = MemoryFrameScope::SinglePool;
                selection.dirty = true;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    TextDisabledUnformatted( "Frame set:" );
    ImGui::SameLine();
    if( !selection.frameSet ) selection.frameSet = m_frames ? m_frames : m_worker.GetFramesBase();
    ImGui::SetNextItemWidth( 180 * GetScale() );
    if( ImGui::BeginCombo( "##memoryFrameSet", selection.frameSet ? GetFrameSetName( *selection.frameSet ) : "No frames" ) )
    {
        for( const auto frameSet : m_worker.GetFrames() )
        {
            const bool selected = frameSet == selection.frameSet;
            if( ImGui::Selectable( GetFrameSetName( *frameSet ), selected ) )
            {
                selection.frameSet = frameSet;
                const auto time = selection.triggerActive && selection.syncFromPlot ? selection.triggerTime : m_vd.zvStart + ( m_vd.zvEnd - m_vd.zvStart ) / 2;
                SelectMemoryFrameAtTime( time );
                if( selection.mapping != MemoryFrameMapping::Valid && GetMemoryFrameCount( *frameSet ) != 0 ) SelectMemoryFrame( frameSet, 0 );
            }
        }
        ImGui::EndCombo();
    }

    const auto frameCount = selection.frameSet ? GetMemoryFrameCount( *selection.frameSet ) : 0;
    const bool hasPrevious = selection.mapping == MemoryFrameMapping::Valid && selection.frameIndex > 0;
    const bool hasNext = selection.mapping == MemoryFrameMapping::Valid && selection.frameIndex >= 0 && size_t( selection.frameIndex + 1 ) < frameCount;
    if( !hasPrevious ) ImGui::BeginDisabled();
    if( ImGui::Button( "<##memoryFramePrevious" ) ) SelectMemoryFrame( selection.frameSet, selection.frameIndex - 1 );
    if( !hasPrevious ) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth( 130 * GetScale() );
    if( ImGui::InputScalar( "##memoryFrameNumber", ImGuiDataType_U64, &selection.frameNumberInput, nullptr, nullptr, nullptr, ImGuiInputTextFlags_EnterReturnsTrue ) && selection.frameSet )
    {
        bool found = false;
        for( size_t i=0; i<frameCount; i++ )
        {
            if( GetFrameNumber( *selection.frameSet, int( i ) ) == selection.frameNumberInput )
            {
                SelectMemoryFrame( selection.frameSet, int( i ) );
                found = true;
                break;
            }
        }
        if( !found ) selection.mapping = MemoryFrameMapping::OutsideRange;
    }
    ImGui::SameLine();
    if( !hasNext ) ImGui::BeginDisabled();
    if( ImGui::Button( ">##memoryFrameNext" ) ) SelectMemoryFrame( selection.frameSet, selection.frameIndex + 1 );
    if( !hasNext ) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox( "Sync from plot/timeline", &selection.syncFromPlot );

    if( selection.triggerActive )
    {
        const auto plotName = selection.triggerNamedMemory && selection.triggerPlot == 0 ? "Default allocator" : m_worker.GetString( selection.triggerPlot );
        TextDisabledUnformatted( "Triggered by:" );
        ImGui::SameLine();
        if( selection.triggerChange >= 0 )
        {
            ImGui::Text( "%s | %s | Value %s | Change +%s", plotName ? plotName : "Unnamed plot", TimeToStringExact( selection.triggerTime ), FormatPlotValue( selection.triggerValue, PlotValueFormatting::Memory ), FormatPlotValue( selection.triggerChange, PlotValueFormatting::Memory ) );
        }
        else
        {
            ImGui::Text( "%s | %s | Value %s | Change %s", plotName ? plotName : "Unnamed plot", TimeToStringExact( selection.triggerTime ), FormatPlotValue( selection.triggerValue, PlotValueFormatting::Memory ), FormatPlotValue( selection.triggerChange, PlotValueFormatting::Memory ) );
        }
        if( !selection.triggerNamedMemory )
        {
            const auto& attribution = m_memInfo.gpuAttribution;
            if( gpuScope && attribution.ready && attribution.protocolPresent )
            {
                TextColoredUnformatted( ImVec4( 0.5f, 1.f, 0.5f, 1.f ), "Exact GTMEM1 pass-resource relations are available for named D3D12 allocations." );
            }
            else
            {
                TextColoredUnformatted( ImVec4( 1.f, 0.8f, 0.2f, 1.f ), "Aggregate GPU values are correlated with same-frame named allocations; they are not fully attributable to those allocations." );
            }
        }
    }

    if( selection.mapping != MemoryFrameMapping::Valid )
    {
        if( selection.mapping == MemoryFrameMapping::BetweenFrames ) TextColoredUnformatted( ImVec4( 1.f, 0.8f, 0.2f, 1.f ), "Between frames for the selected frame set" );
        else if( selection.mapping == MemoryFrameMapping::OutsideRange ) TextColoredUnformatted( ImVec4( 1.f, 0.8f, 0.2f, 1.f ), "Outside frame range for the selected frame set" );
        else TextDisabledUnformatted( "No completed frames available" );
        return;
    }

    if( MemoryFrameSnapshotNeedsRebuild() ) RebuildMemoryFrameSnapshot();
    const auto& snapshot = m_memInfo.frameSnapshot;
    if( !snapshot.valid )
    {
        TextDisabledUnformatted( "Unable to build a snapshot for this frame" );
        return;
    }

    ImGui::Text( "%s | Internal index %d | %s - %s | %s", GetFrameText( *selection.frameSet, selection.frameIndex, snapshot.end - snapshot.begin ), selection.frameIndex, TimeToStringExact( snapshot.begin ), TimeToStringExact( snapshot.end ), TimeToString( snapshot.end - snapshot.begin ) );
    ImGui::SameLine();
    if( ImGui::SmallButton( ICON_FA_MAGNIFYING_GLASS " Zoom to frame" ) ) ZoomToRange( snapshot.begin, snapshot.end );

    if( !snapshot.consistent ) TextColoredUnformatted( ImVec4( 1.f, 0.2f, 0.2f, 1.f ), "Memory frame accounting is inconsistent" );
    if( snapshot.possibleCaptureBaseline )
    {
        TextColoredUnformatted( ImVec4( 1.f, 0.8f, 0.2f, 1.f ), "Possible capture baseline: on-demand replay allocations may not have been created in this frame." );
    }

    DrawMemoryFrameSummary();
    if( gpuScope ) DrawGpuMemoryPassesForSelectedFrame();
    const auto requestedTab = selection.tab;
    const bool forceTabSelection = selection.forceTabSelection;
    selection.forceTabSelection = false;
    if( ImGui::BeginTabBar( "##memoryFrameTabs" ) )
    {
        if( ImGui::BeginTabItem( "Active at frame start", nullptr, forceTabSelection && requestedTab == MemoryFrameTab::ActiveAtStart ? ImGuiTabItemFlags_SetSelected : 0 ) )
        {
            selection.tab = MemoryFrameTab::ActiveAtStart;
            DrawMemoryFrameTable( "##memoryFrameStart", snapshot.activeAtStart, selection.tab );
            ImGui::EndTabItem();
        }
        if( ImGui::BeginTabItem( "Active at frame end", nullptr, forceTabSelection && requestedTab == MemoryFrameTab::ActiveAtEnd ? ImGuiTabItemFlags_SetSelected : 0 ) )
        {
            selection.tab = MemoryFrameTab::ActiveAtEnd;
            DrawMemoryFrameTable( "##memoryFrameEnd", snapshot.activeAtEnd, selection.tab );
            ImGui::EndTabItem();
        }
        if( ImGui::BeginTabItem( "Allocated in frame", nullptr, forceTabSelection && requestedTab == MemoryFrameTab::AllocatedInFrame ? ImGuiTabItemFlags_SetSelected : 0 ) )
        {
            selection.tab = MemoryFrameTab::AllocatedInFrame;
            DrawMemoryFrameTable( "##memoryFrameAllocated", snapshot.allocated, selection.tab );
            ImGui::EndTabItem();
        }
        if( ImGui::BeginTabItem( "Freed in frame", nullptr, forceTabSelection && requestedTab == MemoryFrameTab::FreedInFrame ? ImGuiTabItemFlags_SetSelected : 0 ) )
        {
            selection.tab = MemoryFrameTab::FreedInFrame;
            DrawMemoryFrameTable( "##memoryFrameFreed", snapshot.freed, selection.tab );
            ImGui::EndTabItem();
        }
        if( ImGui::BeginTabItem( "All frame transitions", nullptr, forceTabSelection && requestedTab == MemoryFrameTab::AllTransitions ? ImGuiTabItemFlags_SetSelected : 0 ) )
        {
            selection.tab = MemoryFrameTab::AllTransitions;
            DrawMemoryFrameTable( "##memoryFrameTransitions", snapshot.transitions, selection.tab );
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void View::DrawMemoryFrameTable( const char* id, const std::vector<MemoryEventRef>& data, MemoryFrameTab tab )
{
    if( data.empty() )
    {
        TextDisabledUnformatted( "No allocations in this category" );
        return;
    }

    const auto& selection = m_memInfo.frame;
    const auto& snapshot = m_memInfo.frameSnapshot;
    const bool logicalIdentifier = selection.scope == MemoryFrameScope::AllGpuD3D12Pools || IsGpuD3D12MemoryPool( m_memInfo.pool );
    if( logicalIdentifier ) EnsureGpuMemoryAttribution();
    const auto& attribution = m_memInfo.gpuAttribution;
    const auto tableHeight = ImGui::GetTextLineHeightWithSpacing() * std::min<size_t>( data.size() + 2, 18 );
    const auto flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY;
    if( !ImGui::BeginTable( id, logicalIdentifier ? 22 : 15, flags, ImVec2( 0, tableHeight ) ) ) return;

    ImGui::TableSetupScrollFreeze( 2, 1 );
    ImGui::TableSetupColumn( "Pool", ImGuiTableColumnFlags_WidthFixed, 250 );
    ImGui::TableSetupColumn( logicalIdentifier ? "Logical allocation ID" : "Address", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHide, 150 );
    ImGui::TableSetupColumn( "Size", ImGuiTableColumnFlags_WidthFixed, 100 );
    ImGui::TableSetupColumn( "State", ImGuiTableColumnFlags_WidthFixed, 120 );
    ImGui::TableSetupColumn( "Alloc frame", ImGuiTableColumnFlags_WidthFixed, 100 );
    ImGui::TableSetupColumn( "Free frame", ImGuiTableColumnFlags_WidthFixed, 100 );
    ImGui::TableSetupColumn( "Lifetime frames", ImGuiTableColumnFlags_WidthFixed, 110 );
    ImGui::TableSetupColumn( "Appeared at", ImGuiTableColumnFlags_WidthFixed, 130 );
    ImGui::TableSetupColumn( "Freed at", ImGuiTableColumnFlags_WidthFixed, 130 );
    ImGui::TableSetupColumn( "Duration", ImGuiTableColumnFlags_WidthFixed, 110 );
    ImGui::TableSetupColumn( "Thread", ImGuiTableColumnFlags_WidthFixed, 180 );
    ImGui::TableSetupColumn( "Zone alloc", ImGuiTableColumnFlags_WidthFixed, 180 );
    ImGui::TableSetupColumn( "Zone free", ImGuiTableColumnFlags_WidthFixed, 180 );
    ImGui::TableSetupColumn( "Alloc call stack", ImGuiTableColumnFlags_WidthFixed, 110 );
    ImGui::TableSetupColumn( "Free call stack", ImGuiTableColumnFlags_WidthFixed, 110 );
    if( logicalIdentifier )
    {
        ImGui::TableSetupColumn( "Request scope", ImGuiTableColumnFlags_WidthFixed, 250 );
        ImGui::TableSetupColumn( "Attribution", ImGuiTableColumnFlags_WidthFixed, 130 );
        ImGui::TableSetupColumn( "First use", ImGuiTableColumnFlags_WidthFixed, 250 );
        ImGui::TableSetupColumn( "Last use", ImGuiTableColumnFlags_WidthFixed, 250 );
        ImGui::TableSetupColumn( "Uses this frame", ImGuiTableColumnFlags_WidthFixed, 105 );
        ImGui::TableSetupColumn( "Access this frame", ImGuiTableColumnFlags_WidthFixed, 220 );
        ImGui::TableSetupColumn( "Pass count", ImGuiTableColumnFlags_WidthFixed, 90 );
    }
    ImGui::TableHeadersRow();

    int widgetId = 0;
    ImGuiListClipper clipper;
    clipper.Begin( int( data.size() ) );
    while( clipper.Step() )
    {
        for( int row=clipper.DisplayStart; row<clipper.DisplayEnd; row++ )
        {
            const auto& ref = data[row];
            const auto& mem = m_worker.GetMemoryNamed( ref.pool );
            if( ref.index >= mem.data.size() ) continue;
            const auto& event = mem.data[ref.index];
            const auto ta = event.TimeAlloc();
            const auto tf = event.TimeFree();
            const bool allocated = ta >= snapshot.begin && ta < snapshot.end;
            const bool freed = tf >= snapshot.begin && tf < snapshot.end;
            const bool activeAtEnd = ta < snapshot.end && ( tf < 0 || tf >= snapshot.end );

            const char* state;
            if( snapshot.possibleCaptureBaseline && allocated )
            {
                state = "Capture baseline?";
            }
            else if( allocated && freed )
            {
                state = "Transient";
            }
            else if( allocated && activeAtEnd )
            {
                state = "Survived";
            }
            else if( allocated )
            {
                state = "Allocated";
            }
            else if( freed )
            {
                state = "Freed";
            }
            else if( tab == MemoryFrameTab::ActiveAtEnd )
            {
                state = "Active at end";
            }
            else
            {
                state = "Pre-existing";
            }

            int allocFrame = -1;
            int freeFrame = -1;
            const bool hasAllocFrame = selection.frameSet && FindMemoryFrameAtTime( *selection.frameSet, ta, allocFrame ) == MemoryFrameMapping::Valid;
            const bool hasFreeFrame = tf >= 0 && selection.frameSet && FindMemoryFrameAtTime( *selection.frameSet, tf, freeFrame ) == MemoryFrameMapping::Valid;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted( GetMemoryPoolName( ref.pool ) );

            ImGui::TableNextColumn();
            ImGui::PushID( widgetId++ );
            ImGui::PushFont( g_fonts.mono, FontNormal );
            if( m_memoryAllocInfoPool == ref.pool && m_memoryAllocInfoWindow == int64_t( ref.index ) )
            {
                ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 1.f, 0.f, 0.f, 1.f ) );
                DrawMemoryIdentifier( ref.pool, event );
                ImGui::PopStyleColor();
            }
            else
            {
                DrawMemoryIdentifier( ref.pool, event );
            }
            ImGui::PopFont();
            if( ImGui::IsItemClicked() )
            {
                m_memoryAllocInfoPool = ref.pool;
                m_memoryAllocInfoWindow = int64_t( ref.index );
            }
            if( ImGui::IsItemClicked( 2 ) ) ZoomToRange( ta, tf >= 0 ? tf : m_worker.GetLastTime() );
            if( ImGui::IsItemHovered() )
            {
                m_memoryAllocHover = int64_t( ref.index );
                m_memoryAllocHoverPool = ref.pool;
                m_memoryAllocHoverWait = 2;
            }
            ImGui::PopID();

            ImGui::TableNextColumn();
            ImGui::TextUnformatted( MemSizeToString( event.Size() ) );

            ImGui::TableNextColumn();
            if( activeAtEnd ) TextColoredUnformatted( ImVec4( 0.6f, 1.f, 0.6f, 1.f ), state );
            else if( freed ) TextColoredUnformatted( ImVec4( 1.f, 0.7f, 0.4f, 1.f ), state );
            else ImGui::TextUnformatted( state );

            auto drawFrame = [&]( int frame, bool valid ) {
                if( !valid )
                {
                    ImGui::TextUnformatted( "-" );
                    return;
                }
                ImGui::PushID( widgetId++ );
                if( ImGui::Selectable( RealToString( GetFrameNumber( *selection.frameSet, frame ) ) ) )
                {
                    ZoomToRange( m_worker.GetFrameBegin( *selection.frameSet, frame ), m_worker.GetFrameEnd( *selection.frameSet, frame ) );
                }
                ImGui::PopID();
            };

            ImGui::TableNextColumn();
            drawFrame( allocFrame, hasAllocFrame );
            ImGui::TableNextColumn();
            if( tf < 0 ) TextColoredUnformatted( ImVec4( 0.6f, 1.f, 0.6f, 1.f ), "Active" );
            else drawFrame( freeFrame, hasFreeFrame );

            ImGui::TableNextColumn();
            if( hasAllocFrame && hasFreeFrame && freeFrame >= allocFrame )
            {
                ImGui::TextUnformatted( RealToString( uint64_t( freeFrame - allocFrame + 1 ) ) );
            }
            else if( hasAllocFrame && tf < 0 && selection.frameIndex >= allocFrame )
            {
                ImGui::Text( "%s+", RealToString( uint64_t( selection.frameIndex - allocFrame + 1 ) ) );
            }
            else
            {
                ImGui::TextUnformatted( "-" );
            }

            ImGui::TableNextColumn();
            ImGui::PushID( widgetId++ );
            if( ImGui::Selectable( TimeToStringExact( ta ) ) ) CenterAtTime( ta );
            ImGui::PopID();

            ImGui::TableNextColumn();
            if( tf < 0 )
            {
                TextColoredUnformatted( ImVec4( 0.6f, 1.f, 0.6f, 1.f ), "Active" );
            }
            else
            {
                ImGui::PushID( widgetId++ );
                if( ImGui::Selectable( TimeToStringExact( tf ) ) ) CenterAtTime( tf );
                ImGui::PopID();
            }

            ImGui::TableNextColumn();
            if( tf < 0 ) ImGui::Text( "%s+", TimeToString( std::max<int64_t>( 0, snapshot.end - ta ) ) );
            else ImGui::TextUnformatted( TimeToString( tf - ta ) );

            ImGui::TableNextColumn();
            const auto tidAlloc = m_worker.DecompressThread( event.ThreadAlloc() );
            SmallColorBox( GetThreadColor( tidAlloc, 0 ) );
            ImGui::SameLine();
            ImGui::TextUnformatted( m_worker.GetThreadName( tidAlloc ) );
            if( tf >= 0 && event.ThreadAlloc() != event.ThreadFree() )
            {
                const auto tidFree = m_worker.DecompressThread( event.ThreadFree() );
                ImGui::SameLine();
                ImGui::TextUnformatted( "/" );
                ImGui::SameLine();
                SmallColorBox( GetThreadColor( tidFree, 0 ) );
                ImGui::SameLine();
                ImGui::TextUnformatted( m_worker.GetThreadName( tidFree ) );
            }

            const auto zoneAlloc = FindZoneAtTime( tidAlloc, ta );
            const auto tidFree = tf >= 0 ? m_worker.DecompressThread( event.ThreadFree() ) : 0;
            const auto zoneFree = tf >= 0 ? FindZoneAtTime( tidFree, tf ) : nullptr;
            auto drawZone = [&]( const ZoneEvent* zone, bool sameZone ) {
                if( !zone )
                {
                    ImGui::TextUnformatted( "-" );
                    return;
                }
                const auto& srcloc = m_worker.GetSourceLocation( zone->SrcLoc() );
                const auto text = srcloc.name.active ? m_worker.GetString( srcloc.name ) : m_worker.GetString( srcloc.function );
                ImGui::PushID( widgetId++ );
                if( sameZone ) ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 1.f, 1.f, 0.6f, 1.f ) );
                const bool selected = ImGui::Selectable( text, m_zoneInfoWindow == zone );
                const bool hovered = ImGui::IsItemHovered();
                if( sameZone ) ImGui::PopStyleColor();
                ImGui::PopID();
                if( selected ) ShowZoneInfo( *zone );
                if( hovered )
                {
                    m_zoneHighlight = zone;
                    if( IsMouseClicked( 2 ) ) ZoomToZone( *zone );
                    ZoneTooltip( *zone );
                }
            };

            ImGui::TableNextColumn();
            drawZone( zoneAlloc, false );
            ImGui::TableNextColumn();
            if( tf < 0 ) TextColoredUnformatted( ImVec4( 0.6f, 1.f, 0.6f, 1.f ), "Active" );
            else drawZone( zoneFree, zoneFree && zoneFree == zoneAlloc );

            ImGui::TableNextColumn();
            if( event.CsAlloc() == 0 ) TextDisabledUnformatted( "-" );
            else SmallCallstackButton( "alloc", event.CsAlloc(), widgetId );
            ImGui::TableNextColumn();
            if( event.csFree.Val() == 0 ) TextDisabledUnformatted( "-" );
            else SmallCallstackButton( "free", event.csFree.Val(), widgetId );

            if( logicalIdentifier )
            {
                const GpuMemoryRequestScope* requestScope = nullptr;
                const auto requestLabel = attribution.requestLabelByAllocation.find( event.Ptr() );
                if( requestLabel != attribution.requestLabelByAllocation.end() )
                {
                    const auto scope = attribution.requestScopeByLabel.find( requestLabel->second );
                    if( scope != attribution.requestScopeByLabel.end() ) requestScope = &attribution.requestScopes[scope->second];
                }

                const auto passHistory = attribution.passesByAllocation.find( event.Ptr() );
                const bool hasPassHistory = passHistory != attribution.passesByAllocation.end() && !passHistory->second.empty();

                ImGui::TableNextColumn();
                if( !attribution.ready ) TextDisabledUnformatted( "Indexing..." );
                else if( !attribution.protocolPresent ) TextDisabledUnformatted( "N/A" );
                else if( requestScope )
                {
                    ImGui::PushID( widgetId++ );
                    const bool selected = ImGui::Selectable( requestScope->name.c_str(), m_zoneInfoWindow == requestScope->zone );
                    const bool hovered = ImGui::IsItemHovered();
                    ImGui::PopID();
                    if( selected ) ShowZoneInfo( *requestScope->zone );
                    if( hovered )
                    {
                        m_zoneHighlight = requestScope->zone;
                        ZoneTooltip( *requestScope->zone );
                        if( IsMouseClicked( 2 ) ) ZoomToZone( *requestScope->zone );
                    }
                }
                else TextDisabledUnformatted( "Unknown" );

                ImGui::TableNextColumn();
                if( !attribution.ready ) TextDisabledUnformatted( "Indexing..." );
                else if( !attribution.protocolPresent ) TextDisabledUnformatted( "N/A" );
                else if( requestScope && hasPassHistory )
                {
                    bool complete = true;
                    for( const auto passIndex : passHistory->second ) complete &= attribution.passes[passIndex].complete;
                    if( complete ) TextColoredUnformatted( ImVec4( 0.5f, 1.f, 0.5f, 1.f ), "Request + uses" );
                    else TextColoredUnformatted( ImVec4( 1.f, 0.5f, 0.3f, 1.f ), "Partial relation" );
                }
                else if( hasPassHistory ) ImGui::TextUnformatted( "Uses only" );
                else if( requestScope ) ImGui::TextUnformatted( "Request only" );
                else TextDisabledUnformatted( "Unattributed" );

                ImGui::TableNextColumn();
                if( attribution.ready && attribution.protocolPresent && hasPassHistory ) DrawGpuMemoryPassLink( attribution.passes[passHistory->second.front()], widgetId );
                else TextDisabledUnformatted( "-" );

                ImGui::TableNextColumn();
                if( attribution.ready && attribution.protocolPresent && hasPassHistory ) DrawGpuMemoryPassLink( attribution.passes[passHistory->second.back()], widgetId );
                else TextDisabledUnformatted( "-" );

                uint32_t usesInFrame = 0;
                uint32_t frameUsageMask = 0;
                if( hasPassHistory )
                {
                    for( const auto passIndex : passHistory->second )
                    {
                        const auto& pass = attribution.passes[passIndex];
                        if( pass.start < snapshot.begin || pass.start >= snapshot.end ) continue;
                        usesInFrame++;
                        for( const auto& use : pass.uses )
                        {
                            if( use.allocationId == event.Ptr() ) frameUsageMask |= use.usageMask;
                        }
                    }
                }

                ImGui::TableNextColumn();
                if( attribution.ready && attribution.protocolPresent ) ImGui::TextUnformatted( RealToString( usesInFrame ) );
                else TextDisabledUnformatted( "-" );

                ImGui::TableNextColumn();
                if( attribution.ready && attribution.protocolPresent )
                {
                    const auto usage = FormatGpuMemoryUsage( frameUsageMask );
                    ImGui::TextUnformatted( usage.c_str() );
                }
                else TextDisabledUnformatted( "-" );

                ImGui::TableNextColumn();
                if( attribution.ready && attribution.protocolPresent && hasPassHistory ) ImGui::TextUnformatted( RealToString( passHistory->second.size() ) );
                else if( attribution.ready && attribution.protocolPresent ) ImGui::TextUnformatted( "0" );
                else TextDisabledUnformatted( "-" );
            }
        }
    }
    ImGui::EndTable();
}

void View::DrawMemory()
{
    const auto scale = GetScale();
    ImGui::SetNextWindowSize( ImVec2( 1100 * scale, 500 * scale ), ImGuiCond_FirstUseEver );
    if( m_memInfo.focus )
    {
        ImGui::SetNextWindowFocus();
        m_memInfo.focus = false;
    }
    ImGui::Begin( "Memory", &m_memInfo.show, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse );
    if( ImGui::GetCurrentWindowRead()->SkipItems ) { ImGui::End(); return; }

    auto& memNameMap = m_worker.GetMemNameMap();
    if( memNameMap.size() > 1 )
    {
        TextDisabledUnformatted( ICON_FA_BOX_ARCHIVE " Memory pool:" );
        ImGui::SameLine();
        if( ImGui::BeginCombo( "##memoryPool", m_memInfo.pool == 0 ? "Default allocator" : m_worker.GetString( m_memInfo.pool ) ) )
        {
            for( auto& v : memNameMap )
            {
                if( ImGui::Selectable( v.first == 0 ? "Default allocator" : m_worker.GetString( v.first ) ) )
                {
                    m_memInfo.pool = v.first;
                    m_memInfo.showAllocList = false;
                    m_memInfo.frame.scope = MemoryFrameScope::SinglePool;
                    m_memInfo.frame.dirty = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::Separator();
    }

    DrawMemoryFrameInspector();

    auto& mem = m_worker.GetMemoryNamed( m_memInfo.pool );
    if( mem.data.empty() )
    {
        const auto ty = ImGui::GetTextLineHeight();
        ImGui::PushFont( g_fonts.normal, FontBig );
        ImGui::Dummy( ImVec2( 0, ( ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeight() * 2 ) * 0.5f ) );
        TextCentered( ICON_FA_DOG );
        TextCentered( "No memory data collected" );
        ImGui::PopFont();
        ImGui::End();
        return;
    }

    const bool gpuPool = IsGpuD3D12MemoryPool( m_memInfo.pool );

    TextDisabledUnformatted( "Total allocations:" );
    ImGui::SameLine();
    ImGui::Text( "%-15s", RealToString( mem.data.size() ) );
    ImGui::SameLine();
    TextDisabledUnformatted( "Active allocations:" );
    ImGui::SameLine();
    ImGui::Text( "%-15s", RealToString( mem.active.size() ) );
    ImGui::SameLine();
    TextDisabledUnformatted( "Memory usage:" );
    ImGui::SameLine();
    ImGui::Text( "%-15s", MemSizeToString( mem.usage ) );
    if( gpuPool )
    {
        ImGui::SameLine();
        TextDisabledUnformatted( "Identifier:" );
        ImGui::SameLine();
        ImGui::TextUnformatted( "Logical allocation ID" );
    }
    else
    {
        ImGui::SameLine();
        TextFocused( "Memory span:", MemSizeToString( mem.high - mem.low ) );
    }
    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::SameLine();
    DrawHelpMarker( gpuPool ?
        "Click on a logical allocation ID to display memory allocation info. Middle click to zoom to its lifetime.\n"
        "Active allocations are displayed using green color.\n"
        "A single thread is displayed if alloc and free was performed on the same thread. Otherwise two threads are displayed in order: alloc, free.\n"
        "If alloc and free is performed in the same zone, the free zone is displayed in yellow color." :
        "Click on address to display memory allocation info window. Middle click to zoom to allocation range.\n"
        "Active allocations are displayed using green color.\n"
        "A single thread is displayed if alloc and free was performed on the same thread. Otherwise two threads are displayed in order: alloc, free.\n"
        "If alloc and free is performed in the same zone, the free zone is displayed in yellow color." );
    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::SameLine();
    ImGui::SeparatorEx( ImGuiSeparatorFlags_Vertical );
    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::SameLine();
    ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 2, 2 ) );
    if( ImGui::Checkbox( "Limit range", &m_memInfo.range.active ) )
    {
        if( m_memInfo.range.active && m_memInfo.range.min == 0 && m_memInfo.range.max == 0 )
        {
            m_memInfo.range.min = m_vd.zvStart;
            m_memInfo.range.max = m_vd.zvEnd;
        }
    }
    if( m_memInfo.range.active )
    {
        ImGui::SameLine();
        TextColoredUnformatted( 0xFF00FFFF, ICON_FA_TRIANGLE_EXCLAMATION );
        ImGui::SameLine();
        ToggleButton( ICON_FA_RULER " Limits", m_showRanges );
    }
    ImGui::PopStyleVar();

    ImGui::Separator();
    ImGui::BeginChild( "##memory" );
    if( ImGui::TreeNode( ICON_FA_AT " Allocations" ) )
    {
        const char* findHint = gpuPool ? "Enter logical allocation ID to search for" : "Enter memory address to search for";
        bool findClicked = ImGui::InputTextWithHint( "###address", findHint, m_memInfo.pattern, 1024, ImGuiInputTextFlags_EnterReturnsTrue );
        ImGui::SameLine();
        findClicked |= ImGui::Button( ICON_FA_MAGNIFYING_GLASS " Find" );
        if( findClicked )
        {
            m_memInfo.ptrFind = strtoull( m_memInfo.pattern, nullptr, 0 );
        }
        ImGui::SameLine();
        if( ImGui::Button( ICON_FA_DELETE_LEFT " Clear" ) )
        {
            m_memInfo.ptrFind = 0;
            m_memInfo.pattern[0] = '\0';
        }

        if( m_memInfo.ptrFind != 0 )
        {
            std::vector<const MemEvent*> match;
            match.reserve( mem.active.size() );     // heuristic
            if( m_memInfo.range.active )
            {
                auto it = std::lower_bound( mem.data.begin(), mem.data.end(), m_memInfo.range.min, [] ( const auto& lhs, const auto& rhs ) { return lhs.TimeAlloc() < rhs; } );
                if( it != mem.data.end() )
                {
                    auto end = std::lower_bound( it, mem.data.end(), m_memInfo.range.max, [] ( const auto& lhs, const auto& rhs ) { return lhs.TimeAlloc() < rhs; } );
                    while( it != end )
                    {
                        if( gpuPool ? it->Ptr() == m_memInfo.ptrFind : it->Ptr() <= m_memInfo.ptrFind && it->Ptr() + it->Size() > m_memInfo.ptrFind )
                        {
                            match.emplace_back( it );
                        }
                        ++it;
                    }
                }
            }
            else
            {
                for( auto& v : mem.data )
                {
                    if( gpuPool ? v.Ptr() == m_memInfo.ptrFind : v.Ptr() <= m_memInfo.ptrFind && v.Ptr() + v.Size() > m_memInfo.ptrFind )
                    {
                        match.emplace_back( &v );
                    }
                }
            }

            if( match.empty() )
            {
                ImGui::TextUnformatted( gpuPool ? "Found no allocation with the given logical allocation ID" : "Found no allocations at given address" );
            }
            else
            {
                ListMemData( match, [this, gpuPool]( auto v ) {
                    if( gpuPool )
                    {
                        ImGui::Text( "%" PRIu64, v->Ptr() );
                    }
                    else if( v->Ptr() == m_memInfo.ptrFind )
                    {
                        ImGui::Text( "0x%" PRIx64, m_memInfo.ptrFind );
                    }
                    else
                    {
                        ImGui::Text( "0x%" PRIx64 "+%" PRIu64, v->Ptr(), m_memInfo.ptrFind - v->Ptr() );
                    }
                    }, -1, m_memInfo.pool );
            }
        }
        ImGui::TreePop();
    }

    ImGui::Separator();
    if( ImGui::TreeNode( ICON_FA_HEART_PULSE " Active allocations" ) )
    {
        uint64_t total = 0;
        std::vector<const MemEvent*> items;
        items.reserve( mem.active.size() );
        if( m_memInfo.range.active )
        {
            auto it = std::lower_bound( mem.data.begin(), mem.data.end(), m_memInfo.range.min, [] ( const auto& lhs, const auto& rhs ) { return lhs.TimeAlloc() < rhs; } );
            if( it != mem.data.end() )
            {
                auto end = std::lower_bound( it, mem.data.end(), m_memInfo.range.max, [] ( const auto& lhs, const auto& rhs ) { return lhs.TimeAlloc() < rhs; } );
                while( it != end )
                {
                    const auto tf = it->TimeFree();
                    if( tf < 0 || tf >= m_memInfo.range.max )
                    {
                        items.emplace_back( it );
                        total += it->Size();
                    }
                    ++it;
                }
            }
        }
        else
        {
            auto ptr = mem.data.data();
            for( auto& v : mem.active ) items.emplace_back( ptr + v.second );
            pdqsort_branchless( items.begin(), items.end(), []( const auto& lhs, const auto& rhs ) { return lhs->TimeAlloc() < rhs->TimeAlloc(); } );
            total = mem.usage;
        }

        ImGui::SameLine();
        ImGui::TextDisabled( "(%s)", RealToString( items.size() ) );
        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        TextFocused( "Memory usage:", MemSizeToString( total ) );

        if( !items.empty() )
        {
            ListMemData( items, [this]( auto v ) {
                DrawMemoryIdentifier( m_memInfo.pool, *v );
                }, -1, m_memInfo.pool );
        }
        else
        {
            TextDisabledUnformatted( "No active allocations" );
        }
        ImGui::TreePop();
    }

    ImGui::Separator();
    if( gpuPool )
    {
        TextDisabledUnformatted( ICON_FA_MAP " Memory map is unavailable for logical GPU allocation IDs" );
    }
    else if( ImGui::TreeNode( ICON_FA_MAP " Memory map" ) )
    {
        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        TextFocused( "Single pixel:", MemSizeToString( 1 << ChunkBits ) );
        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        TextFocused( "Single line:", MemSizeToString( PageChunkSize ) );

        auto pages = GetMemoryPages();
        const size_t lines = pages.size();

        ImGui::BeginChild( "##memMap", ImVec2( PageSize + 2, lines + 2 ), false );
        auto draw = ImGui::GetWindowDrawList();
        const auto wpos = ImGui::GetCursorScreenPos() + ImVec2( 1, 1 );
        const auto dpos = wpos + ImVec2( 0.5f, 0.5f );
        draw->AddRect( wpos - ImVec2( 1, 1 ), wpos + ImVec2( PageSize + 1, lines + 1 ), 0xFF666666 );
        draw->AddRectFilled( wpos, wpos + ImVec2( PageSize, lines ), 0xFF444444 );

        size_t line = 0;
        for( auto& page : pages )
        {
            size_t idx = 0;
            while( idx < PageSize )
            {
                if( page.data[idx] == 0 )
                {
                    do
                    {
                        idx++;
                    }
                    while( idx < PageSize && page.data[idx] == 0 );
                }
                else
                {
                    auto val = page.data[idx];
                    const auto i0 = idx;
                    do
                    {
                        idx++;
                    }
                    while( idx < PageSize && page.data[idx] == val );
                    DrawLine( draw, dpos + ImVec2( i0, line ), dpos + ImVec2( idx, line ), MemDecayColor[(uint8_t)val] );
                }
            }
            line++;
        }

        ImGui::EndChild();
        ImGui::TreePop();
    }

    ImGui::PushID( m_memInfo.pool );
    ImGui::Separator();
    if( ImGui::TreeNode( ICON_FA_TREE " Bottom-up call stack tree" ) )
    {
        ImGui::SameLine();
        DrawHelpMarker( "Press ctrl key to display allocation info tooltip. Right click on function name to display allocations list." );
        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        SmallCheckbox( ICON_FA_LAYER_GROUP " Group by function name", &m_groupCallstackTreeByNameBottomUp );
        ImGui::SameLine();
        DrawHelpMarker( "If enabled, only one source location will be displayed (which may be incorrect)." );
        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        bool activeOnlyBottomUp = m_memRangeBottomUp == MemRange::Active;
        if( SmallCheckbox( "Only active allocations", &activeOnlyBottomUp ) )
            m_memRangeBottomUp = activeOnlyBottomUp ? MemRange::Active : MemRange::Full;
        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        bool inactiveOnlyBottomUp = m_memRangeBottomUp == MemRange::Inactive;
        if( SmallCheckbox( "Only inactive allocations", &inactiveOnlyBottomUp ) )
            m_memRangeBottomUp = inactiveOnlyBottomUp ? MemRange::Inactive : MemRange::Full;

        auto tree = GetCallstackFrameTreeBottomUp( mem );
        if( !tree.empty() )
        {
            int idx = 0;
            DrawFrameTreeLevel( tree, idx );
        }
        else
        {
            TextDisabledUnformatted( "No call stack data collected" );
        }

        ImGui::TreePop();
    }

    ImGui::Separator();
    if( ImGui::TreeNode( ICON_FA_TREE " Top-down call stack tree" ) )
    {
        ImGui::SameLine();
        DrawHelpMarker( "Press ctrl key to display allocation info tooltip. Right click on function name to display allocations list." );
        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        SmallCheckbox( ICON_FA_LAYER_GROUP " Group by function name", &m_groupCallstackTreeByNameTopDown );
        ImGui::SameLine();
        DrawHelpMarker( "If enabled, only one source location will be displayed (which may be incorrect)." );
        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        bool activeOnlyTopDown = m_memRangeTopDown == MemRange::Active;
        if( SmallCheckbox( "Only active allocations", &activeOnlyTopDown ) )
            m_memRangeTopDown = activeOnlyTopDown ? MemRange::Active : MemRange::Full;
        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        bool inactiveOnlyTopDown = m_memRangeTopDown == MemRange::Inactive;
        if( SmallCheckbox( "Only inactive allocations", &inactiveOnlyTopDown ) )
            m_memRangeTopDown = inactiveOnlyTopDown ? MemRange::Inactive : MemRange::Full;

        auto tree = GetCallstackFrameTreeTopDown( mem );
        if( !tree.empty() )
        {
            int idx = 0;
            DrawFrameTreeLevel( tree, idx );
        }
        else
        {
            TextDisabledUnformatted( "No call stack data collected" );
        }

        ImGui::TreePop();
    }
    ImGui::PopID();

    ImGui::EndChild();
    ImGui::End();
}

void View::DrawMemoryAllocWindow()
{
    bool show = true;
    ImGui::Begin( "Memory allocation", &show, ImGuiWindowFlags_AlwaysAutoResize );
    if( !ImGui::GetCurrentWindowRead()->SkipItems )
    {
        const auto& mem = m_worker.GetMemoryNamed( m_memoryAllocInfoPool );
        const auto& ev = mem.data[m_memoryAllocInfoWindow];
        const auto tidAlloc = m_worker.DecompressThread( ev.ThreadAlloc() );
        const auto tidFree = m_worker.DecompressThread( ev.ThreadFree() );
        int idx = 0;

        if( ImGui::Button( ICON_FA_MICROSCOPE " Zoom to allocation" ) )
        {
            ZoomToRange( ev.TimeAlloc(), ev.TimeFree() >= 0 ? ev.TimeFree() : m_worker.GetLastTime() );
        }

        if( m_worker.GetMemNameMap().size() > 1 )
        {
            TextFocused( ICON_FA_BOX_ARCHIVE " Pool:", m_memoryAllocInfoPool == 0 ? "Default allocator" : m_worker.GetString( m_memoryAllocInfoPool ) );
        }
        char buf[64];
        if( IsGpuD3D12MemoryPool( m_memoryAllocInfoPool ) )
        {
            sprintf( buf, "%" PRIu64, ev.Ptr() );
            TextFocused( "Logical allocation ID:", buf );
        }
        else
        {
            sprintf( buf, "0x%" PRIx64, ev.Ptr() );
            TextFocused( "Address:", buf );
        }
        TextFocused( "Size:", MemSizeToString( ev.Size() ) );
        if( ev.Size() >= 10000ll )
        {
            ImGui::SameLine();
            ImGui::TextDisabled( "(%s bytes)", RealToString( ev.Size() ) );
        }
        ImGui::Separator();
        TextFocused( "Appeared at", TimeToStringExact( ev.TimeAlloc() ) );
        if( ImGui::IsItemClicked() ) CenterAtTime( ev.TimeAlloc() );
        ImGui::SameLine(); ImGui::Spacing(); ImGui::SameLine();
        SmallColorBox( GetThreadColor( tidAlloc, 0 ) );
        ImGui::SameLine();
        TextFocused( "Thread:", m_worker.GetThreadName( tidAlloc ) );
        ImGui::SameLine();
        ImGui::TextDisabled( "(%s)", RealToString( tidAlloc ) );
        if( m_worker.IsThreadFiber( tidAlloc ) )
        {
            ImGui::SameLine();
            TextColoredUnformatted( ImVec4( 0.2f, 0.6f, 0.2f, 1.f ), "Fiber" );
        }
        if( ev.CsAlloc() != 0 )
        {
            const auto cs = ev.CsAlloc();
            SmallCallstackButton( ICON_FA_ALIGN_JUSTIFY, cs, idx );
            ImGui::SameLine();
            DrawCallstackCalls( cs, 4 );
        }
        if( ev.TimeFree() < 0 )
        {
            TextDisabledUnformatted( "Allocation still active" );
        }
        else
        {
            TextFocused( "Freed at", TimeToStringExact( ev.TimeFree() ) );
            if( ImGui::IsItemClicked() ) CenterAtTime( ev.TimeFree() );
            ImGui::SameLine(); ImGui::Spacing(); ImGui::SameLine();
            SmallColorBox( GetThreadColor( tidFree, 0 ) );
            ImGui::SameLine();
            TextFocused( "Thread:", m_worker.GetThreadName( tidFree ) );
            ImGui::SameLine();
            ImGui::TextDisabled( "(%s)", RealToString( tidFree ) );
            if( m_worker.IsThreadFiber( tidFree ) )
            {
                ImGui::SameLine();
                TextColoredUnformatted( ImVec4( 0.2f, 0.6f, 0.2f, 1.f ), "Fiber" );
            }
            if( ev.csFree.Val() != 0 )
            {
                const auto cs = ev.csFree.Val();
                SmallCallstackButton( ICON_FA_ALIGN_JUSTIFY, cs, idx );
                ImGui::SameLine();
                DrawCallstackCalls( cs, 4 );
            }
            TextFocused( "Duration:", TimeToString( ev.TimeFree() - ev.TimeAlloc() ) );
        }

        if( IsGpuD3D12MemoryPool( m_memoryAllocInfoPool ) ) DrawGpuMemoryAllocationAttribution( ev.Ptr(), idx );

        bool sep = false;
        auto zoneAlloc = FindZoneAtTime( tidAlloc, ev.TimeAlloc() );
        if( zoneAlloc )
        {
            ImGui::Separator();
            sep = true;
            const auto& srcloc = m_worker.GetSourceLocation( zoneAlloc->SrcLoc() );
            const auto txt = srcloc.name.active ? m_worker.GetString( srcloc.name ) : m_worker.GetString( srcloc.function );
            ImGui::PushID( idx++ );
            TextFocused( "Zone alloc:", txt );
            auto hover = ImGui::IsItemHovered();
            ImGui::PopID();
            if( ImGui::IsItemClicked() )
            {
                ShowZoneInfo( *zoneAlloc );
            }
            if( hover )
            {
                m_zoneHighlight = zoneAlloc;
                if( IsMouseClicked( 2 ) )
                {
                    ZoomToZone( *zoneAlloc );
                }
                ZoneTooltip( *zoneAlloc );
            }
        }

        if( ev.TimeFree() >= 0 )
        {
            auto zoneFree = FindZoneAtTime( tidFree, ev.TimeFree() );
            if( zoneFree )
            {
                if( !sep ) ImGui::Separator();
                const auto& srcloc = m_worker.GetSourceLocation( zoneFree->SrcLoc() );
                const auto txt = srcloc.name.active ? m_worker.GetString( srcloc.name ) : m_worker.GetString( srcloc.function );
                TextFocused( "Zone free:", txt );
                auto hover = ImGui::IsItemHovered();
                if( ImGui::IsItemClicked() )
                {
                    ShowZoneInfo( *zoneFree );
                }
                if( hover )
                {
                    m_zoneHighlight = zoneFree;
                    if( IsMouseClicked( 2 ) )
                    {
                        ZoomToZone( *zoneFree );
                    }
                    ZoneTooltip( *zoneFree );
                }
                if( zoneAlloc == zoneFree )
                {
                    ImGui::SameLine();
                    TextDisabledUnformatted( "(same zone)" );
                }
            }
        }
    }
    ImGui::End();
    if( !show ) m_memoryAllocInfoWindow = -1;
}

void View::ListMemData( std::vector<const MemEvent*>& vec, const std::function<void(const MemEvent*)>& DrawAddress, int64_t startTime, uint64_t pool )
{
    if( startTime == -1 ) startTime = 0;
    if( ImGui::BeginTable( "##mem", 8, ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY, ImVec2( 0, ImGui::GetTextLineHeightWithSpacing() * std::min<int64_t>( 1+vec.size(), 15 ) ) ) )
    {
        ImGui::TableSetupScrollFreeze( 0, 1 );
        ImGui::TableSetupColumn( IsGpuD3D12MemoryPool( pool ) ? "Logical allocation ID" : "Address", ImGuiTableColumnFlags_NoHide );
        ImGui::TableSetupColumn( "Size", ImGuiTableColumnFlags_PreferSortDescending );
        ImGui::TableSetupColumn( "Appeared at", ImGuiTableColumnFlags_DefaultSort );
        ImGui::TableSetupColumn( "Duration", ImGuiTableColumnFlags_PreferSortDescending );
        ImGui::TableSetupColumn( "Thread", ImGuiTableColumnFlags_NoSort );
        ImGui::TableSetupColumn( "Zone alloc", ImGuiTableColumnFlags_NoSort );
        ImGui::TableSetupColumn( "Zone free", ImGuiTableColumnFlags_NoSort );
        ImGui::TableSetupColumn( "Call stack", ImGuiTableColumnFlags_NoSort );
        ImGui::TableHeadersRow();

        const auto& mem = m_worker.GetMemoryNamed( pool );
        const auto& sortspec = *ImGui::TableGetSortSpecs()->Specs;
        switch( sortspec.ColumnIndex )
        {
        case 0:
            if( sortspec.SortDirection == ImGuiSortDirection_Ascending )
            {
                pdqsort_branchless( vec.begin(), vec.end(), []( const auto& l, const auto& r ) { return l->Ptr() < r->Ptr(); } );
            }
            else
            {
                pdqsort_branchless( vec.begin(), vec.end(), []( const auto& l, const auto& r ) { return l->Ptr() > r->Ptr(); } );
            }
            break;
        case 1:
            if( sortspec.SortDirection == ImGuiSortDirection_Ascending )
            {
                pdqsort_branchless( vec.begin(), vec.end(), []( const auto& l, const auto& r ) { return l->Size() < r->Size(); } );
            }
            else
            {
                pdqsort_branchless( vec.begin(), vec.end(), []( const auto& l, const auto& r ) { return l->Size() > r->Size(); } );
            }
            break;
        case 2:
            if( sortspec.SortDirection == ImGuiSortDirection_Descending )
            {
                std::reverse( vec.begin(), vec.end() );
            }
            break;
        case 3:
            if( sortspec.SortDirection == ImGuiSortDirection_Ascending )
            {
                pdqsort_branchless( vec.begin(), vec.end(), []( const auto& l, const auto& r ) { return ( l->TimeFree() - l->TimeAlloc() ) < ( r->TimeFree() - r->TimeAlloc() ); } );
            }
            else
            {
                pdqsort_branchless( vec.begin(), vec.end(), []( const auto& l, const auto& r ) { return ( l->TimeFree() - l->TimeAlloc() ) > ( r->TimeFree() - r->TimeAlloc() ); } );
            }
            break;
        default:
            assert( false );
            break;
        }

        int idx = 0;
        ImGuiListClipper clipper;
        clipper.Begin( vec.end() - vec.begin() );
        while( clipper.Step() )
        {
            for( auto i=clipper.DisplayStart; i<clipper.DisplayEnd; i++ )
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();

                auto v = vec[i];
                const auto arrIdx = std::distance( mem.data.begin(), v );

                ImGui::PushFont( g_fonts.mono, FontNormal );
                if( m_memoryAllocInfoPool == pool && m_memoryAllocInfoWindow == arrIdx )
                {
                    ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 1.f, 0.f, 0.f, 1.f ) );
                    DrawAddress( v );
                    ImGui::PopStyleColor();
                }
                else
                {
                    DrawAddress( v );
                    if( ImGui::IsItemClicked() )
                    {
                        m_memoryAllocInfoWindow = arrIdx;
                        m_memoryAllocInfoPool = pool;
                    }
                }
                ImGui::PopFont();
                if( ImGui::IsItemClicked( 2 ) )
                {
                    ZoomToRange( v->TimeAlloc(), v->TimeFree() >= 0 ? v->TimeFree() : m_worker.GetLastTime() );
                }
                if( ImGui::IsItemHovered() )
                {
                    m_memoryAllocHover = arrIdx;
                    m_memoryAllocHoverWait = 2;
                    m_memoryAllocHoverPool = pool;
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted( MemSizeToString( v->Size() ) );
                ImGui::TableNextColumn();
                ImGui::PushID( idx++ );
                if( ImGui::Selectable( TimeToStringExact( v->TimeAlloc() - startTime ) ) )
                {
                    CenterAtTime( v->TimeAlloc() );
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                if( v->TimeFree() < 0 )
                {
                    TextColoredUnformatted( ImVec4( 0.6f, 1.f, 0.6f, 1.f ), TimeToString( m_worker.GetLastTime() - v->TimeAlloc() ) );
                    ImGui::TableNextColumn();
                    const auto tid = m_worker.DecompressThread( v->ThreadAlloc() );
                    SmallColorBox( GetThreadColor( tid, 0 ) );
                    ImGui::SameLine();
                    ImGui::TextUnformatted( m_worker.GetThreadName( tid ) );
                }
                else
                {
                    ImGui::PushID( idx++ );
                    if( ImGui::Selectable( TimeToString( v->TimeFree() - v->TimeAlloc() ) ) )
                    {
                        CenterAtTime( v->TimeFree() );
                    }
                    ImGui::PopID();
                    ImGui::TableNextColumn();
                    if( v->ThreadAlloc() == v->ThreadFree() )
                    {
                        const auto tid = m_worker.DecompressThread( v->ThreadAlloc() );
                        SmallColorBox( GetThreadColor( tid, 0 ) );
                        ImGui::SameLine();
                        ImGui::TextUnformatted( m_worker.GetThreadName( tid ) );
                    }
                    else
                    {
                        const auto tidAlloc = m_worker.DecompressThread( v->ThreadAlloc() );
                        const auto tidFree = m_worker.DecompressThread( v->ThreadFree() );
                        SmallColorBox( GetThreadColor( tidAlloc, 0 ) );
                        ImGui::SameLine();
                        ImGui::TextUnformatted( m_worker.GetThreadName( tidAlloc ) );
                        ImGui::SameLine();
                        ImGui::TextUnformatted( "/" );
                        ImGui::SameLine();
                        SmallColorBox( GetThreadColor( tidFree, 0 ) );
                        ImGui::SameLine();
                        ImGui::TextUnformatted( m_worker.GetThreadName( tidFree ) );
                    }
                }
                ImGui::TableNextColumn();
                auto zone = FindZoneAtTime( m_worker.DecompressThread( v->ThreadAlloc() ), v->TimeAlloc() );
                if( !zone )
                {
                    ImGui::TextUnformatted( "-" );
                }
                else
                {
                    const auto& srcloc = m_worker.GetSourceLocation( zone->SrcLoc() );
                    const auto txt = srcloc.name.active ? m_worker.GetString( srcloc.name ) : m_worker.GetString( srcloc.function );
                    ImGui::PushID( idx++ );
                    auto sel = ImGui::Selectable( txt, m_zoneInfoWindow == zone );
                    auto hover = ImGui::IsItemHovered();
                    ImGui::PopID();
                    if( sel )
                    {
                        ShowZoneInfo( *zone );
                    }
                    if( hover )
                    {
                        m_zoneHighlight = zone;
                        if( IsMouseClicked( 2 ) )
                        {
                            ZoomToZone( *zone );
                        }
                        ZoneTooltip( *zone );
                    }
                }
                ImGui::TableNextColumn();
                if( v->TimeFree() < 0 )
                {
                    TextColoredUnformatted( ImVec4( 0.6f, 1.f, 0.6f, 1.f ), "active" );
                }
                else
                {
                    auto zoneFree = FindZoneAtTime( m_worker.DecompressThread( v->ThreadFree() ), v->TimeFree() );
                    if( !zoneFree )
                    {
                        ImGui::TextUnformatted( "-" );
                    }
                    else
                    {
                        const auto& srcloc = m_worker.GetSourceLocation( zoneFree->SrcLoc() );
                        const auto txt = srcloc.name.active ? m_worker.GetString( srcloc.name ) : m_worker.GetString( srcloc.function );
                        ImGui::PushID( idx++ );
                        bool sel;
                        if( zoneFree == zone )
                        {
                            ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 1.f, 1.f, 0.6f, 1.f ) );
                            sel = ImGui::Selectable( txt, m_zoneInfoWindow == zoneFree );
                            ImGui::PopStyleColor( 1 );
                        }
                        else
                        {
                            sel = ImGui::Selectable( txt, m_zoneInfoWindow == zoneFree );
                        }
                        auto hover = ImGui::IsItemHovered();
                        ImGui::PopID();
                        if( sel )
                        {
                            ShowZoneInfo( *zoneFree );
                        }
                        if( hover )
                        {
                            m_zoneHighlight = zoneFree;
                            if( IsMouseClicked( 2 ) )
                            {
                                ZoomToZone( *zoneFree );
                            }
                            ZoneTooltip( *zoneFree );
                        }
                    }
                }
                ImGui::TableNextColumn();
                if( v->CsAlloc() == 0 )
                {
                    TextDisabledUnformatted( "[alloc]" );
                }
                else
                {
                    SmallCallstackButton( "alloc", v->CsAlloc(), idx );
                }
                ImGui::SameLine();
                ImGui::Spacing();
                ImGui::SameLine();
                if( v->csFree.Val() == 0 )
                {
                    TextDisabledUnformatted( "[free]" );
                }
                else
                {
                    SmallCallstackButton( "free", v->csFree.Val(), idx );
                }
            }
        }
        ImGui::EndTable();
    }
}

void View::DrawAllocList()
{
    const auto scale = GetScale();
    ImGui::SetNextWindowSize( ImVec2( 1100 * scale, 500 * scale ), ImGuiCond_FirstUseEver );
    ImGui::Begin( "Allocations list", &m_memInfo.showAllocList );
    if( ImGui::GetCurrentWindowRead()->SkipItems ) { ImGui::End(); return; }

    std::vector<const MemEvent*> data;
    auto basePtr = m_worker.GetMemoryNamed( m_memInfo.pool ).data.data();
    data.reserve( m_memInfo.allocList.size() );
    for( auto& idx : m_memInfo.allocList )
    {
        data.emplace_back( basePtr + idx );
    }

    TextFocused( "Number of allocations:", RealToString( m_memInfo.allocList.size() ) );
    ListMemData( data, [this]( auto v ) {
        DrawMemoryIdentifier( m_memInfo.pool, *v );
        }, -1, m_memInfo.pool );
    ImGui::End();
}

}
