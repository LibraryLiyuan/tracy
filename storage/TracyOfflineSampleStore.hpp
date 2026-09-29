#ifndef TRACY_OFFLINE_SAMPLE_STORE_HPP
#define TRACY_OFFLINE_SAMPLE_STORE_HPP

#include "TracyDiskIndex64.hpp"
#include "TracyMutableRows.hpp"
#include "../analysis/TracyAnalysisIoPath.hpp"
#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tracy::storage
{

#pragma pack(push,1)
struct OfflineSampleRow
{
    int64_t time = 0;
    uint32_t callstack = 0;
};
#pragma pack(pop)
static_assert( sizeof( OfflineSampleRow ) == 12 );

struct OfflineSampleStoreStats
{
    uint64_t appends = 0, finds = 0, updates = 0, visits = 0, visitedRows = 0;
    uint64_t indexBuilds = 0, indexBuildRows = 0, indexBuildNanoseconds = 0, findNanoseconds = 0;
    uint64_t rowPageLoads = 0, rowPageWrites = 0, rowEvictions = 0, rowSlotAllocations = 0, rowSlotReuses = 0;
    uint64_t indexPageLoads = 0, indexPageWrites = 0, indexEvictions = 0;
};

class OfflineSampleStore
{
    static constexpr uint64_t LocalIdBits = 47;
    static constexpr uint64_t LocalIdMask = ( uint64_t( 1 ) << LocalIdBits ) - 1;
    static constexpr uint64_t StateCacheBytes = 24ull << 10;
    struct Key
    {
        uint64_t thread;
        uint8_t kind;
        bool operator==( const Key& other ) const { return thread == other.thread && kind == other.kind; }
    };
    struct KeyHash { size_t operator()( const Key& key ) const { return size_t( key.thread ^ ( uint64_t( key.kind ) * 0x9E3779B97F4A7C15ull ) ); } };
    struct State
    {
        std::unique_ptr<MutableRows> rows;
        std::unique_ptr<MutableRows> zeroRows;
        uint64_t count = 0;
        int64_t lastTime = -1;
        uint32_t ordinal = 0;
        bool indexed = false;
    };
public:
    OfflineSampleStore( std::filesystem::path root, std::string identity, ConversionResources& resources )
        : m_root( std::move( root ) ), m_identity( std::move( identity ) ), m_resources( resources ), m_stateLease( resources.Workspace() )
    {
        Require( m_identity.size() == 64, "offline_sample_identity" );
        Require( !std::filesystem::exists( analysis::AnalysisIoPath( m_root ) ), "offline_sample_exists" );
        m_resources.BeforeFileGrowth( m_root, 0 );
        std::filesystem::create_directories( analysis::AnalysisIoPath( m_root ) );
    }
    uint64_t Append( uint64_t thread, uint8_t kind, const OfflineSampleRow& value )
    {
        Check();
        Require( !m_sealed && kind < 2 && value.time >= 0 && value.time < ( int64_t( 1 ) << LocalIdBits ), "offline_sample_append" );
        ++m_stats.appends;
        auto& state = GetState( thread, kind );
        auto& rows = kind == 0 && value.time == 0 ? ZeroRows( state ) : *state.rows;
        const auto local = rows.Append( &value );
        Require( local <= LocalIdMask, "offline_sample_local_id" );
        if( kind == 0 && value.time > 0 && state.indexed )
        {
            const auto indexed = m_timeIndex->Count();
            m_timeIndex->Upsert( IndexKey( state, value.time ), local );
            if( m_timeIndex->Count() != indexed + 1 )
                throw std::runtime_error( "offline_sample_duplicate_index:" + std::to_string( thread ) + ":" + std::to_string( kind ) + ":" + std::to_string( value.time ) );
        }
        state.lastTime = std::max( state.lastTime, value.time );
        ++state.count;
        ++m_total;
        return Handle( state, local );
    }
    std::optional<std::pair<uint64_t, OfflineSampleRow>> Find( uint64_t thread, uint8_t kind, int64_t time )
    {
        const auto started = std::chrono::steady_clock::now();
        Check();
        Require( !m_sealed && kind == 0 && time > 0 && time < ( int64_t( 1 ) << LocalIdBits ), "offline_sample_find" );
        ++m_stats.finds;
        const auto found = m_states.find( { thread, kind } );
        if( found == m_states.end() ) { RecordFindTime( started ); return {}; }
        auto& state = found->second;
        EnsureIndex( state );
        const auto local = m_timeIndex->Find( IndexKey( state, time ) );
        if( !local ) { RecordFindTime( started ); return {}; }
        OfflineSampleRow row;
        state.rows->Read( *local, &row );
        Require( row.time == time, "offline_sample_index_value" );
        RecordFindTime( started );
        return std::make_pair( Handle( state, *local ), row );
    }
    void Put( uint64_t handle, const OfflineSampleRow& value )
    {
        Check();
        const auto ordinal = uint32_t( handle >> LocalIdBits );
        const auto local = handle & LocalIdMask;
        Require( !m_sealed && ordinal < m_ordinals.size(), "offline_sample_update" );
        auto& state = *m_ordinals[ordinal];
        Require( local < state.rows->Count(), "offline_sample_update" );
        ++m_stats.updates;
        OfflineSampleRow row;
        state.rows->Read( local, &row );
        Require( row.time == value.time, "offline_sample_update_time" );
        state.rows->Put( local, &value );
    }
    std::optional<int64_t> LastTime( uint64_t thread, uint8_t kind ) const
    {
        const auto found = m_states.find( { thread, kind } );
        return found == m_states.end() ? std::nullopt : std::optional<int64_t>( found->second.lastTime );
    }
    void Seal()
    {
        Check();
        Require( !m_sealed, "offline_sample_sealed" );
        for( auto& [key, state] : m_states )
        {
            state.rows->Seal();
            if( state.zeroRows ) state.zeroRows->Seal();
        }
        if( m_timeIndex ) m_timeIndex->Seal();
        m_sealed = true;
    }
    template<class Visitor> void Visit( uint64_t thread, uint8_t kind, Visitor&& visitor )
    {
        Check();
        Require( m_sealed && kind < 2, "offline_sample_not_sealed" );
        const auto found = m_states.find( { thread, kind } );
        if( found == m_states.end() ) return;
        ++m_stats.visits;
        auto counted = [&]( const OfflineSampleRow& row ) { ++m_stats.visitedRows; visitor( row ); };
        auto& state = found->second;
        if( kind == 1 ) { VisitRows( *state.rows, counted ); return; }
        if( state.zeroRows ) VisitRows( *state.zeroRows, counted );
        if( state.indexed )
        {
            const auto first = uint64_t( state.ordinal ) << LocalIdBits;
            m_timeIndex->VisitRange( first + 1, first + LocalIdMask, [&]( uint64_t, uint64_t local ) {
                OfflineSampleRow row;
                state.rows->Read( local, &row );
                counted( row );
            } );
        }
        else VisitRows( *state.rows, counted );
    }
    uint64_t Count( uint64_t thread, uint8_t kind ) const
    {
        const auto found = m_states.find( { thread, kind } );
        return found == m_states.end() ? 0 : found->second.count;
    }
    uint64_t TotalCount() const { return m_total; }
    uint64_t ThreadCount() const { return m_threads.size(); }
    bool HasTimeIndex() const { return m_timeIndex != nullptr; }
    OfflineSampleStoreStats Statistics() const
    {
        auto stats = m_stats;
        for( const auto& [key, state] : m_states )
        {
            AddRowStats( stats, state.rows->Statistics() );
            if( state.zeroRows ) AddRowStats( stats, state.zeroRows->Statistics() );
        }
        if( m_timeIndex )
        {
            const auto index = m_timeIndex->Statistics();
            stats.indexPageLoads = index.pageLoads;
            stats.indexPageWrites = index.pageWrites;
            stats.indexEvictions = index.evictions;
        }
        return stats;
    }
    void ReduceCache()
    {
        Check();
        for( auto& [key, state] : m_states )
        {
            state.rows->ReduceCache();
            if( state.zeroRows ) state.zeroRows->ReduceCache();
        }
        if( m_timeIndex ) m_timeIndex->ReduceCache();
    }
private:
    static void Require( bool value, const char* message ) { if( !value ) throw std::runtime_error( message ); }
    void Check() const { m_resources.CheckFast(); Require( !m_failed, "offline_sample_failed" ); }
    std::filesystem::path RowsPath( uint32_t ordinal, bool zero ) const
    {
        return m_root / ( std::string( zero ? "zero-" : "rows-" ) + std::to_string( ordinal ) + ".bin" );
    }
    State& GetState( uint64_t thread, uint8_t kind )
    {
        const Key key { thread, kind };
        const auto found = m_states.find( key );
        if( found != m_states.end() ) return found->second;
        Require( m_states.size() < ( 1u << 17 ), "offline_sample_state_count" );
        m_threads.emplace( thread );
        m_stateLease.Resize( ( m_states.size() + 1 ) * 256 + m_threads.size() * 128 + 256 );
        State state;
        state.ordinal = uint32_t( m_ordinals.size() );
        state.rows = std::make_unique<MutableRows>( RowsPath( state.ordinal, false ), sizeof( OfflineSampleRow ), m_resources, StateCacheBytes );
        auto [inserted, ok] = m_states.emplace( key, std::move( state ) );
        Require( ok && inserted->second.ordinal == m_ordinals.size(), "offline_sample_state_insert" );
        m_ordinals.push_back( &inserted->second );
        return inserted->second;
    }
    MutableRows& ZeroRows( State& state )
    {
        if( !state.zeroRows ) state.zeroRows = std::make_unique<MutableRows>( RowsPath( state.ordinal, true ), sizeof( OfflineSampleRow ), m_resources, StateCacheBytes );
        return *state.zeroRows;
    }
    static uint64_t Handle( const State& state, uint64_t local )
    {
        Require( local <= LocalIdMask, "offline_sample_local_id" );
        return ( uint64_t( state.ordinal ) << LocalIdBits ) | local;
    }
    uint64_t IndexKey( const State& state, int64_t time ) const { return ( uint64_t( state.ordinal ) << LocalIdBits ) | uint64_t( time ); }
    void EnsureIndex( State& state )
    {
        if( state.indexed ) return;
        const auto started = std::chrono::steady_clock::now();
        if( !m_timeIndex ) m_timeIndex = std::make_unique<DiskIndex64>( m_root / "time.index", m_identity, m_resources );
        uint64_t local = 0;
        for( ; local < state.rows->Count(); ++local )
        {
            OfflineSampleRow row;
            state.rows->Read( local, &row );
            Require( row.time > 0, "offline_sample_chain" );
            const auto key = IndexKey( state, row.time );
            if( m_timeIndex->Find( key ) ) throw std::runtime_error( "offline_sample_duplicate_before_index" );
            m_timeIndex->Upsert( key, local );
        }
        Require( local == state.rows->Count(), "offline_sample_index_count" );
        state.indexed = true;
        ++m_stats.indexBuilds;
        m_stats.indexBuildRows += local;
        m_stats.indexBuildNanoseconds += uint64_t( std::chrono::duration_cast<std::chrono::nanoseconds>( std::chrono::steady_clock::now() - started ).count() );
    }
    void RecordFindTime( std::chrono::steady_clock::time_point started )
    {
        m_stats.findNanoseconds += uint64_t( std::chrono::duration_cast<std::chrono::nanoseconds>( std::chrono::steady_clock::now() - started ).count() );
    }
    template<class Visitor> static void VisitRows( MutableRows& rows, Visitor& visitor )
    {
        rows.Visit( [&]( const void* data, uint64_t count ) {
            const auto* values = static_cast<const OfflineSampleRow*>( data );
            for( uint64_t i = 0; i < count; ++i ) visitor( values[i] );
        } );
    }
    static void AddRowStats( OfflineSampleStoreStats& output, const MutableRows::Stats& rows )
    {
        output.rowPageLoads += rows.pageLoads;
        output.rowPageWrites += rows.pageWrites;
        output.rowEvictions += rows.evictions;
        output.rowSlotAllocations += rows.slotAllocations;
        output.rowSlotReuses += rows.slotReuses;
    }
    std::filesystem::path m_root;
    std::string m_identity;
    ConversionResources& m_resources;
    analysis::AnalysisWorkspaceReservation m_stateLease;
    std::unique_ptr<DiskIndex64> m_timeIndex;
    std::unordered_map<Key, State, KeyHash> m_states;
    std::vector<State*> m_ordinals;
    std::unordered_set<uint64_t> m_threads;
    OfflineSampleStoreStats m_stats;
    uint64_t m_total = 0;
    bool m_sealed = false, m_failed = false;
};

}
#endif
