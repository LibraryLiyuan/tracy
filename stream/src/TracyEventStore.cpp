#include "TracyEventStore.hpp"
#include "TracySnapshotStream.hpp"
#include "../../public/common/TracyVersion.hpp"
#include "../../server/TracyJnData.hpp"
#include "../../server/TracyTraceFormat.hpp"
#include <ctime>
#include "../../server/TracyBc1.hpp"
#include "../../public/common/TracyJnGpuCatalog.hpp"
#include "../../public/common/TracyProtocol.hpp"
#include "TracyProtocolEventReader.hpp"
#include "../../public/common/tracy_lz4.hpp"
#include "../../server/TracyProtocolObserver.hpp"
#define XXH_INLINE_ALL
#include "../../server/tracy_xxhash.h"
#include "TracyStreamJournal.hpp"
#include "sqlite3.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <span>
#include <string_view>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <deque>
#include <condition_variable>
#include <mutex>
#ifdef _WIN32
#  include <Windows.h>
#  include <bcrypt.h>
#endif

namespace tracy::stream
{
namespace
{
using I = int64_t;
void Require( bool v, const char* m )
{
    if( !v ) throw std::runtime_error( m );
}
std::string Utf8( const std::filesystem::path& p )
{
    auto s = p.u8string();
    return { reinterpret_cast<const char*>( s.data() ), s.size() };
}
std::string Hex( std::span<const uint8_t> b )
{
    const char* h = "0123456789abcdef";
    std::string s;
    for( auto c : b )
    {
        s += h[c >> 4];
        s += h[c & 15];
    }
    return s;
}
I Add( I a, I b )
{
    Require( !( b > 0 && a > INT64_MAX - b ) && !( b < 0 && a < INT64_MIN - b ), "Timestamp overflow" );
    return a + b;
}
I Sub( I a, I b )
{
    Require( !( b > 0 && a < INT64_MIN + b ) && !( b < 0 && a > INT64_MAX + b ), "Timestamp subtraction overflow" );
    return a - b;
}
I Bucket( I n )
{
    I q = n / 1000000;
    if( n < 0 && n % 1000000 ) --q;
    return std::clamp<I>( q, INT32_MIN, INT32_MAX );
}
std::string Sha( const std::filesystem::path& p, const StoreOptions& o )
{
#ifdef _WIN32
    BCRYPT_ALG_HANDLE a = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    struct Guard
    {
        BCRYPT_ALG_HANDLE& a;
        BCRYPT_HASH_HANDLE& h;
        ~Guard()
        {
            if( h ) BCryptDestroyHash( h );
            if( a ) BCryptCloseAlgorithmProvider( a, 0 );
        }
    } g{ a, h };
    Require( BCryptOpenAlgorithmProvider( &a, BCRYPT_SHA256_ALGORITHM, nullptr, 0 ) == 0 && BCryptCreateHash( a, &h, nullptr, 0, nullptr, 0, 0 ) == 0, "SHA256 initialization failed" );
    // Direct reads avoid the stream layer's extra copy; hashing then runs near
    // the hash's own throughput for multi-gigabyte stores. No sequential-scan
    // hint: Windows recycles such pages first, but a hashed store is read
    // again right afterwards.
    const HANDLE f = CreateFileW( p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );
    Require( f != INVALID_HANDLE_VALUE, "Cannot hash file" );
    struct File
    {
        HANDLE f;
        ~File() { CloseHandle( f ); }
    } file{ f };
    std::vector<uint8_t> b( 1 << 20 );
    for( ;; )
    {
        if( o.cancelled && o.cancelled() ) throw std::runtime_error( "Cancelled" );
        DWORD n = 0;
        Require( ReadFile( f, b.data(), DWORD( b.size() ), &n, nullptr ) != 0, "SHA256 read failed" );
        if( !n ) break;
        Require( BCryptHashData( h, b.data(), n, 0 ) == 0, "SHA256 update failed" );
    }
    std::array<uint8_t, 32> d;
    Require( BCryptFinishHash( h, d.data(), ULONG( d.size() ), 0 ) == 0, "SHA256 finish failed" );
    return Hex( d );
#else
    throw std::runtime_error( "Event store identity currently requires Windows SHA256" );
#endif
}
struct Db
{
    sqlite3* handle = nullptr;
    StoreOptions options;
    struct CachedStatement
    {
        sqlite3_stmt* statement = nullptr;
        uint64_t used = 0;
    };
    struct TextHash
    {
        using is_transparent = void;
        size_t operator()( std::string_view text ) const noexcept { return std::hash<std::string_view>{}( text ); }
    };
    std::unordered_map<std::string, CachedStatement, TextHash, std::equal_to<>> cache;
    // Direct-mapped by the address of SQL text with static storage duration.
    // Entries point into stable unordered_map nodes and are cleared on eviction.
    struct StaticSlot
    {
        const char* sql = nullptr;
        CachedStatement* entry = nullptr;
    };
    std::array<StaticSlot, 512> statics{};
    uint64_t clock = 0;
    // A confined connection is never used by two threads at the same time, so
    // SQLite's per-call connection mutex is unnecessary (multi-thread mode).
    Db( const std::filesystem::path& p, bool write, const StoreOptions& o, bool confined = false )
        : options( o )
    {
        Require( o.cacheBytes >= ( 4ull << 20 ) && o.cacheBytes <= ( 256ull << 20 ) && o.resultBytes >= 65536 && o.resultBytes <= ( 64ull << 20 ), "Invalid store budget" );
        if( sqlite3_hard_heap_limit64( 0 ) == 0 ) sqlite3_hard_heap_limit64( 512ll << 20 );
        int rc = sqlite3_open_v2( Utf8( p ).c_str(), &handle, ( write ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE : SQLITE_OPEN_READONLY ) | ( confined ? SQLITE_OPEN_NOMUTEX : 0 ), nullptr );
        if( rc != SQLITE_OK )
        {
            std::string e = handle ? sqlite3_errmsg( handle ) : "SQLite open failed";
            if( handle ) sqlite3_close( handle );
            handle = nullptr;
            throw std::runtime_error( e );
        }
        try
        {
            // Cached statements keep the default 40 lookaside slots occupied, so
            // each execution's cursor memory fell back to the global allocator.
            // A fixed 600 KB per connection keeps these allocations local.
            sqlite3_db_config( handle, SQLITE_DBCONFIG_LOOKASIDE, nullptr, 1200, 500 );
            sqlite3_limit( handle, SQLITE_LIMIT_LENGTH, 1 << 20 );
            sqlite3_progress_handler(
                handle, 4096, []( void* p ) {auto& o=*static_cast<StoreOptions*>(p);return o.cancelled&&o.cancelled()?1:0; }, &options );
            Exec( "PRAGMA mmap_size=0;PRAGMA temp_store=FILE;" );
            Exec( "PRAGMA cache_size=-" + std::to_string( o.cacheBytes / 1024 ) );
            if( write ) Exec( "PRAGMA journal_mode=DELETE;PRAGMA synchronous=FULL;" );
        }
        catch( ... )
        {
            sqlite3_close( handle );
            handle = nullptr;
            throw;
        }
    }
    ~Db()
    {
        for( auto& [k, s] : cache ) sqlite3_finalize( s.statement );
        if( handle ) sqlite3_close( handle );
    }
    void Exec( const std::string& sql )
    {
        char* e = nullptr;
        if( sqlite3_exec( handle, sql.c_str(), nullptr, nullptr, &e ) != SQLITE_OK )
        {
            std::string m = e ? e : sqlite3_errmsg( handle );
            sqlite3_free( e );
            throw std::runtime_error( m );
        }
    }
    CachedStatement& Lookup( std::string_view sql )
    {
        auto it = cache.find( sql );
        if( it != cache.end() ) return it->second;
        if( cache.size() == 96 )
        {
            auto victim = cache.end();
            for( auto entry = cache.begin(); entry != cache.end(); ++entry )
                if( !sqlite3_stmt_busy( entry->second.statement ) && ( victim == cache.end() || entry->second.used < victim->second.used ) ) victim = entry;
            Require( victim != cache.end(), "Pinned SQL statements exceed cache budget" );
            for( auto& slot : statics )
                if( slot.entry == &victim->second ) slot = {};
            sqlite3_finalize( victim->second.statement );
            cache.erase( victim );
        }
        sqlite3_stmt* s = nullptr;
        if( sqlite3_prepare_v2( handle, sql.data(), int( sql.size() ), &s, nullptr ) != SQLITE_OK ) throw std::runtime_error( sqlite3_errmsg( handle ) );
        return cache.emplace( std::string( sql ), CachedStatement{ s, 0 } ).first->second;
    }
    sqlite3_stmt* Ready( CachedStatement& entry )
    {
        entry.used = ++clock;
        sqlite3_reset( entry.statement );
        sqlite3_clear_bindings( entry.statement );
        return entry.statement;
    }
    sqlite3_stmt* Prep( const std::string& sql ) { return Ready( Lookup( sql ) ); }
    // `sql` must have static storage duration; its address identifies the text.
    sqlite3_stmt* PrepStatic( const char* sql )
    {
        auto& slot = statics[( uint64_t( reinterpret_cast<uintptr_t>( sql ) ) * 0x9E3779B97F4A7C15ull ) >> 55];
        if( slot.sql != sql )
        {
            slot.entry = &Lookup( sql );
            slot.sql = sql;
        }
        return Ready( *slot.entry );
    }
    template<size_t N>
    sqlite3_stmt* Prep( const char ( &sql )[N] ) { return PrepStatic( sql ); }
    bool Step( sqlite3_stmt* s )
    {
        if( options.cancelled && options.cancelled() )
        {
            sqlite3_reset( s );
            throw std::runtime_error( "Cancelled" );
        }
        int rc = sqlite3_step( s );
        if( rc == SQLITE_ROW ) return true;
        if( rc == SQLITE_DONE ) return false;
        const std::string error = sqlite3_errmsg( handle );
        const char* sql = sqlite3_sql( s );
        throw std::runtime_error( error + " [SQL: " + ( sql ? sql : "unknown statement" ) + "]" );
    }
    void Run( sqlite3_stmt* s ) { Require( !Step( s ), "Unexpected SQL row" ); }
    I Scalar( const std::string& sql )
    {
        auto s = Prep( sql );
        Require( Step( s ), "Missing SQL scalar" );
        I n = sqlite3_column_int64( s, 0 );
        sqlite3_reset( s );
        return n;
    }
};
void Int( sqlite3_stmt* s, int i, uint64_t v ) { Require( sqlite3_bind_int64( s, i, I( v ) ) == SQLITE_OK, "Integer binding failed" ); }
void Text( sqlite3_stmt* s, int i, std::string_view v ) { Require( sqlite3_bind_text( s, i, v.data(), int( v.size() ), SQLITE_TRANSIENT ) == SQLITE_OK, "Text binding failed" ); }
void Blob( sqlite3_stmt* s, int i, std::span<const uint8_t> v ) { Require( sqlite3_bind_blob( s, i, v.data(), int( v.size() ), SQLITE_TRANSIENT ) == SQLITE_OK, "Blob binding failed" ); }
std::string Col( sqlite3_stmt* s, int i )
{
    auto p = sqlite3_column_text( s, i );
    return p ? std::string( reinterpret_cast<const char*>( p ), sqlite3_column_bytes( s, i ) ) : std::string();
}
void Meta( Db& d, const char* k, std::string_view v )
{
    auto s = d.Prep( "INSERT OR REPLACE INTO metadata VALUES(?,?)" );
    Text( s, 1, k );
    Text( s, 2, v );
    d.Run( s );
}
std::string Meta( Db& d, const char* k )
{
    auto s = d.Prep( "SELECT value FROM metadata WHERE key=?" );
    Text( s, 1, k );
    Require( d.Step( s ), "Missing store metadata" );
    auto v = Col( s, 0 );
    sqlite3_reset( s );
    return v;
}
void Save( Db& d, const StoreInfo& i )
{
    Meta( d, "transcript_verified", i.transcriptVerified ? "1" : "0" );
    Meta( d, "zone_tree", i.zoneTree ? "1" : "0" );
    Meta( d, "zone_tree_nodes", std::to_string( i.zoneTreeNodes ) );
    Meta( d, "session", i.session );
    Meta( d, "source", i.sourceSha256 );
    Meta( d, "program", i.program );
    Meta( d, "gpu_zones", std::to_string( i.gpuZones ) );
    Meta( d, "gpu_incomplete", std::to_string( i.incompleteGpuZones ) );
    Meta( d, "samples", std::to_string( i.samples ) );
    Meta( d, "sample_pending", std::to_string( i.pendingSamples ) );
    Meta( d, "context_regions", std::to_string( i.contextRegions ) );
    Meta( d, "cpu_regions", std::to_string( i.cpuRegions ) );
    Meta( d, "messages", std::to_string( i.messages ) );
    Meta( d, "plot_points", std::to_string( i.plotPoints ) );
    Meta( d, "lock_events", std::to_string( i.lockEvents ) );
    Meta( d, "jobs", std::to_string( i.jobs ) );
    Meta( d, "io_requests", std::to_string( i.ioRequests ) );
    Meta( d, "script_events", std::to_string( i.scriptEvents ) );
    Meta( d, "allocations", std::to_string( i.allocations ) );
    Meta( d, "active_allocations", std::to_string( i.activeAllocations ) );
    Meta( d, "gpu_reference_passes", std::to_string( i.gpuReferencePasses ) );
    Meta( d, "gpu_reference_uses", std::to_string( i.gpuReferenceUses ) );
    Meta( d, "gpu_reference_ends", std::to_string( i.gpuReferenceEnds ) );
    Meta( d, "catalog_generations", std::to_string( i.catalogGenerations ) );
    Meta( d, "catalog_records", std::to_string( i.catalogRecords ) );
    Meta( d, "frame_images", std::to_string( i.frameImages ) );
    Meta( d, "fibers", std::to_string( i.fibers ) );
    Meta( d, "auxiliary_events", std::to_string( i.auxiliaryEvents ) );
    Meta( d, "hardware_samples", std::to_string( i.hardwareSamples ) );
    Meta( d, "hardware_addresses", std::to_string( i.hardwareAddresses ) );
    Meta( d, "symbol_resources", std::to_string( i.symbols ) );
    Meta( d, "source_resources", std::to_string( i.sourceFiles ) );
    Meta( d, "catalog_present", i.catalogPresent ? "1" : "0" );
    Meta( d, "catalog_valid", i.catalogValid ? "1" : "0" );
    Meta( d, "zone_events_misplaced", std::to_string( i.misplacedZoneEvents ) );
    Meta( d, "context_switch_gaps", std::to_string( i.contextSwitchGaps ) );
    for( auto [k, v] : std::array<std::pair<const char*, uint64_t>, 11>{ { { "bytes", i.sourceBytes }, { "valid", i.validBytes }, { "revision", i.revision }, { "events", i.events }, { "zones", i.cpuZones }, { "frames", i.frames }, { "open", i.openZones }, { "unresolved", i.unresolvedDefinitions }, { "first", uint64_t( i.firstNs ) }, { "last", uint64_t( i.lastNs ) }, { "clean", i.cleanEnd ? 1 : 0 } } } ) Meta( d, k, std::to_string( v ) );
}
StoreInfo Load( Db& d )
{
    const auto version = d.Scalar( "PRAGMA user_version" );
    // Schema 17 keeps CPU zones, with their child time, in event blocks;
    // older stores are rebuilt.
    Require( version == EventStoreSchema, "Store schema mismatch; rebuild the event store" );
    StoreInfo i;
    i.schema = uint32_t( version );
    i.session = Meta( d, "session" );
    i.sourceSha256 = Meta( d, "source" );
    i.program = Meta( d, "program" );
    auto n = [&]( const char* k ) { return std::stoull( Meta( d, k ) ); };
    i.sourceBytes = n( "bytes" );
    i.validBytes = n( "valid" );
    i.revision = n( "revision" );
    i.events = n( "events" );
    i.cpuZones = n( "zones" );
    i.frames = n( "frames" );
    i.openZones = n( "open" );
    i.unresolvedDefinitions = n( "unresolved" );
    i.firstNs = I( n( "first" ) );
    i.lastNs = I( n( "last" ) );
    i.cleanEnd = n( "clean" ) != 0;
    i.transcriptVerified = d.Scalar( "SELECT COUNT(*) FROM metadata WHERE key='transcript_verified' AND value='1'" ) != 0;
    i.zoneTree = d.Scalar( "SELECT COUNT(*) FROM metadata WHERE key='zone_tree' AND value='1'" ) != 0;
    if( i.zoneTree ) i.zoneTreeNodes = n( "zone_tree_nodes" );
    i.gpuZones = n( "gpu_zones" );
    i.incompleteGpuZones = n( "gpu_incomplete" );
    i.samples = n( "samples" );
    i.pendingSamples = n( "sample_pending" );
    i.contextRegions = n( "context_regions" );
    i.cpuRegions = n( "cpu_regions" );
    i.messages = n( "messages" );
    i.plotPoints = n( "plot_points" );
    i.lockEvents = n( "lock_events" );
    i.jobs = n( "jobs" );
    i.ioRequests = n( "io_requests" );
    i.scriptEvents = n( "script_events" );
    i.allocations = n( "allocations" );
    i.activeAllocations = n( "active_allocations" );
    i.gpuReferencePasses = n( "gpu_reference_passes" );
    i.gpuReferenceUses = n( "gpu_reference_uses" );
    i.gpuReferenceEnds = n( "gpu_reference_ends" );
    i.fibers = n( "fibers" );
    // Absent in stores built before misplaced zone events were counted.
    if( d.Scalar( "SELECT COUNT(*) FROM metadata WHERE key='zone_events_misplaced'" ) != 0 ) i.misplacedZoneEvents = n( "zone_events_misplaced" );
    if( d.Scalar( "SELECT COUNT(*) FROM metadata WHERE key='context_switch_gaps'" ) != 0 ) i.contextSwitchGaps = n( "context_switch_gaps" );
    i.hardwareSamples = n( "hardware_samples" );
    i.hardwareAddresses = n( "hardware_addresses" );
    i.symbols = n( "symbol_resources" );
    i.sourceFiles = n( "source_resources" );
    i.auxiliaryEvents = n( "auxiliary_events" );
    i.frameImages = n( "frame_images" );
    i.catalogGenerations = n( "catalog_generations" );
    i.catalogRecords = n( "catalog_records" );
    i.catalogPresent = n( "catalog_present" ) != 0;
    i.catalogValid = n( "catalog_valid" ) != 0;
    return i;
}
constexpr QueueType AuxiliaryTypes[] = { QueueType::JnFrame, QueueType::JnGfxDispatch, QueueType::JnGfxEntity, QueueType::JnGfxLink, QueueType::JnRelation, QueueType::JnRuntimeDomainState };
StoreAuxiliaryRecord DecodeAuxiliary( uint64_t id, uint64_t thread, I time, QueueType type, std::span<const uint8_t> bytes )
{
    Require( bytes.size() == QueueDataSize[uint8_t( type )] - 1, "Invalid auxiliary record size" );
    StoreAuxiliaryRecord record;
    record.id = id;
    record.thread = thread;
    record.time = time;
    QueueItem e{};
    memcpy( &e.jnFrame, bytes.data(), bytes.size() );
    switch( type )
    {
    case QueueType::JnFrame: {
        const auto& p = e.jnFrame;
        record.kind = StoreAuxiliaryKind::Frame;
        record.data = StoreFrameIdentity{ p.frameId, p.domainIndex, p.domain, p.phase, p.flags };
        break;
    }
    case QueueType::JnGfxDispatch: {
        const auto& p = e.jnGfxDispatch;
        record.kind = StoreAuxiliaryKind::GfxDispatch;
        record.data = StoreGfxDispatch{ p.dispatchId, p.frameIndex, p.expectedJobs, p.threadingMode, p.flags };
        break;
    }
    case QueueType::JnGfxEntity: {
        const auto& p = e.jnGfxEntity;
        record.kind = StoreAuxiliaryKind::GfxEntity;
        record.data = StoreGfxEntity{ p.entityId, p.parentId, p.gpuQueryId, p.gpuContext, p.kind, p.flags };
        break;
    }
    case QueueType::JnGfxLink: {
        const auto& p = e.jnGfxLink;
        record.kind = StoreAuxiliaryKind::GfxLink;
        record.data = StoreGfxLink{ p.sourceId, p.targetId, p.relation, p.flags };
        break;
    }
    case QueueType::JnRelation: {
        const auto& p = e.jnRelation;
        record.kind = StoreAuxiliaryKind::Relation;
        record.data = StoreRelation{ p.sourceId, p.targetId, p.sourceKind, p.targetKind, p.relationNamespace, p.relation, p.flags };
        break;
    }
    case QueueType::JnRuntimeDomainState: {
        const auto& p = e.jnRuntimeDomainState;
        record.kind = StoreAuxiliaryKind::RuntimeState;
        record.data = StoreRuntimeState{ p.generation, p.requestedFrame, p.domain, p.requestedMode, p.effectiveMode, p.reason, p.flags };
        break;
    }
    default: throw std::runtime_error( "Unknown auxiliary record kind" );
    }
    return record;
}
// Secondary indexes that ingestion never reads. Offline builds create them
// once from the loaded rows instead of maintaining them row by row.
constexpr const char* DeferredIndexes[] = { "gpu_zone_parent", "gpu_zone_context", "gpu_unknown_start", "job_schedule_job" };
void Schema( Db& d ) {
    d.Exec( R"SQL(
PRAGMA user_version=17;
CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL) WITHOUT ROWID;
CREATE TABLE capture_metadata(key TEXT PRIMARY KEY,data BLOB) WITHOUT ROWID;
CREATE TABLE cpu_topology(thread INTEGER PRIMARY KEY,package INTEGER,die INTEGER,core INTEGER);
CREATE TABLE thread_hints(thread INTEGER PRIMARY KEY,event INTEGER,hint INTEGER);
CREATE INDEX thread_hint_event ON thread_hints(event);
CREATE TABLE crash(event INTEGER,thread INTEGER,time INTEGER,message INTEGER,stack INTEGER);
CREATE TABLE queries(seq INTEGER,ord INTEGER,type INTEGER,key INTEGER,used INTEGER DEFAULT 0,extra INTEGER DEFAULT 0,PRIMARY KEY(seq,ord)) WITHOUT ROWID;
CREATE INDEX query_response ON queries(type,key,used,seq,ord);
CREATE INDEX query_response_fifo ON queries(type,used,seq,ord);
CREATE TABLE strings(kind INTEGER,key INTEGER,value TEXT,PRIMARY KEY(kind,key)) WITHOUT ROWID;
CREATE TABLE sources(kind INTEGER,key INTEGER,nkey INTEGER,fkey INTEGER,pkey INTEGER,line INTEGER,color INTEGER,name TEXT,function TEXT,file TEXT,PRIMARY KEY(kind,key)) WITHOUT ROWID;
CREATE TABLE threads(id INTEGER PRIMARY KEY,top INTEGER DEFAULT 0,validation INTEGER DEFAULT 0,nextstack INTEGER DEFAULT 0,fiber INTEGER DEFAULT 0);
CREATE TABLE stacks(id INTEGER PRIMARY KEY,data BLOB UNIQUE);
CREATE TABLE callsites(id INTEGER PRIMARY KEY,stack INTEGER,provenance INTEGER,reason INTEGER,event INTEGER UNIQUE,thread INTEGER,source INTEGER,domain INTEGER,flags INTEGER);
CREATE INDEX callsite_match ON callsites(source,stack,event);
CREATE TABLE fibers(key INTEGER PRIMARY KEY,id INTEGER UNIQUE,group_hint INTEGER);
CREATE TABLE fiber_active(fiber INTEGER PRIMARY KEY,region INTEGER);
CREATE TABLE stackframes(key INTEGER,ordinal INTEGER,name TEXT,file TEXT,image TEXT,line INTEGER,symbol INTEGER,length INTEGER,PRIMARY KEY(key,ordinal)) WITHOUT ROWID;
CREATE TABLE blocks(id INTEGER PRIMARY KEY,stream INTEGER,key INTEGER,first INTEGER,last INTEGER,count INTEGER,tmin INTEGER,tmax INTEGER,tclosed INTEGER,unfinished INTEGER,depth INTEGER,offset INTEGER,size INTEGER,raw INTEGER,checksum INTEGER,summary BLOB);
CREATE INDEX block_key ON blocks(stream,key,first);
CREATE INDEX block_first ON blocks(stream,first);
CREATE VIRTUAL TABLE block_range USING rtree_i32(id,mint,maxt);
CREATE TABLE zone_late(id INTEGER PRIMARY KEY,thread INTEGER,end INTEGER,children INTEGER,name TEXT,color INTEGER,child_time INTEGER DEFAULT 0);
CREATE TABLE zone_late_text(zone INTEGER,event INTEGER,thread INTEGER,text TEXT,PRIMARY KEY(zone,event)) WITHOUT ROWID;
CREATE TABLE open_frames(id INTEGER PRIMARY KEY,thread INTEGER,parent INTEGER,validation INTEGER,generation INTEGER,start INTEGER,depth INTEGER,slot INTEGER,children INTEGER,color INTEGER,flags INTEGER,name TEXT,last_child INTEGER,last_child_end INTEGER,node INTEGER,covered INTEGER,cover_end INTEGER,last_child_start INTEGER,child_time INTEGER);
CREATE TABLE zone_tree(id INTEGER PRIMARY KEY,parent INTEGER,thread INTEGER,skind INTEGER,skey INTEGER,example INTEGER,events INTEGER,closed INTEGER,inclusive INTEGER,self INTEGER,minimum INTEGER,maximum INTEGER);
CREATE TABLE frames(id INTEGER PRIMARY KEY,setid INTEGER,start INTEGER,end INTEGER,continuous INTEGER);
CREATE INDEX frame_set_time ON frames(setid,start,id);
CREATE VIRTUAL TABLE frame_range USING rtree_i32(id,mint,maxt);
CREATE TABLE base_frames(ordinal INTEGER PRIMARY KEY,frame INTEGER UNIQUE);
CREATE TABLE frame_images(id INTEGER PRIMARY KEY,ordinal INTEGER UNIQUE,frame_index INTEGER UNIQUE,width INTEGER,height INTEGER,flipped INTEGER,data BLOB);
CREATE TABLE frame_image_pending(slot INTEGER PRIMARY KEY CHECK(slot=1),data BLOB);
CREATE TABLE frame_state(setid INTEGER PRIMARY KEY,top INTEGER,continuous INTEGER);
CREATE TABLE vsync_sets(key INTEGER PRIMARY KEY,setid INTEGER UNIQUE);
CREATE TABLE frame_summary(level INTEGER,bucket INTEGER,start INTEGER,end INTEGER,count INTEGER,maximum INTEGER,PRIMARY KEY(level,bucket)) WITHOUT ROWID;
CREATE TABLE gpu_contexts(id INTEGER PRIMARY KEY,sequence INTEGER UNIQUE,name TEXT DEFAULT '',thread INTEGER,period REAL,type INTEGER,calibrated INTEGER,offset INTEGER,base_gpu INTEGER,base_cpu INTEGER,scale REAL,last_gpu INTEGER,overflow INTEGER DEFAULT 0,wraps INTEGER DEFAULT 0,zones INTEGER DEFAULT 0,incomplete INTEGER DEFAULT 0);
CREATE TABLE gpu_zones(id INTEGER PRIMARY KEY,context INTEGER,thread INTEGER,parent INTEGER,depth INTEGER,cpu_start INTEGER,cpu_end INTEGER,gpu_start INTEGER,gpu_end INTEGER,skind INTEGER,skey INTEGER,stack INTEGER,callsite INTEGER,query INTEGER);
CREATE INDEX gpu_zone_parent ON gpu_zones(parent,id);
CREATE INDEX gpu_zone_context ON gpu_zones(context,id);
CREATE INDEX gpu_unknown_start ON gpu_zones(id) WHERE gpu_start IS NULL;
CREATE VIRTUAL TABLE gpu_range USING rtree_i32(id,mint,maxt);
CREATE TABLE gpu_notes(id INTEGER PRIMARY KEY,context INTEGER,note INTEGER,name TEXT,UNIQUE(context,note));
CREATE INDEX gpu_note_context ON gpu_notes(context,id);
CREATE TABLE gpu_annotations(id INTEGER PRIMARY KEY,context INTEGER,query INTEGER,note INTEGER,value BLOB,UNIQUE(context,query,note));
CREATE INDEX gpu_annotation_context ON gpu_annotations(context,id);
CREATE INDEX gpu_annotation_query ON gpu_annotations(context,query,id);
CREATE TABLE sample_dictionary(key INTEGER PRIMARY KEY,stack INTEGER);
CREATE TABLE sample_pending(thread INTEGER PRIMARY KEY,id INTEGER,time INTEGER,stack INTEGER);
CREATE INDEX sample_pending_time ON sample_pending(time,id);
CREATE TABLE samples(id INTEGER PRIMARY KEY,thread INTEGER,time INTEGER,stack INTEGER,explicit_context INTEGER DEFAULT 0);
CREATE TABLE sample_groups(thread INTEGER,time INTEGER,id INTEGER,PRIMARY KEY(thread,time)) WITHOUT ROWID;
CREATE INDEX sample_time ON samples(time,id);
CREATE INDEX sample_thread_time ON samples(thread,time,id);
CREATE TABLE context_regions(id INTEGER PRIMARY KEY,thread INTEGER,start INTEGER,end INTEGER,wakeup INTEGER,wake_cpu INTEGER,cpu INTEGER,reason INTEGER,state INTEGER,related INTEGER DEFAULT 0);
CREATE INDEX context_thread_start ON context_regions(thread,start,id);
CREATE VIRTUAL TABLE context_range USING rtree_i32(id,mint,maxt);
CREATE TABLE context_threads(thread INTEGER PRIMARY KEY,top INTEGER,pending_wake INTEGER DEFAULT 0,pending_cpu INTEGER DEFAULT 0,running INTEGER DEFAULT 0,migrations INTEGER DEFAULT 0,last_cpu INTEGER);
CREATE TABLE cpu_regions(id INTEGER PRIMARY KEY,cpu INTEGER,thread INTEGER,start INTEGER,end INTEGER);
CREATE INDEX cpu_region_cpu ON cpu_regions(cpu,id);
CREATE VIRTUAL TABLE cpu_region_range USING rtree_i32(id,mint,maxt);
CREATE TABLE context_cpus(cpu INTEGER PRIMARY KEY,top INTEGER);
CREATE TABLE thread_process(thread INTEGER PRIMARY KEY,pid INTEGER);
CREATE TABLE messages(id INTEGER PRIMARY KEY,thread INTEGER,time INTEGER,text TEXT,string_key INTEGER,color INTEGER,stack INTEGER,app INTEGER);
CREATE INDEX message_time ON messages(app,time,id);
CREATE INDEX message_thread ON messages(thread,time,id);
CREATE TABLE plots(id INTEGER PRIMARY KEY,name TEXT,kind INTEGER DEFAULT 0,format INTEGER DEFAULT 0,steps INTEGER DEFAULT 0,fill INTEGER DEFAULT 1,color INTEGER DEFAULT 0,points INTEGER DEFAULT 0,min REAL DEFAULT 0,max REAL DEFAULT 0,sum BLOB);
CREATE INDEX plot_name ON plots(name,kind);
CREATE TABLE plot_keys(key INTEGER PRIMARY KEY,plot INTEGER);
CREATE TABLE system_plots(kind INTEGER,key INTEGER,plot INTEGER UNIQUE,last_time INTEGER,PRIMARY KEY(kind,key)) WITHOUT ROWID;
CREATE TABLE parameters(event INTEGER PRIMARY KEY,idx INTEGER,name_key INTEGER,is_bool INTEGER,value INTEGER);
CREATE TABLE plot_points(id INTEGER PRIMARY KEY,plot INTEGER,time INTEGER,value BLOB);
CREATE INDEX plot_point_range ON plot_points(plot,time,id);
CREATE INDEX plot_point_time ON plot_points(time,id);
CREATE TABLE locks(id INTEGER PRIMARY KEY,source INTEGER,type INTEGER,announce INTEGER,terminate INTEGER,name TEXT,contended INTEGER DEFAULT 0,locking_thread INTEGER DEFAULT 0,owner INTEGER DEFAULT 0,lock_count INTEGER DEFAULT 0,wait INTEGER DEFAULT 0,wait_shared INTEGER DEFAULT 0,shared INTEGER DEFAULT 0,events INTEGER DEFAULT 0,last_time INTEGER);
CREATE TABLE lock_threads(lock INTEGER,thread INTEGER,ordinal INTEGER,PRIMARY KEY(lock,thread),UNIQUE(lock,ordinal)) WITHOUT ROWID;
CREATE TABLE lock_events(id INTEGER PRIMARY KEY,lock INTEGER,thread INTEGER,time INTEGER,type INTEGER,source INTEGER DEFAULT 0,owner INTEGER,count INTEGER,wait INTEGER,wait_shared INTEGER,shared INTEGER);
CREATE INDEX lock_event_time ON lock_events(time,id);
CREATE INDEX lock_event_lock_time ON lock_events(lock,time,id);
CREATE INDEX lock_event_thread ON lock_events(lock,thread,id);
CREATE TABLE job_types(id INTEGER PRIMARY KEY,event INTEGER,name_key INTEGER,kind INTEGER,flags INTEGER);
CREATE TABLE job_config_state(job INTEGER PRIMARY KEY,kind INTEGER,flags INTEGER);
CREATE TABLE canonical_frames(sequence INTEGER PRIMARY KEY,frame INTEGER,event INTEGER);
CREATE TABLE jobs(id INTEGER PRIMARY KEY,event INTEGER,packed INTEGER DEFAULT 0,schedule INTEGER DEFAULT 0,thread INTEGER DEFAULT 0,type INTEGER DEFAULT 0,count INTEGER DEFAULT 0,grain INTEGER DEFAULT 0,flow INTEGER DEFAULT 0,origin INTEGER DEFAULT 0,kind INTEGER DEFAULT 0,flags INTEGER DEFAULT 0,schema INTEGER DEFAULT 1,expected INTEGER DEFAULT 0,orphan INTEGER DEFAULT 1,boundary INTEGER DEFAULT 0,ready INTEGER,ready_lane INTEGER DEFAULT 0,ready_flags INTEGER DEFAULT 0,queue INTEGER,queue_lane INTEGER DEFAULT 0,first_run INTEGER,completed INTEGER,execution INTEGER DEFAULT 0,wait INTEGER DEFAULT 0,help INTEGER DEFAULT 0,spin INTEGER DEFAULT 0,sleep INTEGER DEFAULT 0,dispatches INTEGER DEFAULT 0,steals INTEGER DEFAULT 0,range_steals INTEGER DEFAULT 0,active_dispatches INTEGER DEFAULT 0,retries INTEGER DEFAULT 0,wait_ends INTEGER DEFAULT 0,continuations INTEGER DEFAULT 0,cancelled INTEGER DEFAULT 0,incomplete INTEGER DEFAULT 0,stack INTEGER DEFAULT 0,callsite INTEGER DEFAULT 0,first_time INTEGER,last_time INTEGER,stack_provenance INTEGER DEFAULT 3,stack_reason INTEGER DEFAULT 0,stage_first INTEGER,stage_last INTEGER,last_event INTEGER DEFAULT 0);
CREATE INDEX job_event ON jobs(event);
CREATE VIRTUAL TABLE job_range USING rtree_i32(id,mint,maxt);
CREATE TABLE job_schedule_stages(id INTEGER PRIMARY KEY,job INTEGER,stage INTEGER,stack INTEGER,callsite INTEGER);
CREATE INDEX job_schedule_job ON job_schedule_stages(job,id);
CREATE TABLE job_stage_callsites(callsite INTEGER,job INTEGER,PRIMARY KEY(callsite,job)) WITHOUT ROWID;
CREATE TABLE dirty_jobs(id INTEGER PRIMARY KEY);
CREATE TABLE dirty_io(id INTEGER PRIMARY KEY);
CREATE TABLE job_spans(job INTEGER,span INTEGER,kind INTEGER,start INTEGER,PRIMARY KEY(job,span,kind)) WITHOUT ROWID;
CREATE TABLE job_dependencies(id INTEGER PRIMARY KEY,job INTEGER,prerequisite INTEGER,handle INTEGER,flags INTEGER);
CREATE INDEX job_dependency_job ON job_dependencies(job,id);
CREATE TABLE job_lanes(job INTEGER,lane INTEGER,PRIMARY KEY(job,lane)) WITHOUT ROWID;
CREATE TABLE io_requests(id INTEGER PRIMARY KEY,event INTEGER,resource INTEGER DEFAULT 0,queued INTEGER DEFAULT 0,thread INTEGER DEFAULT 0,parent INTEGER DEFAULT 0,requested INTEGER DEFAULT 0,origin INTEGER DEFAULT 0,parent_kind INTEGER DEFAULT 0,operation INTEGER DEFAULT 0,source INTEGER DEFAULT 0,priority INTEGER DEFAULT 0,subsystem INTEGER DEFAULT 0,flags INTEGER DEFAULT 0,config_flags INTEGER DEFAULT 0,boundary INTEGER DEFAULT 0,orphan INTEGER DEFAULT 1,start INTEGER,end INTEGER,transferred INTEGER DEFAULT 0,status INTEGER DEFAULT 0,terminals INTEGER DEFAULT 0,stack INTEGER DEFAULT 0,first_time INTEGER,last_time INTEGER);
CREATE INDEX io_request_event ON io_requests(event);
CREATE VIRTUAL TABLE io_range USING rtree_i32(id,mint,maxt);
CREATE TABLE io_stages(id INTEGER PRIMARY KEY,request INTEGER,time INTEGER,thread INTEGER,bytes INTEGER,detail INTEGER,stage INTEGER,status INTEGER,flags INTEGER,stack INTEGER);
CREATE INDEX io_stage_request ON io_stages(request,id);
CREATE TABLE script_frames(id INTEGER PRIMARY KEY,thread INTEGER,frame INTEGER,function_key INTEGER,file_key INTEGER,line INTEGER,runtime INTEGER,flags INTEGER);
CREATE TABLE script_events(id INTEGER PRIMARY KEY,thread INTEGER,time INTEGER,primary_id INTEGER,secondary_id INTEGER,value INTEGER,runtime INTEGER,flags INTEGER,kind INTEGER);
CREATE INDEX script_event_time ON script_events(time,id);
)SQL" );
    d.Exec( R"SQL(
CREATE TABLE hardware_samples(id INTEGER PRIMARY KEY,address INTEGER,kind INTEGER,time INTEGER);
CREATE INDEX hardware_time ON hardware_samples(time,id);
CREATE INDEX hardware_kind_time ON hardware_samples(kind,time,id);
CREATE INDEX hardware_address_time ON hardware_samples(address,time,id);
CREATE INDEX hardware_address_kind_time ON hardware_samples(address,kind,time,id);
CREATE TABLE hardware_addresses(id INTEGER PRIMARY KEY,address INTEGER UNIQUE,c0 INTEGER DEFAULT 0,c1 INTEGER DEFAULT 0,c2 INTEGER DEFAULT 0,c3 INTEGER DEFAULT 0,c4 INTEGER DEFAULT 0,c5 INTEGER DEFAULT 0);
CREATE TABLE symbol_candidates(address INTEGER PRIMARY KEY,name TEXT,file TEXT,line INTEGER,image TEXT,size INTEGER,inlined INTEGER);
CREATE TABLE symbol_resources(id INTEGER PRIMARY KEY,address INTEGER UNIQUE,name TEXT,file TEXT,line INTEGER,image TEXT,call_file TEXT,call_line INTEGER,size INTEGER,inlined INTEGER,status INTEGER,data BLOB);
CREATE TABLE source_resources(id INTEGER PRIMARY KEY,path BLOB UNIQUE,status INTEGER,data BLOB);
CREATE TABLE source_requests(query INTEGER PRIMARY KEY,resource INTEGER,done INTEGER DEFAULT 0);
CREATE TABLE jn_runtime_domains(domain INTEGER PRIMARY KEY);
CREATE TABLE jn_runtime_states(event INTEGER PRIMARY KEY,domain INTEGER,time INTEGER);
CREATE INDEX jn_runtime_at ON jn_runtime_states(domain,time DESC,event DESC);
CREATE TABLE memory_pools(id INTEGER PRIMARY KEY,event INTEGER,allocations INTEGER DEFAULT 0,active INTEGER DEFAULT 0,usage BLOB,frees INTEGER DEFAULT 0,low TEXT,high TEXT);
CREATE INDEX memory_pool_event ON memory_pools(event);
INSERT INTO memory_pools(id,event,usage) VALUES(0,0,zeroblob(8));
CREATE TABLE allocations(id INTEGER PRIMARY KEY,pool INTEGER,address INTEGER,size INTEGER,allocated INTEGER,freed INTEGER,allocation_thread INTEGER,free_thread INTEGER DEFAULT 0,allocation_stack INTEGER DEFAULT 0,free_stack INTEGER DEFAULT 0,callsite INTEGER DEFAULT 0);
CREATE INDEX allocation_pool ON allocations(pool,id);
CREATE INDEX allocation_pool_time ON allocations(pool,allocated,id);
CREATE VIRTUAL TABLE allocation_range USING rtree_i32(id,mint,maxt);
CREATE TABLE memory_active(pool INTEGER,address INTEGER,event INTEGER,PRIMARY KEY(pool,address)) WITHOUT ROWID;
CREATE TABLE zone_display_end(id INTEGER PRIMARY KEY,end INTEGER);
CREATE TABLE custom_frames(id INTEGER PRIMARY KEY,name TEXT,file TEXT,line INTEGER,UNIQUE(name,file,line));
CREATE TABLE gpu_ref_sets(id INTEGER PRIMARY KEY,count INTEGER);
CREATE TABLE gpu_ref_entries(setid INTEGER,ordinal INTEGER,resource INTEGER,mask INTEGER,PRIMARY KEY(setid,ordinal)) WITHOUT ROWID;
CREATE TABLE gpu_ref_passes(id INTEGER PRIMARY KEY,pass INTEGER,time INTEGER,frame INTEGER,thread INTEGER,taxonomy INTEGER,level INTEGER,flags INTEGER);
CREATE INDEX gpu_ref_pass_time ON gpu_ref_passes(time,id);
CREATE TABLE gpu_ref_active(pass INTEGER PRIMARY KEY,time INTEGER);
CREATE TABLE gpu_ref_packets(id INTEGER PRIMARY KEY,pass INTEGER,time INTEGER,thread INTEGER,setid INTEGER,resource INTEGER,mask INTEGER,count INTEGER,flags INTEGER,encoding INTEGER);
CREATE INDEX gpu_ref_packet_time ON gpu_ref_packets(time,id);
CREATE INDEX gpu_ref_packet_pass ON gpu_ref_packets(pass,time,id);
CREATE TABLE gpu_ref_ends(id INTEGER PRIMARY KEY,pass INTEGER,time INTEGER,command_list INTEGER,thread INTEGER,total INTEGER,dropped INTEGER,flags INTEGER);
CREATE INDEX gpu_ref_end_time ON gpu_ref_ends(time,id);
CREATE TABLE catalog_state(present INTEGER,valid INTEGER);
INSERT INTO catalog_state VALUES(0,0);
CREATE TABLE catalog_generations(id INTEGER PRIMARY KEY,event INTEGER,begin_value INTEGER,end_value INTEGER DEFAULT 0,begin INTEGER,end INTEGER DEFAULT 0,sequence INTEGER,batches INTEGER DEFAULT 0,records INTEGER DEFAULT 0,bytes INTEGER DEFAULT 0,unresolved INTEGER DEFAULT 0,state INTEGER,flags INTEGER,ended INTEGER DEFAULT 0,valid INTEGER);
CREATE INDEX catalog_generation_event ON catalog_generations(event);
CREATE TABLE catalog_controls(id INTEGER PRIMARY KEY,generation INTEGER,time INTEGER,value INTEGER,thread INTEGER,sequence INTEGER,kind INTEGER,state INTEGER,flags INTEGER);
CREATE TABLE catalog_pending(id INTEGER PRIMARY KEY,data BLOB);
CREATE TABLE catalog_batches(id INTEGER PRIMARY KEY,generation INTEGER,transport INTEGER,stored INTEGER DEFAULT 0,first_record INTEGER,sequence INTEGER,records INTEGER,bytes INTEGER,kind INTEGER,encoding INTEGER,flags INTEGER,valid INTEGER DEFAULT 1);
CREATE TABLE catalog_kind_counts(kind INTEGER PRIMARY KEY,count INTEGER);
CREATE TABLE catalog_records(id INTEGER PRIMARY KEY,batch INTEGER,generation INTEGER,kind INTEGER,ordinal INTEGER,object INTEGER,time INTEGER,data BLOB);
CREATE INDEX catalog_record_kind ON catalog_records(kind,id);
CREATE INDEX catalog_record_generation ON catalog_records(generation,kind,id);
CREATE INDEX catalog_record_object ON catalog_records(generation,kind,object,id);
CREATE INDEX catalog_record_batch ON catalog_records(batch,ordinal);
CREATE TABLE catalog_intervals(id INTEGER PRIMARY KEY,generation INTEGER,token INTEGER,resource INTEGER,begin INTEGER,end INTEGER);
CREATE INDEX catalog_interval_lookup ON catalog_intervals(generation,token,begin,end);
CREATE TABLE catalog_active(generation INTEGER,token INTEGER,id INTEGER,PRIMARY KEY(generation,token)) WITHOUT ROWID;
CREATE TABLE catalog_lifetimes(id INTEGER PRIMARY KEY,generation INTEGER,resource INTEGER,begin INTEGER,end INTEGER);
CREATE VIRTUAL TABLE catalog_lifetime_range USING rtree_i32(id,mint,maxt);
CREATE TABLE catalog_live_resources(generation INTEGER,resource INTEGER,id INTEGER,PRIMARY KEY(generation,resource)) WITHOUT ROWID;
CREATE TABLE catalog_pass_times(pass INTEGER PRIMARY KEY,time INTEGER,priority INTEGER);
CREATE TABLE catalog_descriptor_heaps(token INTEGER PRIMARY KEY,id INTEGER);
CREATE TABLE catalog_dirty(generation INTEGER PRIMARY KEY);
CREATE TABLE catalog_dirty_batches(id INTEGER PRIMARY KEY);
CREATE TABLE catalog_unresolved(id INTEGER PRIMARY KEY,generation INTEGER);
CREATE INDEX catalog_unresolved_generation ON catalog_unresolved(generation,id);
)SQL" );
}
#include "TracyEventStoreBlocks.inl"
#include "TracyEventStoreBlockRead.inl"
#include "TracyEventStoreBlockStream.inl"
struct AwaitRecordedQuery : std::runtime_error
{
    AwaitRecordedQuery() : std::runtime_error( "Definition response awaits its recorded query" ) {}
};
// The LZ4 frames of a journal's client records, decoded in record order: a
// frame may refer to the last 64 KiB decoded before it.
struct FrameDecoder
{
    std::vector<char> dictionary = std::vector<char>( 65536 );
    size_t dictionarySize = 0;
    // Whether Ingest::Consume decodes the payload `p` of record `r` as a frame.
    static bool Carries( const RecordInfo& r, std::span<const uint8_t> p )
    {
        if( r.type != RecordType::ClientToServer || ( r.flags & RecordFlagHandshake ) || !( r.flags & RecordFlagCompressedFrame ) || p.size() < 4 ) return false;
        uint32_t size;
        memcpy( &size, p.data(), 4 );
        return size && size <= LZ4Size && p.size() == size + 4;
    }
    // Decodes a payload that Carries() a frame into `out`, which holds
    // TargetFrameSize bytes; returns the decoded size, 0 if it does not decode.
    size_t Decode( std::span<const uint8_t> p, std::vector<char>& out )
    {
        uint32_t size;
        memcpy( &size, p.data(), 4 );
        const int n = LZ4_decompress_safe_usingDict( reinterpret_cast<const char*>( p.data() + 4 ), out.data(), int( size ), TargetFrameSize, dictionary.data(), int( dictionarySize ) );
        if( n <= 0 ) return 0;
        if( size_t( n ) >= dictionary.size() )
        {
            memcpy( dictionary.data(), out.data() + n - dictionary.size(), dictionary.size() );
            dictionarySize = dictionary.size();
        }
        else
        {
            size_t retain = std::min( dictionarySize, dictionary.size() - size_t( n ) );
            memmove( dictionary.data(), dictionary.data() + dictionarySize - retain, retain );
            memcpy( dictionary.data() + retain, out.data(), n );
            dictionarySize = retain + n;
        }
        return size_t( n );
    }
};
class Ingest
{
    Db& db;
    StoreInfo& info;
    const StoreOptions& options;
    uint64_t thread = 0, pendingSource = 0, pendingStack = 0, serialStack = 0, symbolFrame = 0;
    uint64_t memoryName = 0;
    uint64_t customFrameIndex = 0;
    uint64_t catalogDescriptorId = 1;
    uint64_t baseFrameCount = 0, frameOffset = 0;
    uint32_t symbolOrdinal = 0, symbolRemaining = 0;
    I reference = 0, serialReference = 0, gpuReference = 0, contextReference = 0, base = 0;
    double multiplier = 0;
    bool welcomed = false, onDemand = false, combineSamples = false, draining = false, hasSingle = false, hasSecond = false;
    bool ignoreMemoryFaults = false;
    bool incremental = false;
    bool zoneRangeLoaded = false;
    // Write-back mirror of `threads` rows. Every row read or written during
    // ingestion goes through this cache; Finish() persists dirty rows before
    // any commit. The innermost open CPU zones of a logical thread are kept in
    // a fixed window that mirrors the persisted parent chain, so their depth,
    // start and validation are not re-read for every nested event. The cache
    // is a plain value member: live checkpoints copy and restore it with the
    // rest of the decoder state.
    // An open CPU zone. Values that change while the zone is open (child
    // count, child time, name, color and the last direct child) stay here
    // until End(), which writes them into the zone's block row, or into
    // `zone_late` when that row was already flushed to an event block.
    // childTime sums the durations of the closed direct children.
    struct ZoneFrame
    {
        uint64_t id = 0, parent = 0, validation = 0, generation = 0, lastChild = 0;
        I start = 0, depth = 0, lastChildEnd = 0, childTime = 0;
        uint32_t slot = 0, children = 0, color = 0;
        bool hasColor = false, hasName = false, lastChildClosed = false;
        std::string name;
        // Call tree node of the zone, and the union of its closed direct
        // children's intervals (covered length, end of the last covered part
        // and start of the last child) for its self time.
        uint64_t node = 0;
        I covered = 0, coverEnd = 0, lastChildStart = INT64_MIN;
        // Node of the last direct child and its source; a child of the same
        // source reuses it without a lookup. Not kept when the frame spills.
        uint64_t childSkey = 0, childNode = 0;
        uint8_t childSkind = 0;
        // The zone's own source, for error reports. Not kept when the frame
        // spills.
        uint64_t skey = 0;
        uint8_t skind = 0;
    };
    static constexpr uint32_t ZoneWindow = 64;
    static constexpr size_t ThreadCacheLimit = 1024;
    struct ThreadState
    {
        uint64_t id = 0, top = 0, validation = 0, nextstack = 0, fiber = 0, used = 0;
        // Call tree node of the thread's last root zone and its source.
        uint64_t rootSkey = 0, rootNode = 0;
        uint8_t rootSkind = 0;
        bool present = false, dirty = false;
        uint32_t head = 0, frames = 0;
        std::array<ZoneFrame, ZoneWindow> window;
    };
    std::vector<ThreadState> threadCache;
    std::unordered_map<uint64_t, uint32_t> threadIndex;
    uint64_t threadClock = 0;
    size_t lastThread = SIZE_MAX;
    // CPU zones and zone texts of each logical thread are buffered and written
    // as compressed blocks. Buffered rows are patched in place; a zone that
    // ends after its row was flushed records its final values in `zone_late`.
    // Zone texts arriving out of (zone,event) order go to `zone_late_text`.
    static constexpr size_t BlockZones = 4096, TextBlockBytes = 128 << 10, TextBlockRows = 1024, BufferedZoneLimit = 1 << 18, BufferedTextLimit = 16 << 20;
    struct ZoneBuffer
    {
        std::vector<ZoneRow> rows;
        size_t count = 0;
        uint64_t generation = 0, used = 0;
        std::vector<TextRow> texts;
        size_t textBytes = 0;
        uint64_t textZone = 0, textEvent = 0;
    };
    BlockFile* blockFile = nullptr;
    std::unordered_map<uint64_t, ZoneBuffer> zoneBuffers;
    size_t bufferedZones = 0, bufferedTextBytes = 0;
    uint64_t zoneGeneration = 0, bufferClock = 0;
    // Event block of each flushed generation that still had open zones.
    std::unordered_map<uint64_t, uint64_t> generationBlock;
    // CPU zones per (kind,key) source, for the unresolved-definition total.
    std::unordered_map<std::pair<uint8_t, uint64_t>, uint64_t, SourceKeyHash> zoneSources;
    // Whole-capture CPU call tree: one node per (parent node, logical thread,
    // source), as global analysis groups zones, with the totals of the zones
    // it holds. It is bounded by the distinct call paths. Self time subtracts
    // the streaming union of the closed children's intervals, which is exact
    // while children start in order inside their parent and end inside it;
    // otherwise, or past ZoneTreeLimit nodes, the tree is dropped and
    // analysis reads the zones themselves.
    struct ZoneNodeKey
    {
        uint64_t parent = 0, thread = 0, skey = 0;
        uint8_t skind = 0;
        bool operator==( const ZoneNodeKey& ) const = default;
    };
    struct ZoneNodeKeyHash
    {
        size_t operator()( const ZoneNodeKey& k ) const
        {
            uint64_t x = k.parent * 0x9E3779B97F4A7C15ull ^ k.thread * 0xC2B2AE3D27D4EB4Full ^ k.skey ^ ( uint64_t( k.skind ) << 56 );
            x ^= x >> 33;
            x *= 0xff51afd7ed558ccdull;
            x ^= x >> 33;
            return size_t( x );
        }
    };
    struct ZoneNode
    {
        ZoneNodeKey key;
        uint64_t example = 0, events = 0, closed = 0;
        I inclusive = 0, self = 0, minimum = 0, maximum = 0;
        bool dirty = true;
    };
    static constexpr size_t ZoneTreeLimit = 1 << 18;
    std::unordered_map<ZoneNodeKey, uint64_t, ZoneNodeKeyHash> zoneNodeIndex;
    std::vector<ZoneNode> zoneNodes;
    bool zoneTreeExact = true, zoneTreeDropped = false;
    void DropZoneTree()
    {
        zoneTreeExact = false;
        zoneNodeIndex = {};
        zoneNodes = {};
    }
    uint64_t TreeNode( uint64_t parent, uint64_t thread, uint8_t kind, uint64_t key, uint64_t event )
    {
        if( !zoneTreeExact ) return 0;
        const ZoneNodeKey k{ parent, thread, key, kind };
        const auto [found, added] = zoneNodeIndex.try_emplace( k, uint64_t( zoneNodes.size() ) + 1 );
        if( added )
        {
            if( zoneNodes.size() >= ZoneTreeLimit )
            {
                DropZoneTree();
                return 0;
            }
            zoneNodes.push_back( { k, event } );
        }
        auto& n = zoneNodes[found->second - 1];
        ++n.events;
        n.dirty = true;
        return found->second;
    }
    uint64_t ChildNode( uint8_t& cachedKind, uint64_t& cachedKey, uint64_t& cachedNode, uint64_t parent, uint64_t thread, uint8_t kind, uint64_t key, uint64_t event )
    {
        if( !zoneTreeExact ) return 0;
        if( cachedNode && cachedKey == key && cachedKind == kind )
        {
            auto& n = zoneNodes[cachedNode - 1];
            ++n.events;
            n.dirty = true;
            return cachedNode;
        }
        const auto node = TreeNode( parent, thread, kind, key, event );
        cachedKind = kind;
        cachedKey = key;
        cachedNode = node;
        return node;
    }
    void CloseTreeNode( const ZoneFrame& f, I end )
    {
        if( !zoneTreeExact || !f.node ) return;
        if( f.coverEnd > end )
        {
            DropZoneTree();
            return;
        }
        auto& n = zoneNodes[f.node - 1];
        const I duration = end - f.start;
        n.minimum = n.closed ? std::min( n.minimum, duration ) : duration;
        n.maximum = n.closed ? std::max( n.maximum, duration ) : duration;
        ++n.closed;
        n.inclusive = Add( n.inclusive, duration );
        n.self = Add( n.self, std::max<I>( 0, duration - f.covered ) );
        n.dirty = true;
    }
    void FlushZoneTree()
    {
        info.zoneTree = zoneTreeExact;
        info.zoneTreeNodes = zoneTreeExact ? zoneNodes.size() : 0;
        if( !zoneTreeExact )
        {
            if( !zoneTreeDropped ) db.Exec( "DELETE FROM zone_tree" );
            zoneTreeDropped = true;
            return;
        }
        for( size_t n = 0; n < zoneNodes.size(); ++n )
        {
            auto& z = zoneNodes[n];
            if( !z.dirty ) continue;
            auto s = db.PrepStatic( "INSERT OR REPLACE INTO zone_tree VALUES(?,?,?,?,?,?,?,?,?,?,?,?)" );
            Int( s, 1, n + 1 );
            Int( s, 2, z.key.parent );
            Int( s, 3, z.key.thread );
            Int( s, 4, z.key.skind );
            Int( s, 5, z.key.skey );
            Int( s, 6, z.example );
            Int( s, 7, z.events );
            Int( s, 8, z.closed );
            Int( s, 9, uint64_t( z.inclusive ) );
            Int( s, 10, uint64_t( z.self ) );
            if( z.closed )
            {
                Int( s, 11, uint64_t( z.minimum ) );
                Int( s, 12, uint64_t( z.maximum ) );
            }
            db.Run( s );
            z.dirty = false;
        }
    }
    // Raw JN packets are buffered per queue type and written as packet blocks.
    // They serve the snapshot export, Job stages and auxiliary records.
    static constexpr size_t PacketBlockRows = 4096;
    static constexpr size_t PacketTypes = size_t( QueueType::JnGpuCatalogBatch ) - size_t( QueueType::JnJobType ) + 1;
    struct PacketBuffer
    {
        std::vector<PacketRow> rows;
        std::vector<uint8_t> payloads;
    };
    std::array<PacketBuffer, PacketTypes> packetBuffers;
    // Event time of the packet types whose readers filter or report by time.
    I PacketTime( QueueType t, const QueueItem& e )
    {
        switch( t )
        {
        case QueueType::JnFrame: return Time( e.jnFrame.time );
        case QueueType::JnGfxDispatch:
        case QueueType::JnGfxEntity:
        case QueueType::JnGfxLink:
        case QueueType::JnRelation:
        case QueueType::JnRuntimeDomainState: return Time( e.jnGfxDispatch.time );
        case QueueType::JnJobStage: return Time( e.jnJobStage.time );
        default: return 0;
        }
    }
    void AppendPacket( QueueType type, uint64_t id, const QueueItem& e )
    {
        auto& b = packetBuffers[size_t( type ) - size_t( QueueType::JnJobType )];
        b.rows.push_back( { id, thread, serialStack, PacketTime( type, e ) } );
        const auto bytes = reinterpret_cast<const uint8_t*>( &e ) + 1;
        b.payloads.insert( b.payloads.end(), bytes, bytes + ( QueueDataSize[uint8_t( type )] - 1 ) );
        if( b.rows.size() == PacketBlockRows ) FlushPackets( type, b );
    }
    void FlushPackets( QueueType type, PacketBuffer& b )
    {
        if( b.rows.empty() ) return;
        const auto raw = EncodePackets( b.rows, b.payloads, size_t( QueueDataSize[uint8_t( type )] - 1 ) );
        const auto stored = Compress( raw );
        const auto offset = blockFile->Append( stored );
        I tmin = INT64_MAX, tmax = INT64_MIN;
        for( const auto& r : b.rows )
        {
            tmin = std::min( tmin, r.time );
            tmax = std::max( tmax, r.time );
        }
        auto s = db.Prep( "INSERT INTO blocks(stream,key,first,last,count,tmin,tmax,tclosed,unfinished,depth,offset,size,raw,checksum,summary) VALUES(?,?,?,?,?,?,?,?,0,0,?,?,?,?,NULL)" );
        Int( s, 1, uint64_t( BlockStream::Packets ) );
        Int( s, 2, uint8_t( type ) );
        Int( s, 3, b.rows.front().id );
        Int( s, 4, b.rows.back().id );
        Int( s, 5, b.rows.size() );
        Int( s, 6, uint64_t( tmin ) );
        Int( s, 7, uint64_t( tmax ) );
        Int( s, 8, uint64_t( tmax ) );
        Int( s, 9, offset );
        Int( s, 10, stored.size() );
        Int( s, 11, raw.size() );
        Int( s, 12, XXH3_64bits( stored.data(), stored.size() ) );
        db.Run( s );
        const auto block = uint64_t( sqlite3_last_insert_rowid( db.handle ) );
        s = db.Prep( "INSERT INTO block_range VALUES(?,?,?)" );
        Int( s, 1, block );
        Int( s, 2, uint64_t( Bucket( tmin ) ) );
        Int( s, 3, uint64_t( Bucket( tmax ) ) );
        db.Run( s );
        b.rows.clear();
        b.payloads.clear();
    }
    void FlushAllPackets()
    {
        for( size_t n = 0; n < PacketTypes; ++n ) FlushPackets( QueueType( size_t( QueueType::JnJobType ) + n ), packetBuffers[n] );
    }
    // An offline build ingests nothing after Finish(), so the flushed buffers
    // return their memory before the summaries and indexes are built.
    void ReleaseBuffers()
    {
        for( auto& [tid, z] : zoneBuffers )
        {
            z.rows = {};
            z.texts = {};
        }
        for( auto& b : packetBuffers ) b = {};
        jobStates = {};
        gpuRows = {};
        zoneNodeIndex = {};
        zoneNodes = {};
    }
    void StoreThread( const ThreadState& t )
    {
        auto s = db.Prep( "INSERT INTO threads(id,top,validation,nextstack,fiber) VALUES(?1,?2,?3,?4,?5) ON CONFLICT(id) DO UPDATE SET top=?2,validation=?3,nextstack=?4,fiber=?5" );
        Int( s, 1, t.id );
        Int( s, 2, t.top );
        Int( s, 3, t.validation );
        Int( s, 4, t.nextstack );
        Int( s, 5, t.fiber );
        db.Run( s );
    }
    void FlushThreads()
    {
        for( auto& t : threadCache )
            if( t.present && t.dirty )
            {
                StoreThread( t );
                t.dirty = false;
            }
    }
    // Returns the mirror of row `id`, loading it on a miss. The reference is
    // valid only until the next cache call.
    ThreadState& CachedThread( uint64_t id )
    {
        if( lastThread < threadCache.size() && threadCache[lastThread].id == id )
        {
            auto& t = threadCache[lastThread];
            t.used = ++threadClock;
            return t;
        }
        auto found = threadIndex.find( id );
        if( found != threadIndex.end() )
        {
            lastThread = found->second;
            auto& t = threadCache[lastThread];
            t.used = ++threadClock;
            return t;
        }
        ThreadState loaded;
        loaded.id = id;
        auto s = db.Prep( "SELECT top,validation,nextstack,fiber FROM threads WHERE id=?" );
        Int( s, 1, id );
        if( db.Step( s ) )
        {
            loaded.present = true;
            loaded.top = sqlite3_column_int64( s, 0 );
            loaded.validation = sqlite3_column_int64( s, 1 );
            loaded.nextstack = sqlite3_column_int64( s, 2 );
            loaded.fiber = sqlite3_column_int64( s, 3 );
        }
        sqlite3_reset( s );
        size_t slot = threadCache.size();
        if( slot == ThreadCacheLimit )
        {
            slot = 0;
            for( size_t n = 1; n < threadCache.size(); ++n )
                if( threadCache[n].used < threadCache[slot].used ) slot = n;
            auto& victim = threadCache[slot];
            if( victim.present && victim.dirty ) StoreThread( victim );
            SpillFrames( victim );
            threadIndex.erase( victim.id );
            victim = loaded;
        }
        else threadCache.push_back( loaded );
        threadIndex[id] = uint32_t( slot );
        lastThread = slot;
        auto& t = threadCache[slot];
        t.used = ++threadClock;
        return t;
    }
    // Equivalent of INSERT OR IGNORE INTO threads(id): the row exists afterwards.
    ThreadState& EnsureThread( uint64_t id )
    {
        auto& t = CachedThread( id );
        if( !t.present )
        {
            t.present = true;
            t.dirty = true;
        }
        return t;
    }
    // Equivalent of a SELECT on `threads` guarded by Require(row).
    ThreadState& ExistingThread( uint64_t id, const char* missing )
    {
        auto& t = CachedThread( id );
        Require( t.present, missing );
        return t;
    }
    // The window holds the innermost open zones of a thread with the top at
    // `head`. Frames pushed out of a full window, and the whole window of a
    // thread leaving the cache, are kept in `open_frames` and reloaded when
    // they become the top again.
    void SpillFrame( uint64_t tid, const ZoneFrame& f )
    {
        auto s = db.Prep( "INSERT INTO open_frames VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19)" );
        Int( s, 1, f.id );
        Int( s, 2, tid );
        Int( s, 3, f.parent );
        Int( s, 4, f.validation );
        Int( s, 5, f.generation );
        Int( s, 6, uint64_t( f.start ) );
        Int( s, 7, uint64_t( f.depth ) );
        Int( s, 8, f.slot );
        Int( s, 9, f.children );
        Int( s, 10, f.color );
        Int( s, 11, ( f.hasColor ? 1 : 0 ) | ( f.hasName ? 2 : 0 ) | ( f.lastChildClosed ? 4 : 0 ) );
        Text( s, 12, f.name );
        Int( s, 13, f.lastChild );
        Int( s, 14, uint64_t( f.lastChildEnd ) );
        Int( s, 15, f.node );
        Int( s, 16, uint64_t( f.covered ) );
        Int( s, 17, uint64_t( f.coverEnd ) );
        Int( s, 18, uint64_t( f.lastChildStart ) );
        Int( s, 19, uint64_t( f.childTime ) );
        db.Run( s );
    }
    void SpillFrames( ThreadState& t )
    {
        for( uint32_t n = 0; n < t.frames; ++n ) SpillFrame( t.id, t.window[( t.head + ZoneWindow - n ) % ZoneWindow] );
        t.frames = 0;
    }
    void PushFrame( ThreadState& t, ZoneFrame frame )
    {
        if( t.frames == ZoneWindow )
        {
            SpillFrame( t.id, t.window[( t.head + 1 ) % ZoneWindow] );
            --t.frames;
        }
        t.head = ( t.head + 1 ) % ZoneWindow;
        t.window[t.head] = std::move( frame );
        ++t.frames;
    }
    void PopFrame( ThreadState& t, uint64_t id )
    {
        Require( t.frames && t.window[t.head].id == id, "CPU zone window out of order" );
        t.window[t.head] = ZoneFrame{};
        t.head = ( t.head + ZoneWindow - 1 ) % ZoneWindow;
        --t.frames;
    }
    // The frame of the thread's innermost open zone, reloaded from
    // `open_frames` when it is not in the window.
    ZoneFrame& TopFrame( ThreadState& t )
    {
        Require( t.present && t.top != 0, "CPU zone stack is empty" );
        if( t.frames )
        {
            Require( t.window[t.head].id == t.top, "CPU zone window out of order" );
            return t.window[t.head];
        }
        auto s = db.Prep( "SELECT parent,validation,generation,start,depth,slot,children,color,flags,name,last_child,last_child_end,node,covered,cover_end,last_child_start,child_time FROM open_frames WHERE id=?" );
        Int( s, 1, t.top );
        Require( db.Step( s ), "CPU zone stack frame is missing" );
        ZoneFrame f;
        f.id = t.top;
        f.parent = sqlite3_column_int64( s, 0 );
        f.validation = sqlite3_column_int64( s, 1 );
        f.generation = sqlite3_column_int64( s, 2 );
        f.start = sqlite3_column_int64( s, 3 );
        f.depth = sqlite3_column_int64( s, 4 );
        f.slot = uint32_t( sqlite3_column_int64( s, 5 ) );
        f.children = uint32_t( sqlite3_column_int64( s, 6 ) );
        f.color = uint32_t( sqlite3_column_int64( s, 7 ) );
        const auto flags = sqlite3_column_int( s, 8 );
        f.hasColor = flags & 1;
        f.hasName = flags & 2;
        f.lastChildClosed = flags & 4;
        f.name = Col( s, 9 );
        f.lastChild = sqlite3_column_int64( s, 10 );
        f.lastChildEnd = sqlite3_column_int64( s, 11 );
        f.node = sqlite3_column_int64( s, 12 );
        f.covered = sqlite3_column_int64( s, 13 );
        f.coverEnd = sqlite3_column_int64( s, 14 );
        f.lastChildStart = sqlite3_column_int64( s, 15 );
        f.childTime = sqlite3_column_int64( s, 16 );
        sqlite3_reset( s );
        s = db.Prep( "DELETE FROM open_frames WHERE id=?" );
        Int( s, 1, t.top );
        db.Run( s );
        PushFrame( t, std::move( f ) );
        return t.window[t.head];
    }
    void Stage( const char* name )
    {
        if( options.stage ) options.stage( name );
    }
    // Exact definitions of the indexes dropped by DeferIndexes().
    std::vector<std::string> deferredIndexes;

public:
    // Offline builds drop the deferred secondary indexes after Schema() and
    // recreate them in Finish() from the same sqlite_master text.
    void DeferIndexes()
    {
        Require( !incremental, "Live stores maintain every index" );
        for( const auto* name : DeferredIndexes )
        {
            auto q = db.Prep( "SELECT sql FROM sqlite_master WHERE type='index' AND name=?" );
            Text( q, 1, name );
            Require( db.Step( q ), "Missing deferred index" );
            deferredIndexes.push_back( Col( q, 0 ) );
            sqlite3_reset( q );
            db.Exec( std::string( "DROP INDEX " ) + name );
        }
    }

private:
#include "TracyEventStoreTranscript.inl"
    std::string single, second, image;
    std::vector<char> decoded = std::vector<char>( TargetFrameSize );
    FrameDecoder frames;
    I Time( I t )
    {
        Require( multiplier > 0 && base != INT64_MIN, "Event precedes valid welcome" );
        double n = double( Add( t, -base ) ) * multiplier;
        Require( std::isfinite( n ) && n >= double( INT64_MIN ) && n < double( INT64_MAX ), "Timestamp conversion overflow" );
        return I( n );
    }
    I Delta( I t )
    {
        reference = Add( reference, t );
        return Time( reference );
    }
    I SerialDelta( I t )
    {
        serialReference = Add( serialReference, t );
        return Time( serialReference );
    }
    I ContextDelta( I t, bool sample = false )
    {
        contextReference = Add( contextReference, t );
        const auto time = sample && contextReference == 0 ? 0 : Time( contextReference );
        info.lastNs = std::max( info.lastNs, time );
        return time;
    }
    uint64_t InternStack( std::span<const uint8_t> bytes )
    {
        Require( !bytes.empty() && bytes.size() % 8 == 0 && bytes.size() <= 65535 * 8, "Stack exceeds snapshot format limit" );
        auto s = db.Prep( "INSERT OR IGNORE INTO stacks(data) VALUES(?)" );
        Blob( s, 1, bytes );
        db.Run( s );
        s = db.Prep( "SELECT id FROM stacks WHERE data=?" );
        Blob( s, 1, bytes );
        Require( db.Step( s ), "Missing interned stack" );
        const uint64_t id = sqlite3_column_int64( s, 0 );
        sqlite3_reset( s );
        return id;
    }
    std::vector<uint8_t> StackBytes( uint64_t id )
    {
        auto s = db.Prep( "SELECT data FROM stacks WHERE id=?" );
        Int( s, 1, id );
        Require( db.Step( s ), "Missing sample stack" );
        const auto n = sqlite3_column_bytes( s, 0 );
        Require( n > 0 && n % 8 == 0 && n <= 65535 * 8, "Invalid sample stack size" );
        const auto p = static_cast<const uint8_t*>( sqlite3_column_blob( s, 0 ) );
        std::vector<uint8_t> bytes( p, p + n );
        sqlite3_reset( s );
        return bytes;
    }
    uint64_t MergeStack( uint64_t first, uint64_t second )
    {
        auto a = StackBytes( first ), b = StackBytes( second );
        Require( a.size() + b.size() <= 65535 * 8, "Merged sample stack exceeds snapshot field limit" );
        a.insert( a.end(), b.begin(), b.end() );
        return InternStack( a );
    }
    void InsertSample( uint64_t id, uint64_t thread, I time, uint64_t stack, bool explicitContext )
    {
        uint64_t existing = 0, oldStack = 0;
        if( time != 0 )
        {
            auto s = db.Prep( "SELECT id,stack FROM samples WHERE thread=? AND time=? ORDER BY id LIMIT 1" );
            Int( s, 1, thread );
            Int( s, 2, uint64_t( time ) );
            if( db.Step( s ) )
            {
                existing = sqlite3_column_int64( s, 0 );
                oldStack = sqlite3_column_int64( s, 1 );
            }
            sqlite3_reset( s );
        }
        if( existing )
        {
            const auto merged = MergeStack( oldStack, stack );
            auto s = db.Prep( "UPDATE samples SET stack=?,explicit_context=max(explicit_context,?) WHERE id=?" );
            Int( s, 1, merged );
            Int( s, 2, explicitContext ? 1 : 0 );
            Int( s, 3, existing );
            db.Run( s );
        }
        else
        {
            auto s = db.Prep( "INSERT INTO samples VALUES(?,?,?,?,?)" );
            Int( s, 1, id );
            Int( s, 2, thread );
            Int( s, 3, uint64_t( time ) );
            Int( s, 4, stack );
            Int( s, 5, explicitContext ? 1 : 0 );
            db.Run( s );
        }
    }
    void Sample( uint64_t id, const QueueCallstackSample& e, bool referenced, uint32_t dictionary, bool explicitContext )
    {
        uint64_t stack = pendingStack;
        if( referenced )
        {
            Require( pendingStack == 0, "Sample reference overlaps staged callstack" );
            auto s = db.Prep( "SELECT stack FROM sample_dictionary WHERE key=?" );
            Int( s, 1, dictionary );
            Require( db.Step( s ), "Unknown sample dictionary reference" );
            stack = sqlite3_column_int64( s, 0 );
            sqlite3_reset( s );
        }
        Require( stack != 0, "Missing sample payload" );
        pendingStack = 0;
        const auto time = ContextDelta( e.time, true );
        Thread( e.thread );
        if( time != 0 )
        {
            auto s = db.Prep( "INSERT OR IGNORE INTO sample_groups VALUES(?,?,?)" );
            Int( s, 1, e.thread );
            Int( s, 2, uint64_t( time ) );
            Int( s, 3, id );
            db.Run( s );
            s = db.Prep( "SELECT id FROM sample_groups WHERE thread=? AND time=?" );
            Int( s, 1, e.thread );
            Int( s, 2, uint64_t( time ) );
            Require( db.Step( s ), "Missing stable sample group" );
            id = sqlite3_column_int64( s, 0 );
            sqlite3_reset( s );
        }
        if( combineSamples && time != 0 && !explicitContext )
        {
            auto s = db.Prep( "SELECT id,time,stack FROM sample_pending WHERE thread=?" );
            Int( s, 1, e.thread );
            if( db.Step( s ) )
            {
                const uint64_t oldId = sqlite3_column_int64( s, 0 ), oldStack = sqlite3_column_int64( s, 2 );
                const I oldTime = sqlite3_column_int64( s, 1 );
                sqlite3_reset( s );
                if( oldTime == time )
                {
                    InsertSample( oldId, e.thread, time, MergeStack( oldStack, stack ), false );
                    s = db.Prep( "DELETE FROM sample_pending WHERE thread=?" );
                    Int( s, 1, e.thread );
                    db.Run( s );
                    return;
                }
                InsertSample( oldId, e.thread, oldTime, oldStack, false );
            }
            sqlite3_reset( s );
            s = db.Prep( "INSERT INTO sample_pending VALUES(?,?,?,?) ON CONFLICT(thread) DO UPDATE SET id=excluded.id,time=excluded.time,stack=excluded.stack" );
            Int( s, 1, e.thread );
            Int( s, 2, id );
            Int( s, 3, uint64_t( time ) );
            Int( s, 4, stack );
            db.Run( s );
        }
        else InsertSample( id, e.thread, time, stack, explicitContext );
    }
    uint64_t ContextTop( uint64_t thread )
    {
        auto s = db.Prep( "SELECT top FROM context_threads WHERE thread=?" );
        Int( s, 1, thread );
        const auto top = db.Step( s ) ? uint64_t( sqlite3_column_int64( s, 0 ) ) : 0;
        sqlite3_reset( s );
        return top;
    }
    void Wakeup( uint64_t id, const QueueThreadWakeup& e )
    {
        const auto time = ContextDelta( e.time );
        const auto top = ContextTop( e.thread );
        if( top )
        {
            auto s = db.Prep( "SELECT end FROM context_regions WHERE id=?" );
            Int( s, 1, top );
            Require( db.Step( s ), "Missing context region" );
            const bool running = sqlite3_column_type( s, 0 ) == SQLITE_NULL;
            sqlite3_reset( s );
            if( running )
            {
                s = db.Prep( "UPDATE context_threads SET pending_wake=?,pending_cpu=? WHERE thread=?" );
                Int( s, 1, uint64_t( time ) );
                Int( s, 2, e.cpu );
                Int( s, 3, e.thread );
                db.Run( s );
                return;
            }
        }
        auto s = db.Prep( "INSERT INTO context_regions VALUES(?,?,?,NULL,?,?,0,-2,-1,0)" );
        Int( s, 1, id );
        Int( s, 2, e.thread );
        Int( s, 3, uint64_t( time ) );
        Int( s, 4, uint64_t( time ) );
        Int( s, 5, e.cpu );
        db.Run( s );
        Range( "INSERT INTO context_range VALUES(?1,?2,?3)", id, time, {} );
        s = db.Prep( "INSERT INTO context_threads(thread,top) VALUES(?,?) ON CONFLICT(thread) DO UPDATE SET top=excluded.top" );
        Int( s, 1, e.thread );
        Int( s, 2, id );
        db.Run( s );
    }
    // Ends a thread's running region at a switch-out.
    void EndContextRegion( uint64_t region, uint64_t thread, I start, I time, int8_t reason, int8_t state )
    {
        auto s = db.Prep( "UPDATE context_regions SET end=?,reason=?,state=? WHERE id=?" );
        Int( s, 1, uint64_t( time ) );
        Int( s, 2, uint64_t( reason ) );
        Int( s, 3, uint64_t( state ) );
        Int( s, 4, region );
        db.Run( s );
        Range( "UPDATE context_range SET mint=?2,maxt=?3 WHERE id=?1", region, start, time );
        s = db.Prep( "UPDATE context_threads SET running=running+? WHERE thread=?" );
        Int( s, 1, uint64_t( Sub( time, start ) ) );
        Int( s, 2, thread );
        db.Run( s );
    }
    // Windows drops context switch events under load, and a capture scoped to
    // the game leaves out other programs' threads, so a switch may not
    // continue what was recorded for its thread or CPU. It is applied as
    // Tracy's viewer applies it, and counted: a switch-out ends the CPU's
    // running region whichever thread that showed, and a thread switched in
    // while it still shows running stops there first. A region is never ended
    // before it started or ended twice.
    void Switch( uint64_t id, const QueueContextSwitch& e )
    {
        const auto time = ContextDelta( e.time );
        bool gap = false;
        if( e.oldThread )
        {
            const auto top = ContextTop( e.oldThread );
            if( top )
            {
                auto s = db.Prep( "SELECT start,end FROM context_regions WHERE id=?" );
                Int( s, 1, top );
                Require( db.Step( s ), "Missing switched-out region" );
                const I start = sqlite3_column_int64( s, 0 );
                const bool running = sqlite3_column_type( s, 1 ) == SQLITE_NULL && start <= time;
                sqlite3_reset( s );
                if( running ) EndContextRegion( top, e.oldThread, start, time, int8_t( e.oldThreadWaitReason ), int8_t( e.oldThreadState ) );
                else gap = true;
            }
            auto s = db.Prep( "SELECT c.top,r.start,r.thread,r.end FROM context_cpus c JOIN cpu_regions r ON r.id=c.top WHERE c.cpu=?" );
            Int( s, 1, e.cpu );
            if( db.Step( s ) )
            {
                const uint64_t cpuTop = sqlite3_column_int64( s, 0 );
                const I start = sqlite3_column_int64( s, 1 );
                const bool owner = uint64_t( sqlite3_column_int64( s, 2 ) ) == e.oldThread;
                const bool running = sqlite3_column_type( s, 3 ) == SQLITE_NULL && start <= time;
                sqlite3_reset( s );
                gap = gap || !owner || !running;
                if( running )
                {
                    s = db.Prep( "UPDATE cpu_regions SET end=? WHERE id=?" );
                    Int( s, 1, uint64_t( time ) );
                    Int( s, 2, cpuTop );
                    db.Run( s );
                    Range( "UPDATE cpu_region_range SET mint=?2,maxt=?3 WHERE id=?1", cpuTop, start, time );
                }
            }
            sqlite3_reset( s );
        }
        if( !e.newThread )
        {
            if( gap ) ++info.contextSwitchGaps;
            return;
        }
        const auto top = ContextTop( e.newThread );
        uint64_t region = id;
        I wakeup = time;
        uint8_t wakeCpu = e.cpu;
        bool placeholder = false;
        if( top )
        {
            auto s = db.Prep( "SELECT r.reason,r.cpu,r.wakeup,t.pending_wake,t.pending_cpu,t.last_cpu,r.start,r.end FROM context_regions r JOIN context_threads t ON t.top=r.id WHERE t.thread=?" );
            Int( s, 1, e.newThread );
            Require( db.Step( s ), "Missing switch-in state" );
            placeholder = sqlite3_column_int( s, 0 ) == -2;
            const I previousWake = sqlite3_column_int64( s, 2 ), pending = sqlite3_column_int64( s, 3 ), start = sqlite3_column_int64( s, 6 );
            const uint8_t pendingCpu = uint8_t( sqlite3_column_int( s, 4 ) );
            const bool migration = sqlite3_column_type( s, 5 ) != SQLITE_NULL && sqlite3_column_int( s, 5 ) != e.cpu;
            const bool running = !placeholder && sqlite3_column_type( s, 7 ) == SQLITE_NULL;
            sqlite3_reset( s );
            if( placeholder )
            {
                region = top;
                wakeup = previousWake;
                wakeCpu = 0;
            }
            else if( pending != 0 && previousWake <= pending && pending <= time )
            {
                wakeup = pending;
                wakeCpu = pendingCpu;
            }
            if( running )
            {
                // Its switch-out was dropped.
                gap = true;
                if( start <= time ) EndContextRegion( top, e.newThread, start, time, -1, -1 );
            }
            if( migration )
            {
                s = db.Prep( "UPDATE context_threads SET migrations=migrations+1 WHERE thread=?" );
                Int( s, 1, e.newThread );
                db.Run( s );
            }
        }
        if( placeholder )
        {
            auto s = db.Prep( "UPDATE context_regions SET start=?,end=NULL,cpu=?,reason=-1,state=-1 WHERE id=?" );
            Int( s, 1, uint64_t( time ) );
            Int( s, 2, e.cpu );
            Int( s, 3, region );
            db.Run( s );
            Range( "UPDATE context_range SET mint=?2,maxt=?3 WHERE id=?1", region, time, {} );
        }
        else
        {
            auto s = db.Prep( "INSERT INTO context_regions VALUES(?,?,?,NULL,?,?,?,-1,-1,0)" );
            Int( s, 1, region );
            Int( s, 2, e.newThread );
            Int( s, 3, uint64_t( time ) );
            Int( s, 4, uint64_t( wakeup ) );
            Int( s, 5, wakeCpu );
            Int( s, 6, e.cpu );
            db.Run( s );
            Range( "INSERT INTO context_range VALUES(?1,?2,?3)", region, time, {} );
        }
        auto s = db.Prep( "INSERT INTO context_threads(thread,top,last_cpu) VALUES(?1,?2,?4) ON CONFLICT(thread) DO UPDATE SET top=excluded.top,last_cpu=excluded.last_cpu,pending_wake=CASE WHEN pending_wake=?3 THEN 0 ELSE pending_wake END" );
        Int( s, 1, e.newThread );
        Int( s, 2, region );
        Int( s, 3, uint64_t( wakeup ) );
        Int( s, 4, e.cpu );
        db.Run( s );
        s = db.Prep( "INSERT INTO cpu_regions VALUES(?,?,?,?,NULL)" );
        Int( s, 1, id );
        Int( s, 2, e.cpu );
        Int( s, 3, e.newThread );
        Int( s, 4, uint64_t( time ) );
        db.Run( s );
        Range( "INSERT INTO cpu_region_range VALUES(?1,?2,?3)", id, time, {} );
        s = db.Prep( "INSERT INTO context_cpus VALUES(?,?) ON CONFLICT(cpu) DO UPDATE SET top=excluded.top" );
        Int( s, 1, e.cpu );
        Int( s, 2, id );
        db.Run( s );
        if( gap ) ++info.contextSwitchGaps;
    }
    void Message( uint64_t id, const QueueItem& e, QueueType type )
    {
        const bool app = type == QueueType::MessageAppInfo;
        const bool literal = type == QueueType::MessageLiteral || type == QueueType::MessageLiteralColor || type == QueueType::MessageLiteralCallstack || type == QueueType::MessageLiteralColorCallstack;
        const bool color = type == QueueType::MessageColor || type == QueueType::MessageColorCallstack || type == QueueType::MessageLiteralColor || type == QueueType::MessageLiteralColorCallstack;
        const bool callstack = type == QueueType::MessageCallstack || type == QueueType::MessageColorCallstack || type == QueueType::MessageLiteralCallstack || type == QueueType::MessageLiteralColorCallstack;
        const auto tid = Logical();
        const auto time = Time( e.message.time );
        uint64_t stack = 0;
        if( callstack )
        {
            auto& t = ExistingThread( tid, "Missing message stack thread" );
            stack = t.nextstack;
            Require( stack != 0, "Missing message callstack" );
            t.nextstack = 0;
            t.dirty = true;
        }
        auto s = db.Prep( "INSERT INTO messages VALUES(?,?,?,?,?,?,?,?)" );
        Int( s, 1, id );
        Int( s, 2, tid );
        Int( s, 3, uint64_t( time ) );
        if( literal ) Int( s, 5, color ? e.messageColorLiteral.text : e.messageLiteral.text );
        else
        {
            const auto text = Take();
            Text( s, 4, text );
        }
        const uint32_t value = color ? 0xFF000000 | ( uint32_t( e.messageColor.b ) << 16 ) | ( uint32_t( e.messageColor.g ) << 8 ) | e.messageColor.r : 0xFFFFFFFF;
        Int( s, 6, value );
        Int( s, 7, stack );
        Int( s, 8, app ? 1 : 0 );
        db.Run( s );
        info.lastNs = std::max( info.lastNs, time );
    }
    static double Double( sqlite3_stmt* s, int column )
    {
        Require( sqlite3_column_bytes( s, column ) == 8, "Invalid stored scalar" );
        double value;
        memcpy( &value, sqlite3_column_blob( s, column ), 8 );
        return value;
    }
    void Scalar( sqlite3_stmt* s, int column, double value ) { Blob( s, column, { reinterpret_cast<const uint8_t*>( &value ), 8 } ); }
    // The plots receiving points: their `plot_keys` rows and point
    // statistics, which a point otherwise looked up and rewrote. The
    // statistics reach `plots` in FlushPlots(), which Finish() calls before
    // anything reads them. Plots are few; past PlotCacheLimit the caches are
    // written back and dropped.
    struct PlotStats
    {
        uint64_t points = 0;
        double min = 0, max = 0, sum = 0;
        bool dirty = false;
    };
    static constexpr size_t PlotCacheLimit = 4096;
    std::unordered_map<uint64_t, uint64_t> plotKeys;
    std::unordered_map<uint64_t, PlotStats> plotStats;
    uint64_t Plot( uint64_t key, uint64_t id )
    {
        const auto cached = plotKeys.find( key );
        if( cached != plotKeys.end() ) return cached->second;
        if( plotKeys.size() >= PlotCacheLimit ) plotKeys.clear();
        auto s = db.Prep( "SELECT plot FROM plot_keys WHERE key=?" );
        Int( s, 1, key );
        if( db.Step( s ) )
        {
            const uint64_t plot = sqlite3_column_int64( s, 0 );
            sqlite3_reset( s );
            plotKeys.emplace( key, plot );
            return plot;
        }
        sqlite3_reset( s );
        s = db.Prep( "INSERT INTO plots(id,sum) VALUES(?,?)" );
        Int( s, 1, id );
        Scalar( s, 2, 0 );
        db.Run( s );
        s = db.Prep( "INSERT INTO plot_keys VALUES(?,?)" );
        Int( s, 1, key );
        Int( s, 2, id );
        db.Run( s );
        plotKeys.emplace( key, id );
        return id;
    }
    void FlushPlots()
    {
        for( auto& [plot, stats] : plotStats )
        {
            if( !stats.dirty ) continue;
            auto s = db.Prep( "UPDATE plots SET points=?,min=?,max=?,sum=? WHERE id=?" );
            Int( s, 1, stats.points );
            Scalar( s, 2, stats.min );
            Scalar( s, 3, stats.max );
            Scalar( s, 4, stats.sum );
            Int( s, 5, plot );
            db.Run( s );
            stats.dirty = false;
        }
    }
    void PlotValue( uint64_t plot, double value )
    {
        auto found = plotStats.find( plot );
        if( found == plotStats.end() )
        {
            if( plotStats.size() >= PlotCacheLimit )
            {
                FlushPlots();
                plotStats.clear();
            }
            auto s = db.Prep( "SELECT points,min,max,sum FROM plots WHERE id=?" );
            Int( s, 1, plot );
            Require( db.Step( s ), "Missing plot stats" );
            PlotStats stats;
            stats.points = sqlite3_column_int64( s, 0 );
            stats.min = sqlite3_column_type(s,1)==SQLITE_BLOB?Double(s,1):sqlite3_column_double(s,1);
            stats.max = sqlite3_column_type(s,2)==SQLITE_BLOB?Double(s,2):sqlite3_column_double(s,2);
            stats.sum = Double( s, 3 );
            sqlite3_reset( s );
            found = plotStats.emplace( plot, stats ).first;
        }
        auto& stats = found->second;
        stats.min = stats.points ? std::min( stats.min, value ) : value;
        stats.max = stats.points ? std::max( stats.max, value ) : value;
        stats.sum = stats.points ? stats.sum + value : value;
        ++stats.points;
        stats.dirty = true;
    }
    void PlotPoint( uint64_t id, uint64_t key, I delta, double value )
    {
        if( !std::isfinite( value ) ) return;
        const auto time = Delta( delta );
        const auto plot = Plot( key, id );
        auto s = db.Prep( "INSERT INTO plot_points VALUES(?,?,?,?)" );
        Int( s, 1, id );
        Int( s, 2, plot );
        Int( s, 3, uint64_t( time ) );
        Scalar( s, 4, value );
        db.Run( s );
        PlotValue( plot, value );
        info.lastNs = std::max( info.lastNs, time );
    }
    void PlotName( uint64_t key, std::string_view name )
    {
        auto s = db.Prep( "SELECT plot FROM plot_keys WHERE key=?" );
        Int( s, 1, key );
        Require( db.Step( s ), "Plot name has no plot" );
        const uint64_t src = sqlite3_column_int64( s, 0 );
        sqlite3_reset( s );
        s = db.Prep( "SELECT id FROM plots WHERE name=? AND kind=0 AND id!=? ORDER BY id LIMIT 1" );
        Text( s, 1, name );
        Int( s, 2, src );
        const uint64_t dst = db.Step( s ) ? uint64_t( sqlite3_column_int64( s, 0 ) ) : 0;
        sqlite3_reset( s );
        if( dst )
        {
            auto points = db.Prep( "SELECT value FROM plot_points WHERE plot=? ORDER BY id" );
            Int( points, 1, src );
            while( db.Step( points ) ) PlotValue( dst, Double( points, 0 ) );
            sqlite3_reset( points );
            s = db.Prep( "UPDATE plot_points SET plot=? WHERE plot=?" );
            Int( s, 1, dst );
            Int( s, 2, src );
            db.Run( s );
            s = db.Prep( "UPDATE plot_keys SET plot=? WHERE plot=?" );
            Int( s, 1, dst );
            Int( s, 2, src );
            db.Run( s );
            for( auto& [cachedKey, plot] : plotKeys )
                if( plot == src ) plot = dst;
            s = db.Prep( "DELETE FROM plots WHERE id=?" );
            Int( s, 1, src );
            db.Run( s );
            plotStats.erase( src );
        }
        else
        {
            s = db.Prep( "UPDATE plots SET name=? WHERE id=?" );
            Text( s, 1, name );
            Int( s, 2, src );
            db.Run( s );
        }
    }
    void PlotConfig( uint64_t id, const QueuePlotConfig& e )
    {
        const auto plot = Plot( e.name, id );
        auto s = db.Prep( "UPDATE plots SET format=?,steps=?,fill=?,color=? WHERE id=?" );
        Int( s, 1, e.type );
        Int( s, 2, e.step );
        Int( s, 3, e.fill );
        Int( s, 4, e.color & 0xFFFFFF );
        Int( s, 5, plot );
        db.Run( s );
    }
    void LockAnnounce( const QueueLockAnnounce& e )
    {
        Require( e.type == LockType::Lockable || e.type == LockType::SharedLockable, "Invalid lock type" );
        auto s = db.Prep( "INSERT INTO locks(id,source,type,announce) VALUES(?,?,?,?)" );
        Int( s, 1, e.id );
        Int( s, 2, e.lckloc );
        Int( s, 3, uint8_t( e.type ) );
        Int( s, 4, uint64_t( Time( e.time ) ) );
        db.Run( s );
    }
    uint8_t LockThread( uint64_t lock, uint64_t thread )
    {
        Thread( thread );
        auto s = db.Prep( "SELECT ordinal FROM lock_threads WHERE lock=? AND thread=?" );
        Int( s, 1, lock );
        Int( s, 2, thread );
        if( db.Step( s ) )
        {
            const auto ordinal = uint8_t( sqlite3_column_int( s, 0 ) );
            sqlite3_reset( s );
            return ordinal;
        }
        sqlite3_reset( s );
        s = db.Prep( "SELECT COUNT(*) FROM lock_threads WHERE lock=?" );
        Int( s, 1, lock );
        db.Step( s );
        const auto count = sqlite3_column_int( s, 0 );
        sqlite3_reset( s );
        Require( count < 64, "Lock exceeds snapshot thread field limit" );
        s = db.Prep( "INSERT INTO lock_threads VALUES(?,?,?)" );
        Int( s, 1, lock );
        Int( s, 2, thread );
        Int( s, 3, count );
        db.Run( s );
        return uint8_t( count );
    }
    void LockEvent( uint64_t id, uint64_t lock, uint64_t thread, I delta, uint8_t type )
    {
        auto s = db.Prep( "SELECT type,locking_thread,owner,lock_count,wait,wait_shared,shared,contended,last_time FROM locks WHERE id=?" );
        Int( s, 1, lock );
        Require( db.Step( s ), "Lock event has no announce" );
        const bool shared = sqlite3_column_int( s, 0 ) == 1;
        uint64_t locking = sqlite3_column_int64( s, 1 ), owner = sqlite3_column_int64( s, 2 ), count = sqlite3_column_int64( s, 3 ), wait = sqlite3_column_int64( s, 4 ), waitShared = sqlite3_column_int64( s, 5 ), sharedList = sqlite3_column_int64( s, 6 );
        bool contended = sqlite3_column_int( s, 7 ) != 0;
        const bool hasLast = sqlite3_column_type( s, 8 ) != SQLITE_NULL;
        const I last = sqlite3_column_int64( s, 8 );
        sqlite3_reset( s );
        if( type == 2 ) thread = locking;
        Require( type < 3 || shared, "Shared event on exclusive lock" );
        const auto ordinal = LockThread( lock, thread );
        const uint64_t bit = uint64_t( 1 ) << ordinal;
        const auto time = SerialDelta( delta );
        Require( !hasLast || last <= time, "Lock timestamp moved backwards" );
        if( type == 0 ) wait |= bit;
        else if( type == 1 )
        {
            Require( count < 255 && ( wait & bit ), "Lock obtain has no wait or exceeds recursion limit" );
            wait &= ~bit;
            owner = thread;
            locking = thread;
            ++count;
        }
        else if( type == 2 )
        {
            Require( count > 0, "Lock release without ownership" );
            --count;
        }
        else if( type == 3 ) waitShared |= bit;
        else if( type == 4 )
        {
            Require( ( waitShared & bit ) && !( sharedList & bit ), "Shared lock obtain has no wait" );
            waitShared &= ~bit;
            sharedList |= bit;
        }
        else
        {
            Require( sharedList & bit, "Shared release without ownership" );
            sharedList &= ~bit;
        }
        contended = contended || ( count && ( wait || waitShared ) ) || ( sharedList && wait );
        s = db.Prep( "INSERT INTO lock_events(id,lock,thread,time,type,owner,count,wait,wait_shared,shared) VALUES(?,?,?,?,?,?,?,?,?,?)" );
        Int( s, 1, id );
        Int( s, 2, lock );
        Int( s, 3, thread );
        Int( s, 4, uint64_t( time ) );
        Int( s, 5, type );
        Int( s, 6, count ? owner : 0 );
        Int( s, 7, count );
        Int( s, 8, wait );
        Int( s, 9, waitShared );
        Int( s, 10, sharedList );
        db.Run( s );
        s = db.Prep( "UPDATE locks SET locking_thread=?,owner=?,lock_count=?,wait=?,wait_shared=?,shared=?,contended=?,events=events+1,last_time=? WHERE id=?" );
        Int( s, 1, locking );
        Int( s, 2, owner );
        Int( s, 3, count );
        Int( s, 4, wait );
        Int( s, 5, waitShared );
        Int( s, 6, sharedList );
        Int( s, 7, contended ? 1 : 0 );
        Int( s, 8, uint64_t( time ) );
        Int( s, 9, lock );
        db.Run( s );
        info.lastNs = std::max( info.lastNs, time );
    }
    void CombinedStack( std::span<const uint8_t> bytes )
    {
        Require( !bytes.empty() && bytes[0] <= 64, "Invalid managed callstack count" );
        const auto count = bytes[0];
        bytes = bytes.subspan( 1 );
        std::vector<uint8_t> stack;
        stack.reserve( size_t( count ) * 8 + 65536 );
        const auto take = [&]( auto& value ) { Require( bytes.size() >= sizeof( value ), "Truncated managed callstack" ); memcpy( &value, bytes.data(), sizeof( value ) ); bytes = bytes.subspan( sizeof( value ) ); };
        const auto string = [&]() { uint16_t n; take( n ); Require( bytes.size() >= n, "Truncated managed frame string" ); std::string text( reinterpret_cast<const char*>( bytes.data() ), n ); bytes = bytes.subspan( n ); return text; };
        for( uint8_t n = 0; n < count; ++n )
        {
            uint32_t line;
            take( line );
            const auto name = string(), file = string();
            auto q = db.Prep( "SELECT id FROM custom_frames WHERE name=? AND file=? AND line=?" );
            Text( q, 1, name );
            Text( q, 2, file );
            Int( q, 3, line );
            uint64_t frame;
            if( db.Step( q ) ) frame = sqlite3_column_int64( q, 0 );
            else
            {
                frame = customFrameIndex++;
                auto insert = db.Prep( "INSERT INTO custom_frames VALUES(?,?,?,?)" );
                Int( insert, 1, frame );
                Text( insert, 2, name );
                Text( insert, 3, file );
                Int( insert, 4, line );
                db.Run( insert );
                insert = db.Prep( "INSERT INTO stackframes VALUES(?,0,?,?,'',?,0,0)" );
                Int( insert, 1, ( 1ull << 62 ) | frame );
                Text( insert, 2, name );
                Text( insert, 3, file );
                Int( insert, 4, line );
                db.Run( insert );
            }
            sqlite3_reset( q );
            const auto key = ( 1ull << 62 ) | frame;
            const auto p = reinterpret_cast<const uint8_t*>( &key );
            stack.insert( stack.end(), p, p + 8 );
        }
        Require( bytes.empty(), "Trailing managed callstack data" );
        if( pendingStack )
        {
            auto q = db.Prep( "SELECT data FROM stacks WHERE id=?" );
            Int( q, 1, pendingStack );
            Require( db.Step( q ), "Missing pending native callstack" );
            const auto n = sqlite3_column_bytes( q, 0 );
            Require( n >= 0 && n % 8 == 0 && n <= 65536, "Native callstack exceeds combined stack budget" );
            const auto p = static_cast<const uint8_t*>( sqlite3_column_blob( q, 0 ) );
            if( n ) stack.insert( stack.end(), p, p + n );
            sqlite3_reset( q );
        }
        auto q = db.Prep( "INSERT OR IGNORE INTO stacks(data) VALUES(?)" );
        Blob( q, 1, stack );
        db.Run( q );
        q = db.Prep( "SELECT id FROM stacks WHERE data=?" );
        Blob( q, 1, stack );
        Require( db.Step( q ), "Missing combined callstack" );
        pendingStack = sqlite3_column_int64( q, 0 );
        sqlite3_reset( q );
    }
    static std::string AddressKey( uint64_t value )
    {
        std::string key( 16, '0' );
        constexpr const char* hex = "0123456789abcdef";
        for( int n = 15; n >= 0; --n )
        {
            key[n] = hex[value & 15];
            value >>= 4;
        }
        return key;
    }
    // The statistics of the memory pools in use, which every allocation and
    // free otherwise read and rewrote. FlushPools() writes them to
    // `memory_pools` in Finish(), before anything reads them. Pools are few;
    // past PoolCacheLimit the cache is written back and dropped.
    struct PoolStats
    {
        uint64_t allocations = 0, active = 0, usage = 0, frees = 0;
        std::optional<std::string> low, high;
        bool dirty = false;
    };
    static constexpr size_t PoolCacheLimit = 4096;
    std::unordered_map<uint64_t, PoolStats> poolStats;
    void FlushPools()
    {
        for( auto& [pool, stats] : poolStats )
        {
            if( !stats.dirty ) continue;
            auto s = db.Prep( "UPDATE memory_pools SET allocations=?,active=?,usage=?,frees=?,low=?,high=? WHERE id=?" );
            Int( s, 1, stats.allocations );
            Int( s, 2, stats.active );
            Blob( s, 3, { reinterpret_cast<const uint8_t*>( &stats.usage ), 8 } );
            Int( s, 4, stats.frees );
            if( stats.low ) Text( s, 5, *stats.low );
            else sqlite3_bind_null( s, 5 );
            if( stats.high ) Text( s, 6, *stats.high );
            else sqlite3_bind_null( s, 6 );
            Int( s, 7, pool );
            db.Run( s );
            stats.dirty = false;
        }
    }
    PoolStats& Pool( uint64_t pool )
    {
        const auto found = poolStats.find( pool );
        if( found != poolStats.end() ) return found->second;
        if( poolStats.size() >= PoolCacheLimit )
        {
            FlushPools();
            poolStats.clear();
        }
        auto s = db.Prep( "SELECT allocations,active,usage,frees,low,high FROM memory_pools WHERE id=?" );
        Int( s, 1, pool );
        Require( db.Step( s ), "Missing memory pool" );
        PoolStats stats;
        stats.allocations = sqlite3_column_int64( s, 0 );
        stats.active = sqlite3_column_int64( s, 1 );
        if( sqlite3_column_bytes( s, 2 ) != 8 )
        {
            sqlite3_reset( s );
            throw std::runtime_error( "Invalid memory usage field" );
        }
        memcpy( &stats.usage, sqlite3_column_blob( s, 2 ), 8 );
        stats.frees = sqlite3_column_int64( s, 3 );
        if( sqlite3_column_type( s, 4 ) != SQLITE_NULL ) stats.low = Col( s, 4 );
        if( sqlite3_column_type( s, 5 ) != SQLITE_NULL ) stats.high = Col( s, 5 );
        sqlite3_reset( s );
        return poolStats.emplace( pool, std::move( stats ) ).first->second;
    }
    uint64_t MemoryPool( uint64_t id, bool named )
    {
        Require( named ? memoryName != 0 : memoryName == 0, "Memory pool payload mismatch" );
        const auto pool = memoryName;
        memoryName = 0;
        // A cached pool has its row.
        if( poolStats.count( pool ) ) return pool;
        auto s = db.Prep( "INSERT OR IGNORE INTO memory_pools(id,event,usage) VALUES(?,?,zeroblob(8))" );
        Int( s, 1, pool );
        Int( s, 2, id );
        db.Run( s );
        return pool;
    }
    void MemoryAlloc( uint64_t id, const QueueMemAlloc& e, bool named, bool withStack, uint32_t callsite = 0 )
    {
        const auto pool = MemoryPool( id, named );
        auto s = db.Prep( "SELECT event FROM memory_active WHERE pool=? AND address=?" );
        Int( s, 1, pool );
        Int( s, 2, e.ptr );
        const bool duplicate = db.Step( s );
        sqlite3_reset( s );
        Require( !duplicate, "Allocation address is already active in this pool" );
        const auto time = SerialDelta( e.time );
        uint64_t size = 0, stack = 0;
        for( int n = 0; n < 6; ++n ) size |= uint64_t( uint8_t( e.size[n] ) ) << ( n * 8 );
        if( withStack )
        {
            Require( serialStack != 0, "Missing allocation callstack" );
            stack = serialStack;
            serialStack = 0;
        }
        else if( callsite )
        {
            s = db.Prep( "SELECT stack FROM callsites WHERE id=?" );
            Int( s, 1, callsite );
            if( db.Step( s ) ) stack = sqlite3_column_int64( s, 0 );
            sqlite3_reset( s );
        }
        s = db.Prep( "INSERT INTO allocations(id,pool,address,size,allocated,allocation_thread,allocation_stack,callsite) VALUES(?,?,?,?,?,?,?,?)" );
        Int( s, 1, id );
        Int( s, 2, pool );
        Int( s, 3, e.ptr );
        Int( s, 4, size );
        Int( s, 5, uint64_t( time ) );
        Int( s, 6, e.thread );
        Int( s, 7, stack );
        Int( s, 8, callsite );
        db.Run( s );
        s = db.Prep( "INSERT INTO memory_active VALUES(?,?,?)" );
        Int( s, 1, pool );
        Int( s, 2, e.ptr );
        Int( s, 3, id );
        db.Run( s );
        auto& stats = Pool( pool );
        ++stats.allocations;
        ++stats.active;
        stats.usage += size;
        // Address keys have a fixed width, so they compare as the addresses.
        auto low = AddressKey( e.ptr ), high = AddressKey( e.ptr + size );
        if( !stats.low || low < *stats.low ) stats.low = std::move( low );
        if( !stats.high || high > *stats.high ) stats.high = std::move( high );
        stats.dirty = true;
        Thread( e.thread );
        // Offline builds index allocations once in Finish().
        if( incremental ) Range( "INSERT INTO allocation_range VALUES(?1,?2,?3)", id, time, {} );
        info.lastNs = std::max( info.lastNs, time );
    }
    void ReleaseAllocation( uint64_t pool, uint64_t id, uint64_t size, I time, uint32_t tid, uint64_t stack )
    {
        auto& stats = Pool( pool );
        --stats.active;
        stats.usage -= size;
        ++stats.frees;
        stats.dirty = true;
        auto s = db.Prep( "UPDATE allocations SET freed=?,free_thread=?,free_stack=? WHERE id=? RETURNING allocated" );
        Int( s, 1, uint64_t( time ) );
        Int( s, 2, tid );
        Int( s, 3, stack );
        Int( s, 4, id );
        Require( db.Step( s ), "Missing allocation" );
        const auto begin = sqlite3_column_int64( s, 0 );
        Require( !db.Step( s ), "Unexpected SQL row" );
        Require( time >= begin, "Allocation free precedes allocation" );
        if( incremental ) Range( "UPDATE allocation_range SET mint=?2,maxt=?3 WHERE id=?1", id, begin, time );
        Thread( tid );
        info.lastNs = std::max( info.lastNs, time );
    }
    void MemoryFree( uint64_t event, const QueueMemFree& e, bool named, bool withStack )
    {
        const auto pool = MemoryPool( event, named );
        const auto time = SerialDelta( e.time );
        uint64_t stack = 0;
        if( withStack )
        {
            Require( serialStack != 0, "Missing free callstack" );
            stack = serialStack;
            serialStack = 0;
        }
        auto s = db.Prep( "SELECT a.id,a.size FROM memory_active m JOIN allocations a ON a.id=m.event WHERE m.pool=? AND m.address=?" );
        Int( s, 1, pool );
        Int( s, 2, e.ptr );
        if( !db.Step( s ) )
        {
            sqlite3_reset( s );
            Require( e.ptr == 0 || ignoreMemoryFaults, "Free address was not allocated in this pool" );
            return;
        }
        const auto id = uint64_t( sqlite3_column_int64( s, 0 ) ), size = uint64_t( sqlite3_column_int64( s, 1 ) );
        sqlite3_reset( s );
        ReleaseAllocation( pool, id, size, time, e.thread, stack );
        s = db.Prep( "DELETE FROM memory_active WHERE pool=? AND address=?" );
        Int( s, 1, pool );
        Int( s, 2, e.ptr );
        db.Run( s );
    }
    void MemoryDiscard( const QueueMemDiscard& e, bool withStack )
    {
        Require( memoryName == 0, "Unexpected discard memory payload" );
        uint64_t stack = 0;
        if( withStack )
        {
            Require( serialStack != 0, "Missing discard callstack" );
            stack = serialStack;
            serialStack = 0;
        }
        auto s = db.Prep( "SELECT id FROM memory_pools WHERE id=?" );
        Int( s, 1, e.name );
        const bool exists = db.Step( s );
        sqlite3_reset( s );
        if( !exists ) return; // Matches native serial-clock handling for an unknown pool.
        const auto time = SerialDelta( e.time );
        auto active = db.Prep( "SELECT a.id,a.size FROM memory_active m JOIN allocations a ON a.id=m.event WHERE m.pool=? ORDER BY a.id" );
        Int( active, 1, e.name );
        while( db.Step( active ) ) ReleaseAllocation( e.name, sqlite3_column_int64( active, 0 ), sqlite3_column_int64( active, 1 ), time, e.thread, stack );
        sqlite3_reset( active );
        s = db.Prep( "DELETE FROM memory_active WHERE pool=?" );
        Int( s, 1, e.name );
        db.Run( s );
        Thread( e.thread );
        info.lastNs = std::max( info.lastNs, time );
    }
#include "TracyEventStoreCatalog.inl"
    void ResourceDefinition( uint64_t set, std::span<const uint8_t> bytes )
    {
        Require( set && set <= UINT32_MAX && !bytes.empty() && bytes.size() % sizeof( JnGpuReferenceSetEntry ) == 0 && bytes.size() / sizeof( JnGpuReferenceSetEntry ) <= UINT16_MAX, "Invalid GPU resource set definition" );
        auto s = db.Prep( "INSERT INTO gpu_ref_sets VALUES(?,?)" );
        Int( s, 1, set );
        Int( s, 2, bytes.size() / sizeof( JnGpuReferenceSetEntry ) );
        db.Run( s );
        // A set has hundreds of entries; they go in statements of
        // EntryChunk rows, since executing a statement cost more than its row.
        constexpr int EntryChunk = 32;
        static const std::string chunk = [] {
            std::string sql = "INSERT INTO gpu_ref_entries VALUES(?,?,?,?)";
            for( int n = 1; n < EntryChunk; ++n ) sql += ",(?,?,?,?)";
            return sql;
        }();
        uint64_t ordinal = 0;
        while( !bytes.empty() )
        {
            const int rows = bytes.size() >= EntryChunk * sizeof( JnGpuReferenceSetEntry ) ? EntryChunk : 1;
            s = rows == EntryChunk ? db.PrepStatic( chunk.c_str() ) : db.Prep( "INSERT INTO gpu_ref_entries VALUES(?,?,?,?)" );
            for( int n = 0; n < rows; ++n )
            {
                JnGpuReferenceSetEntry e;
                memcpy( &e, bytes.data(), sizeof( e ) );
                bytes = bytes.subspan( sizeof( e ) );
                Require( e.resourceId != 0, "GPU resource set has a zero resource ID" );
                Int( s, 4 * n + 1, set );
                Int( s, 4 * n + 2, ordinal++ );
                Int( s, 4 * n + 3, e.resourceId );
                Int( s, 4 * n + 4, e.usageMask );
            }
            db.Run( s );
        }
    }
    void ResourcePass( uint64_t id, const QueueJnGpuReferencePass& e )
    {
        const auto time = Time( e.time );
        auto s = db.Prep( "INSERT INTO gpu_ref_passes VALUES(?,?,?,?,?,?,?,?)" );
        Int( s, 1, id );
        Int( s, 2, e.passId );
        Int( s, 3, uint64_t( time ) );
        Int( s, 4, e.frameIndex );
        Int( s, 5, thread );
        Int( s, 6, e.taxonomyId );
        Int( s, 7, e.taxonomyLevel );
        Int( s, 8, e.flags );
        db.Run( s );
        if( e.passId )
        {
            s = db.Prep( "INSERT INTO gpu_ref_active VALUES(?,?) ON CONFLICT(pass) DO UPDATE SET time=excluded.time" );
            Int( s, 1, e.passId );
            Int( s, 2, uint64_t( time ) );
            db.Run( s );
        }
        info.lastNs = std::max( info.lastNs, time );
        if( e.passId ) CatalogPassTime( e.passId, time, 0 );
    }
    void ResourceUse( uint64_t id, const QueueItem& e, bool setUse )
    {
        Require( id < ( 1ull << 47 ), "GPU resource cursor field overflow" );
        uint64_t pass = 0, resource = 0;
        uint32_t set = 0, mask = 0;
        uint16_t count = 1;
        uint8_t flags, encoding;
        I time;
        if( setUse )
        {
            const auto& use = e.jnGpuReferenceSetUse;
            pass = use.passId;
            set = use.resourceSetId;
            count = use.entryCount;
            flags = use.flags;
            encoding = use.encoding;
            auto s = db.Prep( "SELECT p.time,s.count FROM gpu_ref_active p JOIN gpu_ref_sets s ON s.id=?2 WHERE p.pass=?1" );
            Int( s, 1, pass );
            Int( s, 2, set );
            Require( db.Step( s ) && encoding == 2 && count == sqlite3_column_int64( s, 1 ), "GPU resource use has no matching active pass or set" );
            time = sqlite3_column_int64( s, 0 );
            sqlite3_reset( s );
        }
        else
        {
            const auto& use = e.jnGpuReferenceUse;
            pass = use.passId;
            resource = use.resourceId;
            mask = use.usageMask;
            time = Time( use.time );
            flags = use.flags;
            encoding = 1;
            info.lastNs = std::max( info.lastNs, time );
        }
        auto s = db.Prep( "INSERT INTO gpu_ref_packets VALUES(?,?,?,?,?,?,?,?,?,?)" );
        Int( s, 1, id );
        Int( s, 2, pass );
        Int( s, 3, uint64_t( time ) );
        Int( s, 4, thread );
        Int( s, 5, set );
        Int( s, 6, resource );
        Int( s, 7, mask );
        Int( s, 8, count );
        Int( s, 9, flags );
        Int( s, 10, encoding );
        db.Run( s );
    }
    void ResourceEnd( uint64_t id, const QueueJnGpuReferenceEnd& e )
    {
        const auto time = Time( e.time );
        auto s = db.Prep( "INSERT INTO gpu_ref_ends VALUES(?,?,?,?,?,?,?,?)" );
        Int( s, 1, id );
        Int( s, 2, e.passId );
        Int( s, 3, uint64_t( time ) );
        Int( s, 4, e.commandListId );
        Int( s, 5, thread );
        Int( s, 6, e.totalReferenceCount );
        Int( s, 7, e.droppedReferenceCount );
        Int( s, 8, e.flags );
        db.Run( s );
        s = db.Prep( "DELETE FROM gpu_ref_active WHERE pass=?" );
        Int( s, 1, e.passId );
        db.Run( s );
        info.lastNs = std::max( info.lastNs, time );
    }
    // Tasks whose row and dirty mark were inserted since the last Finish():
    // repeating EnsureTask's INSERT OR IGNOREs cannot change either table.
    struct KnownTask
    {
        uint64_t task = 0, generation = 0;
        bool io = false;
    };
    // First/last times of recently active tasks, merged into the row with the
    // same min/max on eviction and in Finish() before anything reads them.
    struct TaskTimes
    {
        uint64_t task = 0;
        I first = 0, last = 0;
        bool io = false, pending = false;
    };
    static constexpr size_t TaskSlots = 4096;
    std::vector<KnownTask> knownTasks;
    std::vector<TaskTimes> taskTimes;
    uint64_t taskGeneration = 1;
    static size_t TaskSlot( bool io, uint64_t task ) { return size_t( ( ( task * 2 + ( io ? 1 : 0 ) ) * 0x9E3779B97F4A7C15ull ) >> 52 ); }
    void EnsureTask( bool io, uint64_t task, uint64_t event )
    {
        if( knownTasks.empty() ) knownTasks.resize( TaskSlots );
        auto& known = knownTasks[TaskSlot( io, task )];
        if( known.generation == taskGeneration && known.task == task && known.io == io ) return;
        auto s = db.PrepStatic( io ? "INSERT OR IGNORE INTO io_requests(id,event) VALUES(?,?)" : "INSERT OR IGNORE INTO jobs(id,event) VALUES(?,?)" );
        Int( s, 1, task );
        Int( s, 2, event );
        db.Run( s );
        // An offline build finishes once with every task dirty, so only live
        // stores keep the dirty sets.
        if( incremental )
        {
            s = db.PrepStatic( io ? "INSERT OR IGNORE INTO dirty_io VALUES(?)" : "INSERT OR IGNORE INTO dirty_jobs VALUES(?)" );
            Int( s, 1, task );
            db.Run( s );
        }
        known = { task, taskGeneration, io };
    }
    void StoreTaskTime( TaskTimes& t )
    {
        if( !t.pending ) return;
        auto s = db.PrepStatic( t.io ? "UPDATE io_requests SET first_time=min(COALESCE(first_time,?1),?1),last_time=max(COALESCE(last_time,?2),?2) WHERE id=?3" : "UPDATE jobs SET first_time=min(COALESCE(first_time,?1),?1),last_time=max(COALESCE(last_time,?2),?2) WHERE id=?3" );
        Int( s, 1, uint64_t( t.first ) );
        Int( s, 2, uint64_t( t.last ) );
        Int( s, 3, t.task );
        db.Run( s );
        t.pending = false;
    }
    void FlushTaskTimes()
    {
        for( auto& t : taskTimes ) StoreTaskTime( t );
    }
    void TaskTime( bool io, uint64_t task, I time )
    {
        if( taskTimes.empty() ) taskTimes.resize( TaskSlots );
        auto& t = taskTimes[TaskSlot( io, task )];
        if( t.pending && t.task == task && t.io == io )
        {
            t.first = std::min( t.first, time );
            t.last = std::max( t.last, time );
        }
        else
        {
            StoreTaskTime( t );
            t = { task, time, time, io, true };
        }
        info.lastNs = std::max( info.lastNs, time );
    }
    // The Jobs seen recently, each with its complete `jobs` row and config
    // state. Schedule, config and stage packets update the row here exactly
    // as the per-packet statements would; the row is written with one upsert
    // when the Job leaves this bounded working set and by Finish(). A Job that
    // returns later is reloaded from its stored row. Open wait and execution
    // spans stay with their Job; a Job leaving with open spans stores them in
    // `job_spans`. The stack columns are resolved by Finish() alone.
    struct JobSpanStart
    {
        uint32_t span = 0;
        uint8_t kind = 0;
        I start = 0;
    };
    struct JobState
    {
        uint64_t event = 0, packed = 0, thread = 0, type = 0, count = 0, grain = 0, flow = 0, origin = 0, kind = 0, flags = 0, expected = 0;
        I schedule = 0;
        std::optional<I> ready, queue, firstRun, completed, firstTime, lastTime;
        uint64_t readyLane = 0, readyFlags = 0, queueLane = 0;
        std::array<I, 5> spanTotals{};
        uint64_t dispatches = 0, steals = 0, rangeSteals = 0, activeDispatches = 0, retries = 0, waitEnds = 0, continuations = 0;
        std::optional<uint64_t> stageFirst, stageLast;
        // Id of the Job's latest schedule, config, dependency or stage record;
        // with `event` it bounds every record that names the Job.
        uint64_t lastEvent = 0;
        uint64_t configKind = 0, configFlags = 0, used = 0, generation = 0;
        uint8_t schema = 1;
        bool orphan = true, boundary = false, cancelled = false, incomplete = false, hasConfig = false, configDirty = false, dirty = false;
        std::vector<uint64_t> lanes;
        std::vector<JobSpanStart> spans;
        void Raise( uint8_t value ) { schema = std::max( schema, value ); }
    };
    static constexpr size_t JobWorkingSet = 16384, JobIntervalLimit = 65536;
    std::unordered_map<uint64_t, JobState> jobStates;
    // Ids of the Jobs written so far as merged [first,last] intervals. A Job
    // outside them has no stored row, so its first packet needs no lookup.
    // Ids that never form runs stop the tracking, and every new Job is looked
    // up instead.
    std::map<uint64_t, uint64_t> storedJobs;
    bool storedJobsTracked = true;
    uint64_t jobClock = 0, spilledSpans = 0;
    // Callsites known to be defined; stages naming any other callsite are
    // recorded so that its later definition marks their Jobs dirty.
    std::unordered_set<uint64_t> knownCallsites;
    bool JobStored( uint64_t job ) const
    {
        if( !storedJobsTracked ) return true;
        auto next = storedJobs.upper_bound( job );
        return next != storedJobs.begin() && std::prev( next )->second >= job;
    }
    void NoteStoredJob( uint64_t job )
    {
        if( !storedJobsTracked ) return;
        auto next = storedJobs.upper_bound( job );
        if( next != storedJobs.begin() )
        {
            auto prev = std::prev( next );
            if( prev->second >= job ) return;
            if( prev->second + 1 == job )
            {
                prev->second = job;
                if( next != storedJobs.end() && next->first == job + 1 )
                {
                    prev->second = next->second;
                    storedJobs.erase( next );
                }
                return;
            }
        }
        if( next != storedJobs.end() && next->first == job + 1 )
        {
            const auto last = next->second;
            storedJobs.erase( next );
            storedJobs.emplace( job, last );
        }
        else storedJobs.emplace( job, job );
        if( storedJobs.size() > JobIntervalLimit )
        {
            storedJobs.clear();
            storedJobsTracked = false;
        }
    }
    bool LoadJob( uint64_t job, JobState& j )
    {
        auto s = db.PrepStatic( "SELECT event,packed,schedule,thread,type,count,grain,flow,origin,kind,flags,schema,expected,orphan,boundary,ready,ready_lane,ready_flags,queue,queue_lane,first_run,completed,execution,wait,help,spin,sleep,dispatches,steals,range_steals,active_dispatches,retries,wait_ends,continuations,cancelled,incomplete,first_time,last_time,stage_first,stage_last,last_event FROM jobs WHERE id=?" );
        Int( s, 1, job );
        if( !db.Step( s ) )
        {
            sqlite3_reset( s );
            return false;
        }
        const auto u = [&]( int n ) { return uint64_t( sqlite3_column_int64( s, n ) ); };
        const auto o = [&]( int n ) { return sqlite3_column_type( s, n ) == SQLITE_NULL ? std::optional<I>() : std::optional<I>( sqlite3_column_int64( s, n ) ); };
        j.event = u( 0 );
        j.packed = u( 1 );
        j.schedule = I( u( 2 ) );
        j.thread = u( 3 );
        j.type = u( 4 );
        j.count = u( 5 );
        j.grain = u( 6 );
        j.flow = u( 7 );
        j.origin = u( 8 );
        j.kind = u( 9 );
        j.flags = u( 10 );
        j.schema = uint8_t( u( 11 ) );
        j.expected = u( 12 );
        j.orphan = u( 13 ) != 0;
        j.boundary = u( 14 ) != 0;
        j.ready = o( 15 );
        j.readyLane = u( 16 );
        j.readyFlags = u( 17 );
        j.queue = o( 18 );
        j.queueLane = u( 19 );
        j.firstRun = o( 20 );
        j.completed = o( 21 );
        for( int n = 0; n < 5; ++n ) j.spanTotals[n] = I( u( 22 + n ) );
        j.dispatches = u( 27 );
        j.steals = u( 28 );
        j.rangeSteals = u( 29 );
        j.activeDispatches = u( 30 );
        j.retries = u( 31 );
        j.waitEnds = u( 32 );
        j.continuations = u( 33 );
        j.cancelled = u( 34 ) != 0;
        j.incomplete = u( 35 ) != 0;
        j.firstTime = o( 36 );
        j.lastTime = o( 37 );
        if( sqlite3_column_type( s, 38 ) != SQLITE_NULL ) j.stageFirst = u( 38 );
        if( sqlite3_column_type( s, 39 ) != SQLITE_NULL ) j.stageLast = u( 39 );
        j.lastEvent = u( 40 );
        sqlite3_reset( s );
        s = db.PrepStatic( "SELECT kind,flags FROM job_config_state WHERE job=?" );
        Int( s, 1, job );
        if( db.Step( s ) )
        {
            j.hasConfig = true;
            j.configKind = uint64_t( sqlite3_column_int64( s, 0 ) );
            j.configFlags = uint64_t( sqlite3_column_int64( s, 1 ) );
        }
        sqlite3_reset( s );
        return true;
    }
    // The Job named by a packet, created with the packet's event when the Job
    // is new. Live stores mark it dirty once per Finish().
    JobState& Job( uint64_t job, uint64_t event )
    {
        auto found = jobStates.find( job );
        if( found == jobStates.end() )
        {
            if( jobStates.size() >= JobWorkingSet ) EvictJobs();
            JobState j;
            if( !JobStored( job ) || !LoadJob( job, j ) ) j.event = event;
            found = jobStates.emplace( job, std::move( j ) ).first;
        }
        auto& j = found->second;
        j.used = ++jobClock;
        j.dirty = true;
        j.lastEvent = std::max( j.lastEvent, event );
        if( incremental && j.generation != taskGeneration )
        {
            auto s = db.PrepStatic( "INSERT OR IGNORE INTO dirty_jobs VALUES(?)" );
            Int( s, 1, job );
            db.Run( s );
            j.generation = taskGeneration;
        }
        return j;
    }
    void JobTime( JobState& j, I time )
    {
        j.firstTime = j.firstTime ? std::min( *j.firstTime, time ) : time;
        j.lastTime = j.lastTime ? std::max( *j.lastTime, time ) : time;
        info.lastNs = std::max( info.lastNs, time );
    }
    void StoreJob( uint64_t job, JobState& j )
    {
        if( !j.dirty ) return;
        auto s = db.PrepStatic( "INSERT INTO jobs(id,event,packed,schedule,thread,type,count,grain,flow,origin,kind,flags,schema,expected,orphan,boundary,ready,ready_lane,ready_flags,queue,queue_lane,first_run,completed,execution,wait,help,spin,sleep,dispatches,steals,range_steals,active_dispatches,retries,wait_ends,continuations,cancelled,incomplete,first_time,last_time,stage_first,stage_last,last_event) "
                                "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22,?23,?24,?25,?26,?27,?28,?29,?30,?31,?32,?33,?34,?35,?36,?37,?38,?39,?40,?41,?42) "
                                "ON CONFLICT(id) DO UPDATE SET event=excluded.event,packed=excluded.packed,schedule=excluded.schedule,thread=excluded.thread,type=excluded.type,count=excluded.count,grain=excluded.grain,flow=excluded.flow,origin=excluded.origin,kind=excluded.kind,flags=excluded.flags,schema=excluded.schema,expected=excluded.expected,orphan=excluded.orphan,boundary=excluded.boundary,"
                                "ready=excluded.ready,ready_lane=excluded.ready_lane,ready_flags=excluded.ready_flags,queue=excluded.queue,queue_lane=excluded.queue_lane,first_run=excluded.first_run,completed=excluded.completed,execution=excluded.execution,wait=excluded.wait,help=excluded.help,spin=excluded.spin,sleep=excluded.sleep,"
                                "dispatches=excluded.dispatches,steals=excluded.steals,range_steals=excluded.range_steals,active_dispatches=excluded.active_dispatches,retries=excluded.retries,wait_ends=excluded.wait_ends,continuations=excluded.continuations,cancelled=excluded.cancelled,incomplete=excluded.incomplete,first_time=excluded.first_time,last_time=excluded.last_time,stage_first=excluded.stage_first,stage_last=excluded.stage_last,last_event=excluded.last_event" );
        const auto opt = [&]( int n, const std::optional<I>& v ) {
            if( v ) Int( s, n, uint64_t( *v ) );
        };
        Int( s, 1, job );
        Int( s, 2, j.event );
        Int( s, 3, j.packed );
        Int( s, 4, uint64_t( j.schedule ) );
        Int( s, 5, j.thread );
        Int( s, 6, j.type );
        Int( s, 7, j.count );
        Int( s, 8, j.grain );
        Int( s, 9, j.flow );
        Int( s, 10, j.origin );
        Int( s, 11, j.kind );
        Int( s, 12, j.flags );
        Int( s, 13, j.schema );
        Int( s, 14, j.expected );
        Int( s, 15, j.orphan ? 1 : 0 );
        Int( s, 16, j.boundary ? 1 : 0 );
        opt( 17, j.ready );
        Int( s, 18, j.readyLane );
        Int( s, 19, j.readyFlags );
        opt( 20, j.queue );
        Int( s, 21, j.queueLane );
        opt( 22, j.firstRun );
        opt( 23, j.completed );
        for( int n = 0; n < 5; ++n ) Int( s, 24 + n, uint64_t( j.spanTotals[n] ) );
        Int( s, 29, j.dispatches );
        Int( s, 30, j.steals );
        Int( s, 31, j.rangeSteals );
        Int( s, 32, j.activeDispatches );
        Int( s, 33, j.retries );
        Int( s, 34, j.waitEnds );
        Int( s, 35, j.continuations );
        Int( s, 36, j.cancelled ? 1 : 0 );
        Int( s, 37, j.incomplete ? 1 : 0 );
        opt( 38, j.firstTime );
        opt( 39, j.lastTime );
        if( j.stageFirst ) Int( s, 40, *j.stageFirst );
        if( j.stageLast ) Int( s, 41, *j.stageLast );
        Int( s, 42, j.lastEvent );
        db.Run( s );
        if( j.configDirty )
        {
            auto q = db.PrepStatic( "INSERT INTO job_config_state VALUES(?,?,?) ON CONFLICT(job) DO UPDATE SET kind=excluded.kind,flags=excluded.flags" );
            Int( q, 1, job );
            Int( q, 2, j.configKind );
            Int( q, 3, j.configFlags );
            db.Run( q );
            j.configDirty = false;
        }
        for( const auto lane : j.lanes )
        {
            auto q = db.PrepStatic( "INSERT OR IGNORE INTO job_lanes VALUES(?,?)" );
            Int( q, 1, job );
            Int( q, 2, lane );
            db.Run( q );
        }
        j.lanes.clear();
        j.dirty = false;
        NoteStoredJob( job );
    }
    void FlushJobs()
    {
        for( auto& [job, j] : jobStates ) StoreJob( job, j );
    }
    // Writes and drops the least recently used half of the working set.
    void EvictJobs()
    {
        std::vector<std::pair<uint64_t, uint64_t>> order;
        order.reserve( jobStates.size() );
        for( const auto& [job, j] : jobStates ) order.emplace_back( j.used, job );
        const auto half = order.begin() + order.size() / 2;
        std::nth_element( order.begin(), half, order.end() );
        for( auto it = order.begin(); it != half; ++it )
        {
            auto found = jobStates.find( it->second );
            StoreJob( found->first, found->second );
            for( const auto& span : found->second.spans )
            {
                auto q = db.PrepStatic( "INSERT INTO job_spans VALUES(?,?,?,?) ON CONFLICT(job,span,kind) DO UPDATE SET start=excluded.start" );
                Int( q, 1, found->first );
                Int( q, 2, span.span );
                Int( q, 3, span.kind );
                Int( q, 4, uint64_t( span.start ) );
                db.Run( q );
                ++spilledSpans;
            }
            jobStates.erase( found );
        }
    }
    void OpenSpan( JobState& j, uint32_t span, uint8_t kind, I time )
    {
        for( auto& open : j.spans )
            if( open.span == span && open.kind == kind )
            {
                open.start = time;
                return;
            }
        j.spans.push_back( { span, kind, time } );
    }
    void CloseSpan( uint64_t job, JobState& j, uint32_t span, uint8_t kind, I time )
    {
        std::optional<I> start;
        const auto open = std::find_if( j.spans.begin(), j.spans.end(), [&]( const JobSpanStart& o ) { return o.span == span && o.kind == kind; } );
        if( open != j.spans.end() )
        {
            start = open->start;
            j.spans.erase( open );
        }
        else if( spilledSpans )
        {
            auto q = db.PrepStatic( "DELETE FROM job_spans WHERE job=? AND span=? AND kind=? RETURNING start" );
            Int( q, 1, job );
            Int( q, 2, span );
            Int( q, 3, kind );
            if( db.Step( q ) )
            {
                start = sqlite3_column_int64( q, 0 );
                --spilledSpans;
            }
            sqlite3_reset( q );
        }
        if( start && time >= *start ) j.spanTotals[kind] += Sub( time, *start );
    }
    bool CallsiteKnown( uint64_t callsite )
    {
        if( knownCallsites.count( callsite ) ) return true;
        auto q = db.PrepStatic( "SELECT 1 FROM callsites WHERE id=?" );
        Int( q, 1, callsite );
        const bool known = db.Step( q );
        sqlite3_reset( q );
        if( known ) knownCallsites.insert( callsite );
        return known;
    }
    void JobSchedule( uint64_t id, const QueueJnJobSchedule& q )
    {
        auto& j = Job( q.jobId, id );
        const auto time = Time( q.time );
        JobTime( j, time );
        const uint64_t configFlags = j.hasConfig ? j.configFlags : 0;
        const bool orphan = ( q.flags & 128 ) != 0 || ( configFlags & 128 ) != 0;
        j.packed = q.packedHandle;
        j.schedule = time;
        j.thread = thread;
        j.kind = j.hasConfig ? j.configKind : q.kind;
        j.flags = q.flags | configFlags;
        j.expected = q.dependencyCount;
        j.orphan = orphan;
        j.boundary = orphan;
        j.Raise( q.flags & 64 ? 3 : 1 );
    }
    void JobConfig( uint64_t id, const QueueJnJobConfig& q )
    {
        auto& j = Job( q.jobId, id );
        j.configFlags = j.hasConfig ? j.configFlags | q.flags : q.flags;
        j.configKind = q.kind;
        j.hasConfig = true;
        j.configDirty = true;
        if( q.typeId ) j.type = q.typeId;
        if( q.count || !j.count ) j.count = q.count;
        if( q.grainSize || !j.grain ) j.grain = q.grainSize;
        if( q.unityFlowId || !j.flow ) j.flow = q.unityFlowId;
        if( q.originFrameSequence ) j.origin = q.originFrameSequence;
        j.kind = q.kind;
        j.flags |= q.flags;
        if( q.flags & 128 )
        {
            j.boundary = true;
            j.orphan = true;
        }
        j.Raise( q.flags & 64 ? 3 : 1 );
    }
    // The stage row itself is the raw JnJobStage packet in its packet block.
    // Stages that resolve the Job stack are also kept in `job_schedule_stages`
    // for Finish(), and the Job's first and last stage ids locate its stages.
    void JobStage( uint64_t id, const QueueJnJobStage& e )
    {
        auto& j = Job( e.jobId, id );
        const auto time = Time( e.time );
        JobTime( j, time );
        if( !j.stageFirst ) j.stageFirst = id;
        j.stageLast = id;
        uint64_t stack = 0, callsite = 0;
        const auto stage = JnJobStage( e.stage );
        if( stage == JnJobStage::ScheduleCallstack || stage == JnJobStage::WaitCallstack )
        {
            Require( serialStack != 0, "Missing Job serial callstack" );
            stack = serialStack;
            serialStack = 0;
            Require( stack <= UINT32_MAX, "Job stack field overflow" );
        }
        if( stage == JnJobStage::ScheduleCallsite || stage == JnJobStage::WaitCallsite ) callsite = e.spanId;
        if( callsite && !CallsiteKnown( callsite ) )
        {
            auto q = db.PrepStatic( "INSERT OR IGNORE INTO job_stage_callsites VALUES(?,?)" );
            Int( q, 1, callsite );
            Int( q, 2, e.jobId );
            db.Run( q );
        }
        if( stage == JnJobStage::ScheduleCallstack || stage == JnJobStage::ScheduleCallsite )
        {
            auto q = db.PrepStatic( "INSERT INTO job_schedule_stages VALUES(?,?,?,?,?)" );
            Int( q, 1, id );
            Int( q, 2, e.jobId );
            Int( q, 3, e.stage );
            Int( q, 4, stack );
            Int( q, 5, callsite );
            db.Run( q );
        }
        switch( stage )
        {
        case JnJobStage::WorkerSliceBegin:
            OpenSpan( j, e.spanId, 0, time );
            j.firstRun = j.firstRun ? std::min( *j.firstRun, time ) : time;
            if( e.flags & 128 ) ++j.rangeSteals;
            break;
        case JnJobStage::WorkerSliceEnd: CloseSpan( e.jobId, j, e.spanId, 0, time ); break;
        case JnJobStage::WaitBegin: OpenSpan( j, e.spanId, 1, time ); break;
        case JnJobStage::WaitEnd:
            CloseSpan( e.jobId, j, e.spanId, 1, time );
            ++j.waitEnds;
            break;
        case JnJobStage::WaitActiveHelpBegin: OpenSpan( j, e.spanId, 2, time ); break;
        case JnJobStage::WaitActiveHelpEnd: CloseSpan( e.jobId, j, e.spanId, 2, time ); break;
        case JnJobStage::WaitSpinYieldBegin: OpenSpan( j, e.spanId, 3, time ); break;
        case JnJobStage::WaitSpinYieldEnd: CloseSpan( e.jobId, j, e.spanId, 3, time ); break;
        case JnJobStage::WaitSleepBegin: OpenSpan( j, e.spanId, 4, time ); break;
        case JnJobStage::WaitSleepEnd: CloseSpan( e.jobId, j, e.spanId, 4, time ); break;
        case JnJobStage::Completed: j.completed = time; break;
        case JnJobStage::Cancelled: j.cancelled = true; break;
        case JnJobStage::Incomplete: j.incomplete = true; break;
        case JnJobStage::Ready:
            if( !j.ready || *j.ready > time )
            {
                j.ready = time;
                j.readyLane = e.arg0;
                j.readyFlags = e.flags;
                j.Raise( 2 );
            }
            break;
        case JnJobStage::QueueEnter:
            if( !j.queue || *j.queue > time )
            {
                j.queue = time;
                j.queueLane = e.arg0;
                j.Raise( 2 );
            }
            if( e.flags & 64 ) ++j.retries;
            break;
        case JnJobStage::Dispatch:
            ++j.dispatches;
            j.Raise( 2 );
            if( e.flags & 2 ) ++j.activeDispatches;
            if( std::find( j.lanes.begin(), j.lanes.end(), uint64_t( e.arg0 ) ) == j.lanes.end() ) j.lanes.push_back( e.arg0 );
            break;
        case JnJobStage::Steal:
            ++j.steals;
            j.Raise( 2 );
            break;
        case JnJobStage::Continuation:
            ++j.continuations;
            j.Raise( 3 );
            break;
        // The Job stack and callsite are resolved by Finish() from
        // `job_schedule_stages` once the callsite definitions are known.
        case JnJobStage::ScheduleCallstack: break;
        case JnJobStage::ScheduleCallsite: j.Raise( 3 ); break;
        case JnJobStage::WaitCallstack: j.Raise( 2 ); break;
        case JnJobStage::WaitCallsite: j.Raise( 3 ); break;
        default: break;
        }
    }
    void IoStage( uint64_t id, const QueueJnIoStage& e )
    {
        EnsureTask( true, e.requestId, id );
        const auto time = Time( e.time );
        TaskTime( true, e.requestId, time );
        uint64_t tid = thread, stack = 0;
        uint32_t detail = e.detail;
        if( JnIoStage( e.stage ) == JnIoStage::RequestCallstack )
        {
            Require( serialStack != 0, "Missing IO serial callstack" );
            stack = serialStack;
            serialStack = 0;
            tid = e.detail;
            Require( stack <= UINT32_MAX, "IO stack field overflow" );
            detail = uint32_t( stack );
        }
        auto s = db.Prep( "INSERT INTO io_stages VALUES(?,?,?,?,?,?,?,?,?,?)" );
        Int( s, 1, id );
        Int( s, 2, e.requestId );
        Int( s, 3, uint64_t( time ) );
        Int( s, 4, tid );
        Int( s, 5, e.bytes );
        Int( s, 6, detail );
        Int( s, 7, e.stage );
        Int( s, 8, e.status );
        Int( s, 9, e.flags );
        Int( s, 10, stack );
        db.Run( s );
        if( JnIoStage( e.stage ) == JnIoStage::Start )
        {
            s = db.Prep( "UPDATE io_requests SET start=min(COALESCE(start,?1),?1) WHERE id=?2" );
            Int( s, 1, uint64_t( time ) );
            Int( s, 2, e.requestId );
            db.Run( s );
        }
        else if( e.stage >= uint8_t( JnIoStage::Complete ) && e.stage <= uint8_t( JnIoStage::Cancel ) )
        {
            s = db.Prep( "UPDATE io_requests SET end=max(COALESCE(end,?1),?1),transferred=?3,status=?4,terminals=terminals+1 WHERE id=?2" );
            Int( s, 1, uint64_t( time ) );
            Int( s, 2, e.requestId );
            Int( s, 3, e.bytes );
            Int( s, 4, e.status );
            db.Run( s );
        }
        else if( JnIoStage( e.stage ) == JnIoStage::Requeue )
        {
            s = db.Prep( "UPDATE io_requests SET status=? WHERE id=?" );
            Int( s, 1, e.status );
            Int( s, 2, e.requestId );
            db.Run( s );
        }
        else if( stack )
        {
            s = db.Prep( "UPDATE io_requests SET stack=? WHERE id=?" );
            Int( s, 1, stack );
            Int( s, 2, e.requestId );
            db.Run( s );
        }
        if( e.flags & 16 )
        {
            s = db.Prep( "UPDATE io_requests SET boundary=1 WHERE id=?" );
            Int( s, 1, e.requestId );
            db.Run( s );
        }
    }
    // GPU context state, the stacks of open GPU zones and the pending
    // timestamp queries change with every GPU event. They are kept here;
    // Finish() writes the context state back to `gpu_contexts`.
    struct GpuState
    {
        uint64_t thread = 0, overflow = 0, wraps = 0, zones = 0, incomplete = 0;
        double period = 1, scale = 1;
        I offset = 0, baseGpu = 0, baseCpu = 0, last = 0;
        bool calibrated = false, known = false, dirty = false;
    };
    std::array<GpuState, 256> gpuContexts;
    // Open GPU zones (id, depth) per (context, thread), innermost last.
    std::map<std::pair<uint8_t, uint64_t>, std::vector<std::pair<uint64_t, uint32_t>>> gpuStacks;
    // Pending timestamp queries: (context << 16 | query) to zone.
    std::unordered_map<uint32_t, uint64_t> gpuQueries;
    // A GPU zone row is written once its CPU end and both GPU timestamps are
    // known. Finish() writes the rows still waiting, which later completion
    // then updates; rows beyond the working set are written early the same way.
    struct GpuRow
    {
        uint64_t id = 0, thread = 0, parent = 0, skey = 0, stack = 0, callsite = 0;
        I cpuStart = 0, cpuEnd = 0, gpuStart = 0, gpuEnd = 0;
        uint32_t depth = 0;
        uint16_t query = 0;
        uint8_t context = 0, skind = 0;
        bool hasCpuEnd = false, hasGpuStart = false, hasGpuEnd = false, stored = false;
    };
    static constexpr size_t GpuRowLimit = 65536;
    std::unordered_map<uint64_t, GpuRow> gpuRows;
    GpuState& Gpu( uint8_t id )
    {
        auto& c = gpuContexts[id];
        if( c.known ) return c;
        auto s = db.Prep( "SELECT thread,period,calibrated,offset,base_gpu,base_cpu,scale,last_gpu,overflow,wraps,zones,incomplete FROM gpu_contexts WHERE id=?" );
        Int( s, 1, id );
        Require( db.Step( s ), "GPU event has no context" );
        c.thread = sqlite3_column_int64( s, 0 );
        c.period = sqlite3_column_double( s, 1 );
        c.calibrated = sqlite3_column_int( s, 2 ) != 0;
        c.offset = sqlite3_column_int64( s, 3 );
        c.baseGpu = sqlite3_column_int64( s, 4 );
        c.baseCpu = sqlite3_column_int64( s, 5 );
        c.scale = sqlite3_column_double( s, 6 );
        c.last = sqlite3_column_int64( s, 7 );
        c.overflow = sqlite3_column_int64( s, 8 );
        c.wraps = sqlite3_column_int64( s, 9 );
        c.zones = sqlite3_column_int64( s, 10 );
        c.incomplete = sqlite3_column_int64( s, 11 );
        sqlite3_reset( s );
        c.known = true;
        c.dirty = false;
        return c;
    }
    static I GpuInteger( double time )
    {
        Require( std::isfinite( time ) && time >= double( INT64_MIN ) && time < double( INT64_MAX ), "GPU timestamp overflow" );
        return I( time );
    }
    void GpuContext( const QueueGpuNewContext& e )
    {
        Require( e.type != GpuContextType::Invalid && e.type <= GpuContextType::Rocprof && std::isfinite( e.period ) && e.period > 0, "Invalid GPU context" );
        auto cpu = Time( e.cpuTime );
        auto gpu = e.period == 1.f ? e.gpuTime : GpuInteger( double( e.period ) * e.gpuTime );
        auto s = db.Prep( "INSERT INTO gpu_contexts(id,sequence,thread,period,type,calibrated,offset,base_gpu,base_cpu,scale,last_gpu) VALUES(?,(SELECT COUNT(*)+1 FROM gpu_contexts),?,?,?,?,?,?,?,1,0)" );
        Int( s, 1, e.context );
        Int( s, 2, e.thread );
        Require( sqlite3_bind_double( s, 3, e.period ) == SQLITE_OK, "GPU period binding failed" );
        Int( s, 4, uint8_t( e.type ) );
        Int( s, 5, ( e.flags & GpuContextCalibration ) ? 1 : 0 );
        Int( s, 6, uint64_t( Sub( cpu, gpu ) ) );
        Int( s, 7, uint64_t( gpu ) );
        Int( s, 8, uint64_t( cpu ) );
        db.Run( s );
        gpuContexts[e.context] = {};
    }
    void GpuPending( uint8_t context, uint16_t query, uint64_t zone )
    {
        Require( gpuQueries.emplace( ( uint32_t( context ) << 16 ) | query, zone ).second, "Duplicate pending GPU query" );
    }
    void StoreGpuRow( GpuRow& r )
    {
        if( !r.stored )
        {
            auto s = db.PrepStatic( "INSERT INTO gpu_zones(id,context,thread,parent,depth,cpu_start,cpu_end,gpu_start,gpu_end,skind,skey,stack,callsite,query) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)" );
            Int( s, 1, r.id );
            Int( s, 2, r.context );
            Int( s, 3, r.thread );
            Int( s, 4, r.parent );
            Int( s, 5, r.depth );
            Int( s, 6, uint64_t( r.cpuStart ) );
            if( r.hasCpuEnd ) Int( s, 7, uint64_t( r.cpuEnd ) );
            if( r.hasGpuStart ) Int( s, 8, uint64_t( r.gpuStart ) );
            if( r.hasGpuEnd ) Int( s, 9, uint64_t( r.gpuEnd ) );
            Int( s, 10, r.skind );
            Int( s, 11, r.skey );
            Int( s, 12, r.stack );
            Int( s, 13, r.callsite );
            Int( s, 14, r.query );
            db.Run( s );
            r.stored = true;
            return;
        }
        auto s = db.PrepStatic( "UPDATE gpu_zones SET cpu_end=?,gpu_start=?,gpu_end=? WHERE id=?" );
        if( r.hasCpuEnd ) Int( s, 1, uint64_t( r.cpuEnd ) );
        if( r.hasGpuStart ) Int( s, 2, uint64_t( r.gpuStart ) );
        if( r.hasGpuEnd ) Int( s, 3, uint64_t( r.gpuEnd ) );
        Int( s, 4, r.id );
        db.Run( s );
    }
    void CompleteGpuRow( std::unordered_map<uint64_t, GpuRow>::iterator row )
    {
        auto& r = row->second;
        if( !r.hasCpuEnd || !r.hasGpuStart || !r.hasGpuEnd ) return;
        StoreGpuRow( r );
        gpuRows.erase( row );
    }
    // Writes the older half of the waiting rows; their later values update
    // the stored rows.
    void LimitGpuRows()
    {
        std::vector<uint64_t> ids;
        ids.reserve( gpuRows.size() );
        for( const auto& [id, r] : gpuRows ) ids.push_back( id );
        const auto half = ids.begin() + ids.size() / 2;
        std::nth_element( ids.begin(), half, ids.end() );
        for( auto it = ids.begin(); it != half; ++it )
        {
            auto row = gpuRows.find( *it );
            StoreGpuRow( row->second );
            gpuRows.erase( row );
        }
    }
    void FlushGpu()
    {
        for( size_t id = 0; id < gpuContexts.size(); ++id )
        {
            auto& c = gpuContexts[id];
            if( !c.dirty ) continue;
            auto s = db.PrepStatic( "UPDATE gpu_contexts SET offset=?,base_gpu=?,base_cpu=?,scale=?,last_gpu=?,overflow=?,wraps=?,zones=?,incomplete=? WHERE id=?" );
            Int( s, 1, uint64_t( c.offset ) );
            Int( s, 2, uint64_t( c.baseGpu ) );
            Int( s, 3, uint64_t( c.baseCpu ) );
            Require( sqlite3_bind_double( s, 4, c.scale ) == SQLITE_OK, "GPU scale binding failed" );
            Int( s, 5, uint64_t( c.last ) );
            Int( s, 6, c.overflow );
            Int( s, 7, c.wraps );
            Int( s, 8, c.zones );
            Int( s, 9, c.incomplete );
            Int( s, 10, id );
            db.Run( s );
            c.dirty = false;
        }
        for( auto& [id, r] : gpuRows ) StoreGpuRow( r );
    }
    void GpuBegin( uint64_t id, const QueueGpuZoneBeginLean& e, uint64_t source, bool allocated, bool serial, bool callstack, uint64_t callsite )
    {
        auto& c = Gpu( e.context );
        const auto tid = c.thread ? 0 : e.thread;
        const auto time = serial ? SerialDelta( e.cpuTime ) : Delta( e.cpuTime );
        auto& open = gpuStacks[{ e.context, tid }];
        const uint64_t parent = open.empty() ? 0 : open.back().first;
        const uint64_t depth = open.empty() ? 0 : uint64_t( open.back().second ) + 1;
        Require( depth <= UINT32_MAX, "GPU depth overflow" );
        uint64_t stack = 0;
        if( callstack )
        {
            if( serial )
            {
                stack = serialStack;
                serialStack = 0;
            }
            else
            {
                auto& t = ExistingThread( Logical(), "Missing GPU callstack thread" );
                stack = t.nextstack;
                t.nextstack = 0;
                t.dirty = true;
            }
            Require( stack != 0, "Missing GPU callstack payload" );
        }
        GpuRow r;
        r.id = id;
        r.context = e.context;
        r.thread = tid;
        r.parent = parent;
        r.depth = uint32_t( depth );
        r.cpuStart = time;
        r.skind = allocated ? 1 : 0;
        r.skey = source;
        r.stack = stack;
        r.callsite = callsite;
        r.query = e.queryId;
        Require( gpuRows.emplace( id, r ).second, "Duplicate GPU zone" );
        open.emplace_back( id, uint32_t( depth ) );
        GpuPending( e.context, e.queryId, id );
        ++c.zones;
        ++c.incomplete;
        c.dirty = true;
        ++info.gpuZones;
        info.lastNs = std::max( info.lastNs, time );
        if( gpuRows.size() > GpuRowLimit ) LimitGpuRows();
    }
    void GpuEnd( const QueueGpuZoneEnd& e, bool serial )
    {
        Gpu( e.context );
        auto open = gpuStacks.find( { e.context, uint64_t( e.thread ) } );
        Require( open != gpuStacks.end() && !open->second.empty(), "GPU zone stack is empty" );
        const uint64_t id = open->second.back().first;
        open->second.pop_back();
        const auto time = serial ? SerialDelta( e.cpuTime ) : Delta( e.cpuTime );
        auto row = gpuRows.find( id );
        if( row != gpuRows.end() )
        {
            row->second.cpuEnd = time;
            row->second.hasCpuEnd = true;
        }
        else
        {
            auto s = db.PrepStatic( "UPDATE gpu_zones SET cpu_end=? WHERE id=?" );
            Int( s, 1, uint64_t( time ) );
            Int( s, 2, id );
            db.Run( s );
        }
        GpuPending( e.context, e.queryId, id );
        if( row != gpuRows.end() ) CompleteGpuRow( row );
        info.lastNs = std::max( info.lastNs, time );
    }
    void GpuTime( const QueueGpuTime& e )
    {
        auto& c = Gpu( e.context );
        const auto query = gpuQueries.find( ( uint32_t( e.context ) << 16 ) | e.queryId );
        Require( query != gpuQueries.end(), "GPU timestamp has no pending query" );
        const uint64_t id = query->second;
        auto row = gpuRows.find( id );
        bool starting;
        I start = 0;
        if( row != gpuRows.end() )
        {
            starting = !row->second.hasGpuStart;
            start = row->second.gpuStart;
        }
        else
        {
            auto s = db.PrepStatic( "SELECT gpu_start FROM gpu_zones WHERE id=?" );
            Int( s, 1, id );
            Require( db.Step( s ), "GPU timestamp has no pending query" );
            starting = sqlite3_column_type( s, 0 ) == SQLITE_NULL;
            start = sqlite3_column_int64( s, 0 );
            sqlite3_reset( s );
        }
        gpuReference = Add( gpuReference, e.gpuTime );
        auto raw = gpuReference;
        if( c.last >= INT64_MIN + ( 1ll << 31 ) && raw < c.last - ( 1ll << 31 ) )
        {
            Require( c.last > 0 && std::bit_width( uint64_t( c.last ) ) < 63, "Unsupported GPU counter wrap" );
            if( !c.overflow ) c.overflow = uint64_t( 1 ) << std::bit_width( uint64_t( c.last ) );
            ++c.wraps;
        }
        c.last = raw;
        c.dirty = true;
        Require( !c.overflow || c.wraps <= uint64_t( INT64_MAX ) / c.overflow, "GPU wrap overflow" );
        raw = Add( raw, I( c.overflow * c.wraps ) );
        I time;
        if( !c.calibrated ) time = Add( c.period == 1 ? raw : GpuInteger( c.period * raw ), c.offset );
        else time = GpuInteger( ( ( c.period == 1 ? double( raw ) : c.period * raw ) - c.baseGpu ) * c.scale + c.baseCpu );
        if( row != gpuRows.end() )
        {
            auto& r = row->second;
            if( starting )
            {
                r.gpuStart = time;
                r.hasGpuStart = true;
            }
            else
            {
                r.gpuEnd = time;
                r.hasGpuEnd = true;
            }
        }
        else
        {
            auto s = db.PrepStatic( starting ? "UPDATE gpu_zones SET gpu_start=? WHERE id=?" : "UPDATE gpu_zones SET gpu_end=? WHERE id=?" );
            Int( s, 1, uint64_t( time ) );
            Int( s, 2, id );
            db.Run( s );
        }
        // Offline builds index GPU zones once in Finish().
        if( incremental )
        {
            if( starting ) Range( "INSERT INTO gpu_range VALUES(?1,?2,?3)", id, time, {} );
            else Range( "UPDATE gpu_range SET mint=?2,maxt=?3 WHERE id=?1", id, std::min( start, time ), std::max( start, time ) );
        }
        if( !starting ) --c.incomplete;
        gpuQueries.erase( query );
        if( row != gpuRows.end() ) CompleteGpuRow( row );
        info.lastNs = std::max( info.lastNs, time );
    }
    void GpuCalibration( const QueueGpuCalibration& e )
    {
        auto& c = Gpu( e.context );
        Require( c.calibrated, "GPU calibration on uncalibrated context" );
        const I gpu = c.period == 1 ? e.gpuTime : GpuInteger( c.period * e.gpuTime );
        const I delta = Sub( gpu, c.baseGpu );
        Require( delta != 0, "Zero GPU calibration delta" );
        const double scale = double( e.cpuDelta ) / delta;
        Require( std::isfinite( scale ), "Invalid GPU calibration ratio" );
        c.baseGpu = gpu;
        c.baseCpu = Time( e.cpuTime );
        c.scale = scale;
        c.dirty = true;
    }
    void GpuSync( const QueueGpuTimeSync& e )
    {
        auto& c = Gpu( e.context );
        const I gpu = c.period == 1 ? e.gpuTime : GpuInteger( c.period * e.gpuTime );
        c.offset = Sub( Time( e.cpuTime ), gpu );
        c.last = 0;
        c.overflow = 0;
        c.wraps = 0;
        c.dirty = true;
    }
    void Thread( uint64_t id ) { EnsureThread( id ); }
    uint64_t Logical()
    {
        const auto fiber = EnsureThread( thread ).fiber;
        return fiber ? fiber : thread;
    }
    uint64_t Top( uint64_t tid ) { return EnsureThread( tid ).top; }
    void Range( const char* sql, uint64_t id, I begin, std::optional<I> end )
    {
        auto s = db.PrepStatic( sql );
        Int( s, 1, id );
        Int( s, 2, uint64_t( Bucket( begin ) ) );
        Int( s, 3, end ? uint64_t( Bucket( *end ) ) : uint64_t( INT32_MAX ) );
        db.Run( s );
    }
    // Offline builds index CPU/GPU zones, Jobs and I/O requests once, after
    // ingestion, with the same (id, mint, maxt) rows that per-event
    // maintenance produces: the final interval, or the open upper bound. The
    // cursor yields (id, begin, end) in id order with end NULL while open. The
    // R-tree is packed bottom-up in that order and written in SQLite's
    // documented R-tree format ("Database Format of R-Tree Tables" in
    // sqlite3.c): node 1 is the root and stores the tree depth in its first
    // two bytes; every node stores a big-endian cell count followed by cells
    // of a big-endian 64-bit id and big-endian int32 (min,max) bounds. The
    // %_rowid table maps entries to leaves and %_parent maps non-root nodes to
    // parents. Memory is one open node per level. The quick_check after the
    // build runs rtreecheck() over the result.
    struct RangeTable
    {
        // All statements are string literals for the static-SQL statement path.
        const char *name, *rows, *rowid, *parent, *node;
    };
    // A GPU zone is indexed once its GPU start is known; its bounds are the
    // ordered pair of GPU start and end.
    static constexpr RangeTable GpuRanges{ "gpu_range", "SELECT id,CASE WHEN gpu_end IS NULL THEN gpu_start ELSE min(gpu_start,gpu_end) END,CASE WHEN gpu_end IS NULL THEN NULL ELSE max(gpu_start,gpu_end) END FROM gpu_zones WHERE gpu_start IS NOT NULL ORDER BY id", "INSERT INTO gpu_range_rowid VALUES(?,?)", "INSERT INTO gpu_range_parent VALUES(?,?)", "INSERT OR REPLACE INTO gpu_range_node VALUES(?,?)" };
    // Every touched Job or I/O request is indexed; a missing row keeps the
    // whole-capture interval that the placeholder insert gave it.
    // Offline builds index every Job and I/O request once.
    static constexpr RangeTable JobRanges{ "job_range", "SELECT id,COALESCE(first_time,schedule,0),completed FROM jobs ORDER BY id", "INSERT INTO job_range_rowid VALUES(?,?)", "INSERT INTO job_range_parent VALUES(?,?)", "INSERT OR REPLACE INTO job_range_node VALUES(?,?)" };
    static constexpr RangeTable AllocationRanges{ "allocation_range", "SELECT id,allocated,freed FROM allocations ORDER BY id", "INSERT INTO allocation_range_rowid VALUES(?,?)", "INSERT INTO allocation_range_parent VALUES(?,?)", "INSERT OR REPLACE INTO allocation_range_node VALUES(?,?)" };
    static constexpr RangeTable IoRanges{ "io_range", "SELECT id,COALESCE(first_time,queued,0),end FROM io_requests ORDER BY id", "INSERT INTO io_range_rowid VALUES(?,?)", "INSERT INTO io_range_parent VALUES(?,?)", "INSERT OR REPLACE INTO io_range_node VALUES(?,?)" };
    void LoadRanges( const RangeTable& table )
    {
        constexpr size_t Fanout = 51, CellBytes = 16, NodeBytes = 4 + Fanout * CellBytes;
        const std::string name = table.name;
        Require( db.Scalar( "SELECT COUNT(*) FROM " + name + "_rowid" ) == 0 && db.Scalar( "SELECT COUNT(*) FROM " + name + "_parent" ) == 0 && db.Scalar( "SELECT COUNT(*) FROM " + name + "_node" ) == 1 && db.Scalar( "SELECT length(data) FROM " + name + "_node WHERE nodeno=1" ) == I( NodeBytes ), "Unexpected range index layout" );
        const auto rowidSql = table.rowid, parentSql = table.parent, nodeSql = table.node;
        const auto rows = table.rows;
        struct Cell
        {
            I id;
            int32_t lo, hi;
        };
        struct Level
        {
            std::vector<Cell> cells;
            bool written = false;
        };
        std::vector<Level> levels;
        uint64_t nextNode = 2;
        std::array<uint8_t, NodeBytes> data;
        const auto put = [&]( size_t at, uint64_t value, int bytes ) {
            for( int n = 0; n < bytes; ++n ) data[at + n] = uint8_t( value >> ( 8 * ( bytes - 1 - n ) ) );
        };
        // Writes the open node of `level` and returns the cell that refers to it.
        const auto write = [&]( size_t level, bool root ) {
            auto& cells = levels[level].cells;
            const uint64_t node = root ? 1 : nextNode++;
            data.fill( 0 );
            put( 0, root ? level : 0, 2 );
            put( 2, cells.size(), 2 );
            Cell bounds{ I( node ), INT32_MAX, INT32_MIN };
            for( size_t n = 0; n < cells.size(); ++n )
            {
                const auto& cell = cells[n];
                put( 4 + n * CellBytes, uint64_t( cell.id ), 8 );
                put( 4 + n * CellBytes + 8, uint32_t( cell.lo ), 4 );
                put( 4 + n * CellBytes + 12, uint32_t( cell.hi ), 4 );
                bounds.lo = std::min( bounds.lo, cell.lo );
                bounds.hi = std::max( bounds.hi, cell.hi );
                auto map = db.PrepStatic( level == 0 ? rowidSql : parentSql );
                Int( map, 1, uint64_t( cell.id ) );
                Int( map, 2, node );
                db.Run( map );
            }
            auto s = db.PrepStatic( nodeSql );
            Int( s, 1, node );
            Blob( s, 2, data );
            db.Run( s );
            cells.clear();
            levels[level].written = true;
            return bounds;
        };
        const auto add = [&]( auto& self, size_t level, const Cell& cell ) -> void {
            if( levels.size() == level ) levels.emplace_back();
            if( levels[level].cells.size() == Fanout ) self( self, level + 1, write( level, false ) );
            levels[level].cells.push_back( cell );
        };
        auto source = db.PrepStatic( rows );
        while( db.Step( source ) )
        {
            const I lo = Bucket( sqlite3_column_int64( source, 1 ) );
            const I hi = sqlite3_column_type( source, 2 ) == SQLITE_NULL ? INT32_MAX : Bucket( sqlite3_column_int64( source, 2 ) );
            Require( lo <= hi, "rtree constraint failed" );
            add( add, 0, { sqlite3_column_int64( source, 0 ), int32_t( lo ), int32_t( hi ) } );
        }
        sqlite3_reset( source );
        // Close the open nodes bottom-up. The single node of the top level,
        // which never filled, becomes the root.
        for( size_t level = 0; level < levels.size(); ++level )
        {
            if( level + 1 == levels.size() && !levels[level].written )
            {
                write( level, true );
                break;
            }
            add( add, level + 1, write( level, false ) );
        }
    }
    uint64_t Response( ServerQuery type, std::optional<uint64_t> key )
    {
        // ObserveProtocol callbacks may record a response before the send
        // callback records its request. Outbound query order is still retained.
        auto s = db.PrepStatic( key ? "SELECT seq,ord,key FROM queries WHERE type=? AND key=? AND used=0 ORDER BY seq,ord LIMIT 1" : "SELECT seq,ord,key FROM queries WHERE type=? AND used=0 ORDER BY seq,ord LIMIT 1" );
        Int( s, 1, type );
        if( key ) Int( s, 2, *key );
        if( !db.Step( s ) )
        {
            sqlite3_reset( s );
            if( incremental ) throw AwaitRecordedQuery();
            throw std::runtime_error( "Definition response has no matching recorded query" );
        }
        uint64_t seq = sqlite3_column_int64( s, 0 ), ord = sqlite3_column_int64( s, 1 ), ptr = sqlite3_column_int64( s, 2 );
        sqlite3_reset( s );
        s = db.Prep( "UPDATE queries SET used=1 WHERE seq=? AND ord=?" );
        Int( s, 1, seq );
        Int( s, 2, ord );
        db.Run( s );
        return ptr;
    }
    std::string Take()
    {
        Require( hasSingle, "Missing staged string" );
        hasSingle = false;
        return std::move( single );
    }
    void String( QueueType kind, uint64_t key, std::string_view text )
    {
        auto s = db.Prep( "INSERT OR REPLACE INTO strings VALUES(?,?,?)" );
        Int( s, 1, uint8_t( kind ) );
        Int( s, 2, key );
        Text( s, 3, text );
        db.Run( s );
    }
    // The thread's innermost open zone, which a zone end applies to; the end
    // must carry that zone's validation.
    uint64_t ValidTop( uint64_t tid )
    {
        auto& t = CachedThread( tid );
        Require( t.present && t.top != 0, "CPU zone stack is empty" );
        if( t.validation != TopFrame( t ).validation ) throw std::runtime_error( ZoneMismatch( t ) );
        t.validation = 0;
        t.dirty = true;
        return t.top;
    }
    // The thread's innermost open zone, which a zone text, name, color or
    // value applies to, or 0 when the event carries another zone's validation
    // or no zone is open. Such an event was written for a zone that is not
    // innermost; attaching it here would misattribute it, so it is dropped and
    // counted.
    uint64_t MatchedTop( uint64_t tid )
    {
        auto& t = CachedThread( tid );
        const bool match = t.present && t.top != 0 && t.validation == TopFrame( t ).validation;
        if( t.present )
        {
            t.validation = 0;
            t.dirty = true;
        }
        if( match ) return t.top;
        ++info.misplacedZoneEvents;
        return 0;
    }
    // A source location's name, or its function, for error reports.
    std::string SourceText( uint8_t kind, uint64_t key )
    {
        auto s = db.Prep( kind == 1 ? "SELECT name,function FROM sources WHERE kind=1 AND key=?"
                                    : "SELECT (SELECT value FROM strings WHERE key=s.nkey LIMIT 1),(SELECT value FROM strings WHERE key=s.fkey LIMIT 1) FROM sources s WHERE kind=0 AND key=?" );
        Int( s, 1, key );
        std::string text;
        if( db.Step( s ) )
        {
            text = Col( s, 0 );
            if( text.empty() ) text = Col( s, 1 );
        }
        sqlite3_reset( s );
        return text.empty() ? "?" : text;
    }
    // Why a zone event did not match the thread's innermost open zone: the
    // validation it carried, the open zones from the top with their sources,
    // and where the zone it meant is among them.
    std::string ZoneMismatch( ThreadState& t )
    {
        auto s = db.Prep( "SELECT value FROM strings WHERE kind=? AND key=?" );
        Int( s, 1, uint8_t( QueueType::ThreadName ) );
        Int( s, 2, t.id );
        const auto thread = db.Step( s ) ? Col( s, 0 ) : std::string( "?" );
        sqlite3_reset( s );
        std::string m = "CPU zone validation mismatch: thread " + std::to_string( t.id ) + " (" + thread + "): zone end for validation " +
                        std::to_string( t.validation ) + ", but the innermost open zone has validation " + std::to_string( TopFrame( t ).validation ) + ". Open zones from the top:";
        int meant = -1;
        for( uint32_t i = 0; i < t.frames; i++ )
        {
            const auto& f = t.window[( t.head + ZoneWindow - i ) % ZoneWindow];
            if( meant < 0 && f.validation == t.validation ) meant = int( i );
            if( i < 12 )
                m += " [validation " + std::to_string( f.validation ) + ", " + ( f.hasName ? f.name + " / " : std::string() ) + SourceText( f.skind, f.skey ) +
                     ", depth " + std::to_string( f.depth ) + ", start " + std::to_string( f.start ) + " ns]";
        }
        if( t.frames > 12 ) m += " ... " + std::to_string( t.frames - 12 ) + " more in the window";
        if( meant > 0 ) m += ". The zone it meant is " + std::to_string( meant ) + " below the top: its end arrived before the zones above it ended.";
        else m += ". No open zone in the window (" + std::to_string( t.frames ) + " zones) has that validation: it never began on this thread, already ended, or is deeper.";
        return m;
    }
    void FlushZones( uint64_t tid, ZoneBuffer& z )
    {
        if( !z.count ) return;
        ZoneSummary summary;
        I tmin = INT64_MAX, tclosed = INT64_MIN;
        uint64_t unfinished = 0;
        for( size_t n = 0; n < z.count; ++n )
        {
            const auto& r = z.rows[n];
            tmin = std::min( tmin, r.start );
            if( r.open ) ++unfinished;
            else tclosed = std::max( tclosed, r.end );
            ++summary.depth[std::min( r.depth, SummaryDepths - 1 )];
            summary.maximumDepth = std::max( summary.maximumDepth, r.depth );
        }
        const auto raw = EncodeZones( z.rows, z.count );
        const auto stored = Compress( raw );
        const auto offset = blockFile->Append( stored );
        const I tmax = unfinished ? INT64_MAX : tclosed;
        auto s = db.Prep( "INSERT INTO blocks(stream,key,first,last,count,tmin,tmax,tclosed,unfinished,depth,offset,size,raw,checksum,summary) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)" );
        Int( s, 1, uint64_t( BlockStream::CpuZones ) );
        Int( s, 2, tid );
        Int( s, 3, z.rows[0].id );
        Int( s, 4, z.rows[z.count - 1].id );
        Int( s, 5, z.count );
        Int( s, 6, uint64_t( tmin ) );
        Int( s, 7, uint64_t( tmax ) );
        Int( s, 8, uint64_t( tclosed ) );
        Int( s, 9, unfinished );
        Int( s, 10, summary.maximumDepth );
        Int( s, 11, offset );
        Int( s, 12, stored.size() );
        Int( s, 13, raw.size() );
        Int( s, 14, XXH3_64bits( stored.data(), stored.size() ) );
        Blob( s, 15, EncodeSummary( summary ) );
        db.Run( s );
        const auto block = uint64_t( sqlite3_last_insert_rowid( db.handle ) );
        s = db.Prep( "INSERT INTO block_range VALUES(?,?,?)" );
        Int( s, 1, block );
        Int( s, 2, uint64_t( Bucket( tmin ) ) );
        Int( s, 3, uint64_t( Bucket( tmax ) ) );
        db.Run( s );
        if( unfinished ) generationBlock[z.generation] = block;
        bufferedZones -= z.count;
        z.count = 0;
        z.rows.clear();
    }
    void FlushTexts( uint64_t tid, ZoneBuffer& z )
    {
        if( z.texts.empty() ) return;
        const auto raw = EncodeTexts( z.texts );
        const auto stored = Compress( raw );
        const auto offset = blockFile->Append( stored );
        auto s = db.Prep( "INSERT INTO blocks(stream,key,first,last,count,tmin,tmax,tclosed,unfinished,depth,offset,size,raw,checksum,summary) VALUES(?,?,?,?,?,0,0,0,0,0,?,?,?,?,NULL)" );
        Int( s, 1, uint64_t( BlockStream::ZoneText ) );
        Int( s, 2, tid );
        Int( s, 3, z.texts.front().zone );
        Int( s, 4, z.texts.back().zone );
        Int( s, 5, z.texts.size() );
        Int( s, 6, offset );
        Int( s, 7, stored.size() );
        Int( s, 8, raw.size() );
        Int( s, 9, XXH3_64bits( stored.data(), stored.size() ) );
        db.Run( s );
        bufferedTextBytes -= z.textBytes + z.texts.size() * sizeof( TextRow );
        z.texts.clear();
        z.textBytes = 0;
    }
    // Open zones by logical thread, outermost first, collected by FinishZones().
    std::map<uint64_t, std::vector<ZoneFrame>> openStacks;
    uint64_t openZoneCount = 0;
    // Publishes the current state of every open zone, then writes all buffered
    // rows. Ingestion continues afterwards in live stores: the frames keep
    // their values, and their rows are now flushed.
    void FinishZones()
    {
        openStacks.clear();
        openZoneCount = 0;
        for( auto& t : threadCache )
            for( uint32_t n = t.frames; n > 0; --n )
            {
                const auto& f = t.window[( t.head + ZoneWindow - ( n - 1 ) ) % ZoneWindow];
                openStacks[t.id].push_back( f );
            }
        auto q = db.Prep( "SELECT id,thread,parent,validation,generation,start,depth,slot,children,color,flags,name,last_child,last_child_end FROM open_frames" );
        while( db.Step( q ) )
        {
            ZoneFrame f;
            f.id = sqlite3_column_int64( q, 0 );
            const uint64_t tid = sqlite3_column_int64( q, 1 );
            f.parent = sqlite3_column_int64( q, 2 );
            f.validation = sqlite3_column_int64( q, 3 );
            f.generation = sqlite3_column_int64( q, 4 );
            f.start = sqlite3_column_int64( q, 5 );
            f.depth = sqlite3_column_int64( q, 6 );
            f.slot = uint32_t( sqlite3_column_int64( q, 7 ) );
            f.children = uint32_t( sqlite3_column_int64( q, 8 ) );
            f.color = uint32_t( sqlite3_column_int64( q, 9 ) );
            const auto flags = sqlite3_column_int( q, 10 );
            f.hasColor = flags & 1;
            f.hasName = flags & 2;
            f.lastChildClosed = flags & 4;
            f.name = Col( q, 11 );
            f.lastChild = sqlite3_column_int64( q, 12 );
            f.lastChildEnd = sqlite3_column_int64( q, 13 );
            openStacks[tid].push_back( std::move( f ) );
        }
        sqlite3_reset( q );
        for( auto& [tid, frames] : openStacks )
        {
            std::sort( frames.begin(), frames.end(), []( const ZoneFrame& a, const ZoneFrame& b ) { return a.depth < b.depth; } );
            for( const auto& f : frames ) SettleZone( tid, f, std::nullopt );
            openZoneCount += frames.size();
        }
        FlushBuffers();
    }
    void FlushBuffers()
    {
        for( auto& [tid, z] : zoneBuffers )
        {
            FlushZones( tid, z );
            FlushTexts( tid, z );
        }
    }
    // Buffered rows are bounded across all threads: the least recently used
    // buffers are flushed first.
    void LimitBuffers()
    {
        while( bufferedZones > BufferedZoneLimit )
        {
            ZoneBuffer* victim = nullptr;
            uint64_t victimThread = 0;
            for( auto& [tid, z] : zoneBuffers )
                if( z.count && ( !victim || z.used < victim->used ) )
                {
                    victim = &z;
                    victimThread = tid;
                }
            Require( victim != nullptr, "Zone buffer accounting mismatch" );
            FlushZones( victimThread, *victim );
            victim->rows = {};
        }
    }
    void LimitTexts()
    {
        while( bufferedTextBytes > BufferedTextLimit )
        {
            ZoneBuffer* victim = nullptr;
            uint64_t victimThread = 0;
            for( auto& [tid, z] : zoneBuffers )
                if( !z.texts.empty() && ( !victim || z.used < victim->used ) )
                {
                    victim = &z;
                    victimThread = tid;
                }
            Require( victim != nullptr, "Zone text buffer accounting mismatch" );
            FlushTexts( victimThread, *victim );
            victim->texts.shrink_to_fit();
        }
    }
    // Writes the values a zone collected while open: into its buffered row,
    // or into `zone_late` when the row was flushed. Open zones keep end NULL.
    void SettleZone( uint64_t tid, const ZoneFrame& f, std::optional<I> end )
    {
        auto found = zoneBuffers.find( tid );
        if( found != zoneBuffers.end() && found->second.generation == f.generation && f.slot < found->second.count )
        {
            auto& r = found->second.rows[f.slot];
            Require( r.id == f.id, "Buffered CPU zone mismatch" );
            r.open = !end;
            if( end ) r.end = *end;
            r.children = f.children;
            r.childTime = f.childTime;
            r.hasColor = f.hasColor;
            r.color = f.color;
            r.hasName = f.hasName;
            r.name = f.name;
            return;
        }
        auto s = db.Prep( "INSERT OR REPLACE INTO zone_late VALUES(?,?,?,?,?,?,?)" );
        Int( s, 1, f.id );
        Int( s, 2, tid );
        if( end ) Int( s, 3, uint64_t( *end ) );
        Int( s, 4, f.children );
        if( f.hasName ) Text( s, 5, f.name );
        if( f.hasColor ) Int( s, 6, f.color );
        Int( s, 7, uint64_t( f.childTime ) );
        db.Run( s );
        if( !end ) return;
        auto block = generationBlock.find( f.generation );
        Require( block != generationBlock.end(), "Late CPU zone has no event block" );
        s = db.Prep( "UPDATE blocks SET unfinished=unfinished-1,tclosed=MAX(tclosed,?1),tmax=CASE WHEN unfinished>1 THEN tmax ELSE MAX(tclosed,?1) END WHERE id=?2 RETURNING unfinished,tmax" );
        Int( s, 1, uint64_t( *end ) );
        Int( s, 2, block->second );
        Require( db.Step( s ), "Late CPU zone block is missing" );
        const auto unfinished = sqlite3_column_int64( s, 0 );
        const I tmax = sqlite3_column_int64( s, 1 );
        sqlite3_reset( s );
        Require( unfinished >= 0, "Late CPU zone count mismatch" );
        if( unfinished == 0 )
        {
            s = db.Prep( "UPDATE block_range SET maxt=? WHERE id=?" );
            Int( s, 1, uint64_t( Bucket( tmax ) ) );
            Int( s, 2, block->second );
            db.Run( s );
            generationBlock.erase( block );
        }
    }
    void AppendText( uint64_t tid, uint64_t zone, uint64_t event, std::string text )
    {
        auto& z = zoneBuffers[tid];
        z.used = ++bufferClock;
        if( zone < z.textZone )
        {
            auto s = db.Prep( "INSERT INTO zone_late_text VALUES(?,?,?,?)" );
            Int( s, 1, zone );
            Int( s, 2, event );
            Int( s, 3, tid );
            Text( s, 4, text );
            db.Run( s );
            return;
        }
        z.textZone = zone;
        z.textEvent = event;
        z.textBytes += text.size();
        bufferedTextBytes += text.size() + sizeof( TextRow );
        z.texts.push_back( { zone, event, std::move( text ) } );
        if( z.textBytes >= TextBlockBytes || z.texts.size() >= TextBlockRows ) FlushTexts( tid, z );
        else if( bufferedTextBytes > BufferedTextLimit ) LimitTexts();
    }
    void Begin( uint64_t id, I start, uint64_t source, int kind, uint32_t callsite, bool callstack )
    {
        const uint64_t tid = Logical(), parent = Top( tid );
        auto& t = EnsureThread( tid );
        I depth = 0;
        uint64_t node = 0;
        if( parent )
        {
            auto& p = TopFrame( t );
            Require( p.id == parent, "CPU zone parent mismatch" );
            depth = p.depth + 1;
            ++p.children;
            p.lastChild = id;
            p.lastChildClosed = false;
            if( start < p.start || start < p.lastChildStart ) DropZoneTree();
            p.lastChildStart = start;
            node = ChildNode( p.childSkind, p.childSkey, p.childNode, p.node, tid, uint8_t( kind ), source, id );
        }
        else node = ChildNode( t.rootSkind, t.rootSkey, t.rootNode, 0, tid, uint8_t( kind ), source, id );
        Require( depth <= INT32_MAX, "CPU nesting depth overflow" );
        const uint64_t validation = t.validation, stack = callstack ? t.nextstack : 0;
        if( callstack ) Require( stack != 0, "Missing CPU zone callstack" );
        auto& z = zoneBuffers[tid];
        z.used = ++bufferClock;
        if( !z.count ) z.generation = ++zoneGeneration;
        auto& r = z.rows.emplace_back();
        r.id = id;
        r.parent = parent;
        r.start = start;
        r.depth = uint32_t( depth );
        r.skind = uint8_t( kind );
        r.skey = source;
        r.stack = stack;
        r.callsite = callsite;
        r.validation = validation;
        ZoneFrame f;
        f.id = id;
        f.parent = parent;
        f.validation = validation;
        f.generation = z.generation;
        f.start = start;
        f.depth = depth;
        f.slot = uint32_t( z.count++ );
        f.node = node;
        f.coverEnd = start;
        f.skey = source;
        f.skind = uint8_t( kind );
        ++bufferedZones;
        t.top = id;
        t.validation = 0;
        if( callstack ) t.nextstack = 0;
        t.dirty = true;
        PushFrame( t, std::move( f ) );
        ++zoneSources[{ uint8_t( kind ), source }];
        ++info.cpuZones;
        info.lastNs = std::max( info.lastNs, start );
        if( z.count == BlockZones ) FlushZones( tid, z );
        else if( bufferedZones > BufferedZoneLimit ) LimitBuffers();
    }
    void End( I end )
    {
        const auto tid = Logical(), id = ValidTop( tid );
        auto& t = CachedThread( tid );
        auto& f = TopFrame( t );
        Require( f.id == id, "CPU zone window out of order" );
        Require( end >= f.start, "CPU zone end precedes begin" );
        SettleZone( tid, f, end );
        CloseTreeNode( f, end );
        const auto parent = f.parent;
        const I start = f.start;
        PopFrame( t, id );
        t.top = parent;
        t.dirty = true;
        if( parent )
        {
            auto& p = TopFrame( t );
            p.childTime += end - start;
            if( p.lastChild == id )
            {
                p.lastChildClosed = true;
                p.lastChildEnd = end;
            }
            if( end > p.coverEnd )
            {
                p.covered += end - std::max( start, p.coverEnd );
                p.coverEnd = end;
            }
        }
        info.lastNs = std::max( info.lastNs, end );
    }
#include "TracyEventStoreCodeIngest.inl"
    void CloseFiber( uint64_t fiber, I time )
    {
        auto q = db.Prep( "SELECT r.id,r.start,r.end FROM fiber_active a JOIN context_regions r ON r.id=a.region WHERE a.fiber=?" );
        Int( q, 1, fiber );
        Require( db.Step( q ), "Fiber has no active scheduling region" );
        const auto id = uint64_t( sqlite3_column_int64( q, 0 ) );
        const auto start = sqlite3_column_int64( q, 1 );
        Require( sqlite3_column_type( q, 2 ) == SQLITE_NULL && time >= start, "Invalid Fiber region end" );
        sqlite3_reset( q );
        q = db.Prep( "UPDATE context_regions SET end=? WHERE id=?" );
        Int( q, 1, uint64_t( time ) );
        Int( q, 2, id );
        db.Run( q );
        Range( "UPDATE context_range SET mint=?2,maxt=?3 WHERE id=?1", id, start, time );
        q = db.Prep( "UPDATE context_threads SET running=running+? WHERE thread=?" );
        Int( q, 1, uint64_t( Sub( time, start ) ) );
        Int( q, 2, fiber );
        db.Run( q );
        q = db.Prep( "DELETE FROM fiber_active WHERE fiber=?" );
        Int( q, 1, fiber );
        db.Run( q );
    }
    void FiberEnter( uint64_t event, const QueueFiberEnter& e )
    {
        const auto time = Delta( e.time );
        info.lastNs = std::max( info.lastNs, time );
        auto q = db.Prep( "SELECT id FROM fibers WHERE key=?" );
        Int( q, 1, e.fiber );
        uint64_t logical = 0;
        if( db.Step( q ) ) logical = sqlite3_column_int64( q, 0 );
        sqlite3_reset( q );
        if( !logical )
        {
            Require( info.fibers < ( 1ull << 32 ), "Fiber identity limit exceeded" );
            logical = ( 1ull << 32 ) | info.fibers++;
            q = db.Prep( "INSERT INTO fibers VALUES(?,?,?)" );
            Int( q, 1, e.fiber );
            Int( q, 2, logical );
            Int( q, 3, uint64_t( int64_t( e.groupHint ) ) );
            db.Run( q );
            Thread( logical );
        }
        const uint64_t old = EnsureThread( e.thread ).fiber;
        if( old ) CloseFiber( old, time );
        auto& physical = CachedThread( e.thread );
        physical.fiber = logical;
        physical.dirty = true;
        q = db.Prep( "INSERT INTO context_regions VALUES(?1,?2,?3,NULL,?3,0,0,99,-1,?4)" );
        Int( q, 1, event );
        Int( q, 2, logical );
        Int( q, 3, uint64_t( time ) );
        Int( q, 4, e.thread );
        db.Run( q );
        Range( "INSERT INTO context_range VALUES(?,?,?)", event, time, {} );
        q = db.Prep( "INSERT INTO context_threads(thread,top) VALUES(?,?) ON CONFLICT(thread) DO UPDATE SET top=excluded.top" );
        Int( q, 1, logical );
        Int( q, 2, event );
        db.Run( q );
        q = db.Prep( "INSERT INTO fiber_active VALUES(?,?)" );
        Int( q, 1, logical );
        Int( q, 2, event );
        db.Run( q );
    }
    void FiberLeave( const QueueFiberLeave& e )
    {
        const auto time = Delta( e.time );
        info.lastNs = std::max( info.lastNs, time );
        const uint64_t fiber = ExistingThread( e.thread, "Fiber leave has no physical thread" ).fiber;
        Require( fiber != 0, "Fiber leave has no active fiber" );
        CloseFiber( fiber, time );
        auto& physical = CachedThread( e.thread );
        physical.fiber = 0;
        physical.dirty = true;
    }
    void Auxiliary( uint64_t id, const QueueItem& e, I time )
    {
        const auto type = e.hdr.type;
        // The record itself is the raw packet in its packet block; decoding
        // here keeps rejecting malformed records during ingestion.
        const std::span<const uint8_t> payload( reinterpret_cast<const uint8_t*>( &e.jnFrame ), QueueDataSize[uint8_t( type )] - 1 );
        DecodeAuxiliary( id, thread, time, type, payload );
        if( type == QueueType::JnRuntimeDomainState )
        {
            auto s = db.Prep( "INSERT OR IGNORE INTO jn_runtime_domains VALUES(?)" );
            Int( s, 1, e.jnRuntimeDomainState.domain );
            db.Run( s );
            s = db.Prep( "INSERT INTO jn_runtime_states VALUES(?,?,?)" );
            Int( s, 1, id );
            Int( s, 2, e.jnRuntimeDomainState.domain );
            Int( s, 3, uint64_t( time ) );
            db.Run( s );
        }
        ++info.auxiliaryEvents;
    }
    void ImageData( std::span<const uint8_t> bytes )
    {
        Require( !bytes.empty() && bytes.size() % 8 == 0 && bytes.size() <= TargetFrameSize, "Invalid Frame Image BC1 payload" );
        std::vector<uint8_t> canonical( bytes.begin(), bytes.end() );
        bc1::FixOrder( reinterpret_cast<char*>( canonical.data() ), canonical.size() / 8 );
        bc1::Rdo( reinterpret_cast<char*>( canonical.data() ), canonical.size() / 8 );
        auto s = db.Prep( "INSERT INTO frame_image_pending VALUES(1,?)" );
        Blob( s, 1, canonical );
        db.Run( s );
    }
    void FrameImage( uint64_t id, const QueueFrameImage& e )
    {
        auto s = db.Prep( "SELECT length(data) FROM frame_image_pending WHERE slot=1" );
        Require( db.Step( s ), "Frame Image has no pixel payload" );
        const auto size = uint64_t( sqlite3_column_int64( s, 0 ) );
        sqlite3_reset( s );
        Require( frameOffset <= INT64_MAX, "Frame Image offset overflow" );
        const auto frameIndex = Sub( int64_t( e.frame ) + 1, int64_t( frameOffset ) );
        if( onDemand && frameIndex <= 1 )
        {
            db.Exec( "DELETE FROM frame_image_pending" );
            return;
        }
        Require( frameIndex > 0 && frameIndex <= UINT32_MAX, "Invalid Frame Image index" );
        Require( e.w && e.h && e.w % 4 == 0 && e.h % 4 == 0 && uint64_t( e.w ) * e.h / 2 == size && e.flip <= 1, "Frame Image dimensions do not match BC1 payload" );
        s = db.Prep( "INSERT INTO frame_images SELECT ?,?,?,?,?,?,data FROM frame_image_pending WHERE slot=1" );
        Int( s, 1, id );
        Int( s, 2, info.frameImages );
        Int( s, 3, frameIndex );
        Int( s, 4, e.w );
        Int( s, 5, e.h );
        Int( s, 6, e.flip );
        db.Run( s );
        ++info.frameImages;
        db.Exec( "DELETE FROM frame_image_pending" );
    }
    void Frame( uint64_t set, I time, bool continuous, bool ending = false )
    {
        auto s = db.Prep( "SELECT top,continuous FROM frame_state WHERE setid=?" );
        Int( s, 1, set );
        uint64_t top = 0;
        if( db.Step( s ) )
        {
            top = sqlite3_column_int64( s, 0 );
            Require( ( sqlite3_column_int( s, 1 ) != 0 ) == continuous, "Frame continuity changed" );
        }
        sqlite3_reset( s );
        if( top )
        {
            s = db.Prep( "SELECT start,end FROM frames WHERE id=?" );
            Int( s, 1, top );
            db.Step( s );
            I begin = sqlite3_column_int64( s, 0 );
            bool open = sqlite3_column_type( s, 1 ) == SQLITE_NULL;
            sqlite3_reset( s );
            Require( time >= begin, "Frame timestamp moved backwards" );
            if( continuous || ending )
            {
                Require( open, "Frame ended twice" );
                s = db.Prep( "UPDATE frames SET end=? WHERE id=?" );
                Int( s, 1, uint64_t( time ) );
                Int( s, 2, top );
                db.Run( s );
                Range( "UPDATE frame_range SET mint=?2,maxt=?3 WHERE id=?1", top, begin, time );
                if( set == 0 )
                    for( int level = 0; level < 3; ++level )
                    {
                        I width = level == 0 ? 1000000000ll : level == 1 ? 60000000000ll
                                                                         : 3600000000000ll;
                        I bucket = begin / width;
                        if( begin < 0 && begin % width ) --bucket;
                        s = db.Prep( "INSERT INTO frame_summary VALUES(?,?,?,?,1,?) ON CONFLICT(level,bucket) DO UPDATE SET start=min(start,excluded.start),end=max(end,excluded.end),count=count+1,maximum=max(maximum,excluded.maximum)" );
                        Int( s, 1, level );
                        Int( s, 2, uint64_t( bucket ) );
                        Int( s, 3, uint64_t( begin ) );
                        Int( s, 4, uint64_t( time ) );
                        Int( s, 5, uint64_t( time - begin ) );
                        db.Run( s );
                    }
            }
            else Require( !open, "Overlapping discontinuous frames" );
        }
        else Require( !ending || onDemand, "Frame end has no begin" );
        if( !ending )
        {
            uint64_t id = ++info.frames;
            s = db.Prep( "INSERT INTO frames VALUES(?,?,?,NULL,?)" );
            Int( s, 1, id );
            Int( s, 2, set );
            Int( s, 3, uint64_t( time ) );
            Int( s, 4, continuous );
            db.Run( s );
            Range( "INSERT INTO frame_range VALUES(?,?,?)", id, time, {} );
            if( set == 0 )
            {
                s = db.Prep( "INSERT INTO base_frames VALUES(?,?)" );
                Int( s, 1, baseFrameCount++ );
                Int( s, 2, id );
                db.Run( s );
            }
            s = db.Prep( "INSERT OR REPLACE INTO frame_state VALUES(?,?,?)" );
            Int( s, 1, set );
            Int( s, 2, id );
            Int( s, 3, continuous );
            db.Run( s );
        }
        info.lastNs = std::max( info.lastNs, time );
    }
#include "TracyEventStoreTelemetry.inl"
    void Event( const ProtocolEventView& view )
    {
        const auto& e = view.fixed;
        auto t = e.hdr.type;
        auto bytes = view.payload;
        auto id = ++info.events;
        if( draining && !( t >= QueueType::StringData || t == QueueType::SourceLocation || t == QueueType::SingleStringData || t == QueueType::SecondStringData || t == QueueType::CallstackFrameSize || t == QueueType::CallstackFrame || t == QueueType::SymbolInformation || t == QueueType::Terminate || t == QueueType::AckServerQueryNoop || t == QueueType::AckSourceCodeNotAvailable || t == QueueType::AckSymbolCodeNotAvailable ) )
        {
            hasSingle = hasSecond = false;
            pendingStack = serialStack = pendingSource = 0;
            return;
        }
        TranscriptEvent( view );
        if( t >= QueueType::JnJobType && t <= QueueType::JnGpuCatalogBatch ) AppendPacket( t, id, e );
        switch( t )
        {
        case QueueType::SysTimeReport: TelemetryPlot(id,2,0,Time(e.sysTime.time),e.sysTime.sysTime);break;
        case QueueType::Terminate:case QueueType::KeepAlive:case QueueType::Crash:case QueueType::AckServerQueryNoop:break; // Control/credit events have no snapshot payload.
        case QueueType::SysPowerReport: TelemetryPlot(id,3,e.sysPower.name,Time(e.sysPower.time),double(e.sysPower.delta));break;
        case QueueType::FrameVsync: Vsync(id,e.frameVsync);break;
        case QueueType::ParamSetup: Parameter(id,e.paramSetup);break;
        case QueueType::JnGpuCatalogBatchData: CatalogPayload( e.stringTransfer.ptr, bytes ); break;
        case QueueType::JnGpuCatalogControl: CatalogControl( id, e.jnGpuCatalogControl ); break;
        case QueueType::JnGpuCatalogBatch: CatalogBatch( id, e.jnGpuCatalogBatch ); break;
        case QueueType::JnGpuReferenceSetDefinition: ResourceDefinition( e.stringTransfer.ptr, bytes ); break;
        case QueueType::JnGpuReferencePass: ResourcePass( id, e.jnGpuReferencePass ); break;
        case QueueType::JnGpuReferenceUse: ResourceUse( id, e, false ); break;
        case QueueType::JnGpuReferenceSetUse: ResourceUse( id, e, true ); break;
        case QueueType::JnGpuReferenceEnd: ResourceEnd( id, e.jnGpuReferenceEnd ); break;
        case QueueType::CpuTopology: {
            auto q = db.Prep( "INSERT INTO cpu_topology VALUES(?,?,?,?)" );
            Int( q, 1, e.cpuTopology.thread );
            Int( q, 2, e.cpuTopology.package );
            Int( q, 3, e.cpuTopology.die );
            Int( q, 4, e.cpuTopology.core );
            db.Run( q );
            break;
        }
        case QueueType::ThreadGroupHint: {
            auto q = db.Prep( "INSERT INTO thread_hints VALUES(?,?,?) ON CONFLICT(thread) DO UPDATE SET event=excluded.event,hint=excluded.hint" );
            Int( q, 1, e.threadGroupHint.thread );
            Int( q, 2, id );
            Int( q, 3, uint64_t( int64_t( e.threadGroupHint.groupHint ) ) );
            db.Run( q );
            break;
        }
        case QueueType::CrashReport: {
            const auto tid = Logical();
            const uint64_t stack = ExistingThread( tid, "Crash thread is missing" ).nextstack;
            db.Exec( "DELETE FROM crash" );
            auto q = db.Prep( "INSERT INTO crash VALUES(?,?,?,?,?)" );
            Int( q, 1, id );
            Int( q, 2, tid );
            Int( q, 3, uint64_t( Time( e.crashReport.time ) ) );
            Int( q, 4, e.crashReport.text );
            Int( q, 5, stack );
            db.Run( q );
            break;
        }
        case QueueType::ThreadContext:
            thread = e.threadCtx.thread;
            reference = 0;
            Thread( thread );
            break;
        case QueueType::ZoneValidation: {
            // UPDATE semantics: an absent row is left absent.
            auto& t = CachedThread( Logical() );
            if( t.present )
            {
                t.validation = e.zoneValidation.id;
                t.dirty = true;
            }
            break;
        }
        case QueueType::ZoneBegin:
        case QueueType::ZoneBeginCallstack:
        case QueueType::JnZoneBeginCallsite: Begin( id, Delta( e.zoneBegin.time ), e.zoneBegin.srcloc, 0, t == QueueType::JnZoneBeginCallsite ? e.jnZoneBeginCallsite.callsiteId : 0, t == QueueType::ZoneBeginCallstack ); break;
        case QueueType::ZoneBeginAllocSrcLoc:
        case QueueType::ZoneBeginAllocSrcLocCallstack:
            Require( pendingSource != 0, "Missing allocated source location" );
            Begin( id, Delta( e.zoneBeginLean.time ), pendingSource, 1, 0, t == QueueType::ZoneBeginAllocSrcLocCallstack );
            pendingSource = 0;
            break;
        case QueueType::ZoneEnd: End( Delta( e.zoneEnd.time ) ); break;
        case QueueType::PlotDataInt: PlotPoint( id, e.plotDataInt.name, e.plotDataInt.time, double( e.plotDataInt.val ) ); break;
        case QueueType::PlotDataFloat: PlotPoint( id, e.plotDataFloat.name, e.plotDataFloat.time, e.plotDataFloat.val ); break;
        case QueueType::PlotDataDouble: PlotPoint( id, e.plotDataDouble.name, e.plotDataDouble.time, e.plotDataDouble.val ); break;
        case QueueType::PlotConfig: PlotConfig( id, e.plotConfig ); break;
        case QueueType::GpuNewContext: GpuContext( e.gpuNewContext ); break;
        case QueueType::GpuTime: GpuTime( e.gpuTime ); break;
        case QueueType::GpuCalibration: GpuCalibration( e.gpuCalibration ); break;
        case QueueType::GpuTimeSync: GpuSync( e.gpuTimeSync ); break;
        case QueueType::GpuZoneBegin:
        case QueueType::GpuZoneBeginCallstack:
        case QueueType::GpuZoneBeginSerial:
        case QueueType::GpuZoneBeginCallstackSerial:
        case QueueType::JnGpuZoneBeginCallsite:
            GpuBegin( id, e.gpuZoneBegin, e.gpuZoneBegin.srcloc, false,
                      t == QueueType::GpuZoneBeginSerial || t == QueueType::GpuZoneBeginCallstackSerial || t == QueueType::JnGpuZoneBeginCallsite,
                      t == QueueType::GpuZoneBeginCallstack || t == QueueType::GpuZoneBeginCallstackSerial,
                      t == QueueType::JnGpuZoneBeginCallsite ? e.jnGpuZoneBeginCallsite.callsiteId : 0 );
            break;
        case QueueType::GpuZoneBeginAllocSrcLoc:
        case QueueType::GpuZoneBeginAllocSrcLocCallstack:
        case QueueType::GpuZoneBeginAllocSrcLocSerial:
        case QueueType::GpuZoneBeginAllocSrcLocCallstackSerial:
            Require( pendingSource != 0, "Missing GPU allocated source" );
            GpuBegin( id, e.gpuZoneBeginLean, pendingSource, true,
                      t == QueueType::GpuZoneBeginAllocSrcLocSerial || t == QueueType::GpuZoneBeginAllocSrcLocCallstackSerial,
                      t == QueueType::GpuZoneBeginAllocSrcLocCallstack || t == QueueType::GpuZoneBeginAllocSrcLocCallstackSerial, 0 );
            pendingSource = 0;
            break;
        case QueueType::GpuZoneEnd: GpuEnd( e.gpuZoneEnd, false ); break;
        case QueueType::GpuZoneEndSerial: GpuEnd( e.gpuZoneEnd, true ); break;
        case QueueType::JnFrame: {
            const auto time = Time( e.jnFrame.time );
            Auxiliary( id, e, time );
            if( e.jnFrame.phase == uint8_t( JnFramePhase::Begin ) && ( e.jnFrame.flags & uint8_t( JnFrameFlags::Canonical ) ) != 0 )
            {
                auto s = db.Prep( "INSERT INTO canonical_frames VALUES(?,?,?) ON CONFLICT(sequence) DO UPDATE SET frame=excluded.frame,event=excluded.event" );
                Int( s, 1, uint32_t( e.jnFrame.frameId ) );
                Int( s, 2, e.jnFrame.frameId );
                Int( s, 3, id );
                db.Run( s );
            }
            info.lastNs = std::max( info.lastNs, time );
            break;
        }
        case QueueType::JnJobType: {
            auto s = db.Prep( "INSERT INTO job_types VALUES(?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET event=excluded.event,name_key=excluded.name_key,kind=excluded.kind,flags=excluded.flags" );
            Int( s, 1, e.jnJobType.typeId );
            Int( s, 2, id );
            Int( s, 3, e.jnJobType.name );
            Int( s, 4, e.jnJobType.kind );
            Int( s, 5, e.jnJobType.flags );
            db.Run( s );
            break;
        }
        case QueueType::JnJobSchedule: JobSchedule( id, e.jnJobSchedule ); break;
        case QueueType::JnJobConfig: JobConfig( id, e.jnJobConfig ); break;
        case QueueType::JnJobDependency: {
            auto& q = e.jnJobDependency;
            Job( q.jobId, id );
            auto s = db.Prep( "INSERT INTO job_dependencies VALUES(?,?,?,?,?)" );
            Int( s, 1, id );
            Int( s, 2, q.jobId );
            Int( s, 3, q.prerequisiteJobId );
            Int( s, 4, q.prerequisiteHandle );
            Int( s, 5, q.flags );
            db.Run( s );
            break;
        }
        case QueueType::JnIoRequest: {
            auto& q = e.jnIoRequest;
            EnsureTask( true, q.requestId, id );
            const auto time = Time( q.time );
            TaskTime( true, q.requestId, time );
            auto s = db.Prep( "UPDATE io_requests SET resource=?,queued=?,thread=?,operation=?,source=?,priority=?,subsystem=?,flags=?,boundary=boundary|?,orphan=0 WHERE id=?" );
            Int( s, 1, q.resourceId );
            Int( s, 2, uint64_t( time ) );
            Int( s, 3, thread );
            Int( s, 4, q.operation );
            Int( s, 5, q.source );
            Int( s, 6, q.priority );
            Int( s, 7, q.subsystem );
            Int( s, 8, q.flags );
            Int( s, 9, q.flags & 16 ? 1 : 0 );
            Int( s, 10, q.requestId );
            db.Run( s );
            break;
        }
        case QueueType::JnIoConfig: {
            auto& q = e.jnIoConfig;
            EnsureTask( true, q.requestId, id );
            auto s = db.Prep( "UPDATE io_requests SET parent=?,requested=?,origin=?,parent_kind=?,config_flags=?,boundary=boundary|? WHERE id=?" );
            Int( s, 1, q.parentId );
            Int( s, 2, q.requestedBytes );
            Int( s, 3, q.originFrameSequence );
            Int( s, 4, q.parentKind );
            Int( s, 5, q.flags );
            Int( s, 6, q.flags & 16 ? 1 : 0 );
            Int( s, 7, q.requestId );
            db.Run( s );
            break;
        }
        case QueueType::JnScriptFrame: {
            auto& q = e.jnScriptFrame;
            auto s = db.Prep( "INSERT INTO script_frames VALUES(?,?,?,?,?,?,?,?)" );
            Int( s, 1, id );
            Int( s, 2, thread );
            Int( s, 3, q.frameId );
            Int( s, 4, q.function );
            Int( s, 5, q.file );
            Int( s, 6, q.line );
            Int( s, 7, q.runtime );
            Int( s, 8, q.flags );
            db.Run( s );
            break;
        }
        case QueueType::JnScriptStack: {
            auto& q = e.jnScriptStack;
            const auto time = Time( q.time );
            auto s = db.Prep( "INSERT INTO script_events VALUES(?,?,?,?,?,?,?,?,?)" );
            Int( s, 1, id );
            Int( s, 2, thread );
            Int( s, 3, uint64_t( time ) );
            Int( s, 4, q.primaryId );
            Int( s, 5, q.secondaryId );
            Int( s, 6, q.value );
            Int( s, 7, q.runtime );
            Int( s, 8, q.flags );
            Int( s, 9, q.kind );
            db.Run( s );
            info.lastNs = std::max( info.lastNs, time );
            break;
        }
        case QueueType::JnGfxDispatch:
        case QueueType::JnGfxEntity:
        case QueueType::JnGfxLink:
        case QueueType::JnRelation:
        case QueueType::JnRuntimeDomainState: {
            const auto time = Time( e.jnGfxDispatch.time );
            Auxiliary( id, e, time );
            if( t == QueueType::JnGfxEntity && e.jnGfxEntity.entityId && e.jnGfxEntity.kind == uint8_t( JnGfxEntityKind::ExplicitGpuPass ) ) CatalogPassTime( e.jnGfxEntity.entityId, time, 1 );
            if( t == QueueType::JnGfxLink && e.jnGfxLink.sourceId && e.jnGfxLink.relation == uint8_t( JnGfxRelation::RangeEvidenceComplete ) ) CatalogPassTime( e.jnGfxLink.sourceId, time, 2 );
            info.lastNs = std::max( info.lastNs, time );
            break;
        }
        case QueueType::SingleStringData:
            Require( !hasSingle, "Single-string staging overflow" );
            single.assign( reinterpret_cast<const char*>( bytes.data() ), bytes.size() );
            hasSingle = true;
            break;
        case QueueType::SecondStringData:
            Require( !hasSecond, "Second-string staging overflow" );
            second.assign( reinterpret_cast<const char*>( bytes.data() ), bytes.size() );
            hasSecond = true;
            break;
        case QueueType::ZoneText:
        case QueueType::ZoneValue: {
            std::string text;
            if( t == QueueType::ZoneText ) text = Take();
            const auto tid = Logical();
            const auto zone = MatchedTop( tid );
            if( !zone ) break;
            if( t == QueueType::ZoneValue )
            {
                char b[64];
                std::snprintf( b, sizeof( b ), "%llu [0x%llx]", (unsigned long long)e.zoneValue.value, (unsigned long long)e.zoneValue.value );
                text = b;
            }
            AppendText( tid, zone, id, std::move( text ) );
            break;
        }
        case QueueType::ZoneName: {
            auto name = Take();
            const auto tid = Logical();
            if( !MatchedTop( tid ) ) break;
            auto& f = TopFrame( CachedThread( tid ) );
            f.name = std::move( name );
            f.hasName = true;
            break;
        }
        case QueueType::ZoneColor: {
            const auto tid = Logical();
            if( !MatchedTop( tid ) ) break;
            auto& f = TopFrame( CachedThread( tid ) );
            f.color = ( uint32_t( e.zoneColor.b ) << 16 ) | ( uint32_t( e.zoneColor.g ) << 8 ) | e.zoneColor.r;
            f.hasColor = true;
            break;
        }
        case QueueType::FrameMarkMsg:
        case QueueType::FrameMarkMsgStart:
        case QueueType::FrameMarkMsgEnd: Frame( e.frameMark.name, Time( e.frameMark.time ), t == QueueType::FrameMarkMsg, t == QueueType::FrameMarkMsgEnd ); break;
        case QueueType::StringData:
        case QueueType::ThreadName:
        case QueueType::FrameName:
        case QueueType::PlotName:
        case QueueType::FiberName: {
            auto q = t == QueueType::StringData ? ServerQueryString : t == QueueType::ThreadName ? ServerQueryThreadString
                                                                  : t == QueueType::FrameName    ? ServerQueryFrameName
                                                                  : t == QueueType::FiberName    ? ServerQueryFiberName
                                                                                                 : ServerQueryPlotName;
            Response( q, e.stringTransfer.ptr );
            String( t, e.stringTransfer.ptr, { reinterpret_cast<const char*>( bytes.data() ), bytes.size() } );
            if( t == QueueType::PlotName ) PlotName( e.stringTransfer.ptr, { reinterpret_cast<const char*>( bytes.data() ), bytes.size() } );
            break;
        }
        case QueueType::ExternalName:
        case QueueType::ExternalThreadName: {
            auto s = db.Prep( "SELECT 1 FROM strings WHERE kind=? AND key=?" );
            Int( s, 1, uint8_t( t ) );
            Int( s, 2, e.stringTransfer.ptr );
            Require( !db.Step( s ), "Duplicate external name response" );
            sqlite3_reset( s );
            s = db.Prep( "SELECT 1 FROM strings WHERE kind=? AND key=?" );
            Int( s, 1, uint8_t( t == QueueType::ExternalName ? QueueType::ExternalThreadName : QueueType::ExternalName ) );
            Int( s, 2, e.stringTransfer.ptr );
            const bool paired = db.Step( s );
            sqlite3_reset( s );
            if( !paired ) Response( ServerQueryExternalName, e.stringTransfer.ptr );
            String( t, e.stringTransfer.ptr, { reinterpret_cast<const char*>( bytes.data() ), bytes.size() } );
            break;
        }
        case QueueType::TidToPid: {
            auto s = db.Prep( "INSERT OR IGNORE INTO thread_process VALUES(?,?)" );
            Int( s, 1, e.tidToPid.tid );
            Int( s, 2, e.tidToPid.pid );
            db.Run( s );
            break;
        }
        case QueueType::SourceLocation: {
            auto key = Response( ServerQuerySourceLocation, {} );
            auto s = db.Prep( "INSERT INTO sources(kind,key,nkey,fkey,pkey,line,color) VALUES(0,?,?,?,?,?,?)" );
            Int( s, 1, key );
            Int( s, 2, e.srcloc.name );
            Int( s, 3, e.srcloc.function );
            Int( s, 4, e.srcloc.file );
            Int( s, 5, e.srcloc.line );
            Int( s, 6, ( uint32_t( e.srcloc.b ) << 16 ) | ( uint32_t( e.srcloc.g ) << 8 ) | e.srcloc.r );
            db.Run( s );
            break;
        }
        case QueueType::SourceLocationPayload: {
            Require( pendingSource == 0 && bytes.size() >= 10, "Invalid allocated source" );
            uint32_t color, line;
            memcpy( &color, bytes.data(), 4 );
            memcpy( &line, bytes.data() + 4, 4 );
            auto p = bytes.subspan( 8 );
            auto end = std::find( p.begin(), p.end(), 0 );
            Require( end != p.end(), "Unterminated source function" );
            std::string func( reinterpret_cast<const char*>( p.data() ), end - p.begin() );
            p = p.subspan( func.size() + 1 );
            end = std::find( p.begin(), p.end(), 0 );
            Require( end != p.end(), "Unterminated source file" );
            std::string file( reinterpret_cast<const char*>( p.data() ), end - p.begin() );
            p = p.subspan( file.size() + 1 );
            auto s = db.Prep( "INSERT INTO sources(kind,key,line,color,name,function,file) VALUES(1,?,?,?,?,?,?)" );
            Int( s, 1, id );
            Int( s, 2, line );
            Int( s, 3, ( ( color & 0xff0000 ) >> 16 ) | ( color & 0xff00 ) | ( ( color & 0xff ) << 16 ) );
            Text( s, 4, { reinterpret_cast<const char*>( p.data() ), p.size() } );
            Text( s, 5, func );
            Text( s, 6, file );
            db.Run( s );
            pendingSource = id;
            break;
        }
        case QueueType::CallstackPayload:
        case QueueType::CallstackSampleDictionary: {
            Require( bytes.size() % 8 == 0 && !bytes.empty(), "Invalid callstack payload" );
            Require( pendingStack == 0, "Callstack staging overflow" );
            auto s = db.Prep( "INSERT OR IGNORE INTO stacks(data) VALUES(?)" );
            Blob( s, 1, bytes );
            db.Run( s );
            s = db.Prep( "SELECT id FROM stacks WHERE data=?" );
            Blob( s, 1, bytes );
            db.Step( s );
            uint64_t stack = sqlite3_column_int64( s, 0 );
            sqlite3_reset( s );
            if( t == QueueType::CallstackPayload ) pendingStack = stack;
            else
            {
                Require( e.stringTransfer.ptr > 0 && e.stringTransfer.ptr <= UINT32_MAX, "Invalid sample dictionary identifier" );
                s = db.Prep( "INSERT INTO sample_dictionary VALUES(?,?)" );
                Int( s, 1, e.stringTransfer.ptr );
                Int( s, 2, stack );
                db.Run( s );
            }
            break;
        }
        case QueueType::Callstack:
        case QueueType::CallstackAlloc: {
            Require( pendingStack != 0, "Missing CPU stack payload" );
            auto& t = CachedThread( Logical() );
            if( t.present )
            {
                t.nextstack = pendingStack;
                t.dirty = true;
            }
            pendingStack = 0;
            break;
        }
        case QueueType::CallstackSerial:
            if( !pendingStack ) throw std::runtime_error( "Missing serial stack payload at event " + std::to_string( id ) );
            serialStack = pendingStack;
            pendingStack = 0;
            break;
        case QueueType::JnCallsiteDefinition: {
            Require( !( e.jnCallsiteDefinition.flags & 1 ) || serialStack, "Missing callsite stack" );
            auto s = db.Prep( "INSERT INTO callsites VALUES(?,?,?,?,?,?,?,?,?)" );
            Int( s, 1, e.jnCallsiteDefinition.callsiteId );
            Int( s, 2, e.jnCallsiteDefinition.flags & 1 ? serialStack : 0 );
            Int( s, 3, e.jnCallsiteDefinition.provenance );
            Int( s, 4, e.jnCallsiteDefinition.unavailableReason );
            Int( s, 5, id );
            Int( s, 6, e.jnCallsiteDefinition.thread );
            Int( s, 7, e.jnCallsiteDefinition.srcloc );
            Int( s, 8, e.jnCallsiteDefinition.domain );
            Int( s, 9, e.jnCallsiteDefinition.flags );
            db.Run( s );
            knownCallsites.insert( e.jnCallsiteDefinition.callsiteId );
            s = db.Prep( "INSERT OR IGNORE INTO dirty_jobs SELECT job FROM job_stage_callsites WHERE callsite=?" );
            Int( s, 1, e.jnCallsiteDefinition.callsiteId );
            db.Run( s );
            serialStack = 0;
            break;
        }
        case QueueType::CallstackFrameSize:
            Response( ServerQueryCallstackFrame, e.callstackFrameSize.ptr );
            Require( symbolRemaining == 0, "Overlapping symbol definitions" );
            symbolFrame = e.callstackFrameSize.ptr;
            symbolRemaining = e.callstackFrameSize.size;
            symbolOrdinal = 0;
            image = Take();
            break;
        case QueueType::CallstackFrame: {
            Require( symbolRemaining && hasSecond, "Missing symbol strings" );
            auto name = Take();
            auto s = db.Prep( "INSERT INTO stackframes VALUES(?,?,?,?,?,?,?,?)" );
            Int( s, 1, symbolFrame );
            Int( s, 2, symbolOrdinal++ );
            Text( s, 3, name );
            Text( s, 4, second );
            Text( s, 5, image );
            Int( s, 6, e.callstackFrame.line );
            Int( s, 7, e.callstackFrame.symAddr );
            Int( s, 8, e.callstackFrame.symLen );
            db.Run( s );
            SymbolCandidate( e.callstackFrame, name, second, image, symbolRemaining > 1 );
            hasSecond = false;
            --symbolRemaining;
            break;
        }
        case QueueType::FiberEnter: FiberEnter( id, e.fiberEnter ); break;
        case QueueType::FiberLeave: FiberLeave( e.fiberLeave ); break;
        case QueueType::CallstackSample:
        case QueueType::CallstackSampleContextSwitch: Sample( id, e.callstackSample, false, 0, t == QueueType::CallstackSampleContextSwitch ); break;
        case QueueType::CallstackSampleRef:
        case QueueType::CallstackSampleContextSwitchRef: Sample( id, e.callstackSampleRef, true, e.callstackSampleRef.stackId, t == QueueType::CallstackSampleContextSwitchRef ); break;
        case QueueType::ContextSwitch: Switch( id, e.contextSwitch ); break;
        case QueueType::ThreadWakeup: Wakeup( id, e.threadWakeup ); break;
        case QueueType::JnJobStage:
            JobStage( id, e.jnJobStage );
            break;
        case QueueType::JnIoStage: IoStage( id, e.jnIoStage ); break;
        case QueueType::MemAlloc:
        case QueueType::MemAllocNamed:
        case QueueType::JnMemAllocCallsiteNamed:
        case QueueType::MemAllocCallstack:
        case QueueType::MemAllocCallstackNamed:
            MemoryAlloc( id, e.memAlloc, t == QueueType::MemAllocNamed || t == QueueType::MemAllocCallstackNamed || t == QueueType::JnMemAllocCallsiteNamed, t == QueueType::MemAllocCallstack || t == QueueType::MemAllocCallstackNamed, t == QueueType::JnMemAllocCallsiteNamed ? e.jnMemAllocCallsite.callsiteId : 0 );
            break;
        case QueueType::MemFree:
        case QueueType::MemFreeNamed:
        case QueueType::MemFreeCallstack:
        case QueueType::MemFreeCallstackNamed:
            MemoryFree( id, e.memFree, t == QueueType::MemFreeNamed || t == QueueType::MemFreeCallstackNamed, t == QueueType::MemFreeCallstack || t == QueueType::MemFreeCallstackNamed );
            break;
        case QueueType::MemDiscard:
        case QueueType::MemDiscardCallstack:
            MemoryDiscard( e.memDiscard, t == QueueType::MemDiscardCallstack );
            break;
        case QueueType::MemNamePayload:
            Require( memoryName == 0 && e.memName.name != 0, "Overlapping memory pool payload" );
            memoryName = e.memName.name;
            break;
        case QueueType::LockWait:
            LockEvent( id, e.lockWait.id, e.lockWait.thread, e.lockWait.time, 0 );
            break;
        case QueueType::LockObtain: LockEvent( id, e.lockObtain.id, e.lockObtain.thread, e.lockObtain.time, 1 ); break;
        case QueueType::LockSharedWait: LockEvent( id, e.lockWait.id, e.lockWait.thread, e.lockWait.time, 3 ); break;
        case QueueType::LockSharedObtain: LockEvent( id, e.lockObtain.id, e.lockObtain.thread, e.lockObtain.time, 4 ); break;
        case QueueType::LockRelease: LockEvent( id, e.lockRelease.id, 0, e.lockRelease.time, 2 ); break;
        case QueueType::LockSharedRelease: LockEvent( id, e.lockReleaseShared.id, e.lockReleaseShared.thread, e.lockReleaseShared.time, 5 ); break;
        case QueueType::LockAnnounce: LockAnnounce( e.lockAnnounce ); break;
        case QueueType::LockTerminate: {
            auto s = db.Prep( "UPDATE locks SET terminate=? WHERE id=?" );
            Int( s, 1, uint64_t( Time( e.lockTerminate.time ) ) );
            Int( s, 2, e.lockTerminate.id );
            db.Run( s );
            Require( sqlite3_changes( db.handle ) == 1, "Unknown lock terminate" );
            break;
        }
        case QueueType::LockMark: {
            auto s = db.Prep( "SELECT id FROM lock_events WHERE lock=? AND thread=? AND type IN(0,1,3,4) ORDER BY id DESC LIMIT 1" );
            Int( s, 1, e.lockMark.id );
            Int( s, 2, e.lockMark.thread );
            Require( db.Step( s ), "Lock mark has no acquisition or wait" );
            const uint64_t event = sqlite3_column_int64( s, 0 );
            sqlite3_reset( s );
            s = db.Prep( "UPDATE lock_events SET source=? WHERE id=?" );
            Int( s, 1, e.lockMark.srcloc );
            Int( s, 2, event );
            db.Run( s );
            break;
        }
        case QueueType::SymbolInformation: SymbolInformation( id, e.symbolInformation ); break;
        case QueueType::SymbolCode: SymbolCode( e.stringTransfer.ptr, bytes ); break;
        case QueueType::SourceCode: SourceCode( uint32_t( e.stringTransfer.ptr ), bytes, true ); break;
        case QueueType::AckSourceCodeNotAvailable: SourceCode( e.sourceCodeNotAvailable.id, {}, false ); break;
        case QueueType::AckSymbolCodeNotAvailable: ++symbolCodeAcks; break;
        case QueueType::HwSampleCpuCycle:
        case QueueType::HwSampleInstructionRetired:
        case QueueType::HwSampleCacheReference:
        case QueueType::HwSampleCacheMiss:
        case QueueType::HwSampleBranchRetired:
        case QueueType::HwSampleBranchMiss: HardwareSample( id, e.hwSample, uint8_t( t ) - uint8_t( QueueType::HwSampleCpuCycle ) ); break;
        case QueueType::GpuContextName: {
            Gpu( e.gpuContextName.context );
            auto s = db.Prep( "UPDATE gpu_contexts SET name=? WHERE id=?" );
            auto name = Take();
            Text( s, 1, name );
            Int( s, 2, e.gpuContextName.context );
            db.Run( s );
            break;
        }
        case QueueType::GpuAnnotationName: {
            Gpu( e.gpuAnnotationName.context );
            auto s = db.Prep( "INSERT INTO gpu_notes VALUES(?,?,?,?) ON CONFLICT(context,note) DO UPDATE SET name=excluded.name" );
            auto name = Take();
            Int( s, 1, id );
            Int( s, 2, e.gpuAnnotationName.context );
            Int( s, 3, uint64_t( e.gpuAnnotationName.noteId ) );
            Text( s, 4, name );
            db.Run( s );
            break;
        }
        case QueueType::GpuZoneAnnotation: {
            Gpu( e.zoneAnnotation.context );
            auto s = db.Prep( "INSERT INTO gpu_annotations VALUES(?,?,?,?,?) ON CONFLICT(context,query,note) DO UPDATE SET value=excluded.value" );
            Int( s, 1, id );
            Int( s, 2, e.zoneAnnotation.context );
            Int( s, 3, e.zoneAnnotation.queryId );
            Int( s, 4, uint64_t( e.zoneAnnotation.noteId ) );
            Blob( s, 5, { reinterpret_cast<const uint8_t*>( &e.zoneAnnotation.value ), sizeof( double ) } );
            db.Run( s );
            break;
        }
        case QueueType::LockName: {
            const auto name = Take();
            auto s = db.Prep( "UPDATE locks SET name=? WHERE id=?" );
            Text( s, 1, name );
            Int( s, 2, e.lockName.id );
            db.Run( s );
            Require( sqlite3_changes( db.handle ) == 1, "Unknown lock name" );
            break;
        }
        case QueueType::Message:
        case QueueType::MessageColor:
        case QueueType::MessageCallstack:
        case QueueType::MessageColorCallstack:
        case QueueType::MessageLiteral:
        case QueueType::MessageLiteralColor:
        case QueueType::MessageLiteralCallstack:
        case QueueType::MessageLiteralColorCallstack:
        case QueueType::MessageAppInfo: Message( id, e, t ); break;
        case QueueType::FrameImageData: ImageData( bytes ); break;
        case QueueType::FrameImage: FrameImage( id, e.frameImage ); break;
        case QueueType::CallstackAllocPayload: CombinedStack( bytes ); break;
        default: break;
        }
        if( ( id & 4095 ) == 0 )
        {
            Require( uint64_t( db.Scalar( "PRAGMA page_count" ) ) * uint64_t( db.Scalar( "PRAGMA page_size" ) ) + ( blockFile ? blockFile->Size() : 0 ) <= options.diskBytes, "Store disk budget exceeded" );
        }
    }

public:
    void SetBlockFile( BlockFile* file ) { blockFile = file; }
    Ingest( Db& d, StoreInfo& i, const StoreOptions& o, bool live = false )
        : db( d )
        , info( i )
        , options( o )
        , incremental( live )
    {
    }
    // Server queries are the only records the query pass reads.
    static bool QueryRecord( const RecordInfo& r ) { return r.type == RecordType::ServerToClient && ( r.flags & RecordFlagServerQuery ); }
    void Queries( const RecordInfo& r, std::span<const uint8_t> p )
    {
        if( !QueryRecord( r ) ) return;
        Require( p.size() % ServerQueryPacketSize == 0, "Truncated server transcript" );
        uint64_t ordinal = 0;
        while( !p.empty() )
        {
            ServerQueryPacket q;
            memcpy( &q, p.data(), sizeof( q ) );
            Require( q.type <= ServerQueryDataTransferPart, "Unknown server query" );
            auto s = db.Prep( "INSERT INTO queries(seq,ord,type,key,extra) VALUES(?,?,?,?,?)" );
            Int( s, 1, r.sequence );
            Int( s, 2, ordinal++ );
            Int( s, 3, q.type );
            Int( s, 4, q.ptr );
            Int( s, 5, q.extra );
            db.Run( s );
            CodeQuery( q );
            p = p.subspan( sizeof( q ) );
            if((++queryCount&4095)==0)
            {
                Require(uint64_t(db.Scalar("PRAGMA page_count"))*uint64_t(db.Scalar("PRAGMA page_size"))+(blockFile?blockFile->Size():0)<=options.diskBytes,"Store disk budget exceeded");
            }
        }
    }
    uint64_t queryCount = 0;
    void Consume( const RecordInfo& r, std::span<const uint8_t> p ) { Consume( r, p, nullptr ); }
    // `frame` is the record's frame when RecordPrefetch decoded it ahead with
    // a FrameDecoder of the same journal, empty if it did not decode. Every
    // record of a journal is consumed either way.
    void Consume( const RecordInfo& r, std::span<const uint8_t> p, const std::span<const uint8_t>* frame )
    {
        TranscriptRecord( r, p );
        if( r.type == RecordType::SessionEnd )
        {
            Require( p.size() == 32, "Invalid SessionEnd" );
            uint32_t reason;
            memcpy( &reason, p.data() + 4, 4 );
            info.cleanEnd = reason == uint32_t( ProtocolCloseReason::CaptureComplete );
            return;
        }
        if( r.type == RecordType::Diagnostic && ( r.flags & RecordFlagLocalControl ) )
        {
            Require( !p.empty(), "Empty drain control" );
            if( p[0] == 1 || p[0] == 2 || p[0] == 4 ) draining = true;
            return;
        }
        if( r.type != RecordType::ClientToServer ) return;
        if( r.flags & RecordFlagHandshake )
        {
            if( p.size() == 1 )
            {
                Require( p[0] == HandshakeWelcome, "Unsupported handshake" );
                return;
            }
            if( p.size() == WelcomeMessageSize )
            {
                Require( !welcomed, "Duplicate welcome" );
                WelcomeMessage w;
                memcpy( &w, p.data(), sizeof( w ) );
                Require( std::isfinite( w.timerMul ) && w.timerMul > 0, "Invalid timer multiplier" );
                base = w.initBegin;
                multiplier = w.timerMul;
                onDemand = w.flags & WelcomeFlag::OnDemand;
                ignoreMemoryFaults = onDemand || ( w.flags & WelcomeFlag::IgnoreMemFaults );
                combineSamples = w.flags & WelcomeFlag::CombineSamples;
                info.program.assign( w.programName, std::find( w.programName, w.programName + sizeof( w.programName ), '\0' ) );
                auto header = db.Prep( "INSERT INTO capture_metadata VALUES('welcome',?)" );
                Blob( header, 1, p );
                db.Run( header );
                welcomed = true;
                Frame( 0, 0, true );
                Frame( 0, Time( w.initEnd ), true );
                return;
            }
            if( p.size() == OnDemandPayloadMessageSize )
            {
                Require( welcomed && onDemand, "Unexpected on-demand header" );
                OnDemandPayloadMessage w;
                memcpy( &w, p.data(), sizeof( w ) );
                auto header = db.Prep( "INSERT INTO capture_metadata VALUES('on_demand',?)" );
                Blob( header, 1, p );
                db.Run( header );
                frameOffset = w.frames;
                Frame( 0, Time( I( w.currentTime ) ), true );
                return;
            }
            throw std::runtime_error( "Unknown client handshake bytes" );
        }
        Require( welcomed && ( r.flags & RecordFlagCompressedFrame ) && p.size() >= 4, "Invalid client frame" );
        uint32_t size;
        memcpy( &size, p.data(), 4 );
        Require( size && size <= LZ4Size && p.size() == size + 4, "Invalid LZ4 envelope" );
        auto events = frame ? *frame : std::span<const uint8_t>( reinterpret_cast<const uint8_t*>( decoded.data() ), frames.Decode( p, decoded ) );
        Require( !events.empty(), "LZ4 frame decode failed" );
        while( !events.empty() )
        {
            ProtocolEventView e;
            Require( ReadProtocolEvent( events, e ), "Malformed protocol event" );
            Event( e );
        }
    }
    void Finish( bool partial = false )
    {
        Require( welcomed || partial, "No welcome metadata" );
        Require( !symbolRemaining || partial, "Incomplete symbol definition" );
        Require( partial || transfer.empty() || transferOffset == transfer.size(), "Incomplete code query transfer" );
        FinishZones();
        FlushThreads();
        FlushJobs();
        FlushPlots();
        FlushPools();
        FlushAllPackets();
        FlushGpu();
        FlushZoneTree();
        if( !incremental ) ReleaseBuffers();
        FlushTaskTimes();
        if( !deferredIndexes.empty() )
        {
            Stage( "Build store indexes" );
            // CREATE INDEX sorts with helper threads. SQLite sizes every
            // in-memory sort run like the page cache and holds one run per
            // thread plus the one being filled, so with 3 helpers and a quarter
            // of the cache the runs together stay within the cache budget.
            // Measured on the 32M-zone store: 3 helpers with 8 MiB runs build
            // as fast as 4 with 32 MiB runs.
            const auto cacheKiB = options.cacheBytes / 1024;
            db.Exec( "PRAGMA threads=3;PRAGMA cache_size=-" + std::to_string( cacheKiB / 4 ) );
            for( const auto& sql : deferredIndexes ) db.Exec( sql );
            db.Exec( "PRAGMA threads=0;PRAGMA cache_size=-" + std::to_string( cacheKiB ) );
            deferredIndexes.clear();
        }
        if( !incremental && !zoneRangeLoaded )
        {
            Stage( "Index zone ranges" );
            LoadRanges( GpuRanges );
            zoneRangeLoaded = true;
            Stage( "Summarize store" );
        }
        FinishCodeQueries( partial );
        db.Exec("UPDATE plots SET name=COALESCE((SELECT value FROM strings s JOIN system_plots p ON p.key=s.key WHERE p.kind=3 AND p.plot=plots.id AND s.kind="+std::to_string(uint8_t(QueueType::StringData))+"),'???') WHERE kind=3");
        FinishTranscript( partial );
        Require( partial || db.Scalar( "SELECT COUNT(*) FROM frame_image_pending" ) == 0, "Incomplete Frame Image payload" );
        // Resolve final definitions in protocol order, retaining the native
        // callsite identity when a later exact stack supplements the Job.
        // Live stores reset their dirty Jobs first. In an offline build every
        // Job is dirty and still has the default stack values.
        if( incremental ) db.Exec( "UPDATE jobs SET stack=0,callsite=0,stack_provenance=3,stack_reason=0 WHERE id IN(SELECT id FROM dirty_jobs)" );
        auto stacks = db.Prep( incremental ? "SELECT s.job,s.stage,s.stack,s.callsite,c.stack,c.provenance,c.reason FROM job_schedule_stages s LEFT JOIN callsites c ON c.id=s.callsite WHERE s.job IN(SELECT id FROM dirty_jobs) ORDER BY s.id" : "SELECT s.job,s.stage,s.stack,s.callsite,c.stack,c.provenance,c.reason FROM job_schedule_stages s LEFT JOIN callsites c ON c.id=s.callsite ORDER BY s.id" );
        while( db.Step( stacks ) )
        {
            const auto job = uint64_t( sqlite3_column_int64( stacks, 0 ) );
            if( sqlite3_column_int( stacks, 1 ) == int( JnJobStage::ScheduleCallstack ) )
            {
                auto update = db.Prep( "UPDATE jobs SET stack=?,stack_provenance=2 WHERE id=?" );
                Int( update, 1, sqlite3_column_int64( stacks, 2 ) );
                Int( update, 2, job );
                db.Run( update );
            }
            else if( sqlite3_column_type( stacks, 4 ) == SQLITE_NULL )
            {
                auto update = db.Prep( "UPDATE jobs SET callsite=? WHERE id=?" );
                Int( update, 1, sqlite3_column_int64( stacks, 3 ) );
                Int( update, 2, job );
                db.Run( update );
            }
            else
            {
                // The callsite and its definition's stack, in one rewrite of
                // the row: a Job row is wide, and every Job has such a stage.
                auto update = db.Prep( "UPDATE jobs SET callsite=?1,stack=?2,stack_provenance=?3,stack_reason=CASE WHEN ?4!=0 THEN ?4 ELSE stack_reason END WHERE id=?5" );
                Int( update, 1, sqlite3_column_int64( stacks, 3 ) );
                Int( update, 2, sqlite3_column_int64( stacks, 4 ) );
                Int( update, 3, sqlite3_column_int64( stacks, 5 ) );
                // The Worker keeps a previously reported unavailable
                // reason when a later definition has no reason.
                Int( update, 4, sqlite3_column_int64( stacks, 6 ) );
                Int( update, 5, job );
                db.Run( update );
            }
        }
        sqlite3_reset( stacks );
        info.openZones = openZoneCount;
        info.samples = db.Scalar( "SELECT COUNT(*) FROM samples" );
        info.pendingSamples = db.Scalar( "SELECT COUNT(*) FROM sample_pending" );
        info.contextRegions = db.Scalar( "SELECT COUNT(*) FROM context_regions" );
        info.cpuRegions = db.Scalar( "SELECT COUNT(*) FROM cpu_regions" );
        info.messages = db.Scalar( "SELECT COUNT(*) FROM messages WHERE app=0" );
        info.plotPoints = db.Scalar( "SELECT COUNT(*) FROM plot_points" );
        info.lockEvents = db.Scalar( "SELECT COUNT(*) FROM lock_events" );
        info.jobs = db.Scalar( "SELECT COUNT(*) FROM jobs" );
        info.ioRequests = db.Scalar( "SELECT COUNT(*) FROM io_requests" );
        info.scriptEvents = db.Scalar( "SELECT COUNT(*) FROM script_events" );
        info.allocations = db.Scalar( "SELECT COUNT(*) FROM allocations" );
        info.activeAllocations = db.Scalar( "SELECT COUNT(*) FROM memory_active" );
        info.gpuReferencePasses = db.Scalar( "SELECT COUNT(*) FROM gpu_ref_passes" );
        info.gpuReferenceUses = db.Scalar( "SELECT COALESCE(SUM(count),0) FROM gpu_ref_packets" );
        info.gpuReferenceEnds = db.Scalar( "SELECT COUNT(*) FROM gpu_ref_ends" );
        CatalogFinish( partial );
        info.catalogGenerations = db.Scalar( "SELECT COUNT(*) FROM catalog_generations" );
        info.catalogRecords = db.Scalar( "SELECT COUNT(*) FROM catalog_records" );
        info.catalogPresent = db.Scalar( "SELECT present FROM catalog_state" ) != 0;
        info.catalogValid = db.Scalar( "SELECT valid FROM catalog_state" ) != 0;
        if( info.allocations )
        {
            // An open zone ends, for display, where its last direct child
            // ended, or where that child is displayed to end when it is open.
            for( const auto& [tid, frames] : openStacks )
            {
                I above = 0;
                for( auto f = frames.rbegin(); f != frames.rend(); ++f )
                {
                    I end = f->start;
                    if( f->lastChild ) end = f->lastChildClosed ? f->lastChildEnd : ( f != frames.rbegin() && ( f - 1 )->id == f->lastChild ? above : end );
                    auto insert = db.Prep( "INSERT OR REPLACE INTO zone_display_end VALUES(?,?)" );
                    Int( insert, 1, f->id );
                    Int( insert, 2, uint64_t( end ) );
                    db.Run( insert );
                    above = end;
                }
            }
        }
        if( incremental )
        {
            db.Exec( "INSERT OR REPLACE INTO job_range SELECT id,0,2147483647 FROM dirty_jobs;INSERT OR REPLACE INTO io_range SELECT id,0,2147483647 FROM dirty_io;" );
            auto jobRanges = db.Prep( "SELECT id,COALESCE(first_time,schedule),completed FROM jobs WHERE id IN(SELECT id FROM dirty_jobs)" );
            while( db.Step( jobRanges ) )
            {
                const uint64_t id = sqlite3_column_int64( jobRanges, 0 );
                std::optional<I> end;
                if( sqlite3_column_type( jobRanges, 2 ) != SQLITE_NULL ) end = sqlite3_column_int64( jobRanges, 2 );
                Range( "UPDATE job_range SET mint=?2,maxt=?3 WHERE id=?1", id, sqlite3_column_int64( jobRanges, 1 ), end );
            }
            sqlite3_reset( jobRanges );
            auto ioRanges = db.Prep( "SELECT id,COALESCE(first_time,queued),end FROM io_requests WHERE id IN(SELECT id FROM dirty_io)" );
            while( db.Step( ioRanges ) )
            {
                const uint64_t id = sqlite3_column_int64( ioRanges, 0 );
                std::optional<I> end;
                if( sqlite3_column_type( ioRanges, 2 ) != SQLITE_NULL ) end = sqlite3_column_int64( ioRanges, 2 );
                Range( "UPDATE io_range SET mint=?2,maxt=?3 WHERE id=?1", id, sqlite3_column_int64( ioRanges, 1 ), end );
            }
            sqlite3_reset( ioRanges );
        }
        else
        {
            // An offline build calls Finish() once, so every Job and I/O
            // request is dirty and its final interval is indexed directly,
            // as is every allocation.
            LoadRanges( JobRanges );
            LoadRanges( IoRanges );
            LoadRanges( AllocationRanges );
        }
        db.Exec( "DELETE FROM dirty_jobs;DELETE FROM dirty_io;" );
        ++taskGeneration; // Known tasks no longer have their dirty mark.
        info.incompleteGpuZones = db.Scalar( "SELECT COUNT(*) FROM gpu_zones WHERE gpu_start IS NULL OR gpu_end IS NULL OR cpu_end IS NULL" );
        info.unresolvedDefinitions = 0;
        for( const auto& [source, count] : zoneSources )
        {
            auto q = db.Prep( "SELECT 1 FROM sources WHERE kind=? AND key=?" );
            Int( q, 1, source.first );
            Int( q, 2, source.second );
            if( !db.Step( q ) ) info.unresolvedDefinitions += count;
            sqlite3_reset( q );
        }
        const auto kind = std::to_string( uint8_t( QueueType::StringData ) );
        info.unresolvedDefinitions += db.Scalar( "SELECT count(*) FROM sources s WHERE s.kind=0 AND ((s.nkey!=0 AND NOT EXISTS(SELECT 1 FROM strings WHERE kind=" + kind + " AND key=s.nkey)) OR (s.fkey!=0 AND NOT EXISTS(SELECT 1 FROM strings WHERE kind=" + kind + " AND key=s.fkey)) OR (s.pkey!=0 AND NOT EXISTS(SELECT 1 FROM strings WHERE kind=" + kind + " AND key=s.pkey)))" );
        info.unresolvedDefinitions += db.Scalar( "SELECT COUNT(*) FROM gpu_zones z LEFT JOIN sources s ON s.kind=z.skind AND s.key=z.skey WHERE s.key IS NULL" );
        info.unresolvedDefinitions += symbolRemaining + ( transfer.empty() ? 0 : 1 );
        info.unresolvedDefinitions += db.Scalar( "SELECT COUNT(*) FROM queries WHERE used=0 AND type IN(11,12,13)" );
    }
};
void ReadPayload( std::ifstream& f, const RecordInfo& r, std::vector<uint8_t>& p )
{
    Require( r.payloadSize <= uint64_t( LZ4Size ) + 4, "Record exceeds decoder budget" );
    p.resize( size_t( r.payloadSize ) );
    f.clear();
    f.seekg( std::streamoff( r.offset + RecordHeaderSize ) );
    f.read( reinterpret_cast<char*>( p.data() ), p.size() );
    Require( bool( f ), "Record read failed" );
}
std::vector<uint8_t> Payload( std::ifstream& f, const RecordInfo& r )
{
    std::vector<uint8_t> p;
    ReadPayload( f, r, p );
    return p;
}
// Reads journal records ahead of the ingestion on a thread of its own: the
// journal reads, record checks and frame decoding overlap the ingestion,
// which takes the records in order. At most Depth records wait, each with
// its payload and decoded frame.
class RecordPrefetch
{
public:
    struct Item
    {
        RecordInfo record;
        std::vector<uint8_t> payload;
        std::vector<char> frame = std::vector<char>( TargetFrameSize );
        size_t frameSize = 0;
        bool decoded = false;
        // The `frame` argument of Ingest::Consume.
        const std::span<const uint8_t>* Frame()
        {
            span = { reinterpret_cast<const uint8_t*>( frame.data() ), frameSize };
            return decoded ? &span : nullptr;
        }

    private:
        std::span<const uint8_t> span;
    };
    explicit RecordPrefetch( const std::filesystem::path& p )
        : input( p, std::ios::binary )
    {
        Require( bool( input ), "Cannot open journal for reading" );
        for( auto& item : items ) free.push_back( &item );
    }
    RecordPrefetch( const RecordPrefetch& ) = delete;
    RecordPrefetch& operator=( const RecordPrefetch& ) = delete;
    ~RecordPrefetch() { Stop(); }
    // Runs `source( *this )` on the worker; it calls Emit() for every record,
    // in journal order.
    template<class F>
    void Start( F&& source )
    {
        worker = std::thread( [this, source = std::forward<F>( source )]() mutable { Run( [&] { source( *this ); } ); } );
    }
    // The worker reads the records the ingestion asks for with Request().
    void StartRequests()
    {
        worker = std::thread( [this] { Run( [this] { Serve(); } ); } );
    }
    // On the worker: reads record `r` and decodes its frame into a free slot.
    void Emit( const RecordInfo& r )
    {
        Item* item;
        {
            std::unique_lock l( lock );
            changed.wait( l, [&] { return stopping.load() || !free.empty(); } );
            if( stopping ) throw Stopped();
            item = free.back();
            free.pop_back();
        }
        item->record = r;
        ReadPayload( input, r, item->payload );
        item->decoded = FrameDecoder::Carries( r, item->payload );
        item->frameSize = item->decoded ? decoder.Decode( item->payload, item->frame ) : 0;
        {
            std::lock_guard l( lock );
            ready.push_back( item );
        }
        changed.notify_all();
    }
    void Request( const RecordInfo& r )
    {
        {
            std::lock_guard l( lock );
            requests.push_back( r );
        }
        changed.notify_all();
    }
    bool Stopping() const { return stopping.load(); }
    // The next record in order, or nullptr when the source has ended; rethrows
    // a failure of the worker once the records before it were taken.
    Item* Next()
    {
        std::unique_lock l( lock );
        changed.wait( l, [&] { return !ready.empty() || done; } );
        if( ready.empty() )
        {
            if( error ) std::rethrow_exception( error );
            return nullptr;
        }
        auto* item = ready.front();
        ready.pop_front();
        return item;
    }
    void Release( Item* item )
    {
        {
            std::lock_guard l( lock );
            free.push_back( item );
        }
        changed.notify_all();
    }
    // Stops the worker and rethrows its failure, if any.
    void Finish()
    {
        Stop();
        if( error ) std::rethrow_exception( error );
    }

private:
    static constexpr size_t Depth = 4;
    struct Stopped
    {
    };
    template<class F>
    void Run( F&& body )
    {
        std::exception_ptr failure;
        try
        {
            body();
        }
        catch( const Stopped& )
        {
        }
        catch( ... )
        {
            if( !stopping ) failure = std::current_exception();
        }
        {
            std::lock_guard l( lock );
            error = failure;
            done = true;
        }
        changed.notify_all();
    }
    void Serve()
    {
        for( ;; )
        {
            RecordInfo r;
            {
                std::unique_lock l( lock );
                changed.wait( l, [&] { return stopping.load() || !requests.empty(); } );
                if( stopping ) throw Stopped();
                r = requests.front();
                requests.pop_front();
            }
            Emit( r );
        }
    }
    void Stop()
    {
        if( !worker.joinable() ) return;
        {
            std::lock_guard l( lock );
            stopping = true;
        }
        changed.notify_all();
        worker.join();
    }
    std::ifstream input;
    FrameDecoder decoder;
    std::array<Item, Depth> items;
    std::vector<Item*> free;
    std::deque<Item*> ready;
    std::deque<RecordInfo> requests;
    std::mutex lock;
    std::condition_variable changed;
    std::atomic<bool> stopping{ false };
    bool done = false;
    std::exception_ptr error;
    std::thread worker;
};
std::string SnapshotCallsiteJoin()
{
    return "LEFT JOIN callsites matched ON matched.id=(SELECT candidate.id FROM callsites candidate WHERE z.skind=0 AND candidate.source=z.skey AND candidate.stack=CASE WHEN z.stack!=0 THEN z.stack ELSE COALESCE(c.stack,0) END ORDER BY candidate.event LIMIT 1) ";
}
std::string ZoneColumns()
{
    auto kind = std::to_string( uint8_t( QueueType::StringData ) );
    return "SELECT z.id,z.thread,z.parent,z.depth,z.start,z.end,CASE WHEN z.stack!=0 THEN z.stack ELSE COALESCE(c.stack,0) END,COALESCE(matched.id,0),COALESCE(z.name,CASE WHEN s.key IS NULL THEN '???' WHEN z.skind=1 THEN COALESCE(NULLIF(s.name,''),s.function,'') WHEN s.nkey!=0 THEN COALESCE(n.value,'???') WHEN s.fkey=0 THEN '' ELSE COALESCE(f.value,'???') END),COALESCE(s.function,f.value,''),COALESCE(s.file,p.value,''),COALESCE(s.line,0),COALESCE(z.color,s.color,0),z.callsite,COALESCE(matched.provenance,3),COALESCE(matched.reason,0),z.child_time FROM temp.zone_page z LEFT JOIN sources s ON s.kind=z.skind AND s.key=z.skey LEFT JOIN strings n ON n.kind=" + kind + " AND n.key=s.nkey LEFT JOIN strings f ON f.kind=" + kind + " AND f.key=s.fkey LEFT JOIN strings p ON p.kind=" + kind + " AND p.key=s.pkey LEFT JOIN callsites c ON c.id=z.callsite " + SnapshotCallsiteJoin();
}
StoreZone ReadZone( Db& d, const BlockFile& file, ZoneBlockCache& cache, sqlite3_stmt* s )
{
    StoreZone z;
    z.id = sqlite3_column_int64( s, 0 );
    z.thread = sqlite3_column_int64( s, 1 );
    z.parent = sqlite3_column_int64( s, 2 );
    z.depth = sqlite3_column_int( s, 3 );
    z.start = sqlite3_column_int64( s, 4 );
    if( sqlite3_column_type( s, 5 ) != SQLITE_NULL ) z.end = sqlite3_column_int64( s, 5 );
    z.stack = sqlite3_column_int64( s, 6 );
    z.callsite = sqlite3_column_int64( s, 7 );
    z.name = Col( s, 8 );
    z.function = Col( s, 9 );
    z.file = Col( s, 10 );
    z.line = sqlite3_column_int( s, 11 );
    z.color = sqlite3_column_int( s, 12 );
    z.wireCallsite = sqlite3_column_int64( s, 13 );
    z.provenance = uint8_t( sqlite3_column_int( s, 14 ) );
    z.unavailableReason = uint8_t( sqlite3_column_int( s, 15 ) );
    z.childTime = sqlite3_column_int64( s, 16 );
    for( auto& t : ZoneTextRows( d, file, cache, z.thread, z.id, 0, SIZE_MAX ) )
    {
        if( z.text.size() + t.text.size() + 1 > d.options.resultBytes / 4 )
        {
            z.textMore = true;
            break;
        }
        if( !z.text.empty() ) z.text += '\n';
        z.text += t.text;
    }
    return z;
}
std::string GpuColumns()
{
    const auto kind = std::to_string( uint8_t( QueueType::StringData ) );
    return "SELECT z.id,z.context,z.thread,z.parent,z.depth,z.cpu_start,z.cpu_end,z.gpu_start,z.gpu_end,CASE WHEN z.stack!=0 THEN z.stack ELSE COALESCE(c.stack,0) END,COALESCE(matched.id,0),z.query,COALESCE(NULLIF(s.name,''),NULLIF(n.value,''),NULLIF(s.function,''),f.value,CASE WHEN s.key IS NULL THEN '???' ELSE '' END),COALESCE(s.function,f.value,''),COALESCE(s.file,p.value,''),COALESCE(s.line,0),COALESCE(s.color,0),COALESCE(matched.provenance,3),COALESCE(matched.reason,0),z.callsite FROM gpu_zones z LEFT JOIN sources s ON s.kind=z.skind AND s.key=z.skey LEFT JOIN strings n ON n.kind=" + kind + " AND n.key=s.nkey LEFT JOIN strings f ON f.kind=" + kind + " AND f.key=s.fkey LEFT JOIN strings p ON p.kind=" + kind + " AND p.key=s.pkey LEFT JOIN callsites c ON c.id=z.callsite " + SnapshotCallsiteJoin();
}
StoreGpuZone ReadGpu( sqlite3_stmt* s )
{
    StoreGpuZone z;
    z.id = sqlite3_column_int64( s, 0 );
    z.context = sqlite3_column_int64( s, 1 );
    z.thread = sqlite3_column_int64( s, 2 );
    z.parent = sqlite3_column_int64( s, 3 );
    z.depth = sqlite3_column_int( s, 4 );
    z.cpuStart = sqlite3_column_int64( s, 5 );
    if( sqlite3_column_type( s, 6 ) != SQLITE_NULL ) z.cpuEnd = sqlite3_column_int64( s, 6 );
    if( sqlite3_column_type( s, 7 ) != SQLITE_NULL ) z.gpuStart = sqlite3_column_int64( s, 7 );
    if( sqlite3_column_type( s, 8 ) != SQLITE_NULL ) z.gpuEnd = sqlite3_column_int64( s, 8 );
    z.stack = sqlite3_column_int64( s, 9 );
    z.callsite = sqlite3_column_int64( s, 10 );
    z.query = uint16_t( sqlite3_column_int( s, 11 ) );
    z.name = Col( s, 12 );
    z.function = Col( s, 13 );
    z.file = Col( s, 14 );
    z.line = sqlite3_column_int( s, 15 );
    z.color = sqlite3_column_int( s, 16 );
    z.provenance = uint8_t( sqlite3_column_int( s, 17 ) );
    z.unavailableReason = uint8_t( sqlite3_column_int( s, 18 ) );
    z.wireCallsite = sqlite3_column_int64( s, 19 );
    return z;
}
size_t GpuBytes( const StoreGpuZone& z ) { return sizeof( z ) + z.name.size() + z.function.size() + z.file.size(); }
double StoredDouble( sqlite3_stmt* s, int column )
{
    Require( sqlite3_column_bytes( s, column ) == 8, "Invalid scalar payload" );
    double v;
    memcpy( &v, sqlite3_column_blob( s, column ), 8 );
    return v;
}
std::string MessageColumns()
{
    return "SELECT m.id,m.thread,m.time,COALESCE(m.text,s.value,''),m.color,m.stack,m.app FROM messages m LEFT JOIN strings s ON s.kind=" + std::to_string( uint8_t( QueueType::StringData ) ) + " AND s.key=m.string_key ";
}
StoreMessage ReadMessage( sqlite3_stmt* s )
{
    StoreMessage m;
    m.id = sqlite3_column_int64( s, 0 );
    m.thread = sqlite3_column_int64( s, 1 );
    m.time = sqlite3_column_int64( s, 2 );
    m.text = Col( s, 3 );
    m.color = uint32_t( sqlite3_column_int64( s, 4 ) );
    m.stack = sqlite3_column_int64( s, 5 );
    m.appInfo = sqlite3_column_int( s, 6 ) != 0;
    return m;
}
std::string LockColumns( bool events )
{
    const auto kind = std::to_string( uint8_t( QueueType::StringData ) );
    const auto select = events ? "SELECT e.id,e.lock,e.thread,e.time,e.type,e.owner,e.count,e.wait,e.wait_shared,e.shared,COALESCE(NULLIF(s.name,''),n.value,NULLIF(s.function,''),f.value,''),COALESCE(s.function,f.value,''),COALESCE(s.file,p.value,''),COALESCE(s.line,0) FROM lock_events e " : "SELECT l.id,l.type,l.announce,l.terminate,COALESCE(l.name,NULLIF(s.name,''),n.value,NULLIF(s.function,''),f.value,''),COALESCE(s.function,f.value,''),COALESCE(s.file,p.value,''),COALESCE(s.line,0),l.contended,l.events,(SELECT COUNT(*) FROM lock_threads WHERE lock=l.id) FROM locks l ";
    return std::string( select ) + "LEFT JOIN sources s ON s.kind=0 AND s.key=" + ( events ? "e.source " : "l.source " ) + "LEFT JOIN strings n ON n.kind=" + kind + " AND n.key=s.nkey LEFT JOIN strings f ON f.kind=" + kind + " AND f.key=s.fkey LEFT JOIN strings p ON p.kind=" + kind + " AND p.key=s.pkey ";
}
StoreLockEvent ReadLockEvent( Db& d, sqlite3_stmt* s )
{
    StoreLockEvent e;
    e.id = sqlite3_column_int64( s, 0 );
    e.lock = sqlite3_column_int64( s, 1 );
    e.thread = sqlite3_column_int64( s, 2 );
    e.time = sqlite3_column_int64( s, 3 );
    e.type = uint8_t( sqlite3_column_int( s, 4 ) );
    e.owner = sqlite3_column_int64( s, 5 );
    e.count = uint32_t( sqlite3_column_int64( s, 6 ) );
    e.waitMask = sqlite3_column_int64( s, 7 );
    e.waitShared = sqlite3_column_int64( s, 8 );
    e.sharedMask = sqlite3_column_int64( s, 9 );
    e.name = Col( s, 10 );
    e.function = Col( s, 11 );
    e.file = Col( s, 12 );
    e.line = uint32_t( sqlite3_column_int( s, 13 ) );
    auto t = d.Prep( "SELECT thread,ordinal FROM lock_threads WHERE lock=? ORDER BY ordinal" );
    Int( t, 1, e.lock );
    while( d.Step( t ) )
    {
        const uint64_t thread = sqlite3_column_int64( t, 0 );
        const auto ordinal = sqlite3_column_int( t, 1 );
        Require( ordinal >= 0 && ordinal < 64, "Invalid lock thread ordinal" );
        const auto bit = uint64_t( 1 ) << ordinal;
        if( e.waitMask & bit ) e.waiters.push_back( thread );
        if( e.waitShared & bit ) e.sharedWaiters.push_back( thread );
        if( e.sharedMask & bit ) e.sharedOwners.push_back( thread );
    }
    sqlite3_reset( t );
    return e;
}
struct Manifest
{
    std::string data, source, blocks;
    uint32_t schema = 0;
    uint64_t blockBytes = 0;
    // File identities recorded at build time, empty when absent.
    std::string dataIdentity, blocksIdentity, sourceIdentity;
};
// Size, last write time, volume and file id of a file, as text. A file that
// keeps all four was not rewritten since they were recorded.
std::string FileIdentity( const std::filesystem::path& p )
{
#ifdef _WIN32
    const HANDLE f = CreateFileW( p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );
    Require( f != INVALID_HANDLE_VALUE, "Cannot identify file" );
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    FILE_ID_INFO id{};
    const bool ok = GetFileInformationByHandleEx( f, FileBasicInfo, &basic, sizeof( basic ) ) && GetFileInformationByHandleEx( f, FileStandardInfo, &standard, sizeof( standard ) ) && GetFileInformationByHandleEx( f, FileIdInfo, &id, sizeof( id ) );
    CloseHandle( f );
    Require( ok, "Cannot identify file" );
    std::array<uint8_t, 16> fileId;
    memcpy( fileId.data(), id.FileId.Identifier, fileId.size() );
    return std::to_string( standard.EndOfFile.QuadPart ) + ":" + std::to_string( basic.LastWriteTime.QuadPart ) + ":" + std::to_string( id.VolumeSerialNumber ) + ":" + Hex( fileId );
#else
    return {};
#endif
}
std::string JobColumns()
{
    const auto kind = std::to_string( uint8_t( QueueType::StringData ) );
    return "SELECT j.id,j.event,j.packed,j.schedule,j.thread,j.type,j.count,j.grain,j.flow,j.origin,j.kind,j.flags,j.schema,j.expected,j.orphan,j.boundary,j.ready,j.ready_lane,j.ready_flags,j.queue,j.queue_lane,j.first_run,j.completed,j.execution,j.wait,j.help,j.spin,j.sleep,j.dispatches,j.steals,j.range_steals,j.active_dispatches,j.retries,j.wait_ends,j.continuations,j.cancelled,j.incomplete,j.stack,j.callsite,j.stack_provenance,j.stack_reason,COALESCE(n.value,'<unknown job type>'),COALESCE(f.frame,0) FROM jobs j LEFT JOIN job_types t ON t.id=j.type LEFT JOIN strings n ON n.kind=" + kind + " AND n.key=t.name_key LEFT JOIN canonical_frames f ON f.sequence=j.origin ";
}
StoreJob ReadJob( Db& d, sqlite3_stmt* s )
{
    StoreJob j;
    j.id = sqlite3_column_int64( s, 0 );
    j.event = sqlite3_column_int64( s, 1 );
    j.packedHandle = sqlite3_column_int64( s, 2 );
    j.schedule = sqlite3_column_int64( s, 3 );
    j.thread = sqlite3_column_int64( s, 4 );
    j.type = uint32_t( sqlite3_column_int( s, 5 ) );
    j.count = uint32_t( sqlite3_column_int( s, 6 ) );
    j.grain = uint32_t( sqlite3_column_int( s, 7 ) );
    j.flow = uint32_t( sqlite3_column_int( s, 8 ) );
    j.originSequence = uint32_t( sqlite3_column_int( s, 9 ) );
    j.kind = uint8_t( sqlite3_column_int( s, 10 ) );
    j.flags = uint8_t( sqlite3_column_int( s, 11 ) );
    j.schema = uint16_t( sqlite3_column_int( s, 12 ) );
    j.expectedDependencies = uint16_t( sqlite3_column_int( s, 13 ) );
    j.orphan = sqlite3_column_int( s, 14 ) != 0;
    j.captureBoundary = sqlite3_column_int( s, 15 ) != 0;
    const auto time = [&]( int column ) -> std::optional<I> { return sqlite3_column_type( s, column ) == SQLITE_NULL ? std::optional<I>{} : std::optional<I>{ sqlite3_column_int64( s, column ) }; };
    j.ready = time( 16 );
    j.readyLane = uint32_t( sqlite3_column_int( s, 17 ) );
    j.readyFlags = uint8_t( sqlite3_column_int( s, 18 ) );
    j.queueEnter = time( 19 );
    j.queueLane = uint32_t( sqlite3_column_int( s, 20 ) );
    j.firstRun = time( 21 );
    j.completed = time( 22 );
    j.execution = sqlite3_column_int64( s, 23 );
    j.wait = sqlite3_column_int64( s, 24 );
    j.activeHelp = sqlite3_column_int64( s, 25 );
    j.spin = sqlite3_column_int64( s, 26 );
    j.sleep = sqlite3_column_int64( s, 27 );
    j.dispatches = uint32_t( sqlite3_column_int( s, 28 ) );
    j.steals = uint32_t( sqlite3_column_int( s, 29 ) );
    j.rangeSteals = uint32_t( sqlite3_column_int( s, 30 ) );
    j.activeDispatches = uint32_t( sqlite3_column_int( s, 31 ) );
    j.retries = uint32_t( sqlite3_column_int( s, 32 ) );
    j.waitEnds = uint32_t( sqlite3_column_int( s, 33 ) );
    j.continuations = uint32_t( sqlite3_column_int( s, 34 ) );
    j.cancelled = sqlite3_column_int( s, 35 ) != 0;
    j.incomplete = sqlite3_column_int( s, 36 ) != 0;
    j.stack = sqlite3_column_int64( s, 37 );
    j.callsite = sqlite3_column_int64( s, 38 );
    j.provenance = uint8_t( sqlite3_column_int( s, 39 ) );
    j.reason = uint8_t( sqlite3_column_int( s, 40 ) );
    j.name = Col( s, 41 );
    j.originFrame = sqlite3_column_int64( s, 42 );
    if( j.ready )
    {
        auto q = d.Prep( "SELECT COUNT(*),COUNT(p.completed),MAX(p.completed) FROM job_dependencies dep LEFT JOIN jobs p ON p.id=dep.prerequisite WHERE dep.job=?" );
        Int( q, 1, j.id );
        Require( d.Step( q ), "Missing dependency aggregate" );
        const auto count = sqlite3_column_int64( q, 0 ), complete = sqlite3_column_int64( q, 1 );
        const auto completed = sqlite3_column_int64( q, 2 );
        if( count && count == complete && *j.ready >= completed ) j.dependencyLatency = Sub( *j.ready, completed );
        sqlite3_reset( q );
    }
    return j;
}
std::string IoColumns() { return "SELECT id,event,resource,queued,thread,parent,requested,origin,parent_kind,operation,source,priority,subsystem,flags,config_flags,boundary,orphan,start,end,transferred,status,terminals,stack FROM io_requests "; }
StoreIoRequest ReadIo( sqlite3_stmt* s )
{
    StoreIoRequest r;
    r.id = sqlite3_column_int64( s, 0 );
    r.event = sqlite3_column_int64( s, 1 );
    r.resource = sqlite3_column_int64( s, 2 );
    r.queued = sqlite3_column_int64( s, 3 );
    r.thread = sqlite3_column_int64( s, 4 );
    r.parent = sqlite3_column_int64( s, 5 );
    r.requested = sqlite3_column_int64( s, 6 );
    r.originSequence = uint32_t( sqlite3_column_int( s, 7 ) );
    r.parentKind = uint8_t( sqlite3_column_int( s, 8 ) );
    r.operation = uint8_t( sqlite3_column_int( s, 9 ) );
    r.source = uint8_t( sqlite3_column_int( s, 10 ) );
    r.priority = uint8_t( sqlite3_column_int( s, 11 ) );
    r.subsystem = uint8_t( sqlite3_column_int( s, 12 ) );
    r.flags = uint8_t( sqlite3_column_int( s, 13 ) );
    r.configFlags = uint8_t( sqlite3_column_int( s, 14 ) );
    r.captureBoundary = sqlite3_column_int( s, 15 ) != 0;
    r.orphan = sqlite3_column_int( s, 16 ) != 0;
    if( sqlite3_column_type( s, 17 ) != SQLITE_NULL ) r.start = sqlite3_column_int64( s, 17 );
    if( sqlite3_column_type( s, 18 ) != SQLITE_NULL ) r.end = sqlite3_column_int64( s, 18 );
    r.transferred = sqlite3_column_int64( s, 19 );
    r.status = uint8_t( sqlite3_column_int( s, 20 ) );
    r.terminals = uint32_t( sqlite3_column_int( s, 21 ) );
    r.stack = sqlite3_column_int64( s, 22 );
    return r;
}
Manifest ReadManifest( const std::filesystem::path& dir, const StoreOptions& options )
{
    std::ifstream f( dir / "manifest.json", std::ios::binary );
    Require( bool( f ), "Missing manifest" );
    std::string s( 4097, '\0' );
    f.read( s.data(), s.size() );
    Require( f.gcount() <= 4096, "Manifest exceeds budget" );
    s.resize( size_t( f.gcount() ) );
    // SQLite's built-in JSON parser keeps this independent of GUI/Query JSON dependencies.
    Db parser( std::filesystem::path( ":memory:" ), true, options );
    auto q = parser.Prep( "SELECT json_valid(?1)" );
    Text( q, 1, s );
    Require( parser.Step( q ) && sqlite3_column_int( q, 0 ), "Invalid manifest JSON" );
    q = parser.Prep( "SELECT json_type(?1),json_extract(?1,'$.magic'),json_type(?1,'$.schema'),json_extract(?1,'$.schema'),json_type(?1,'$.data_sha256'),json_extract(?1,'$.data_sha256'),json_type(?1,'$.source_sha256'),json_extract(?1,'$.source_sha256')" );
    Text( q, 1, s );
    Require( parser.Step( q ) && Col( q, 0 ) == "object" && Col( q, 2 ) == "integer", "Manifest schema mismatch" );
    const auto schema = sqlite3_column_int64( q, 3 );
    Require( schema >= 2 && schema <= EventStoreSchema && Col( q, 1 ) == "TRCSTORE" + std::to_string( schema ), "Manifest schema mismatch" );
    Require( schema == EventStoreSchema, "Store schema mismatch; rebuild the event store" );
    Require( Col( q, 4 ) == "text" && Col( q, 6 ) == "text", "Invalid manifest digest type" );
    Manifest result{ Col( q, 5 ), Col( q, 7 ), {}, uint32_t( schema ) };
    q = parser.Prep( "SELECT json_type(?1,'$.blocks_sha256'),json_extract(?1,'$.blocks_sha256'),json_type(?1,'$.blocks_bytes'),json_extract(?1,'$.blocks_bytes')" );
    Text( q, 1, s );
    Require( parser.Step( q ) && Col( q, 0 ) == "text" && Col( q, 2 ) == "integer" && sqlite3_column_int64( q, 3 ) >= 0, "Manifest event block identity missing" );
    result.blocks = Col( q, 1 );
    result.blockBytes = uint64_t( sqlite3_column_int64( q, 3 ) );
    for( const auto& hash : { result.data, result.source, result.blocks } ) Require( hash.size() == 64 && hash.find_first_not_of( "0123456789abcdef" ) == std::string::npos, "Invalid manifest digest" );
    q = parser.Prep( "SELECT json_extract(?1,'$.data_identity'),json_extract(?1,'$.blocks_identity'),json_extract(?1,'$.source_identity')" );
    Text( q, 1, s );
    Require( parser.Step( q ), "Invalid manifest identity" );
    if( sqlite3_column_type( q, 0 ) == SQLITE_TEXT && sqlite3_column_type( q, 1 ) == SQLITE_TEXT && sqlite3_column_type( q, 2 ) == SQLITE_TEXT )
    {
        result.dataIdentity = Col( q, 0 );
        result.blocksIdentity = Col( q, 1 );
        result.sourceIdentity = Col( q, 2 );
    }
    q = parser.Prep( "SELECT COUNT(*) FROM (SELECT key FROM json_each(?1) GROUP BY key HAVING COUNT(*)>1)" );
    Text( q, 1, s );
    Require( parser.Step( q ) && sqlite3_column_int64( q, 0 ) == 0, "Duplicate manifest fields" );
    return result;
}
}
struct StoreAnalysis::Impl
{
    std::filesystem::path directory;
    std::unique_ptr<Db> db;
    StoreAnalysisSummary info;
    // A call tree analysis keeps its own store handle: Search() reads the
    // zones and texts of `thread` (all threads when empty) from it.
    std::unique_ptr<EventStore> zones;
    std::optional<uint64_t> thread;
    explicit Impl( const StoreOptions& options )
    {
        directory=std::filesystem::temp_directory_path()/("tracy-global-analysis-"+Hex(MakeFileHeader(ProtocolVersion).sessionId));
        Require(std::filesystem::create_directory(directory),"Cannot create analysis workspace");
        try { auto limited=options;limited.cacheBytes=4ull<<20;db=std::make_unique<Db>(directory/"analysis.sqlite",true,limited);db->Exec("PRAGMA journal_mode=OFF;PRAGMA synchronous=OFF;PRAGMA temp.cache_size=-4096"); }
        catch(...) { Cleanup();throw; }
    }
    void Cleanup()
    {
        db.reset();std::error_code error;
        // This directory was created exclusively above. Remove only owned files.
        std::filesystem::remove(directory/"analysis.sqlite",error);
        std::filesystem::remove(directory/"analysis.sqlite-journal",error);
        std::filesystem::remove(directory,error);
    }
    ~Impl(){Cleanup();}
};
StoreAnalysis::StoreAnalysis( std::unique_ptr<Impl> impl ) : m_impl(std::move(impl)) {}
StoreAnalysis::~StoreAnalysis()=default;
const StoreAnalysisSummary& StoreAnalysis::Info() const { return m_impl->info; }
struct EventStore::Impl
{
    Db db;
    StoreInfo info;
    std::filesystem::path source, directory;
    BlockFile blocks;
    ZoneBlockCache zoneCache;
    // Zone and text blocks by logical thread, built by the first cursor.
    std::optional<ZoneThreadIndex> zoneIndex;
    // GPU zone timelines with every 4096th zone id, built on first use.
    std::optional<GpuGroupIndex> gpuIndex;
    // Packet blocks of each export type with their running counts.
    std::unordered_map<uint8_t, PacketTypeIndex> packetIndex;
    // Every 4096th Job id and the Job count, built on first use.
    std::optional<std::pair<std::vector<uint64_t>, uint64_t>> jobIndex;
    // Timeline numbers of the zone sources, built by TimelineSources().
    std::optional<std::unordered_map<std::pair<uint8_t, uint64_t>, uint32_t, SourceKeyHash>> timelineSources;
    // A revision of a live store rather than a finished store.
    bool snapshot = false;
    Impl( const std::filesystem::path& p, const StoreOptions& o, const std::filesystem::path& journal, bool snapshot = false )
        : db( p, false, o, o.threadConfined )
        , source( std::filesystem::weakly_canonical( journal ) )
        , directory( std::filesystem::weakly_canonical( p.parent_path() ) )
        , blocks( p.parent_path() / "events.blocks", false )
    {
        zoneCache.zones.budget = o.cacheBytes / 2;
        zoneCache.texts.budget = o.cacheBytes / 4;
        zoneCache.packets.budget = o.cacheBytes / 4;
        this->snapshot = snapshot;
        if( snapshot ) db.Exec( "BEGIN" );
        info = Load( db );
    }
};
EventStore::EventStore( std::unique_ptr<Impl> p )
    : m_impl( std::move( p ) )
{
}
EventStore::~EventStore() = default;
namespace
{
std::filesystem::path LiveDirectory( const std::filesystem::path& path )
{
    auto directory = EventStore::Directory( path );
    directory += ".live-" + Hex( MakeFileHeader( ProtocolVersion ).sessionId );
    Require( std::filesystem::create_directory( directory ), "Cannot create live event store" );
    return directory;
}
}
struct LiveEventStore::Impl
{
    std::filesystem::path path, directory;
    StoreOptions options;
    StoreInfo info;
    Db db;
    BlockFile blocks;
    std::unique_ptr<Ingest> ingest;
    JournalTailReader reader;
    uint64_t decoded = 0;
    uint64_t checkpointBytes = 0;
    std::atomic<uint64_t> publishedDecoded{ 0 };
    bool failed = false;
    Impl( const std::filesystem::path& path, const StoreOptions& options )
        : path( path )
        , directory( LiveDirectory( path ) )
        , options( options )
        , db( directory / "events.sqlite", true, options, true ) // Only the Update thread uses this writer.
        , blocks( directory / "events.blocks", true )
        , reader( path )
    {
        // This database is a rebuildable projection of the durable journal.
        // Avoid forcing a device flush for every DDL statement or live batch.
        db.Exec( "PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;PRAGMA wal_autocheckpoint=0;BEGIN IMMEDIATE" );
        Schema( db );
        db.Exec( "CREATE TABLE live_input(seq INTEGER PRIMARY KEY,offset INTEGER,stamp INTEGER,size INTEGER,flags INTEGER,type INTEGER)" );
        Save( db, info );
        db.Exec( "COMMIT" );
        Require( uint64_t( db.Scalar( "PRAGMA page_count" ) ) * uint64_t( db.Scalar( "PRAGMA page_size" ) ) <= options.diskBytes, "Live store disk budget is smaller than its schema" );
        ingest = std::make_unique<Ingest>( db, info, this->options, true );
        ingest->SetBlockFile( &blocks );
    }
};
LiveEventStore::LiveEventStore( std::unique_ptr<Impl> impl )
    : m_impl( std::move( impl ) )
{
}
LiveEventStore::~LiveEventStore() = default;
std::unique_ptr<LiveEventStore> LiveEventStore::Create( const std::filesystem::path& path, const StoreOptions& options )
{
    return std::unique_ptr<LiveEventStore>( new LiveEventStore( std::make_unique<Impl>( path, options ) ) );
}
bool LiveEventStore::Update( uint64_t maxRecords )
{
    Require( maxRecords > 0 && maxRecords <= 4096, "Live batch limit must be between 1 and 4096 records" );
    auto& state = *m_impl;
    Require( !state.failed, "Live store failed; rebuild is required" );
    const auto oldInfo = state.info;
    const auto oldDecoded = state.decoded;
    const auto availableBytes = std::filesystem::file_size( state.path );
    auto checkpoint = std::make_unique<Ingest>( *state.ingest );
    std::ifstream input( state.path, std::ios::binary );
    Require( bool( input ), "Cannot read live journal" );
    state.db.Exec( "BEGIN IMMEDIATE" );
    bool readerAdvanced = false;
    try
    {
        ScanOptions options;
        options.maxCollectedRecords = 0;
        options.maxNewRecords = maxRecords;
        options.cancelled = state.options.cancelled;
        options.recordVisitor = [&]( const RecordInfo& record ) {
            if( Ingest::QueryRecord( record ) ) state.ingest->Queries( record, Payload( input, record ) );
            auto q = state.db.Prep( "INSERT INTO live_input VALUES(?,?,?,?,?,?)" );
            Int( q, 1, record.sequence );
            Int( q, 2, record.offset );
            Int( q, 3, record.monotonicNs );
            Int( q, 4, record.payloadSize );
            Int( q, 5, record.flags );
            Int( q, 6, uint16_t( record.type ) );
            state.db.Run( q );
            if( state.options.progress ) state.options.progress( record.offset + RecordHeaderSize + record.payloadSize + RecordTrailerSize, availableBytes );
        };
        const auto result = state.reader.Poll( options );
        readerAdvanced = true;
        if( !result.HasValidHeader() )
        {
            state.db.Exec( "ROLLBACK" );
            return false;
        }
        Require( result.header.protocolVersion == ProtocolVersion, "Unsupported live journal protocol" );
        if( result.lastSequence == oldInfo.revision && Hex( result.header.sessionId ) == oldInfo.session && !state.db.Scalar( "SELECT EXISTS(SELECT 1 FROM live_input)" ) )
        {
            state.db.Exec( "ROLLBACK" );
            return false;
        }
         // Requests are known before decoding this batch. If their observer has
        // not yet published a request, retain only record locations on disk and
        // retry after more input arrives. No raw protocol frames stay resident.
        auto decodeCheckpoint = std::make_unique<Ingest>( *state.ingest );
        const auto decodeInfo = state.info;
        const auto decodeCount = state.decoded;
        state.db.Exec( "SAVEPOINT live_decode" );
        try
        {
            for( uint64_t count = 0; count < maxRecords; ++count )
            {
                auto q = state.db.Prep( "SELECT seq,offset,stamp,size,flags,type FROM live_input ORDER BY seq LIMIT 1" );
                if( !state.db.Step( q ) )
                {
                    sqlite3_reset( q );
                    break;
                }
                RecordInfo record;
                record.sequence = sqlite3_column_int64( q, 0 );
                record.offset = sqlite3_column_int64( q, 1 );
                record.monotonicNs = sqlite3_column_int64( q, 2 );
                record.payloadSize = sqlite3_column_int64( q, 3 );
                record.flags = uint32_t( sqlite3_column_int64( q, 4 ) );
                record.type = RecordType( sqlite3_column_int( q, 5 ) );
                sqlite3_reset( q );
                const auto payload = Payload( input, record );
                state.ingest->Consume( record, payload );
                ++state.decoded;
                state.info.revision = record.sequence;
                state.info.validBytes = record.offset + RecordHeaderSize + record.payloadSize + RecordTrailerSize;
                q = state.db.Prep( "DELETE FROM live_input WHERE seq=?" );
                Int( q, 1, record.sequence );
                state.db.Run( q );
            }
            state.db.Exec( "RELEASE live_decode" );
        }
        catch( const AwaitRecordedQuery& )
        {
            state.db.Exec( "ROLLBACK TO live_decode;RELEASE live_decode" );
            state.ingest = std::move( decodeCheckpoint );
            state.info = decodeInfo;
            state.decoded = decodeCount;
            Require( !result.complete, "Complete transcript has a response without its recorded query" );
        }
        const bool backlog = state.db.Scalar( "SELECT EXISTS(SELECT 1 FROM live_input)" ) != 0;
        state.info.session = Hex( result.header.sessionId );
        state.info.sourceSha256 = backlog ? "" : Hex( state.reader.PrefixDigest() );
        state.info.sourceBytes = result.fileSize;
        state.ingest->Finish( !result.complete || backlog );
        Save( state.db, state.info );
        const uint64_t pageSize=state.db.Scalar("PRAGMA page_size"),tempPageSize=state.db.Scalar("PRAGMA temp.page_size");
        const uint64_t mainPages=state.db.Scalar("PRAGMA page_count"),tempPages=state.db.Scalar("PRAGMA temp.page_count");
        Require(mainPages<=UINT64_MAX/pageSize && tempPages<=UINT64_MAX/tempPageSize,"Live disk page count overflow");
        uint64_t disk=mainPages*pageSize;const uint64_t temporaryDisk=tempPages*tempPageSize;
        Require(disk<=UINT64_MAX-temporaryDisk,"Live temporary disk size overflow");disk+=temporaryDisk;
        for( const auto* name : { "events.sqlite-wal", "events.sqlite-shm", "events.blocks" } )
        {
            const auto file = state.directory / name;
            if( std::filesystem::exists( file ) )
            {
                const auto size = std::filesystem::file_size( file );
                Require( disk <= UINT64_MAX - size, "Live store disk size overflow" );
                disk += size;
            }
        }
        int cacheBytes = 0, highWater = 0;
        Require( sqlite3_db_status( state.db.handle, SQLITE_DBSTATUS_CACHE_USED, &cacheBytes, &highWater, 0 ) == SQLITE_OK, "Cannot reserve live commit space" );
        const uint64_t reserve = uint64_t( std::max( 0, cacheBytes ) ) * 2 + 4096;
        Require( disk <= UINT64_MAX - reserve, "Live commit reserve overflow" );
        disk += reserve;
        Require( disk <= state.options.diskBytes, "Live store disk budget exceeded" );
        state.db.Exec( "COMMIT" );
        state.publishedDecoded = state.decoded;
         // Readers may pin old WAL frames. Throttle checkpoint attempts instead
         // of repeatedly flushing the same prefix while those readers are open.
        const auto wal = state.directory / "events.sqlite-wal";
        const auto walBytes = std::filesystem::exists( wal ) ? std::filesystem::file_size( wal ) : 0;
        if( result.complete || walBytes >= state.checkpointBytes + ( 16ull << 20 ) )
        {
            sqlite3_exec( state.db.handle, "PRAGMA wal_checkpoint(PASSIVE)", nullptr, nullptr, nullptr );
            state.checkpointBytes = walBytes;
        }
        return state.info.revision != oldInfo.revision;
    }
    catch( ... )
    {
        sqlite3_exec( state.db.handle, "ROLLBACK", nullptr, nullptr, nullptr );
        state.info = oldInfo;
        state.decoded = oldDecoded;
        state.ingest = std::move( checkpoint );
        if( readerAdvanced ) state.failed = true;
        throw;
    }
}
std::unique_ptr<EventStore> LiveEventStore::Snapshot( const StoreOptions& readOptions ) const
{
    return std::unique_ptr<EventStore>( new EventStore( std::make_unique<EventStore::Impl>( m_impl->directory / "events.sqlite", readOptions, m_impl->path, true ) ) );
}
uint64_t LiveEventStore::DecodedRecords() const { return m_impl->publishedDecoded.load(); }
std::filesystem::path EventStore::Directory( const std::filesystem::path& p )
{
    auto d = p;
    d += ".store";
    return d;
}
namespace
{
// Builds a store for journal `p` in a new work directory, then commits,
// checks, hashes and publishes it. `feed` decodes the journal into the
// ingestion and fills the source fields of the info.
template<class Feed>
StoreInfo BuildStore( const std::filesystem::path& p, const StoreOptions& o, Feed&& feed )
{
    auto dir = EventStore::Directory( p );
    auto work = dir;
    work += ".building-" + Hex( MakeFileHeader( ProtocolVersion ).sessionId );
    Require( std::filesystem::create_directory( work ), "Cannot create store work directory" );
    StoreInfo i;
    const auto stage = [&]( const char* name ) {
        if( o.stage ) o.stage( name );
    };
    {
        // The build connection stays on this thread for its whole lifetime.
        Db db( work / "events.sqlite", true, o, true );
        BlockFile blocks( work / "events.blocks", true );
        blocks.WriteInBackground();
        db.Exec( "BEGIN IMMEDIATE" );
        Schema( db );
        // This directory is unpublished until the final durable commit and hash.
        // Cache spill keeps the single transaction bounded without per-batch fsync.
        Ingest ingest( db, i, o );
        ingest.SetBlockFile( &blocks );
        ingest.DeferIndexes();
        feed( ingest, i, stage );
        stage( "Finish store" );
        ingest.Finish();
        Save( db, i );
        stage( "Commit store" );
        // Rows of the commit point into the blocks, so the blocks are durable first.
        blocks.Flush();
        db.Exec( "COMMIT" );
        Require( uint64_t( db.Scalar( "PRAGMA page_count" ) ) * uint64_t( db.Scalar( "PRAGMA page_size" ) ) + blocks.Size() <= o.diskBytes, "Store disk budget exceeded" );
        i.blockBytes = blocks.Size();
        stage( "Check and hash store" );
        // The committed file no longer changes, so its identity hash runs while
        // quick_check reads it. Only this thread polls o.cancelled; the hash
        // worker observes a local stop flag.
        std::atomic<bool> stop{ false };
        StoreOptions hashOptions;
        hashOptions.cancelled = [&stop] { return stop.load(); };
        auto hashes = std::async( std::launch::async, [&] { return std::make_tuple( Sha( p, hashOptions ), Sha( work / "events.sqlite", hashOptions ), Sha( work / "events.blocks", hashOptions ) ); } );
        try
        {
            auto check = db.Prep( "PRAGMA quick_check" );
            Require( db.Step( check ) && Col( check, 0 ) == "ok", "Store structure validation failed" );
            sqlite3_reset( check );
            while( hashes.wait_for( std::chrono::milliseconds( 50 ) ) != std::future_status::ready )
                if( o.cancelled && o.cancelled() ) stop = true;
        }
        catch( ... )
        {
            stop = true;
            hashes.wait();
            throw;
        }
        const auto [source, data, blockHash] = hashes.get();
        Require( source == i.sourceSha256, "Source changed during build" );
        i.dataSha256 = data;
        i.blockSha256 = blockHash;
    }
    // The connection is closed, so the store files are final; their identities
    // let a later Open() skip hashing while the files stay unchanged.
    const auto dataIdentity = FileIdentity( work / "events.sqlite" ), blocksIdentity = FileIdentity( work / "events.blocks" ), sourceIdentity = FileIdentity( p );
    std::ofstream f( work / "manifest.json", std::ios::binary );
    Require( bool( f ), "Cannot create manifest" );
    f << "{\"magic\":\"TRCSTORE" << EventStoreSchema << "\",\"schema\":" << EventStoreSchema << ",\"data_sha256\":\"" << i.dataSha256 << "\",\"blocks_sha256\":\"" << i.blockSha256 << "\",\"blocks_bytes\":" << i.blockBytes << ",\"source_sha256\":\"" << i.sourceSha256 << "\",\"data_identity\":\"" << dataIdentity << "\",\"blocks_identity\":\"" << blocksIdentity << "\",\"source_identity\":\"" << sourceIdentity << "\",\"transcript_verified\":" << ( i.transcriptVerified ? "true" : "false" ) << ",\"capabilities\":[\"cpu\",\"frames\",\"definitions\",\"gpu\",\"samples\",\"context_switch\",\"locks\",\"plots\",\"messages\",\"jobs\",\"io\",\"scripts\",\"memory_allocations\",\"managed_callstacks\",\"gpu_references\",\"gpu_catalog\",\"frame_images\",\"jn_relations\",\"symbol_code\",\"source_code\",\"hardware_samples\",\"fibers\",\"export_metadata\",\"cpu_analysis\",\"gpu_analysis\",\"bounded_snapshot_export\"],\"unsupported_domains\":[\"global_analysis_other_domains\"]}";
    f.close();
    Require( bool( f ), "Manifest write failed" );
    auto backup = dir;
    backup += ".previous-" + Hex( MakeFileHeader( ProtocolVersion ).sessionId );
    const bool replacing = std::filesystem::exists( dir );
    if( replacing ) std::filesystem::rename( dir, backup );
    try
    {
        std::filesystem::rename( work, dir );
    }
    catch( ... )
    {
        if( replacing ) std::filesystem::rename( backup, dir );
        throw;
    }
    return i;
}
}
StoreInfo EventStore::Build( const std::filesystem::path& p, const StoreOptions& o )
{
    if( std::filesystem::exists( Directory( p ) ) && !o.rebuild ) return Open( p, o )->Info();
    return BuildStore( p, o, [&]( Ingest& ingest, StoreInfo& i, const auto& stage ) {
        stage( "Hash source journal" );
        i.sourceSha256 = Sha( p, o );
        std::ifstream input( p, std::ios::binary );
        ScanOptions s;
        s.maxCollectedRecords = 0;
        s.recordVisitor = [&]( const RecordInfo& r ) {if(o.cancelled&&o.cancelled())throw std::runtime_error("Cancelled");if(Ingest::QueryRecord(r))ingest.Queries(r,Payload(input,r)); };
        auto scan = ScanJournal( p, s );
        Require( scan.HasRecoverablePrefix() && scan.header.protocolVersion == ProtocolVersion, "Invalid journal or protocol" );
        Require( scan.code != ScanCode::CorruptTail && scan.code != ScanCode::IoError, "Corrupt journal input" );
        i.session = Hex( scan.header.sessionId );
        i.sourceBytes = scan.fileSize;
        i.validBytes = scan.validSize;
        i.revision = scan.lastSequence;
        stage( "Index journal" );
        // The worker checks and reads the records and decodes their frames
        // while this thread ingests the ones before.
        ScanResult end;
        RecordPrefetch prefetch( p );
        prefetch.Start( [&]( RecordPrefetch& feed ) {
            ScanOptions read;
            read.maxCollectedRecords = 0;
            read.cancelled = [&] { return feed.Stopping(); };
            read.recordVisitor = [&]( const RecordInfo& r ) { feed.Emit( r ); };
            end = ScanJournalPrefix( p, scan.validSize, read );
        } );
        while( auto* item = prefetch.Next() )
        {
            if( o.cancelled && o.cancelled() ) throw std::runtime_error( "Cancelled" );
            const auto& r = item->record;
            ingest.Consume( r, item->payload, item->Frame() );
            if( o.progress ) o.progress( r.offset + RecordHeaderSize + r.payloadSize + RecordTrailerSize, scan.validSize );
            prefetch.Release( item );
        }
        prefetch.Finish();
        Require( end.validSize == scan.validSize && end.prefixCrc32c == scan.prefixCrc32c, "Journal changed during build" );
    } );
}
StoreInfo EventStore::BuildFollowing( const std::filesystem::path& p, const std::function<bool()>& captureEnded, const StoreOptions& o )
{
    Require( !std::filesystem::exists( Directory( p ) ) || o.rebuild, "Event store already exists" );
    return BuildStore( p, o, [&]( Ingest& ingest, StoreInfo& i, const auto& stage ) {
        stage( "Index journal" );
        // A response may reach the journal shortly before its recorded query.
        // Every record is offered to the query pass when it is read, and
        // decoded only once this much later journal data has been read, or
        // when the journal is final. A response is then matched among the
        // queries recorded before the read position, as in Build(), where a
        // later query always has a later sequence.
        constexpr uint64_t LagBytes = 16ull << 20;
        // This build is invisible until BuildStore commits and publishes it.
        // Hash appended bytes once per batch instead of rehashing the entire
        // history on every poll, then verify the full prefix before committing.
        JournalTailReader reader( p, JournalTailReader::PrefixValidation::Deferred );
        std::ifstream input( p, std::ios::binary );
        // The worker reads and decodes the records released for ingestion
        // while this thread ingests the ones before.
        RecordPrefetch prefetch( p );
        prefetch.StartRequests();
        std::deque<RecordInfo> pending;
        uint64_t pendingBytes = 0;
        bool ended = false;
        ScanResult last;
        for( ;; )
        {
            if( o.cancelled && o.cancelled() ) throw std::runtime_error( "Cancelled" );
            // Read the end condition before polling, so a poll that finds no new
            // records after the capture ended has seen the final journal.
            ended = ended || ( captureEnded && captureEnded() );
            uint64_t added = 0;
            ScanOptions s;
            s.maxCollectedRecords = 0;
            s.maxNewRecords = 4096;
            s.recordVisitor = [&]( const RecordInfo& r ) {
                if( Ingest::QueryRecord( r ) ) ingest.Queries( r, Payload( input, r ) );
                pending.push_back( r );
                pendingBytes += r.payloadSize + RecordHeaderSize + RecordTrailerSize;
                ++added;
            };
            last = reader.Poll( s );
            Require( last.code != ScanCode::CorruptTail && last.code != ScanCode::IoError, "Corrupt journal input" );
            if( last.HasValidHeader() ) Require( last.header.protocolVersion == ProtocolVersion, "Invalid journal or protocol" );
            const bool final = last.complete || ( ended && added == 0 );
            size_t released = 0;
            while( !pending.empty() && ( final || pendingBytes > LagBytes ) )
            {
                const auto r = pending.front();
                pending.pop_front();
                pendingBytes -= r.payloadSize + RecordHeaderSize + RecordTrailerSize;
                prefetch.Request( r );
                ++released;
            }
            for( ; released != 0; --released )
            {
                auto* item = prefetch.Next();
                Require( item != nullptr, "Journal prefetch ended early" );
                const auto& r = item->record;
                ingest.Consume( r, item->payload, item->Frame() );
                if( o.progress ) o.progress( r.offset + RecordHeaderSize + r.payloadSize + RecordTrailerSize, last.validSize );
                prefetch.Release( item );
            }
            if( final ) break;
            if( added == 0 ) std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
        }
        prefetch.Finish();
        Require( last.HasRecoverablePrefix(), "Invalid journal or protocol" );
        stage( "Verify followed journal prefix" );
        reader.VerifyPrefix( o.cancelled );
        i.session = Hex( last.header.sessionId );
        i.sourceBytes = std::filesystem::file_size( p );
        i.validBytes = last.validSize;
        i.revision = last.lastSequence;
        // The reader hashed the committed prefix while validating it; a final
        // journal without a torn tail is that prefix.
        i.sourceSha256 = i.sourceBytes == last.validSize ? Hex( reader.PrefixDigest() ) : Sha( p, o );
    } );
}
namespace
{
void VerifyStoreContent( const std::filesystem::path& p, const std::filesystem::path& dir, const Manifest& manifest, const StoreOptions& o )
{
    Require( Sha( dir / "events.sqlite", o ) == manifest.data, "Store checksum mismatch" );
    Require( std::filesystem::file_size( dir / "events.blocks" ) == manifest.blockBytes && Sha( dir / "events.blocks", o ) == manifest.blocks, "Event block checksum mismatch" );
    Require( Sha( p, o ) == manifest.source, "Source identity mismatch" );
}
}
std::unique_ptr<EventStore> EventStore::Open( const std::filesystem::path& p, const StoreOptions& o )
{
    auto dir = Directory( p );
    auto manifest = ReadManifest( dir, o );
    if( o.stage ) o.stage( "Verify store identity" );
    // Unchanged files skip hashing only when the caller asked for it and the
    // manifest recorded their identities.
    const bool unchanged = !o.verifyContent && !manifest.dataIdentity.empty() && FileIdentity( dir / "events.sqlite" ) == manifest.dataIdentity && FileIdentity( dir / "events.blocks" ) == manifest.blocksIdentity && FileIdentity( p ) == manifest.sourceIdentity;
    if( !unchanged ) VerifyStoreContent( p, dir, manifest, o );
    auto impl = std::make_unique<Impl>( dir / "events.sqlite", o, p );
    Require( impl->info.schema == manifest.schema, "Manifest/database schema mismatch" );
    Require( impl->info.sourceSha256 == manifest.source, "Manifest source identity mismatch" );
    Require( std::filesystem::file_size( p ) == impl->info.sourceBytes, "Source identity mismatch" );
    impl->info.dataSha256 = manifest.data;
    return std::unique_ptr<EventStore>( new EventStore( std::move( impl ) ) );
}
std::unique_ptr<EventStore> EventStore::OpenSibling( const StoreOptions& o ) const
{
    Require( !m_impl->snapshot, "A live store revision has no sibling handle" );
    auto impl = std::make_unique<Impl>( m_impl->directory / "events.sqlite", o, m_impl->source );
    Require( impl->info.schema == m_impl->info.schema && impl->info.sourceSha256 == m_impl->info.sourceSha256 && impl->info.revision == m_impl->info.revision, "Sibling store mismatch" );
    Require( std::filesystem::file_size( m_impl->source ) == impl->info.sourceBytes, "Source identity mismatch" );
    impl->info.dataSha256 = m_impl->info.dataSha256;
    return std::unique_ptr<EventStore>( new EventStore( std::move( impl ) ) );
}
uint32_t EventStore::StoredSchema( const std::filesystem::path& p )
{
    std::ifstream f( Directory( p ) / "manifest.json", std::ios::binary );
    if( !f ) return 0;
    std::string s( 4097, '\0' );
    f.read( s.data(), s.size() );
    if( f.gcount() > 4096 ) return 0;
    s.resize( size_t( f.gcount() ) );
    try
    {
        Db parser( std::filesystem::path( ":memory:" ), true, StoreOptions{} );
        auto q = parser.Prep( "SELECT CASE WHEN json_valid(?1) AND json_type(?1,'$.schema')='integer' THEN json_extract(?1,'$.schema') ELSE 0 END" );
        ::tracy::stream::Text( q, 1, s );
        if( !parser.Step( q ) ) return 0;
        const auto schema = sqlite3_column_int64( q, 0 );
        return schema > 0 && schema <= UINT32_MAX ? uint32_t( schema ) : 0;
    }
    catch( const std::exception& )
    {
        return 0;
    }
}
void EventStore::VerifyContent( const std::filesystem::path& p, const StoreOptions& o )
{
    const auto dir = Directory( p );
    VerifyStoreContent( p, dir, ReadManifest( dir, o ), o );
}
const StoreInfo& EventStore::Info() const { return m_impl->info; }
#include "TracyEventStoreAnalysis.inl"
#include "TracyEventStoreCodeRead.inl"
#ifdef _WIN32
#include "TracyEventStoreExport.inl"
#else
StoreExportResult EventStore::ExportSnapshot(const std::filesystem::path&,const StoreExportOptions&) const { throw std::runtime_error("Bounded snapshot export currently requires Windows"); }
#endif
StoreCaptureMetadata EventStore::CaptureMetadata() const
{
    auto& d = m_impl->db;
    StoreCaptureMetadata result;
    auto q = d.Prep( "SELECT data FROM capture_metadata WHERE key='welcome'" );
    if( !d.Step( q ) ) return result;
    Require( sqlite3_column_bytes( q, 0 ) == sizeof( WelcomeMessage ), "Invalid stored welcome" );
    WelcomeMessage w;
    memcpy( &w, sqlite3_column_blob( q, 0 ), sizeof( w ) );
    sqlite3_reset( q );
    result.available = true;
    result.timerMultiplier = w.timerMul;
    result.initBegin = w.initBegin;
    result.initEnd = w.initEnd;
    result.samplingPeriod = w.samplingPeriod;
    result.resolution = w.resolution;
    result.epoch = w.epoch;
    result.executableTime = w.exectime;
    result.pid = w.pid;
    result.cpuId = w.cpuId;
    result.flags = w.flags;
    result.architecture = w.cpuArch;
    memcpy( result.cpuManufacturer.data(), w.cpuManufacturer, 12 );
    result.program.assign( w.programName, std::find( w.programName, w.programName + sizeof( w.programName ), '\0' ) );
    result.host.assign( w.hostInfo, std::find( w.hostInfo, w.hostInfo + sizeof( w.hostInfo ), '\0' ) );
    q = d.Prep( "SELECT data FROM capture_metadata WHERE key='on_demand'" );
    if( d.Step( q ) )
    {
        Require( sqlite3_column_bytes( q, 0 ) == sizeof( OnDemandPayloadMessage ), "Invalid stored on-demand header" );
        OnDemandPayloadMessage o;
        memcpy( &o, sqlite3_column_blob( q, 0 ), sizeof( o ) );
        result.frameOffset = o.frames;
    }
    sqlite3_reset( q );
    return result;
}
StorePage<StoreCpuTopology> EventStore::CpuTopology( uint64_t after, size_t limit ) const
{
    auto& d = m_impl->db;
    limit = CodePageLimit<StoreCpuTopology>( d, limit );
    StorePage<StoreCpuTopology> page;
    if( !limit ) return page;
    page.values.reserve( limit );
    auto q = d.Prep( "SELECT package,die,core,thread FROM cpu_topology WHERE thread>=? ORDER BY thread LIMIT ?" );
    Int( q, 1, after );
    Int( q, 2, limit + 1 );
    while( d.Step( q ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StoreCpuTopology v;
        v.package = uint32_t( sqlite3_column_int64( q, 0 ) );
        v.die = uint32_t( sqlite3_column_int64( q, 1 ) );
        v.core = uint32_t( sqlite3_column_int64( q, 2 ) );
        v.thread = uint32_t( sqlite3_column_int64( q, 3 ) );
        page.next = uint64_t( v.thread ) + 1;
        page.values.push_back( v );
    }
    sqlite3_reset( q );
    return page;
}
StorePage<StoreThreadHint> EventStore::ThreadHints( uint64_t after, size_t limit ) const
{
    auto& d = m_impl->db;
    limit = CodePageLimit<StoreThreadHint>( d, limit );
    StorePage<StoreThreadHint> page;
    if( !limit ) return page;
    page.values.reserve( limit );
    auto q = d.Prep( "SELECT thread,event,hint FROM thread_hints WHERE event>? ORDER BY event LIMIT ?" );
    Int( q, 1, after );
    Int( q, 2, limit + 1 );
    while( d.Step( q ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StoreThreadHint v;
        v.thread = sqlite3_column_int64( q, 0 );
        v.event = sqlite3_column_int64( q, 1 );
        v.hint = int32_t( sqlite3_column_int( q, 2 ) );
        page.next = v.event;
        page.values.push_back( v );
    }
    sqlite3_reset( q );
    return page;
}
std::optional<StoreCrash> EventStore::Crash() const
{
    auto& d = m_impl->db;
    auto q = d.Prep( "SELECT event,thread,time,message,stack FROM crash" );
    if( !d.Step( q ) ) return {};
    StoreCrash c;
    c.event = sqlite3_column_int64( q, 0 );
    c.thread = sqlite3_column_int64( q, 1 );
    c.time = sqlite3_column_int64( q, 2 );
    c.message = sqlite3_column_int64( q, 3 );
    c.stack = sqlite3_column_int64( q, 4 );
    sqlite3_reset( q );
    return c;
}
StorePage<StoreFiber> EventStore::Fibers( uint64_t after, size_t limit ) const
{
    auto& d = m_impl->db;
    limit = CodePageLimit<StoreFiber>( d, limit, 4096 );
    StorePage<StoreFiber> page;
    if( !limit ) return page;
    page.values.reserve( limit );
    auto q = d.Prep( "SELECT f.id,f.key,f.group_hint,COALESCE(c.running,0),(SELECT COUNT(*) FROM context_regions r WHERE r.thread=f.id),substr(CAST(s.value AS BLOB),1,4096),length(CAST(s.value AS BLOB)) FROM fibers f LEFT JOIN context_threads c ON c.thread=f.id LEFT JOIN strings s ON s.kind=?3 AND s.key=f.key WHERE f.id>?1 ORDER BY f.id LIMIT ?2" );
    Int( q, 1, after );
    Int( q, 2, limit + 1 );
    Int( q, 3, uint8_t( QueueType::FiberName ) );
    while( d.Step( q ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StoreFiber f;
        f.id = sqlite3_column_int64( q, 0 );
        f.key = sqlite3_column_int64( q, 1 );
        f.groupHint = int32_t( sqlite3_column_int( q, 2 ) );
        f.running = sqlite3_column_int64( q, 3 );
        f.regions = sqlite3_column_int64( q, 4 );
        f.name = Col( q, 5 );
        f.nameBytes = sqlite3_column_int64( q, 6 );
        f.nameResolved = sqlite3_column_type( q, 6 ) != SQLITE_NULL;
        f.nameMore = f.name.size() < f.nameBytes;
        page.next = f.id;
        page.values.push_back( std::move( f ) );
    }
    sqlite3_reset( q );
    return page;
}
StoreBinaryChunk EventStore::FiberNameBytes( uint64_t id, uint64_t offset, size_t maxBytes ) const
{
    auto& d = m_impl->db;
    Require( offset < INT64_MAX && maxBytes > 0 && maxBytes <= d.options.resultBytes - sizeof( StoreBinaryChunk ), "Fiber name result budget exceeded" );
    auto q = d.Prep( "SELECT length(CAST(s.value AS BLOB)),substr(CAST(s.value AS BLOB),?2,?3) FROM fibers f JOIN strings s ON s.key=f.key AND s.kind=?4 WHERE f.id=?1" );
    Int( q, 1, id );
    Int( q, 2, offset + 1 );
    Int( q, 3, maxBytes );
    Int( q, 4, uint8_t( QueueType::FiberName ) );
    Require( d.Step( q ), "Fiber name is unresolved" );
    StoreBinaryChunk chunk;
    chunk.offset = offset;
    chunk.totalBytes = sqlite3_column_int64( q, 0 );
    const auto n = sqlite3_column_bytes( q, 1 );
    const auto p = static_cast<const uint8_t*>( sqlite3_column_blob( q, 1 ) );
    if( n ) chunk.data.assign( p, p + n );
    chunk.more = offset < chunk.totalBytes && uint64_t( n ) < chunk.totalBytes - offset;
    sqlite3_reset( q );
    return chunk;
}
StorePage<StoreCallsite> EventStore::Callsites( uint64_t after, size_t limit ) const
{
    auto& d = m_impl->db;
    limit = CodePageLimit<StoreCallsite>( d, limit );
    StorePage<StoreCallsite> page;
    if( !limit ) return page;
    page.values.reserve( limit );
    auto q = d.Prep( "SELECT id,event,thread,source,stack,domain,flags,provenance,reason FROM callsites WHERE event>? ORDER BY event LIMIT ?" );
    Int( q, 1, after );
    Int( q, 2, limit + 1 );
    while( d.Step( q ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StoreCallsite c;
        c.id = sqlite3_column_int64( q, 0 );
        c.event = sqlite3_column_int64( q, 1 );
        c.thread = sqlite3_column_int64( q, 2 );
        c.source = sqlite3_column_int64( q, 3 );
        c.stack = sqlite3_column_int64( q, 4 );
        c.domain = uint8_t( sqlite3_column_int( q, 5 ) );
        c.flags = uint8_t( sqlite3_column_int( q, 6 ) );
        c.provenance = uint8_t( sqlite3_column_int( q, 7 ) );
        c.reason = uint8_t( sqlite3_column_int( q, 8 ) );
        page.next = c.event;
        page.values.push_back( c );
    }
    sqlite3_reset( q );
    return page;
}
std::vector<StoreEntityKey> AuxiliaryEntities( const StoreAuxiliaryRecord& record )
{
    std::vector<StoreEntityKey> result;
    const auto add = [&]( uint8_t kind, uint64_t id ) { if(id && std::find(result.begin(),result.end(),StoreEntityKey{kind,id}) == result.end()) result.push_back({kind,id}); };
    const auto frame = uint8_t( JnEntityKind::Frame ), gfx = uint8_t( JnEntityKind::GfxEntity );
    std::visit( [&]( const auto& p ) {
        using T = std::decay_t<decltype( p )>;
        if constexpr( std::is_same_v<T, StoreFrameIdentity> ) add( frame, p.frame );
        else if constexpr( std::is_same_v<T, StoreGfxDispatch> )
        {
            add( gfx, p.dispatch );
            add( frame, p.frame );
        }
        else if constexpr( std::is_same_v<T, StoreGfxEntity> )
        {
            add( gfx, p.entity );
            add( gfx, p.parent );
            if( p.kind == uint8_t( JnGfxEntityKind::ExplicitGpuPass ) ) add( uint8_t( JnEntityKind::GpuPass ), p.entity );
            else if( p.kind == uint8_t( JnGfxEntityKind::GpuSegment ) ) add( uint8_t( JnEntityKind::GpuSegment ), p.entity );
            else if( p.kind == uint8_t( JnGfxEntityKind::CommandList ) ) add( uint8_t( JnEntityKind::CommandList ), p.entity );
            else if( p.kind == uint8_t( JnGfxEntityKind::Submission ) ) add( uint8_t( JnEntityKind::Submission ), p.entity );
        }
        else if constexpr( std::is_same_v<T, StoreGfxLink> )
        {
             // Match the typed Relation emitted by EmitJnGfxLink, including
             // captures where only the legacy GfxLink was recorded.
            auto source = JnEntityKind::GfxEntity, target = JnEntityKind::GfxEntity;
            switch( JnGfxRelation( p.relation ) )
            {
            case JnGfxRelation::Executes: source = JnEntityKind::Job; break;
            case JnGfxRelation::Produces: target = JnEntityKind::CommandList; break;
            case JnGfxRelation::Submits:
                source = JnEntityKind::CommandList;
                target = JnEntityKind::Submission;
                break;
            case JnGfxRelation::RunsOnGpu: target = JnEntityKind::GpuSegment; break;
            case JnGfxRelation::RecordedOnCommandList:
            case JnGfxRelation::RangeEvidenceComplete:
                source = JnEntityKind::GpuPass;
                target = JnEntityKind::CommandList;
                break;
            case JnGfxRelation::BelongsToFrame:
                source = JnEntityKind::GpuPass;
                target = JnEntityKind::Frame;
                break;
            case JnGfxRelation::BelongsToCamera:
                source = JnEntityKind::GpuPass;
                target = JnEntityKind::Camera;
                break;
            case JnGfxRelation::BelongsToView:
                source = JnEntityKind::GpuPass;
                target = JnEntityKind::View;
                break;
            case JnGfxRelation::ReferencesResources: source = target = JnEntityKind::GpuPass; break;
            case JnGfxRelation::ClassifiesAsTaxonomy:
                source = JnEntityKind::GpuPass;
                target = JnEntityKind::GpuTaxonomy;
                break;
            case JnGfxRelation::GpuSegmentReferencesResources:
                source = JnEntityKind::GpuSegment;
                target = JnEntityKind::GpuPass;
                break;
            default: break;
            }
            add( uint8_t( source ), p.source );
            add( uint8_t( target ), p.target );
        }
        else if constexpr( std::is_same_v<T, StoreRelation> )
        {
            add( p.sourceKind, p.source );
            add( p.targetKind, p.target );
        }
        else if constexpr( std::is_same_v<T, StoreRuntimeState> ) add( frame, p.requestedFrame );
    },
                record.data );
    return result;
}
StorePage<StoreAuxiliaryRecord> EventStore::AuxiliaryEvents( I begin, I end, std::optional<StoreAuxiliaryKind> kind, std::optional<StoreEntityKey> entity, uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "Auxiliary page limit exceeded" );
    if( kind ) Require( size_t( *kind ) < std::size( AuxiliaryTypes ), "Unknown auxiliary kind" );
    if( begin >= end || limit == 0 ) return {};
    auto& d = m_impl->db;
    limit = std::min<size_t>( limit, ( d.options.resultBytes - sizeof( StorePage<StoreAuxiliaryRecord> ) ) / sizeof( StoreAuxiliaryRecord ) );
    // Records are the raw packets of the auxiliary types. The packet blocks
    // that overlap the window and hold ids above the cursor are read in order
    // of their first id until no later block can enter the page.
    std::string types;
    for( size_t n = 0; n < std::size( AuxiliaryTypes ); ++n )
        if( !kind || size_t( *kind ) == n ) types += ( types.empty() ? "" : "," ) + std::to_string( uint8_t( AuxiliaryTypes[n] ) );
    auto q = d.Prep( "SELECT b.id,b.first FROM blocks b WHERE b.stream=3 AND b.key IN(" + types + ") AND b.id IN(SELECT id FROM block_range WHERE mint<=?1 AND maxt>=?2) AND b.tmin<?3 AND b.tmax>=?4 AND b.last>?5 ORDER BY b.first" );
    Int( q, 1, uint64_t( Bucket( end - 1 ) ) );
    Int( q, 2, uint64_t( Bucket( begin ) ) );
    Int( q, 3, uint64_t( end ) );
    Int( q, 4, uint64_t( begin ) );
    Int( q, 5, after );
    std::vector<std::pair<uint64_t, uint64_t>> candidates;
    while( d.Step( q ) ) candidates.emplace_back( sqlite3_column_int64( q, 0 ), sqlite3_column_int64( q, 1 ) );
    sqlite3_reset( q );
    const auto byId = []( const StoreAuxiliaryRecord& a, const StoreAuxiliaryRecord& b ) { return a.id < b.id; };
    std::vector<StoreAuxiliaryRecord> found;
    for( const auto& [block, first] : candidates )
    {
        if( found.size() > limit )
        {
            std::nth_element( found.begin(), found.begin() + limit, found.end(), byId );
            found.resize( limit + 1 );
            if( first > std::max_element( found.begin(), found.end(), byId )->id ) break;
        }
        const auto packets = LoadPacketBlock( d, m_impl->blocks, m_impl->zoneCache, block );
        for( size_t n = 0; n < packets->rows.size(); ++n )
        {
            const auto& r = packets->rows[n];
            if( r.id <= after || r.time < begin || r.time >= end ) continue;
            auto record = DecodeAuxiliary( r.id, r.thread, r.time, QueueType( packets->type ), packets->Payload( n ) );
            if( entity )
            {
                const auto keys = AuxiliaryEntities( record );
                if( std::find( keys.begin(), keys.end(), *entity ) == keys.end() ) continue;
            }
            found.push_back( std::move( record ) );
        }
    }
    std::sort( found.begin(), found.end(), byId );
    StorePage<StoreAuxiliaryRecord> page;
    page.more = found.size() > limit;
    page.values.reserve( limit );
    for( size_t n = 0; n < found.size() && n < limit; ++n ) page.values.push_back( std::move( found[n] ) );
    if( !page.values.empty() ) page.next = page.values.back().id;
    return page;
}
StorePage<StoreAuxiliaryRecord> EventStore::RuntimeStatesAt( I time, uint16_t afterDomain, size_t limit ) const
{
    Require( limit <= 256, "Runtime state page limit exceeded" );
    if( limit == 0 || afterDomain > 255 ) return {};
    auto& d = m_impl->db;
    limit = std::min<size_t>( limit, ( d.options.resultBytes - sizeof( StorePage<StoreAuxiliaryRecord> ) ) / sizeof( StoreAuxiliaryRecord ) );
    auto q = d.Prep( "SELECT domain,event FROM (SELECT d.domain domain,(SELECT s.event FROM jn_runtime_states s WHERE s.domain=d.domain AND s.time<=?1 ORDER BY s.time DESC,s.event DESC LIMIT 1) event FROM jn_runtime_domains d WHERE d.domain>=?2) WHERE event IS NOT NULL ORDER BY domain LIMIT ?3" );
    Int( q, 1, uint64_t( time ) );
    Int( q, 2, afterDomain );
    Int( q, 3, limit + 1 );
    StorePage<StoreAuxiliaryRecord> page;
    page.values.reserve( limit );
    try
    {
        while( d.Step( q ) )
        {
            if( page.values.size() == limit )
            {
                page.more = true;
                break;
            }
            const uint64_t event = sqlite3_column_int64( q, 1 );
            const auto packet = FindPacket( d, m_impl->blocks, m_impl->zoneCache, uint8_t( QueueType::JnRuntimeDomainState ), event );
            Require( packet.has_value(), "Missing runtime state record" );
            const auto& r = packet->block->rows[packet->index];
            page.values.push_back( DecodeAuxiliary( event, r.thread, r.time, QueueType::JnRuntimeDomainState, packet->block->Payload( packet->index ) ) );
            page.next = uint64_t( sqlite3_column_int64( q, 0 ) ) + 1;
        }
    }
    catch( ... )
    {
        sqlite3_reset( q );
        throw;
    }
    sqlite3_reset( q );
    return page;
}
StorePage<StoreCatalogResource> EventStore::CatalogResources( I begin, I end, std::optional<uint64_t> generation, uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "Catalog resource page limit exceeded" );
    StorePage<StoreCatalogResource> page;
    if( !limit || begin >= end ) return page;
    auto& d = m_impl->db;
    auto s = d.Prep( std::string( "SELECT id,generation,resource,begin,end FROM catalog_lifetimes WHERE id IN(SELECT id FROM catalog_lifetime_range WHERE mint<=?1 AND maxt>=?2) AND begin<?3 AND(end IS NULL OR end>?4 OR(end=begin AND end>=?4)) AND id>?5" ) + ( generation ? " AND generation=?7" : "" ) + " ORDER BY id LIMIT ?6" );
    Int( s, 1, uint64_t( Bucket( end - 1 ) ) );
    Int( s, 2, uint64_t( Bucket( begin ) ) );
    Int( s, 3, uint64_t( end ) );
    Int( s, 4, uint64_t( begin ) );
    Int( s, 5, after );
    Int( s, 6, limit + 1 );
    if( generation ) Int( s, 7, *generation );
    size_t bytes = 0;
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StoreCatalogResource r;
        r.id = sqlite3_column_int64( s, 0 );
        r.generation = sqlite3_column_int64( s, 1 );
        r.resource = sqlite3_column_int64( s, 2 );
        r.begin = sqlite3_column_int64( s, 3 );
        if( sqlite3_column_type( s, 4 ) != SQLITE_NULL ) r.end = sqlite3_column_int64( s, 4 );
        auto record = d.Prep( "SELECT data FROM catalog_records WHERE generation=?1 AND kind=0 AND object=?2 ORDER BY CASE WHEN time<=?3 THEN 0 ELSE 1 END,CASE WHEN time<=?3 THEN time END DESC,CASE WHEN time>?3 THEN time END,id DESC LIMIT 1" );
        Int( record, 1, r.generation );
        Int( record, 2, r.resource );
        Int( record, 3, uint64_t( end - 1 ) );
        Require( d.Step( record ) && sqlite3_column_bytes( record, 0 ) == sizeof( JnGpuCatalogResourceRecordV1 ), "Missing resource descriptor" );
        const auto p = static_cast<const uint8_t*>( sqlite3_column_blob( record, 0 ) );
        r.descriptor.assign( p, p + sizeof( JnGpuCatalogResourceRecordV1 ) );
        JnGpuCatalogResourceRecordV1 descriptor;
        memcpy( &descriptor, p, sizeof( descriptor ) );
        sqlite3_reset( record );
        if( descriptor.nameId )
        {
            record = d.Prep( "SELECT data FROM catalog_records WHERE generation=? AND kind=9 AND object=? ORDER BY id DESC LIMIT 1" );
            Int( record, 1, r.generation );
            Int( record, 2, descriptor.nameId );
            if( d.Step( record ) )
            {
                const auto n = sqlite3_column_bytes( record, 0 );
                Require( n >= sizeof( JnGpuCatalogStringRecordHeaderV1 ), "Invalid Catalog string" );
                const auto text = static_cast<const char*>( sqlite3_column_blob( record, 0 ) );
                r.name.assign( text + sizeof( JnGpuCatalogStringRecordHeaderV1 ), n - sizeof( JnGpuCatalogStringRecordHeaderV1 ) );
            }
            sqlite3_reset( record );
        }
        record = d.Prep( "SELECT valid FROM catalog_generations WHERE id=?" );
        Int( record, 1, r.generation );
        Require( d.Step( record ), "Missing resource generation" );
        r.valid = sqlite3_column_int( record, 0 ) != 0;
        sqlite3_reset( record );
        const auto cost = sizeof( r ) + r.name.size() + r.descriptor.size();
        if( bytes + cost > d.options.resultBytes )
        {
            Require( !page.values.empty(), "Catalog resource exceeds result budget" );
            page.more = true;
            break;
        }
        bytes += cost;
        page.next = r.id;
        page.values.push_back( std::move( r ) );
    }
    sqlite3_reset( s );
    return page;
}
StorePage<StoreCatalogGeneration> EventStore::CatalogGenerations( uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "Catalog generation page limit exceeded" );
    StorePage<StoreCatalogGeneration> page;
    if( !limit ) return page;
    auto& d = m_impl->db;
    limit = std::min<size_t>( limit, d.options.resultBytes / sizeof( StoreCatalogGeneration ) );
    auto s = d.Prep( "SELECT id,event,begin_value,end_value,begin,end,sequence,batches,records,bytes,unresolved,state,flags,ended,valid FROM catalog_generations WHERE event>? ORDER BY event LIMIT ?" );
    Int( s, 1, after );
    Int( s, 2, limit + 1 );
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StoreCatalogGeneration g;
        g.id = sqlite3_column_int64( s, 0 );
        g.event = sqlite3_column_int64( s, 1 );
        g.beginValue = sqlite3_column_int64( s, 2 );
        g.endValue = sqlite3_column_int64( s, 3 );
        g.begin = sqlite3_column_int64( s, 4 );
        g.end = sqlite3_column_int64( s, 5 );
        g.sequence = uint32_t( sqlite3_column_int64( s, 6 ) );
        g.batches = uint32_t( sqlite3_column_int64( s, 7 ) );
        g.records = sqlite3_column_int64( s, 8 );
        g.bytes = sqlite3_column_int64( s, 9 );
        g.unresolved = sqlite3_column_int64( s, 10 );
        g.state = uint8_t( sqlite3_column_int( s, 11 ) );
        g.flags = uint8_t( sqlite3_column_int( s, 12 ) );
        g.ended = sqlite3_column_int( s, 13 ) != 0;
        g.valid = sqlite3_column_int( s, 14 ) != 0;
        page.next = g.event;
        page.values.push_back( g );
    }
    sqlite3_reset( s );
    return page;
}
StorePage<StoreCatalogRecord> EventStore::CatalogRecords( uint8_t kind, std::optional<uint64_t> generation, uint64_t after, size_t limit ) const
{
    Require( kind <= 9 && limit <= 4096, "Invalid Catalog page request" );
    StorePage<StoreCatalogRecord> page;
    if( !limit ) return page;
    auto& d = m_impl->db;
    auto s = d.Prep( std::string( "SELECT r.id,r.generation,r.object,r.time,r.ordinal,r.data,b.sequence FROM catalog_records r JOIN catalog_batches b ON b.id=r.batch WHERE r.kind=?1 AND r.id>?2" ) + ( generation ? " AND r.generation=?4" : "" ) + " ORDER BY r.id LIMIT ?3" );
    Int( s, 1, kind );
    Int( s, 2, after );
    Int( s, 3, limit + 1 );
    if( generation ) Int( s, 4, *generation );
    size_t bytes = 0;
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        const auto size = sqlite3_column_bytes( s, 5 );
        Require( size >= 0 && size <= 1024, "Catalog record exceeds format budget" );
        const auto cost = sizeof( StoreCatalogRecord ) + size;
        if( bytes + cost > d.options.resultBytes )
        {
            Require( !page.values.empty(), "Catalog record exceeds result budget" );
            page.more = true;
            break;
        }
        StoreCatalogRecord record;
        record.id = sqlite3_column_int64( s, 0 );
        record.generation = sqlite3_column_int64( s, 1 );
        record.object = sqlite3_column_int64( s, 2 );
        if( sqlite3_column_type( s, 3 ) != SQLITE_NULL ) record.time = sqlite3_column_int64( s, 3 );
        record.ordinal = uint32_t( sqlite3_column_int64( s, 4 ) );
        record.sequence = uint32_t( sqlite3_column_int64( s, 6 ) );
        record.kind = kind;
        const auto p = static_cast<const uint8_t*>( sqlite3_column_blob( s, 5 ) );
        record.data.assign( p, p + size );
        bytes += cost;
        page.next = record.id;
        page.values.push_back( std::move( record ) );
    }
    sqlite3_reset( s );
    return page;
}
StorePage<StoreCatalogBatch> EventStore::CatalogBatches( std::optional<uint64_t> generation, uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "Catalog batch page limit exceeded" );
    StorePage<StoreCatalogBatch> page;
    if( !limit ) return page;
    auto& d = m_impl->db;
    limit = std::min<size_t>( limit, d.options.resultBytes / sizeof( StoreCatalogBatch ) );
    auto s = d.Prep( std::string( "SELECT id,generation,transport,stored,first_record,sequence,records,bytes,kind,encoding,flags,valid FROM catalog_batches WHERE id>?1" ) + ( generation ? " AND generation=?3" : "" ) + " ORDER BY id LIMIT ?2" );
    Int( s, 1, after );
    Int( s, 2, limit + 1 );
    if( generation ) Int( s, 3, *generation );
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StoreCatalogBatch b;
        b.id = sqlite3_column_int64( s, 0 );
        b.generation = sqlite3_column_int64( s, 1 );
        b.transportChecksum = sqlite3_column_int64( s, 2 );
        b.storedChecksum = sqlite3_column_int64( s, 3 );
        b.firstRecord = sqlite3_column_int64( s, 4 );
        b.sequence = uint32_t( sqlite3_column_int64( s, 5 ) );
        b.records = uint32_t( sqlite3_column_int64( s, 6 ) );
        b.bytes = uint32_t( sqlite3_column_int64( s, 7 ) );
        b.kind = uint8_t( sqlite3_column_int( s, 8 ) );
        b.encoding = uint8_t( sqlite3_column_int( s, 9 ) );
        b.flags = uint8_t( sqlite3_column_int( s, 10 ) );
        b.valid = sqlite3_column_int( s, 11 ) != 0;
        page.next = b.id;
        page.values.push_back( b );
    }
    sqlite3_reset( s );
    return page;
}
StorePage<StoreGpuReferencePass> EventStore::GpuReferencePasses( I begin, I end, uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "GPU pass page limit exceeded" );
    StorePage<StoreGpuReferencePass> page;
    if( begin >= end || !limit ) return page;
    auto& d = m_impl->db;
    limit = std::min<size_t>( limit, d.options.resultBytes / sizeof( StoreGpuReferencePass ) );
    auto s = d.Prep( "SELECT id,pass,time,frame,thread,taxonomy,level,flags FROM gpu_ref_passes WHERE time>=?1 AND time<?2 AND id>?3 ORDER BY id LIMIT ?4" );
    Int( s, 1, uint64_t( begin ) );
    Int( s, 2, uint64_t( end ) );
    Int( s, 3, after );
    Int( s, 4, limit + 1 );
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StoreGpuReferencePass p;
        p.id = sqlite3_column_int64( s, 0 );
        p.pass = sqlite3_column_int64( s, 1 );
        p.time = sqlite3_column_int64( s, 2 );
        p.frame = sqlite3_column_int64( s, 3 );
        p.thread = sqlite3_column_int64( s, 4 );
        p.taxonomy = uint32_t( sqlite3_column_int64( s, 5 ) );
        p.level = uint8_t( sqlite3_column_int( s, 6 ) );
        p.flags = uint8_t( sqlite3_column_int( s, 7 ) );
        page.next = p.id;
        page.values.push_back( p );
    }
    sqlite3_reset( s );
    return page;
}
StorePage<StoreGpuReference> EventStore::GpuReferences( I begin, I end, std::optional<uint64_t> pass, uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "GPU reference page limit exceeded" );
    StorePage<StoreGpuReference> page;
    if( begin >= end || !limit ) return page;
    auto& d = m_impl->db;
    limit = std::min<size_t>( limit, d.options.resultBytes / sizeof( StoreGpuReference ) );
    page.values.reserve( limit );
    const auto afterPacket = after >> 16;
    auto s = d.Prep( std::string( "SELECT id,pass,time,thread,setid,resource,mask,count,flags,encoding FROM gpu_ref_packets WHERE time>=?1 AND time<?2 AND id>=?3" ) + ( pass ? " AND pass=?5" : "" ) + " ORDER BY id LIMIT ?4" );
    Int( s, 1, uint64_t( begin ) );
    Int( s, 2, uint64_t( end ) );
    Int( s, 3, afterPacket );
    // The cursor packet may already be exhausted. Include that skipped row
    // plus one real look-ahead packet when all subsequent uses are legacy.
    Int( s, 4, limit + 2 );
    if( pass ) Int( s, 5, *pass );
    while( d.Step( s ) )
    {
        StoreGpuReference use;
        use.packet = sqlite3_column_int64( s, 0 );
        use.pass = sqlite3_column_int64( s, 1 );
        use.time = sqlite3_column_int64( s, 2 );
        use.thread = sqlite3_column_int64( s, 3 );
        use.set = uint32_t( sqlite3_column_int64( s, 4 ) );
        use.flags = uint8_t( sqlite3_column_int( s, 8 ) );
        use.encoding = uint8_t( sqlite3_column_int( s, 9 ) );
        const uint64_t start = use.packet == afterPacket ? ( after & 0xffff ) + 1 : 0;
        const auto append = [&]( uint64_t ordinal, uint64_t resource, uint32_t mask ) {
            if( page.values.size() == limit )
            {
                page.more = true;
                return false;
            }
            use.id = ( use.packet << 16 ) | ordinal;
            use.ordinal = uint16_t( ordinal );
            use.resource = resource;
            use.mask = mask;
            page.next = use.id;
            page.values.push_back( use );
            return true;
        };
        if( use.encoding == 1 )
        {
            if( start == 0 && !append( 0, sqlite3_column_int64( s, 5 ), uint32_t( sqlite3_column_int64( s, 6 ) ) ) ) break;
        }
        else
        {
            auto entries = d.Prep( "SELECT ordinal,resource,mask FROM gpu_ref_entries WHERE setid=? AND ordinal>=? ORDER BY ordinal LIMIT ?" );
            Int( entries, 1, use.set );
            Int( entries, 2, start );
            Int( entries, 3, limit - page.values.size() + 1 );
            while( d.Step( entries ) )
                if( !append( sqlite3_column_int64( entries, 0 ), sqlite3_column_int64( entries, 1 ), uint32_t( sqlite3_column_int64( entries, 2 ) ) ) ) break;
            sqlite3_reset( entries );
            if( page.more ) break;
        }
    }
    sqlite3_reset( s );
    return page;
}
StorePage<StoreGpuReferenceEnd> EventStore::GpuReferenceEnds( I begin, I end, uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "GPU end page limit exceeded" );
    StorePage<StoreGpuReferenceEnd> page;
    if( begin >= end || !limit ) return page;
    auto& d = m_impl->db;
    limit = std::min<size_t>( limit, d.options.resultBytes / sizeof( StoreGpuReferenceEnd ) );
    auto s = d.Prep( "SELECT id,pass,time,command_list,thread,total,dropped,flags FROM gpu_ref_ends WHERE time>=?1 AND time<?2 AND id>?3 ORDER BY id LIMIT ?4" );
    Int( s, 1, uint64_t( begin ) );
    Int( s, 2, uint64_t( end ) );
    Int( s, 3, after );
    Int( s, 4, limit + 1 );
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StoreGpuReferenceEnd e;
        e.id = sqlite3_column_int64( s, 0 );
        e.pass = sqlite3_column_int64( s, 1 );
        e.time = sqlite3_column_int64( s, 2 );
        e.commandList = sqlite3_column_int64( s, 3 );
        e.thread = sqlite3_column_int64( s, 4 );
        e.total = uint32_t( sqlite3_column_int64( s, 5 ) );
        e.dropped = uint16_t( sqlite3_column_int( s, 6 ) );
        e.flags = uint8_t( sqlite3_column_int( s, 7 ) );
        page.next = e.id;
        page.values.push_back( e );
    }
    sqlite3_reset( s );
    return page;
}
namespace
{
StoreAllocation ReadAllocation( Db& d, const BlockFile& file, ZoneBlockCache& cache, sqlite3_stmt* s )
{
    StoreAllocation a;
    a.id = sqlite3_column_int64( s, 0 );
    a.pool = sqlite3_column_int64( s, 1 );
    a.address = sqlite3_column_int64( s, 2 );
    a.size = sqlite3_column_int64( s, 3 );
    a.allocated = sqlite3_column_int64( s, 4 );
    if( sqlite3_column_type( s, 5 ) != SQLITE_NULL ) a.freed = sqlite3_column_int64( s, 5 );
    a.allocationThread = sqlite3_column_int64( s, 6 );
    a.freeThread = sqlite3_column_int64( s, 7 );
    a.allocationStack = sqlite3_column_int64( s, 8 );
    a.freeStack = sqlite3_column_int64( s, 9 );
    a.callsite = sqlite3_column_int64( s, 10 );
    // Innermost zone of the thread covering `time`; open zones end where
    // they are displayed to end.
    const auto zoneAt = [&]( uint64_t thread, I time ) {
        uint64_t best = 0;
        I bestStart = INT64_MIN;
        for( const auto block : ZoneBlocks( d, time, time == INT64_MAX ? time : time + 1, thread, 0 ) )
            for( const auto& r : LoadZoneBlock( d, file, cache, block )->rows )
            {
                if( r.start > time || r.start < bestStart || ( r.start == bestStart && r.id < best ) ) continue;
                I end = r.start;
                if( !r.open ) end = r.end;
                else
                {
                    auto q = d.Prep( "SELECT end FROM zone_display_end WHERE id=?" );
                    Int( q, 1, r.id );
                    if( d.Step( q ) ) end = sqlite3_column_int64( q, 0 );
                    sqlite3_reset( q );
                }
                if( end < time ) continue;
                best = r.id;
                bestStart = r.start;
            }
        return best;
    };
    a.allocationZone = zoneAt( a.allocationThread, a.allocated );
    if( a.freed ) a.freeZone = zoneAt( a.freeThread, *a.freed );
    return a;
}
const char* AllocationColumns() { return "SELECT id,pool,address,size,allocated,freed,allocation_thread,free_thread,allocation_stack,free_stack,callsite FROM allocations "; }
}
StorePage<StoreAllocation> EventStore::Allocations( I begin, I end, std::optional<uint64_t> pool, uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "Allocation page limit exceeded" );
    StorePage<StoreAllocation> page;
    if( begin >= end || !limit ) return page;
    auto& d = m_impl->db;
    limit = std::min<size_t>( limit, d.options.resultBytes / sizeof( StoreAllocation ) );
    page.values.reserve( limit );
    auto s = d.Prep( std::string( AllocationColumns() ) + "WHERE id IN(SELECT id FROM allocation_range WHERE mint<=?1 AND maxt>=?2) AND allocated<?3 AND (freed IS NULL OR freed>?4 OR (freed=allocated AND freed>=?4)) AND id>?5" + ( pool ? " AND pool=?7" : "" ) + " ORDER BY id LIMIT ?6" );
    Int( s, 1, uint64_t( Bucket( end - 1 ) ) );
    Int( s, 2, uint64_t( Bucket( begin ) ) );
    Int( s, 3, uint64_t( end ) );
    Int( s, 4, uint64_t( begin ) );
    Int( s, 5, after );
    Int( s, 6, limit + 1 );
    if( pool ) Int( s, 7, *pool );
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        auto a = ReadAllocation( d, m_impl->blocks, m_impl->zoneCache, s );
        page.next = a.id;
        page.values.push_back( a );
    }
    sqlite3_reset( s );
    return page;
}
StoreLiveAllocations EventStore::LiveAllocations( I time, uint64_t pool ) const
{
    StoreLiveAllocations live;
    auto& d = m_impl->db;
    const char* const filter = "WHERE id IN(SELECT id FROM allocation_range WHERE mint<=?1 AND maxt>=?2) AND allocated<?3 AND (freed IS NULL OR freed>?4 OR (freed=allocated AND freed>=?4)) AND pool=?5";
    const auto bind = [&]( sqlite3_stmt* s ) {
        Int( s, 1, uint64_t( Bucket( time ) ) );
        Int( s, 2, uint64_t( Bucket( time ) ) );
        Int( s, 3, uint64_t( time + 1 ) );
        Int( s, 4, uint64_t( time ) );
        Int( s, 5, pool );
    };
    auto s = d.Prep( std::string( "SELECT COUNT(*),TOTAL(size) FROM allocations " ) + filter );
    bind( s );
    if( d.Step( s ) )
    {
        live.count = sqlite3_column_int64( s, 0 );
        live.bytes = uint64_t( sqlite3_column_double( s, 1 ) );
    }
    sqlite3_reset( s );
    if( !live.count ) return live;
    live.largest.reserve( live.count );
    s = d.Prep( std::string( "SELECT id,pool,address,size,allocated,freed,allocation_thread FROM allocations " ) + filter + " ORDER BY size DESC,id" );
    bind( s );
    while( d.Step( s ) )
    {
        StoreAllocation a;
        a.id = sqlite3_column_int64( s, 0 );
        a.pool = sqlite3_column_int64( s, 1 );
        a.address = sqlite3_column_int64( s, 2 );
        a.size = sqlite3_column_int64( s, 3 );
        a.allocated = sqlite3_column_int64( s, 4 );
        if( sqlite3_column_type( s, 5 ) != SQLITE_NULL ) a.freed = sqlite3_column_int64( s, 5 );
        a.allocationThread = sqlite3_column_int64( s, 6 );
        live.largest.push_back( a );
    }
    sqlite3_reset( s );
    return live;
}
std::optional<StoreAllocation> EventStore::Allocation( uint64_t id ) const
{
    auto& d = m_impl->db;
    auto s = d.Prep( std::string( AllocationColumns() ) + "WHERE id=?" );
    Int( s, 1, id );
    if( !d.Step( s ) ) return {};
    auto value = ReadAllocation( d, m_impl->blocks, m_impl->zoneCache, s );
    sqlite3_reset( s );
    return value;
}
StorePage<StoreMemoryPool> EventStore::MemoryPools( uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "Memory pool page limit exceeded" );
    StorePage<StoreMemoryPool> page;
    if( !limit ) return page;
    auto& d = m_impl->db;
    auto s = d.Prep( "SELECT p.id,p.event,p.allocations,p.active,p.usage,p.frees,p.low,p.high,CASE WHEN p.id=0 THEN 'Default allocator' ELSE COALESCE(n.value,'???') END FROM memory_pools p LEFT JOIN strings n ON n.kind=" + std::to_string( uint8_t( QueueType::StringData ) ) + " AND n.key=p.id WHERE p.event>=? ORDER BY p.event LIMIT ?" );
    Int( s, 1, after );
    Int( s, 2, limit + 1 );
    size_t bytes = 0;
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StoreMemoryPool p;
        p.id = sqlite3_column_int64( s, 0 );
        p.event = sqlite3_column_int64( s, 1 );
        p.allocations = sqlite3_column_int64( s, 2 );
        p.active = sqlite3_column_int64( s, 3 );
        Require( sqlite3_column_bytes( s, 4 ) == 8, "Invalid memory pool usage" );
        memcpy( &p.activeBytes, sqlite3_column_blob( s, 4 ), 8 );
        p.frees = sqlite3_column_int64( s, 5 );
        if( sqlite3_column_type( s, 6 ) != SQLITE_NULL )
        {
            p.low = std::stoull( Col( s, 6 ), nullptr, 16 );
            if( p.low == UINT64_MAX ) p.low = 0;
        }
        if( sqlite3_column_type( s, 7 ) != SQLITE_NULL ) p.high = std::stoull( Col( s, 7 ), nullptr, 16 );
        p.name = Col( s, 8 );
        const auto n = sizeof( p ) + p.name.size();
        if( bytes + n > d.options.resultBytes )
        {
            Require( !page.values.empty(), "Memory pool exceeds result budget" );
            page.more = true;
            break;
        }
        bytes += n;
        page.next = p.event + 1;
        page.values.push_back( std::move( p ) );
    }
    sqlite3_reset( s );
    return page;
}
StorePage<StoreJob> EventStore::Jobs( I begin, I end, uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "Job page limit exceeded" );
    StorePage<StoreJob> page;
    if( begin >= end || !limit ) return page;
    auto& d = m_impl->db;
    auto s = d.Prep( JobColumns() + "WHERE j.id IN(SELECT id FROM job_range WHERE mint<=?1 AND maxt>=?2) AND COALESCE(j.first_time,j.schedule)<?3 AND (j.completed IS NULL OR j.completed>?4 OR (j.completed=COALESCE(j.first_time,j.schedule) AND j.completed>=?4)) AND j.event>?5 ORDER BY j.event LIMIT ?6" );
    Int( s, 1, uint64_t( Bucket( end - 1 ) ) );
    Int( s, 2, uint64_t( Bucket( begin ) ) );
    Int( s, 3, uint64_t( end ) );
    Int( s, 4, uint64_t( begin ) );
    Int( s, 5, after );
    Int( s, 6, limit + 1 );
    size_t bytes = 0;
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        auto j = ReadJob( d, s );
        const auto n = sizeof( j ) + j.name.size();
        if( bytes + n > d.options.resultBytes )
        {
            Require( !page.values.empty(), "Job exceeds result budget" );
            page.more = true;
            break;
        }
        bytes += n;
        page.next = j.event;
        page.values.push_back( std::move( j ) );
    }
    sqlite3_reset( s );
    return page;
}
std::optional<StoreJob> EventStore::Job( uint64_t id ) const
{
    auto& d = m_impl->db;
    auto s = d.Prep( JobColumns() + "WHERE j.id=?" );
    Int( s, 1, id );
    if( !d.Step( s ) ) return {};
    auto job = ReadJob( d, s );
    sqlite3_reset( s );
    Require( sizeof( job ) + job.name.size() <= d.options.resultBytes, "Job exceeds result budget" );
    return job;
}
StorePage<StoreJobStage> EventStore::JobStages( uint64_t job, uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "Job stage page limit exceeded" );
    StorePage<StoreJobStage> page;
    if( !limit ) return page;
    auto& d = m_impl->db;
    limit = std::min<size_t>( limit, d.options.resultBytes / sizeof( StoreJobStage ) );
    page.values.reserve( limit );
    // A Job's stages are the JnJobStage packets naming it between its first
    // and last stage ids. Thread, span, stack and callsite are derived from
    // the packet as ingestion derived them.
    auto q = d.Prep( "SELECT stage_first,stage_last FROM jobs WHERE id=?" );
    Int( q, 1, job );
    uint64_t first = 0, last = 0;
    if( d.Step( q ) && sqlite3_column_type( q, 0 ) != SQLITE_NULL )
    {
        first = sqlite3_column_int64( q, 0 );
        last = sqlite3_column_int64( q, 1 );
    }
    sqlite3_reset( q );
    if( !first || last <= after ) return page;
    struct Callsite
    {
        bool found = false;
        uint64_t stack = 0;
        uint8_t provenance = 3, reason = 0;
    };
    std::unordered_map<uint64_t, Callsite> callsites;
    const auto callsiteOf = [&]( uint64_t id ) {
        auto found = callsites.find( id );
        if( found != callsites.end() ) return found->second;
        Callsite c;
        auto cq = d.Prep( "SELECT stack,provenance,reason FROM callsites WHERE id=?" );
        Int( cq, 1, id );
        if( d.Step( cq ) )
        {
            c.found = true;
            c.stack = sqlite3_column_int64( cq, 0 );
            c.provenance = uint8_t( sqlite3_column_int( cq, 1 ) );
            c.reason = uint8_t( sqlite3_column_int( cq, 2 ) );
        }
        sqlite3_reset( cq );
        return callsites.emplace( id, c ).first->second;
    };
    ForPackets( d, m_impl->blocks, m_impl->zoneCache, uint8_t( QueueType::JnJobStage ), std::max( first, after ), I( last ), [&]( const PacketBlock& packets, size_t n ) {
        const auto& r = packets.rows[n];
        if( r.id <= after ) return true;
        QueueJnJobStage p;
        Require( packets.size >= sizeof( p ), "Invalid stored Job stage" );
        memcpy( &p, packets.Payload( n ).data(), sizeof( p ) );
        if( p.jobId != job ) return true;
        if( page.values.size() == limit )
        {
            page.more = true;
            return false;
        }
        const auto stage = JnJobStage( p.stage );
        const bool callstack = stage == JnJobStage::ScheduleCallstack || stage == JnJobStage::WaitCallstack;
        const uint64_t stack = callstack ? r.stack : 0;
        StoreJobStage e;
        e.id = r.id;
        e.job = job;
        e.time = r.time;
        e.thread = callstack ? p.arg0 : r.thread;
        e.span = callstack ? uint32_t( stack ) : p.spanId;
        e.arg0 = p.arg0;
        e.arg1 = p.arg1;
        e.stage = p.stage;
        e.flags = p.flags;
        e.callsite = stage == JnJobStage::ScheduleCallsite || stage == JnJobStage::WaitCallsite ? p.spanId : 0;
        const auto c = callsiteOf( e.callsite );
        e.stack = stack ? stack : c.found ? c.stack : 0;
        e.provenance = e.callsite ? ( c.found ? c.provenance : 3 ) : stack ? 2 : 3;
        e.reason = c.found ? c.reason : 0;
        page.next = e.id;
        page.values.push_back( e );
        return true;
    } );
    return page;
}
StorePage<StoreJobDependency> EventStore::JobDependencies( uint64_t job, uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "Dependency page limit exceeded" );
    StorePage<StoreJobDependency> page;
    if( !limit ) return page;
    auto& d = m_impl->db;
    limit = std::min<size_t>( limit, d.options.resultBytes / sizeof( StoreJobDependency ) );
    page.values.reserve( limit );
    auto s = d.Prep( "SELECT id,job,prerequisite,handle,flags FROM job_dependencies WHERE job=? AND id>? ORDER BY id LIMIT ?" );
    Int( s, 1, job );
    Int( s, 2, after );
    Int( s, 3, limit + 1 );
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StoreJobDependency dep{ uint64_t( sqlite3_column_int64( s, 0 ) ), uint64_t( sqlite3_column_int64( s, 1 ) ), uint64_t( sqlite3_column_int64( s, 2 ) ), uint64_t( sqlite3_column_int64( s, 3 ) ), uint8_t( sqlite3_column_int( s, 4 ) ) };
        page.next = dep.id;
        page.values.push_back( dep );
    }
    sqlite3_reset( s );
    return page;
}
StorePage<uint32_t> EventStore::JobLanes( uint64_t job, uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "Job lane page limit exceeded" );
    StorePage<uint32_t> page;
    if( !limit || after > UINT32_MAX ) return page;
    auto& d = m_impl->db;
    limit = std::min<size_t>( limit, d.options.resultBytes / sizeof( uint32_t ) );
    auto s = d.Prep( "SELECT lane FROM job_lanes WHERE job=? AND lane>=? ORDER BY lane LIMIT ?" );
    Int( s, 1, job );
    Int( s, 2, after );
    Int( s, 3, limit + 1 );
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        const auto lane = uint32_t( sqlite3_column_int64( s, 0 ) );
        page.next = uint64_t( lane ) + 1;
        page.values.push_back( lane );
    }
    sqlite3_reset( s );
    return page;
}
StorePage<StoreIoRequest> EventStore::IoRequests( I begin, I end, uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "IO page limit exceeded" );
    StorePage<StoreIoRequest> page;
    if( begin >= end || !limit ) return page;
    auto& d = m_impl->db;
    limit = std::min<size_t>( limit, d.options.resultBytes / sizeof( StoreIoRequest ) );
    page.values.reserve( limit );
    auto s = d.Prep( IoColumns() + "WHERE id IN(SELECT id FROM io_range WHERE mint<=?1 AND maxt>=?2) AND COALESCE(first_time,queued)<?3 AND (end IS NULL OR end>?4 OR(end=COALESCE(first_time,queued) AND end>=?4)) AND event>?5 ORDER BY event LIMIT ?6" );
    Int( s, 1, uint64_t( Bucket( end - 1 ) ) );
    Int( s, 2, uint64_t( Bucket( begin ) ) );
    Int( s, 3, uint64_t( end ) );
    Int( s, 4, uint64_t( begin ) );
    Int( s, 5, after );
    Int( s, 6, limit + 1 );
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        auto request = ReadIo( s );
        page.next = request.event;
        page.values.push_back( request );
    }
    sqlite3_reset( s );
    return page;
}
std::optional<StoreIoRequest> EventStore::IoRequest( uint64_t id ) const
{
    auto& d = m_impl->db;
    auto s = d.Prep( IoColumns() + "WHERE id=?" );
    Int( s, 1, id );
    if( !d.Step( s ) ) return {};
    auto request = ReadIo( s );
    sqlite3_reset( s );
    return request;
}
StorePage<StoreIoStage> EventStore::IoStages( uint64_t request, uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "IO stage page limit exceeded" );
    StorePage<StoreIoStage> page;
    if( !limit ) return page;
    auto& d = m_impl->db;
    limit = std::min<size_t>( limit, d.options.resultBytes / sizeof( StoreIoStage ) );
    page.values.reserve( limit );
    auto s = d.Prep( "SELECT id,request,time,thread,bytes,detail,stage,status,flags,stack FROM io_stages WHERE request=? AND id>? ORDER BY id LIMIT ?" );
    Int( s, 1, request );
    Int( s, 2, after );
    Int( s, 3, limit + 1 );
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StoreIoStage e;
        e.id = sqlite3_column_int64( s, 0 );
        e.request = sqlite3_column_int64( s, 1 );
        e.time = sqlite3_column_int64( s, 2 );
        e.thread = sqlite3_column_int64( s, 3 );
        e.bytes = sqlite3_column_int64( s, 4 );
        e.detail = uint32_t( sqlite3_column_int( s, 5 ) );
        e.stage = uint8_t( sqlite3_column_int( s, 6 ) );
        e.status = uint8_t( sqlite3_column_int( s, 7 ) );
        e.flags = uint8_t( sqlite3_column_int( s, 8 ) );
        e.stack = sqlite3_column_int64( s, 9 );
        page.next = e.id;
        page.values.push_back( e );
    }
    sqlite3_reset( s );
    return page;
}
StorePage<StoreScriptFrame> EventStore::ScriptFrames( uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "Script frame page limit exceeded" );
    StorePage<StoreScriptFrame> page;
    if( !limit ) return page;
    auto& d = m_impl->db;
    const auto kind = std::to_string( uint8_t( QueueType::StringData ) );
    auto s = d.Prep( "SELECT f.id,f.thread,f.frame,f.line,f.runtime,f.flags,COALESCE(n.value,''),COALESCE(p.value,'') FROM script_frames f LEFT JOIN strings n ON n.kind=" + kind + " AND n.key=f.function_key LEFT JOIN strings p ON p.kind=" + kind + " AND p.key=f.file_key WHERE f.id>? ORDER BY f.id LIMIT ?" );
    Int( s, 1, after );
    Int( s, 2, limit + 1 );
    size_t bytes = 0;
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StoreScriptFrame f;
        f.id = sqlite3_column_int64( s, 0 );
        f.thread = sqlite3_column_int64( s, 1 );
        f.frame = uint32_t( sqlite3_column_int( s, 2 ) );
        f.line = uint32_t( sqlite3_column_int( s, 3 ) );
        f.runtime = uint8_t( sqlite3_column_int( s, 4 ) );
        f.flags = uint8_t( sqlite3_column_int( s, 5 ) );
        f.function = Col( s, 6 );
        f.file = Col( s, 7 );
        const auto n = sizeof( f ) + f.function.size() + f.file.size();
        if( bytes + n > d.options.resultBytes )
        {
            Require( !page.values.empty(), "Script frame exceeds result budget" );
            page.more = true;
            break;
        }
        bytes += n;
        page.next = f.id;
        page.values.push_back( std::move( f ) );
    }
    sqlite3_reset( s );
    return page;
}
StorePage<StoreScriptEvent> EventStore::ScriptEvents( I begin, I end, uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "Script page limit exceeded" );
    StorePage<StoreScriptEvent> page;
    if( begin >= end || !limit ) return page;
    auto& d = m_impl->db;
    auto s = d.Prep( "SELECT e.id,e.thread,e.time,e.primary_id,e.secondary_id,e.value,e.runtime,e.flags,e.kind,CASE WHEN e.kind=3 THEN COALESCE(n.value,'') ELSE '' END FROM script_events e LEFT JOIN strings n ON n.kind=" + std::to_string( uint8_t( QueueType::StringData ) ) + " AND n.key=e.secondary_id WHERE e.time>=?1 AND e.time<?2 AND e.id>?3 ORDER BY e.id LIMIT ?4" );
    Int( s, 1, uint64_t( begin ) );
    Int( s, 2, uint64_t( end ) );
    Int( s, 3, after );
    Int( s, 4, limit + 1 );
    size_t bytes = 0;
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StoreScriptEvent e;
        e.id = sqlite3_column_int64( s, 0 );
        e.thread = sqlite3_column_int64( s, 1 );
        e.time = sqlite3_column_int64( s, 2 );
        e.primary = sqlite3_column_int64( s, 3 );
        e.secondary = sqlite3_column_int64( s, 4 );
        e.value = uint32_t( sqlite3_column_int( s, 5 ) );
        e.runtime = uint8_t( sqlite3_column_int( s, 6 ) );
        e.flags = uint8_t( sqlite3_column_int( s, 7 ) );
        e.kind = uint8_t( sqlite3_column_int( s, 8 ) );
        e.text = Col( s, 9 );
        const auto n = sizeof( e ) + e.text.size();
        if( bytes + n > d.options.resultBytes )
        {
            Require( !page.values.empty(), "Script event exceeds result budget" );
            page.more = true;
            break;
        }
        bytes += n;
        page.next = e.id;
        page.values.push_back( std::move( e ) );
    }
    sqlite3_reset( s );
    return page;
}
StorePage<StoreMessage> EventStore::Messages( I begin, I end, std::optional<uint64_t> thread, uint64_t after, size_t limit, bool app ) const
{
    Require( limit <= 4096, "Message page limit exceeded" );
    StorePage<StoreMessage> page;
    if( begin >= end || !limit ) return page;
    auto& d = m_impl->db;
    auto s = d.Prep( MessageColumns() + "WHERE m.time>=?1 AND m.time<?2 AND m.id>?3 AND m.app=?5 " + std::string( thread ? "AND m.thread=?6 " : "" ) + "ORDER BY m.id LIMIT ?4" );
    Int( s, 1, uint64_t( begin ) );
    Int( s, 2, uint64_t( end ) );
    Int( s, 3, after );
    Int( s, 4, limit + 1 );
    Int( s, 5, app ? 1 : 0 );
    if( thread ) Int( s, 6, *thread );
    size_t bytes = 0;
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        auto m = ReadMessage( s );
        const auto n = sizeof( m ) + m.text.size();
        if( bytes + n > d.options.resultBytes )
        {
            Require( !page.values.empty(), "Message exceeds result budget" );
            page.more = true;
            break;
        }
        bytes += n;
        page.next = m.id;
        page.values.push_back( std::move( m ) );
    }
    sqlite3_reset( s );
    return page;
}
std::optional<StoreMessage> EventStore::Message( uint64_t id ) const
{
    auto& d = m_impl->db;
    auto s = d.Prep( MessageColumns() + "WHERE m.id=?" );
    Int( s, 1, id );
    if( !d.Step( s ) ) return {};
    auto message = ReadMessage( s );
    sqlite3_reset( s );
    Require( sizeof( message ) + message.text.size() <= d.options.resultBytes, "Message exceeds result budget" );
    return message;
}
StorePage<StoreParameter> EventStore::Parameters(uint64_t after,size_t limit) const
{
    Require(limit<=4096,"Parameter page limit exceeded");StorePage<StoreParameter> page;if(!limit)return page;auto& d=m_impl->db;
    auto q=d.Prep("SELECT p.event,p.idx,p.is_bool,p.value,COALESCE(s.value,'???') FROM parameters p LEFT JOIN strings s ON s.key=p.name_key AND s.kind=? WHERE p.event>? ORDER BY p.event LIMIT ?");Int(q,1,uint8_t(QueueType::StringData));Int(q,2,after);Int(q,3,limit+1);size_t bytes=0;
    while(d.Step(q))
    {
        StoreParameter p;p.event=sqlite3_column_int64(q,0);p.index=uint32_t(sqlite3_column_int64(q,1));p.boolean=sqlite3_column_int(q,2)!=0;p.value=int32_t(sqlite3_column_int64(q,3));p.name=Col(q,4);const auto n=sizeof(p)+p.name.size();
        if(page.values.size()==limit || bytes+n>d.options.resultBytes){Require(!page.values.empty(),"Parameter exceeds result budget");page.more=true;break;}bytes+=n;page.next=p.event;page.values.push_back(std::move(p));
    }
    sqlite3_reset(q);return page;
}
StorePage<StorePlot> EventStore::Plots( uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "Plot page limit exceeded" );
    StorePage<StorePlot> page;
    if( !limit ) return page;
    auto& d = m_impl->db;
    auto s = d.Prep( "SELECT id,name,kind,format,steps,fill,color,points,min,max,sum FROM plots WHERE id>? ORDER BY id LIMIT ?" );
    Int( s, 1, after );
    Int( s, 2, limit + 1 );
    size_t bytes = 0;
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StorePlot p;
        p.id = sqlite3_column_int64( s, 0 );
        p.name = Col( s, 1 );
        p.kind = uint8_t( sqlite3_column_int( s, 2 ) );
        p.format = uint8_t( sqlite3_column_int( s, 3 ) );
        p.steps = sqlite3_column_int( s, 4 ) != 0;
        p.fill = sqlite3_column_int( s, 5 ) != 0;
        p.color = uint32_t( sqlite3_column_int64( s, 6 ) );
        p.points = sqlite3_column_int64( s, 7 );
        p.min = sqlite3_column_type(s,8)==SQLITE_BLOB?StoredDouble(s,8):sqlite3_column_double(s,8);
        p.max = sqlite3_column_type(s,9)==SQLITE_BLOB?StoredDouble(s,9):sqlite3_column_double(s,9);
        p.sum = StoredDouble( s, 10 );
        const auto n = sizeof( p ) + p.name.size();
        if( bytes + n > d.options.resultBytes )
        {
            Require( !page.values.empty(), "Plot exceeds result budget" );
            page.more = true;
            break;
        }
        bytes += n;
        page.next = p.id;
        page.values.push_back( std::move( p ) );
    }
    sqlite3_reset( s );
    return page;
}
StorePage<StorePlotPoint> EventStore::PlotPoints( I begin, I end, std::optional<uint64_t> plot, uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "Plot point page limit exceeded" );
    StorePage<StorePlotPoint> page;
    if( begin >= end || !limit ) return page;
    auto& d = m_impl->db;
    limit = std::min<size_t>( limit, d.options.resultBytes / sizeof( StorePlotPoint ) );
    page.values.reserve( limit );
    auto s = d.Prep( "SELECT id,plot,time,value FROM plot_points WHERE time>=?1 AND time<?2 AND id>?3 " + std::string( plot ? "AND plot=?5 " : "" ) + "ORDER BY id LIMIT ?4" );
    Int( s, 1, uint64_t( begin ) );
    Int( s, 2, uint64_t( end ) );
    Int( s, 3, after );
    Int( s, 4, limit + 1 );
    if( plot ) Int( s, 5, *plot );
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StorePlotPoint p{ uint64_t( sqlite3_column_int64( s, 0 ) ), uint64_t( sqlite3_column_int64( s, 1 ) ), sqlite3_column_int64( s, 2 ), StoredDouble( s, 3 ) };
        page.next = p.id;
        page.values.push_back( p );
    }
    sqlite3_reset( s );
    return page;
}
StorePage<StoreLock> EventStore::Locks( uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "Lock page limit exceeded" );
    StorePage<StoreLock> page;
    if( !limit ) return page;
    auto& d = m_impl->db;
    auto s = d.Prep( LockColumns( false ) + "WHERE l.id>=? ORDER BY l.id LIMIT ?" );
    Int( s, 1, after );
    Int( s, 2, limit + 1 );
    size_t bytes = 0;
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StoreLock l;
        l.id = sqlite3_column_int64( s, 0 );
        l.type = uint8_t( sqlite3_column_int( s, 1 ) );
        l.announce = sqlite3_column_int64( s, 2 );
        if( sqlite3_column_type( s, 3 ) != SQLITE_NULL ) l.terminate = sqlite3_column_int64( s, 3 );
        l.name = Col( s, 4 );
        l.function = Col( s, 5 );
        l.file = Col( s, 6 );
        l.line = uint32_t( sqlite3_column_int( s, 7 ) );
        l.contended = sqlite3_column_int( s, 8 ) != 0;
        l.events = sqlite3_column_int64( s, 9 );
        l.threads = sqlite3_column_int64( s, 10 );
        const auto n = sizeof( l ) + l.name.size() + l.function.size() + l.file.size();
        if( bytes + n > d.options.resultBytes )
        {
            Require( !page.values.empty(), "Lock exceeds result budget" );
            page.more = true;
            break;
        }
        bytes += n;
        page.next = l.id + 1;
        page.values.push_back( std::move( l ) );
    }
    sqlite3_reset( s );
    return page;
}
StorePage<StoreLockEvent> EventStore::LockEvents( I begin, I end, std::optional<uint64_t> lock, uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "Lock event page limit exceeded" );
    StorePage<StoreLockEvent> page;
    if( begin >= end || !limit ) return page;
    auto& d = m_impl->db;
    auto s = d.Prep( LockColumns( true ) + "WHERE e.time>=?1 AND e.time<?2 AND e.id>?3 " + std::string( lock ? "AND e.lock=?5 " : "" ) + "ORDER BY e.id LIMIT ?4" );
    Int( s, 1, uint64_t( begin ) );
    Int( s, 2, uint64_t( end ) );
    Int( s, 3, after );
    Int( s, 4, limit + 1 );
    if( lock ) Int( s, 5, *lock );
    size_t bytes = 0;
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        auto e = ReadLockEvent( d, s );
        const auto n = sizeof( e ) + e.name.size() + e.function.size() + e.file.size() + ( e.waiters.size() + e.sharedWaiters.size() + e.sharedOwners.size() ) * 8;
        if( bytes + n > d.options.resultBytes )
        {
            Require( !page.values.empty(), "Lock event exceeds result budget" );
            page.more = true;
            break;
        }
        bytes += n;
        page.next = e.id;
        page.values.push_back( std::move( e ) );
    }
    sqlite3_reset( s );
    return page;
}
StorePage<StoreSample> EventStore::Samples( I begin, I end, std::optional<uint64_t> thread, uint64_t after, size_t limit, bool includePending ) const
{
    Require( limit <= 4096, "Sample page limit exceeded" );
    StorePage<StoreSample> page;
    if( begin >= end || !limit ) return page;
    auto& d = m_impl->db;
    limit = std::min<size_t>( limit, d.options.resultBytes / sizeof( StoreSample ) );
    page.values.reserve( limit );
    const auto predicate = " WHERE time>=?1 AND time<?2 AND id>?3 " + std::string( thread ? "AND thread=?5 " : "" );
    auto sql = "SELECT id,thread,time,stack,pending,explicit_context,EXISTS(SELECT 1 FROM context_regions c WHERE c.thread=s.thread AND c.start=s.time AND c.reason!=-2) FROM (SELECT id,thread,time,stack,0 AS pending,explicit_context FROM samples" + predicate;
    if( includePending ) sql += " UNION ALL SELECT id,thread,time,stack,1 AS pending,0 AS explicit_context FROM sample_pending" + predicate + " AND NOT EXISTS(SELECT 1 FROM samples m WHERE m.id=sample_pending.id) ";
    sql += ") s ORDER BY id LIMIT ?4";
    auto s = d.Prep( sql );
    Int( s, 1, uint64_t( begin ) );
    Int( s, 2, uint64_t( end ) );
    Int( s, 3, after );
    Int( s, 4, limit + 1 );
    if( thread ) Int( s, 5, *thread );
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StoreSample sample;
        sample.id = sqlite3_column_int64( s, 0 );
        sample.thread = sqlite3_column_int64( s, 1 );
        sample.time = sqlite3_column_int64( s, 2 );
        sample.stack = sqlite3_column_int64( s, 3 );
        sample.pending = sqlite3_column_int( s, 4 ) != 0;
        sample.explicitContextSwitch = sqlite3_column_int( s, 5 ) != 0;
        sample.contextSwitch = sqlite3_column_int( s, 6 ) != 0;
        page.next = sample.id;
        page.values.push_back( sample );
    }
    sqlite3_reset( s );
    return page;
}
std::optional<StoreSample> EventStore::Sample( uint64_t id ) const
{
    auto& d = m_impl->db;
    auto s = d.Prep( "SELECT id,thread,time,stack,pending,explicit_context,EXISTS(SELECT 1 FROM context_regions c WHERE c.thread=x.thread AND c.start=x.time AND c.reason!=-2) FROM (SELECT id,thread,time,stack,0 AS pending,explicit_context FROM samples WHERE id=?1 UNION ALL SELECT id,thread,time,stack,1 AS pending,0 AS explicit_context FROM sample_pending WHERE id=?1) x ORDER BY pending LIMIT 1" );
    Int( s, 1, id );
    if( !d.Step( s ) ) return {};
    StoreSample sample;
    sample.id = sqlite3_column_int64( s, 0 );
    sample.thread = sqlite3_column_int64( s, 1 );
    sample.time = sqlite3_column_int64( s, 2 );
    sample.stack = sqlite3_column_int64( s, 3 );
    sample.pending = sqlite3_column_int( s, 4 ) != 0;
    sample.explicitContextSwitch = sqlite3_column_int( s, 5 ) != 0;
    sample.contextSwitch = sqlite3_column_int( s, 6 ) != 0;
    sqlite3_reset( s );
    return sample;
}
StorePage<StoreContextRegion> EventStore::ContextRegions( I begin, I end, std::optional<uint64_t> thread, uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "Context page limit exceeded" );
    StorePage<StoreContextRegion> page;
    if( begin >= end || !limit ) return page;
    auto& d = m_impl->db;
    limit = std::min<size_t>( limit, d.options.resultBytes / sizeof( StoreContextRegion ) );
    page.values.reserve( limit );
    auto s = d.Prep( "SELECT r.id,r.thread,r.start,r.end,r.wakeup,r.wake_cpu,r.cpu,r.reason,r.state,r.related FROM context_regions r WHERE r.id IN(SELECT id FROM context_range WHERE mint<=?1 AND maxt>=?2) AND r.start<?3 AND (r.end IS NULL OR r.end>?4 OR (r.end=r.start AND r.start>=?4)) AND r.id>?5 " + std::string( thread ? "AND r.thread=?7 " : "" ) + "ORDER BY r.id LIMIT ?6" );
    Int( s, 1, uint64_t( Bucket( end - 1 ) ) );
    Int( s, 2, uint64_t( Bucket( begin ) ) );
    Int( s, 3, uint64_t( end ) );
    Int( s, 4, uint64_t( begin ) );
    Int( s, 5, after );
    Int( s, 6, limit + 1 );
    if( thread ) Int( s, 7, *thread );
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StoreContextRegion r;
        r.id = sqlite3_column_int64( s, 0 );
        r.thread = sqlite3_column_int64( s, 1 );
        r.start = sqlite3_column_int64( s, 2 );
        if( sqlite3_column_type( s, 3 ) != SQLITE_NULL ) r.end = sqlite3_column_int64( s, 3 );
        if( sqlite3_column_type( s, 4 ) != SQLITE_NULL ) r.wakeup = sqlite3_column_int64( s, 4 );
        r.wakeupCpu = uint8_t( sqlite3_column_int( s, 5 ) );
        r.cpu = uint8_t( sqlite3_column_int( s, 6 ) );
        r.reason = int8_t( sqlite3_column_int( s, 7 ) );
        r.state = int8_t( sqlite3_column_int( s, 8 ) );
        r.relatedThread = sqlite3_column_int64( s, 9 );
        page.next = r.id;
        page.values.push_back( r );
    }
    sqlite3_reset( s );
    return page;
}
StorePage<StoreContextRegion> EventStore::CpuRegions( I begin, I end, std::optional<uint8_t> cpu, uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "CPU region page limit exceeded" );
    StorePage<StoreContextRegion> page;
    if( begin >= end || !limit ) return page;
    auto& d = m_impl->db;
    limit = std::min<size_t>( limit, d.options.resultBytes / sizeof( StoreContextRegion ) );
    page.values.reserve( limit );
    auto s = d.Prep( "SELECT r.id,r.thread,r.start,r.end,r.cpu FROM cpu_regions r WHERE r.id IN(SELECT id FROM cpu_region_range WHERE mint<=?1 AND maxt>=?2) AND r.start<?3 AND (r.end IS NULL OR r.end>?4 OR (r.end=r.start AND r.start>=?4)) AND r.id>?5 " + std::string( cpu ? "AND r.cpu=?7 " : "" ) + "ORDER BY r.id LIMIT ?6" );
    Int( s, 1, uint64_t( Bucket( end - 1 ) ) );
    Int( s, 2, uint64_t( Bucket( begin ) ) );
    Int( s, 3, uint64_t( end ) );
    Int( s, 4, uint64_t( begin ) );
    Int( s, 5, after );
    Int( s, 6, limit + 1 );
    if( cpu ) Int( s, 7, *cpu );
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StoreContextRegion r;
        r.id = sqlite3_column_int64( s, 0 );
        r.thread = sqlite3_column_int64( s, 1 );
        r.start = sqlite3_column_int64( s, 2 );
        if( sqlite3_column_type( s, 3 ) != SQLITE_NULL ) r.end = sqlite3_column_int64( s, 3 );
        r.cpu = uint8_t( sqlite3_column_int( s, 4 ) );
        page.next = r.id;
        page.values.push_back( r );
    }
    sqlite3_reset( s );
    return page;
}
StorePage<StoreGpuContext> EventStore::GpuContexts( uint64_t after, size_t limit ) const
{
    Require( limit <= 256, "GPU context page limit exceeded" );
    StorePage<StoreGpuContext> page;
    if( !limit ) return page;
    auto& d = m_impl->db;
    auto s = d.Prep( "SELECT c.id,c.sequence,c.thread,c.period,c.type,c.calibrated,c.name,c.overflow,c.zones,c.incomplete FROM gpu_contexts c WHERE c.sequence>? ORDER BY c.sequence LIMIT ?" );
    Int( s, 1, after );
    Int( s, 2, limit + 1 );
    size_t bytes = 0;
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StoreGpuContext c;
        c.id = sqlite3_column_int64( s, 0 );
        c.sequence = sqlite3_column_int64( s, 1 );
        c.thread = sqlite3_column_int64( s, 2 );
        c.period = sqlite3_column_double( s, 3 );
        c.type = uint8_t( sqlite3_column_int( s, 4 ) );
        c.calibrated = sqlite3_column_int( s, 5 ) != 0;
        c.name = Col( s, 6 );
        c.overflow = sqlite3_column_int64( s, 7 );
        c.zones = sqlite3_column_int64( s, 8 );
        c.incomplete = sqlite3_column_int64( s, 9 );
        auto n = sizeof( c ) + c.name.size();
        if( bytes + n > d.options.resultBytes )
        {
            Require( !page.values.empty(), "GPU context exceeds result budget" );
            page.more = true;
            break;
        }
        bytes += n;
        page.next = c.sequence;
        page.values.push_back( std::move( c ) );
    }
    sqlite3_reset( s );
    return page;
}
StorePage<StoreGpuZone> EventStore::GpuZones( I begin, I end, std::optional<uint64_t> context, uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "GPU page limit exceeded" );
    StorePage<StoreGpuZone> page;
    if( begin >= end || !limit ) return page;
    auto& d = m_impl->db;
    auto s = d.Prep( GpuColumns() + "WHERE z.id IN(SELECT id FROM gpu_range WHERE mint<=?1 AND maxt>=?2 UNION ALL SELECT id FROM gpu_zones WHERE gpu_start IS NULL) AND (z.gpu_start IS NULL OR (z.gpu_start<?3 AND (z.gpu_end IS NULL OR z.gpu_end>?4 OR (z.gpu_end=z.gpu_start AND z.gpu_start>=?4))) OR (z.gpu_end<z.gpu_start AND z.gpu_end<?3 AND z.gpu_start>?4)) AND z.id>?5 " + ( context ? "AND z.context=?6 " : "" ) + "ORDER BY z.id LIMIT ?7" );
    Int( s, 1, uint64_t( Bucket( end - 1 ) ) );
    Int( s, 2, uint64_t( Bucket( begin ) ) );
    Int( s, 3, uint64_t( end ) );
    Int( s, 4, uint64_t( begin ) );
    Int( s, 5, after );
    if( context ) Int( s, 6, *context );
    Int( s, 7, limit + 1 );
    size_t bytes = 0;
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        auto z = ReadGpu( s );
        auto n = GpuBytes( z );
        if( bytes + n > d.options.resultBytes )
        {
            Require( !page.values.empty(), "GPU zone exceeds result budget" );
            page.more = true;
            break;
        }
        bytes += n;
        page.next = z.id;
        page.values.push_back( std::move( z ) );
    }
    sqlite3_reset( s );
    return page;
}
std::optional<StoreGpuZone> EventStore::GpuZone( uint64_t id ) const
{
    auto& d = m_impl->db;
    auto s = d.Prep( GpuColumns() + "WHERE z.id=?" );
    Int( s, 1, id );
    if( !d.Step( s ) ) return {};
    auto z = ReadGpu( s );
    sqlite3_reset( s );
    Require( GpuBytes( z ) <= d.options.resultBytes, "GPU zone exceeds result budget" );
    return z;
}
StorePage<StoreGpuZone> EventStore::GpuChildren( uint64_t id, uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "GPU child page limit exceeded" );
    StorePage<StoreGpuZone> page;
    if( !limit ) return page;
    auto& d = m_impl->db;
    auto s = d.Prep( GpuColumns() + "WHERE z.parent=? AND z.id>? ORDER BY z.id LIMIT ?" );
    Int( s, 1, id );
    Int( s, 2, after );
    Int( s, 3, limit + 1 );
    size_t bytes = 0;
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        auto z = ReadGpu( s );
        auto n = GpuBytes( z );
        if( bytes + n > d.options.resultBytes )
        {
            Require( !page.values.empty(), "GPU child exceeds result budget" );
            page.more = true;
            break;
        }
        bytes += n;
        page.next = z.id;
        page.values.push_back( std::move( z ) );
    }
    sqlite3_reset( s );
    return page;
}
StorePage<StoreGpuNote> EventStore::GpuNotes( uint64_t context, uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "GPU note page limit exceeded" );
    StorePage<StoreGpuNote> page;
    if( !limit ) return page;
    auto& d = m_impl->db;
    auto s = d.Prep( "SELECT id,note,name FROM gpu_notes WHERE context=? AND id>? ORDER BY id LIMIT ?" );
    Int( s, 1, context );
    Int( s, 2, after );
    Int( s, 3, limit + 1 );
    size_t bytes = 0;
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StoreGpuNote note{ uint64_t( sqlite3_column_int64( s, 0 ) ), sqlite3_column_int64( s, 1 ), Col( s, 2 ) };
        auto n = sizeof( note ) + note.name.size();
        if( bytes + n > d.options.resultBytes )
        {
            Require( !page.values.empty(), "GPU note exceeds result budget" );
            page.more = true;
            break;
        }
        bytes += n;
        page.next = note.id;
        page.values.push_back( std::move( note ) );
    }
    sqlite3_reset( s );
    return page;
}
StorePage<StoreGpuAnnotation> EventStore::GpuAnnotations( uint64_t context, std::optional<uint16_t> query, uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "GPU annotation page limit exceeded" );
    StorePage<StoreGpuAnnotation> page;
    if( !limit ) return page;
    auto& d = m_impl->db;
    auto s = d.Prep( "SELECT a.id,a.query,a.note,a.value,COALESCE(n.name,'') FROM gpu_annotations a LEFT JOIN gpu_notes n ON n.context=a.context AND n.note=a.note WHERE a.context=?1 AND a.id>?2 " + std::string( query ? "AND a.query=?3 " : "" ) + "ORDER BY a.id LIMIT ?4" );
    Int( s, 1, context );
    Int( s, 2, after );
    if( query ) Int( s, 3, *query );
    Int( s, 4, limit + 1 );
    size_t bytes = 0;
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StoreGpuAnnotation a;
        a.id = sqlite3_column_int64( s, 0 );
        a.query = uint16_t( sqlite3_column_int( s, 1 ) );
        a.note = sqlite3_column_int64( s, 2 );
        a.name = Col( s, 4 );
        Require( sqlite3_column_bytes( s, 3 ) == 8, "Invalid GPU annotation value bits" );
        memcpy( &a.valueBits, sqlite3_column_blob( s, 3 ), 8 );
        auto n = sizeof( a ) + a.name.size();
        if( bytes + n > d.options.resultBytes )
        {
            Require( !page.values.empty(), "GPU annotation exceeds result budget" );
            page.more = true;
            break;
        }
        bytes += n;
        page.next = a.id;
        page.values.push_back( std::move( a ) );
    }
    sqlite3_reset( s );
    return page;
}
StoreCacheStats EventStore::CacheStats() const
{
    auto read = [&]( int operation ) {
        int current = 0, high = 0;
        Require( sqlite3_db_status( m_impl->db.handle, operation, &current, &high, 0 ) == SQLITE_OK, "Cannot read cache statistics" );
        return uint64_t( current );
    };
    StoreCacheStats stats{ read( SQLITE_DBSTATUS_CACHE_USED ), read( SQLITE_DBSTATUS_CACHE_HIT ), read( SQLITE_DBSTATUS_CACHE_MISS ) };
    const auto& blocks = m_impl->zoneCache;
    stats.blockBytes = blocks.zones.bytes + blocks.texts.bytes + blocks.packets.bytes;
    stats.blockHits = blocks.zones.hits + blocks.texts.hits + blocks.packets.hits;
    stats.blockMisses = blocks.zones.misses + blocks.texts.misses + blocks.packets.misses;
    return stats;
}
#include "TracyEventStoreTimeline.inl"
StorePage<StoreZone> EventStore::Zones( I begin, I end, std::optional<uint64_t> thread, uint64_t after, size_t limit ) const
{
    StorePage<StoreZone> page;
    if( begin >= end || limit == 0 ) return page;
    Require( limit <= 4096, "CPU page limit exceeded" );
    auto& d = m_impl->db;
    std::vector<ZoneRef> found;
    for( const auto block : ZoneBlocks( d, begin, end, thread, after ) )
    {
        if( found.size() > limit )
        {
            std::nth_element( found.begin(), found.begin() + limit, found.end(), []( const ZoneRef& a, const ZoneRef& b ) { return a.row.id < b.row.id; } );
            found.resize( limit + 1 );
            uint64_t first;
            auto q = d.Prep( "SELECT first FROM blocks WHERE id=?" );
            Int( q, 1, block );
            Require( d.Step( q ), "Missing event block" );
            first = sqlite3_column_int64( q, 0 );
            sqlite3_reset( q );
            const auto largest = std::max_element( found.begin(), found.end(), []( const ZoneRef& a, const ZoneRef& b ) { return a.row.id < b.row.id; } )->row.id;
            if( first > largest ) break;
        }
        const auto zones = LoadZoneBlock( d, m_impl->blocks, m_impl->zoneCache, block );
        for( const auto& r : zones->rows )
            if( r.id > after && ZoneOverlaps( r, begin, end ) ) found.push_back( { zones->thread, r } );
    }
    std::sort( found.begin(), found.end(), []( const ZoneRef& a, const ZoneRef& b ) { return a.row.id < b.row.id; } );
    if( found.size() > limit + 1 ) found.resize( limit + 1 );
    StageZones( d, found );
    auto s = d.Prep( ZoneColumns() + "ORDER BY z.id" );
    size_t bytes = 0;
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        auto z = ReadZone( d, m_impl->blocks, m_impl->zoneCache, s );
        auto n = sizeof( z ) + z.name.size() + z.function.size() + z.file.size() + z.text.size();
        if( bytes + n > d.options.resultBytes )
        {
            Require( !page.values.empty(), "Single zone exceeds result budget" );
            page.more = true;
            break;
        }
        bytes += n;
        page.next = z.id;
        page.values.push_back( std::move( z ) );
    }
    sqlite3_reset( s );
    return page;
}
std::optional<StoreZone> EventStore::Zone( uint64_t id ) const
{
    auto& d = m_impl->db;
    const auto zone = FindZone( d, m_impl->blocks, m_impl->zoneCache, id );
    if( !zone ) return {};
    StageZones( d, { *zone } );
    auto s = d.Prep( ZoneColumns() );
    Require( d.Step( s ), "Staged CPU zone is missing" );
    auto z = ReadZone( d, m_impl->blocks, m_impl->zoneCache, s );
    sqlite3_reset( s );
    return z;
}
std::vector<uint64_t> EventStore::Stack( uint64_t id ) const
{
    auto& d = m_impl->db;
    auto s = d.Prep( "SELECT data FROM stacks WHERE id=?" );
    Int( s, 1, id );
    if( !d.Step( s ) ) return {};
    auto size = sqlite3_column_bytes( s, 0 );
    Require( size >= 0 && size % 8 == 0 && size <= 65535 * 8 && uint64_t( size ) <= d.options.resultBytes, "Stack payload exceeds result or format budget" );
    std::vector<uint64_t> values( size / 8 );
    if( size ) memcpy( values.data(), sqlite3_column_blob( s, 0 ), size );
    sqlite3_reset( s );
    return values;
}
StorePage<StoreStackFrame> EventStore::StackFrames( uint64_t id, uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "Symbol page limit exceeded" );
    StorePage<StoreStackFrame> page;
    if( !limit ) return page;
    auto& d = m_impl->db;
    auto sizeQuery = d.Prep( "SELECT length(data) FROM stacks WHERE id=?" );
    Int( sizeQuery, 1, id );
    if( !d.Step( sizeQuery ) ) return page;
    const auto size = sqlite3_column_int64( sizeQuery, 0 );
    sqlite3_reset( sizeQuery );
    Require( size >= 0 && size % 8 == 0 && size <= 65535 * 8, "Invalid stack payload size" );
    uint64_t ordinal = 0;
    size_t bytes = 0;
    std::array<uint64_t, 256> addresses;
    for( I offset = 0; offset < size; offset += sizeof( addresses ) )
    {
        const auto count = size_t( std::min<I>( sizeof( addresses ), size - offset ) );
        auto chunk = d.Prep( "SELECT substr(data,?2,?3) FROM stacks WHERE id=?1" );
        Int( chunk, 1, id );
        Int( chunk, 2, offset + 1 );
        Int( chunk, 3, count );
        Require( d.Step( chunk ) && sqlite3_column_bytes( chunk, 0 ) == count, "Incomplete stack address page" );
        memcpy( addresses.data(), sqlite3_column_blob( chunk, 0 ), count );
        sqlite3_reset( chunk );
        for( size_t entry = 0; entry < count / 8; ++entry )
        {
            const auto address = addresses[entry];
            auto s = d.Prep( "SELECT ordinal,name,file,image,line,symbol,length,(SELECT MAX(ordinal) FROM stackframes WHERE key=?1) FROM stackframes WHERE key=?1 ORDER BY ordinal" );
            Int( s, 1, address );
            bool resolved = d.Step( s );
            do
            {
                StoreStackFrame frame;
                frame.index = ++ordinal;
                if( frame.index > after )
                {
                    frame.address = ( address >> 62 ) == 1 ? address & ( ( 1ull << 62 ) - 1 ) : address;
                    if( resolved )
                    {
                        frame.name = Col( s, 1 );
                        frame.file = Col( s, 2 );
                        frame.image = Col( s, 3 );
                        frame.line = sqlite3_column_int( s, 4 );
                        frame.symbol = sqlite3_column_int64( s, 5 );
                        frame.symbolLength = sqlite3_column_int64( s, 6 );
                        frame.inlineFrame = sqlite3_column_int64( s, 0 ) != sqlite3_column_int64( s, 7 );
                    }
                    const auto n = sizeof( frame ) + frame.name.size() + frame.file.size() + frame.image.size();
                    if( page.values.size() == limit || bytes + n > d.options.resultBytes )
                    {
                        sqlite3_reset( s );
                        Require( !page.values.empty(), "Single symbol exceeds result budget" );
                        page.more = true;
                        return page;
                    }
                    bytes += n;
                    page.next = frame.index;
                    page.values.push_back( std::move( frame ) );
                }
            } while( resolved && d.Step( s ) );
            sqlite3_reset( s );
        }
    }
    return page;
}
StorePage<StoreText> EventStore::Text( uint64_t id, uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "Text page limit exceeded" );
    StorePage<StoreText> page;
    if( !limit ) return page;
    auto& d = m_impl->db;
    const auto zone = FindZone( d, m_impl->blocks, m_impl->zoneCache, id );
    if( !zone ) return page;
    size_t bytes = 0;
    for( auto& t : ZoneTextRows( d, m_impl->blocks, m_impl->zoneCache, zone->thread, id, after, limit + 1 ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        if( bytes + t.text.size() > d.options.resultBytes )
        {
            Require( !page.values.empty(), "Text fragment exceeds budget" );
            page.more = true;
            break;
        }
        bytes += t.text.size();
        page.next = t.event;
        page.values.push_back( std::move( t ) );
    }
    return page;
}
StorePage<StoreZone> EventStore::Children( uint64_t id, uint64_t after, size_t limit ) const
{
    StorePage<StoreZone> page;
    Require( limit <= 4096, "Child page limit exceeded" );
    if( !limit ) return page;
    auto& d = m_impl->db;
    // A thread's zones are stored in preorder, so the direct children of a
    // zone follow it on its thread up to the first zone that is not deeper.
    // The walk starts at the block holding the cursor and also ends once
    // every recorded child is found.
    std::vector<ZoneRef> found;
    if( const auto parent = FindZone( d, m_impl->blocks, m_impl->zoneCache, id ); parent && parent->row.children )
    {
        const auto& p = parent->row;
        const uint64_t from = std::max( after, id );
        auto q = d.Prep( "SELECT id FROM blocks WHERE stream=1 AND key=?1 AND first>=COALESCE((SELECT first FROM blocks WHERE stream=1 AND key=?1 AND first<=?2 ORDER BY first DESC LIMIT 1),0) ORDER BY first" );
        Int( q, 1, parent->thread );
        Int( q, 2, from );
        try
        {
            bool done = false;
            while( !done && d.Step( q ) )
            {
                const auto zones = LoadZoneBlock( d, m_impl->blocks, m_impl->zoneCache, sqlite3_column_int64( q, 0 ) );
                for( const auto& r : zones->rows )
                {
                    if( r.id <= from ) continue;
                    if( r.depth <= p.depth )
                    {
                        done = true;
                        break;
                    }
                    if( r.parent != id ) continue;
                    found.push_back( { zones->thread, r } );
                    if( found.size() > limit || ( !after && found.size() == p.children ) )
                    {
                        done = true;
                        break;
                    }
                }
            }
        }
        catch( ... )
        {
            sqlite3_reset( q );
            throw;
        }
        sqlite3_reset( q );
    }
    std::sort( found.begin(), found.end(), []( const ZoneRef& a, const ZoneRef& b ) { return a.row.id < b.row.id; } );
    if( found.size() > limit + 1 ) found.resize( limit + 1 );
    StageZones( d, found );
    auto s = d.Prep( ZoneColumns() + "ORDER BY z.id" );
    size_t bytes = 0;
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        auto z = ReadZone( d, m_impl->blocks, m_impl->zoneCache, s );
        auto n = sizeof( z ) + z.name.size() + z.function.size() + z.file.size() + z.text.size();
        if( bytes + n > d.options.resultBytes )
        {
            Require( !page.values.empty(), "Child exceeds result budget" );
            page.more = true;
            break;
        }
        bytes += n;
        page.next = z.id;
        page.values.push_back( std::move( z ) );
    }
    sqlite3_reset( s );
    return page;
}
StorePage<StoreFrame> EventStore::Frames( I begin, I end, uint64_t after, size_t limit ) const
{
    StorePage<StoreFrame> page;
    if( begin >= end || limit == 0 ) return page;
    Require( limit <= 4096, "Frame page limit exceeded" );
    auto& d = m_impl->db;
    limit = std::min<size_t>( limit, d.options.resultBytes / sizeof( StoreFrame ) );
    page.values.reserve( limit );
    const std::string imageColumn = ", (SELECT i.id FROM base_frames b JOIN frame_images i ON i.frame_index=b.ordinal WHERE b.frame=f.id)";
    auto s = d.Prep( "SELECT f.id,f.setid,f.start,f.end,f.continuous" + imageColumn + " FROM frames f WHERE f.id IN(SELECT id FROM frame_range WHERE mint<=?1 AND maxt>=?2) AND f.start<?3 AND (f.end IS NULL OR f.end>?4 OR (f.end=f.start AND f.start>=?4)) AND f.id>?5 ORDER BY f.id LIMIT ?6" );
    Int( s, 1, uint64_t( Bucket( end - 1 ) ) );
    Int( s, 2, uint64_t( Bucket( begin ) ) );
    Int( s, 3, uint64_t( end ) );
    Int( s, 4, uint64_t( begin ) );
    Int( s, 5, after );
    Int( s, 6, limit + 1 );
    while( d.Step( s ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StoreFrame f;
        f.id = sqlite3_column_int64( s, 0 );
        f.set = sqlite3_column_int64( s, 1 );
        f.start = sqlite3_column_int64( s, 2 );
        if( sqlite3_column_type( s, 3 ) != SQLITE_NULL ) f.end = sqlite3_column_int64( s, 3 );
        f.continuous = sqlite3_column_int( s, 4 ) != 0;
        if( sqlite3_column_type( s, 5 ) != SQLITE_NULL ) f.image = sqlite3_column_int64( s, 5 );
        page.next = f.id;
        page.values.push_back( f );
    }
    sqlite3_reset( s );
    return page;
}
namespace
{
std::string ImageColumns( bool ranged = false )
{
    const std::string fields = "SELECT i.id,i.ordinal,i.frame_index,i.width,i.height,i.flipped,length(i.data),f.id,f.start,f.end ";
    // Drive window reads from overlapping frames, never the full image history.
    return fields + ( ranged ? "FROM frame_range r CROSS JOIN frames f ON f.id=r.id CROSS JOIN base_frames b ON b.frame=f.id CROSS JOIN frame_images i ON i.frame_index=b.ordinal " : "FROM frame_images i LEFT JOIN base_frames b ON b.ordinal=i.frame_index LEFT JOIN frames f ON f.id=b.frame " );
}
StoreFrameImage ReadImage( sqlite3_stmt* s )
{
    StoreFrameImage image;
    image.id = sqlite3_column_int64( s, 0 );
    image.index = sqlite3_column_int64( s, 1 );
    image.frameIndex = sqlite3_column_int64( s, 2 );
    image.width = uint16_t( sqlite3_column_int( s, 3 ) );
    image.height = uint16_t( sqlite3_column_int( s, 4 ) );
    image.flipped = sqlite3_column_int( s, 5 ) != 0;
    image.bc1Bytes = sqlite3_column_int64( s, 6 );
    if( sqlite3_column_type( s, 7 ) != SQLITE_NULL ) image.frame = sqlite3_column_int64( s, 7 );
    if( sqlite3_column_type( s, 8 ) != SQLITE_NULL ) image.start = sqlite3_column_int64( s, 8 );
    if( sqlite3_column_type( s, 9 ) != SQLITE_NULL ) image.end = sqlite3_column_int64( s, 9 );
    return image;
}
StorePage<StoreFrameImage> ImagePage( Db& d, sqlite3_stmt* s, size_t limit )
{
    StorePage<StoreFrameImage> result;
    result.values.reserve( limit );
    while( d.Step( s ) )
    {
        if( result.values.size() == limit )
        {
            result.more = true;
            break;
        }
        result.values.push_back( ReadImage( s ) );
        result.next = result.values.back().id;
    }
    sqlite3_reset( s );
    return result;
}
}
StorePage<StoreFrameImage> EventStore::FrameImages( uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "Frame Image page limit exceeded" );
    if( limit == 0 ) return {};
    auto& d = m_impl->db;
    limit = std::min<size_t>( limit, ( d.options.resultBytes - sizeof( StorePage<StoreFrameImage> ) ) / sizeof( StoreFrameImage ) );
    auto s = d.Prep( ImageColumns() + "WHERE i.id>?1 ORDER BY i.id LIMIT ?2" );
    Int( s, 1, after );
    Int( s, 2, limit + 1 );
    return ImagePage( d, s, limit );
}
StorePage<StoreFrameImage> EventStore::FrameImagesInRange( I begin, I end, uint64_t after, size_t limit ) const
{
    Require( limit <= 4096, "Frame Image page limit exceeded" );
    if( begin >= end || limit == 0 ) return {};
    auto& d = m_impl->db;
    limit = std::min<size_t>( limit, ( d.options.resultBytes - sizeof( StorePage<StoreFrameImage> ) ) / sizeof( StoreFrameImage ) );
    auto s = d.Prep( ImageColumns( true ) + "WHERE r.mint<=?1 AND r.maxt>=?2 AND f.start<?3 AND (f.end IS NULL OR f.end>?4 OR (f.end=f.start AND f.start>=?4)) AND i.id>?5 ORDER BY i.id LIMIT ?6" );
    Int( s, 1, uint64_t( Bucket( end - 1 ) ) );
    Int( s, 2, uint64_t( Bucket( begin ) ) );
    Int( s, 3, uint64_t( end ) );
    Int( s, 4, uint64_t( begin ) );
    Int( s, 5, after );
    Int( s, 6, limit + 1 );
    return ImagePage( d, s, limit );
}
std::optional<StoreFrameImage> EventStore::FrameImage( uint64_t id ) const
{
    auto& d = m_impl->db;
    auto s = d.Prep( ImageColumns() + "WHERE i.id=?" );
    Int( s, 1, id );
    if( !d.Step( s ) ) return {};
    auto result = ReadImage( s );
    sqlite3_reset( s );
    return result;
}
StoreBinaryChunk EventStore::FrameImageBytes( uint64_t id, uint64_t offset, size_t maxBytes ) const
{
    auto& d = m_impl->db;
    Require( maxBytes > 0 && maxBytes <= d.options.resultBytes - sizeof( StoreBinaryChunk ), "Frame Image chunk exceeds result budget" );
    Require( offset < INT64_MAX, "Frame Image byte offset overflow" );
    auto s = d.Prep( "SELECT length(data),substr(data,?2,?3) FROM frame_images WHERE id=?1" );
    Int( s, 1, id );
    Int( s, 2, offset + 1 );
    Int( s, 3, maxBytes );
    Require( d.Step( s ), "Unknown Frame Image" );
    StoreBinaryChunk result;
    result.offset = offset;
    result.totalBytes = sqlite3_column_int64( s, 0 );
    const auto size = sqlite3_column_bytes( s, 1 );
    const auto p = static_cast<const uint8_t*>( sqlite3_column_blob( s, 1 ) );
    if( size ) result.data.assign( p, p + size );
    result.more = offset < result.totalBytes && uint64_t( size ) < result.totalBytes - offset;
    sqlite3_reset( s );
    return result;
}
std::optional<StoreFrameSet> EventStore::FrameSet( uint64_t id ) const
{
    auto& d = m_impl->db;
    auto s = d.Prep( "SELECT continuous,(SELECT COUNT(*) FROM frames WHERE setid=?1),(SELECT COUNT(*) FROM frames WHERE setid=?1 AND end IS NOT NULL),(SELECT value FROM strings WHERE kind=?2 AND key=?1) FROM frame_state WHERE setid=?1" );
    Int( s, 1, id );
    Int( s, 2, uint8_t( QueueType::FrameName ) );
    if( !d.Step( s ) ) return {};
    StoreFrameSet result;
    result.id = id;
    result.continuous = sqlite3_column_int( s, 0 ) != 0;
    result.frames = sqlite3_column_int64( s, 1 );
    result.completeFrames = sqlite3_column_int64( s, 2 );
    result.name = id == 0 ? "Frames" : id >> 63 ? "Vsync " + std::to_string( uint32_t( id ) )
                                                : Col( s, 3 );
    sqlite3_reset( s );
    return result;
}
std::vector<StoreOverview> EventStore::Overview( I begin, I end, size_t limit ) const
{
    Require( limit > 0 && limit <= 4096, "Overview page budget exceeded" );
    if( begin >= end ) return {};
    int level = 0;
    uint64_t span = uint64_t( end ) - uint64_t( begin );
    while( level < 2 && span / ( level == 0 ? 1000000000ull : 60000000000ull ) > limit ) ++level;
    auto& d = m_impl->db;
    auto s = d.Prep( "SELECT start,end,count,maximum FROM frame_summary WHERE level=? AND end>? AND start<? ORDER BY bucket LIMIT ?" );
    Int( s, 1, level );
    Int( s, 2, uint64_t( begin ) );
    Int( s, 3, uint64_t( end ) );
    Int( s, 4, limit );
    std::vector<StoreOverview> values;
    while( d.Step( s ) ) values.push_back( { sqlite3_column_int64( s, 0 ), sqlite3_column_int64( s, 1 ), sqlite3_column_int64( s, 3 ), uint64_t( sqlite3_column_int64( s, 2 ) ) } );
    sqlite3_reset( s );
    return values;
}
#include "TracyEventStoreQueryAccess.inl"
}
