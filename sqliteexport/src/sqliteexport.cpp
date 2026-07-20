#ifdef _WIN32
#  include <windows.h>
#  include <bcrypt.h>
#  include <winsqlite/winsqlite3.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../server/TracyFileRead.hpp"
#include "../../server/TracyWorker.hpp"

namespace fs = std::filesystem;

namespace
{

[[noreturn]] void Fail( const std::string& message )
{
    throw std::runtime_error( message );
}

std::string Hex( const uint8_t* data, size_t size )
{
    static constexpr char lut[] = "0123456789abcdef";
    std::string out;
    out.resize( size * 2 );
    for( size_t i = 0; i < size; ++i )
    {
        out[i * 2] = lut[data[i] >> 4];
        out[i * 2 + 1] = lut[data[i] & 0x0F];
    }
    return out;
}

template<typename T>
std::array<uint8_t, sizeof( T )> RawBytes( const T& value )
{
    std::array<uint8_t, sizeof( T )> out {};
    memcpy( out.data(), &value, sizeof( T ) );
    return out;
}

std::array<uint8_t, 8> U64Bytes( uint64_t value )
{
    return RawBytes( value );
}

std::string U64Hex( uint64_t value )
{
    std::ostringstream ss;
    ss << std::hex << std::setfill( '0' ) << std::setw( 16 ) << value;
    return ss.str();
}

class Sha256
{
public:
    Sha256()
    {
        Check( BCryptOpenAlgorithmProvider( &m_algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0 ), "BCryptOpenAlgorithmProvider" );

        DWORD resultSize = 0;
        DWORD cbResult = 0;
        Check( BCryptGetProperty(
            m_algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>( &resultSize ),
            sizeof( resultSize ),
            &cbResult,
            0 ), "BCryptGetProperty(BCRYPT_OBJECT_LENGTH)" );

        m_object.resize( resultSize );
        Check( BCryptCreateHash(
            m_algorithm,
            &m_hash,
            m_object.data(),
            static_cast<ULONG>( m_object.size() ),
            nullptr,
            0,
            0 ), "BCryptCreateHash" );
    }

    Sha256( const Sha256& ) = delete;
    Sha256& operator=( const Sha256& ) = delete;

    ~Sha256()
    {
        if( m_hash ) BCryptDestroyHash( m_hash );
        if( m_algorithm ) BCryptCloseAlgorithmProvider( m_algorithm, 0 );
    }

    void Update( const void* data, size_t size )
    {
        const auto* ptr = static_cast<const uint8_t*>( data );
        while( size != 0 )
        {
            const ULONG chunk = static_cast<ULONG>( std::min<size_t>( size, std::numeric_limits<ULONG>::max() ) );
            Check( BCryptHashData( m_hash, const_cast<PUCHAR>( ptr ), chunk, 0 ), "BCryptHashData" );
            ptr += chunk;
            size -= chunk;
        }
    }

    std::array<uint8_t, 32> Finish()
    {
        if( m_finished ) Fail( "SHA-256 hash was finalized twice" );
        m_finished = true;
        std::array<uint8_t, 32> digest {};
        Check( BCryptFinishHash( m_hash, digest.data(), static_cast<ULONG>( digest.size() ), 0 ), "BCryptFinishHash" );
        return digest;
    }

    static std::array<uint8_t, 32> Digest( const void* data, size_t size )
    {
        Sha256 hash;
        hash.Update( data, size );
        return hash.Finish();
    }

private:
    static void Check( NTSTATUS status, const char* operation )
    {
        if( status < 0 )
        {
            std::ostringstream ss;
            ss << operation << " failed with NTSTATUS 0x" << std::hex << static_cast<uint32_t>( status );
            Fail( ss.str() );
        }
    }

    BCRYPT_ALG_HANDLE m_algorithm = nullptr;
    BCRYPT_HASH_HANDLE m_hash = nullptr;
    std::vector<uint8_t> m_object;
    bool m_finished = false;
};

struct FileDigest
{
    uint64_t size = 0;
    std::array<uint8_t, 32> sha256 {};
};

FileDigest DigestFile( const fs::path& path )
{
    constexpr size_t BufferSize = 16 * 1024 * 1024;
    std::ifstream input( path, std::ios::binary );
    if( !input ) Fail( "Cannot open file for SHA-256: " + path.string() );

    Sha256 hash;
    std::vector<uint8_t> buffer( BufferSize );
    uint64_t size = 0;
    for(;;)
    {
        input.read( reinterpret_cast<char*>( buffer.data() ), static_cast<std::streamsize>( buffer.size() ) );
        const auto count = static_cast<size_t>( input.gcount() );
        if( count == 0 ) break;
        hash.Update( buffer.data(), count );
        size += count;
    }
    if( !input.eof() ) Fail( "Read error while hashing file: " + path.string() );
    return FileDigest { size, hash.Finish() };
}

class Statement
{
public:
    Statement( sqlite3* db, const char* sql )
        : m_db( db )
    {
        const int rc = sqlite3_prepare_v2( db, sql, -1, &m_stmt, nullptr );
        if( rc != SQLITE_OK ) FailSqlite( "prepare", sql );
    }

    Statement( const Statement& ) = delete;
    Statement& operator=( const Statement& ) = delete;

    ~Statement()
    {
        if( m_stmt ) sqlite3_finalize( m_stmt );
    }

    void BindNull( int index )
    {
        Check( sqlite3_bind_null( m_stmt, index ), "bind null" );
    }

    void BindInt( int index, int value )
    {
        Check( sqlite3_bind_int( m_stmt, index, value ), "bind int" );
    }

    void BindInt64( int index, int64_t value )
    {
        Check( sqlite3_bind_int64( m_stmt, index, static_cast<sqlite3_int64>( value ) ), "bind int64" );
    }

    void BindDouble( int index, double value )
    {
        Check( sqlite3_bind_double( m_stmt, index, value ), "bind double" );
    }

    void BindText( int index, const std::string& value )
    {
        Check( sqlite3_bind_text64(
            m_stmt,
            index,
            value.data(),
            static_cast<sqlite3_uint64>( value.size() ),
            SQLITE_TRANSIENT,
            SQLITE_UTF8 ), "bind text" );
    }

    void BindText( int index, const char* value )
    {
        if( value )
        {
            Check( sqlite3_bind_text( m_stmt, index, value, -1, SQLITE_TRANSIENT ), "bind text" );
        }
        else
        {
            BindNull( index );
        }
    }

    void BindBlob( int index, const void* data, size_t size )
    {
        static constexpr uint8_t empty = 0;
        if( size == 0 ) data = &empty;
        Check( sqlite3_bind_blob64(
            m_stmt,
            index,
            data,
            static_cast<sqlite3_uint64>( size ),
            SQLITE_TRANSIENT ), "bind blob" );
    }

    template<size_t N>
    void BindBlob( int index, const std::array<uint8_t, N>& data )
    {
        BindBlob( index, data.data(), data.size() );
    }

    void StepDone()
    {
        const int rc = sqlite3_step( m_stmt );
        if( rc != SQLITE_DONE ) FailSqlite( "step", nullptr );
        Reset();
    }

    bool StepRow()
    {
        const int rc = sqlite3_step( m_stmt );
        if( rc == SQLITE_ROW ) return true;
        if( rc == SQLITE_DONE ) return false;
        FailSqlite( "step", nullptr );
    }

    const unsigned char* ColumnText( int index ) const
    {
        return sqlite3_column_text( m_stmt, index );
    }

    int64_t ColumnInt64( int index ) const
    {
        return sqlite3_column_int64( m_stmt, index );
    }

    const void* ColumnBlob( int index ) const
    {
        return sqlite3_column_blob( m_stmt, index );
    }

    int ColumnBytes( int index ) const
    {
        return sqlite3_column_bytes( m_stmt, index );
    }

    void Reset()
    {
        Check( sqlite3_reset( m_stmt ), "reset" );
        Check( sqlite3_clear_bindings( m_stmt ), "clear bindings" );
    }

private:
    void Check( int rc, const char* action )
    {
        if( rc != SQLITE_OK )
        {
            std::ostringstream ss;
            ss << action << " failed: " << sqlite3_errmsg( m_db );
            Fail( ss.str() );
        }
    }

    [[noreturn]] void FailSqlite( const char* action, const char* sql )
    {
        std::ostringstream ss;
        ss << "SQLite " << action << " failed: " << sqlite3_errmsg( m_db );
        if( sql ) ss << "\nSQL: " << sql;
        Fail( ss.str() );
    }

    sqlite3* m_db = nullptr;
    sqlite3_stmt* m_stmt = nullptr;
};

class Database
{
public:
    explicit Database( const fs::path& path )
    {
        const auto utf8 = path.u8string();
        const int rc = sqlite3_open_v2(
            reinterpret_cast<const char*>( utf8.c_str() ),
            &m_db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_EXRESCODE,
            nullptr );
        if( rc != SQLITE_OK )
        {
            const std::string error = m_db ? sqlite3_errmsg( m_db ) : "no database handle";
            if( m_db ) sqlite3_close_v2( m_db );
            m_db = nullptr;
            Fail( "Cannot create SQLite database: " + error );
        }
        sqlite3_extended_result_codes( m_db, 1 );
    }

    Database( const Database& ) = delete;
    Database& operator=( const Database& ) = delete;

    ~Database()
    {
        if( m_db ) sqlite3_close_v2( m_db );
    }

    sqlite3* Handle() const { return m_db; }

    void Exec( const char* sql )
    {
        char* error = nullptr;
        const int rc = sqlite3_exec( m_db, sql, nullptr, nullptr, &error );
        if( rc != SQLITE_OK )
        {
            const std::string message = error ? error : sqlite3_errmsg( m_db );
            if( error ) sqlite3_free( error );
            Fail( "SQLite exec failed: " + message + "\nSQL: " + sql );
        }
    }

    int64_t ScalarInt64( const char* sql )
    {
        Statement stmt( m_db, sql );
        if( !stmt.StepRow() ) Fail( std::string( "Query returned no row: " ) + sql );
        return stmt.ColumnInt64( 0 );
    }

    std::string ScalarText( const char* sql )
    {
        Statement stmt( m_db, sql );
        if( !stmt.StepRow() ) Fail( std::string( "Query returned no row: " ) + sql );
        const auto* text = stmt.ColumnText( 0 );
        return text ? reinterpret_cast<const char*>( text ) : std::string();
    }

    void Close()
    {
        if( !m_db ) return;
        const int rc = sqlite3_close_v2( m_db );
        if( rc != SQLITE_OK ) Fail( "sqlite3_close_v2 failed" );
        m_db = nullptr;
    }

private:
    sqlite3* m_db = nullptr;
};

std::string Utf8OrEmpty( const char* value )
{
    return value ? std::string( value ) : std::string();
}

void BindU64( Statement& stmt, int blobIndex, int hexIndex, uint64_t value )
{
    stmt.BindBlob( blobIndex, U64Bytes( value ) );
    stmt.BindText( hexIndex, U64Hex( value ) );
}

void BindDoubleExact( Statement& stmt, int realIndex, int bitsIndex, double value )
{
    stmt.BindDouble( realIndex, value );
    stmt.BindBlob( bitsIndex, RawBytes( value ) );
}

void BindFloatExact( Statement& stmt, int realIndex, int bitsIndex, float value )
{
    stmt.BindDouble( realIndex, static_cast<double>( value ) );
    stmt.BindBlob( bitsIndex, RawBytes( value ) );
}

} // namespace

namespace tracy
{

class SqliteExporter
{
public:
    SqliteExporter( Worker& worker, fs::path input, fs::path output, FileDigest frozenInput )
        : m_worker( worker )
        , m_input( std::move( input ) )
        , m_output( std::move( output ) )
        , m_frozenInput( frozenInput )
    {
    }

    void Run();

private:
    void CreateSchema();
    void InsertMeta( const std::string& key, const std::string& value );
    void Audit( const std::string& section, const std::string& table, uint64_t sourceCount, uint64_t exportedCount, const std::string& details = {} );
    void ExportRawCapture();
    void ExportMetadata();
    void ExportFrames();
    void ExportStrings();
    void ExportSourceLocations();
    void ExportLocks();
    void ExportMessagesAndThreads();
    void ExportGpu();
    void ExportPlots();
    void ExportMemory();
    void ExportCallstacks();
    void ExportFrameImages();
    void ExportContextSwitches();
    void ExportSymbolsAndSamples();
    void ExportDerivedData();
    void ValidateAndPublish();
    std::string StringRefText( const StringRef& ref ) const;
    std::string StringIdxText( const StringIdx& idx ) const;
    void BindOptionalStringIdx( Statement& stmt, int indexColumn, int textColumn, const StringIdx& idx ) const;
    uint64_t OriginalStringPointer( const char* value ) const;
    uint32_t StringIndex( const char* value ) const;
    uint64_t DecompressLocalThread( uint16_t compressed ) const;
    uint64_t DecompressExternalThread( uint16_t compressed ) const;

    Worker& m_worker;
    fs::path m_input;
    fs::path m_output;
    fs::path m_partial;
    FileDigest m_frozenInput;
    std::unique_ptr<Database> m_db;
    std::unordered_map<const char*, uint64_t> m_originalStringPointer;
    std::unordered_map<const char*, uint32_t> m_stringIndex;
    std::unordered_map<const PlotData*, int64_t> m_plotId;
    int64_t m_nextCpuZoneId = 1;
    int64_t m_nextGpuZoneId = 1;
};

void SqliteExporter::CreateSchema()
{
    m_db->Exec( R"SQL(
PRAGMA page_size = 65536;
PRAGMA journal_mode = DELETE;
PRAGMA synchronous = FULL;
PRAGMA foreign_keys = ON;
PRAGMA temp_store = MEMORY;

CREATE TABLE meta (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
) WITHOUT ROWID;

CREATE TABLE export_audit (
    section TEXT PRIMARY KEY,
    table_name TEXT NOT NULL,
    source_count INTEGER NOT NULL,
    exported_count INTEGER NOT NULL,
    database_count INTEGER NOT NULL,
    status TEXT NOT NULL,
    details TEXT NOT NULL
) WITHOUT ROWID;

CREATE TABLE capture_chunks (
    chunk_index INTEGER PRIMARY KEY,
    byte_offset INTEGER NOT NULL,
    byte_count INTEGER NOT NULL,
    sha256 TEXT NOT NULL,
    data BLOB NOT NULL
);

CREATE TABLE capture_metadata (
    singleton INTEGER PRIMARY KEY CHECK(singleton = 1),
    trace_version INTEGER NOT NULL,
    resolution INTEGER NOT NULL,
    timer_mul REAL NOT NULL,
    timer_mul_bits BLOB NOT NULL,
    first_time_ns INTEGER NOT NULL,
    last_time_ns INTEGER NOT NULL,
    frame_offset_u64 BLOB NOT NULL,
    frame_offset_hex TEXT NOT NULL,
    pid_u64 BLOB NOT NULL,
    pid_hex TEXT NOT NULL,
    sampling_period_ns INTEGER NOT NULL,
    cpu_arch INTEGER NOT NULL,
    cpu_id INTEGER NOT NULL,
    cpu_manufacturer BLOB NOT NULL,
    on_demand INTEGER NOT NULL,
    inconsistent_samples INTEGER NOT NULL,
    capture_name BLOB NOT NULL,
    capture_name_text TEXT NOT NULL,
    capture_program BLOB NOT NULL,
    capture_program_text TEXT NOT NULL,
    capture_time_u64 BLOB NOT NULL,
    capture_time_hex TEXT NOT NULL,
    executable_time_u64 BLOB NOT NULL,
    executable_time_hex TEXT NOT NULL,
    host_info BLOB NOT NULL,
    host_info_text TEXT NOT NULL,
    load_time_ns INTEGER NOT NULL
);

CREATE TABLE cpu_topology (
    package_id INTEGER NOT NULL,
    die_id INTEGER NOT NULL,
    core_id INTEGER NOT NULL,
    sibling_ordinal INTEGER NOT NULL,
    hardware_thread INTEGER NOT NULL,
    PRIMARY KEY(package_id, die_id, core_id, sibling_ordinal)
) WITHOUT ROWID;

CREATE TABLE crash_event (
    singleton INTEGER PRIMARY KEY CHECK(singleton = 1),
    thread_u64 BLOB NOT NULL,
    thread_hex TEXT NOT NULL,
    time_ns INTEGER NOT NULL,
    message_u64 BLOB NOT NULL,
    message_hex TEXT NOT NULL,
    callstack_id INTEGER NOT NULL
);

CREATE TABLE frame_sets (
    frame_set_id INTEGER PRIMARY KEY,
    name_u64 BLOB NOT NULL,
    name_hex TEXT NOT NULL,
    name_text TEXT NOT NULL,
    continuous INTEGER NOT NULL,
    is_base INTEGER NOT NULL,
    min_ns INTEGER NOT NULL,
    max_ns INTEGER NOT NULL,
    total_ns INTEGER NOT NULL,
    sum_sq REAL NOT NULL,
    sum_sq_bits BLOB NOT NULL
);

CREATE TABLE frames (
    frame_set_id INTEGER NOT NULL REFERENCES frame_sets(frame_set_id),
    frame_index INTEGER NOT NULL,
    start_ns INTEGER NOT NULL,
    stored_end_ns INTEGER NOT NULL,
    effective_end_ns INTEGER NOT NULL,
    frame_image_index INTEGER NOT NULL,
    PRIMARY KEY(frame_set_id, frame_index)
) WITHOUT ROWID;

CREATE TABLE string_data (
    string_index INTEGER PRIMARY KEY,
    original_pointer_u64 BLOB NOT NULL,
    original_pointer_hex TEXT NOT NULL,
    byte_count INTEGER NOT NULL,
    value_blob BLOB NOT NULL,
    value_text TEXT NOT NULL
);

CREATE TABLE string_map (
    string_key_u64 BLOB NOT NULL,
    string_key_hex TEXT PRIMARY KEY,
    string_index INTEGER NOT NULL REFERENCES string_data(string_index)
) WITHOUT ROWID;

CREATE TABLE thread_names (
    thread_u64 BLOB NOT NULL,
    thread_hex TEXT PRIMARY KEY,
    string_index INTEGER NOT NULL REFERENCES string_data(string_index),
    name_text TEXT NOT NULL
) WITHOUT ROWID;

CREATE TABLE external_names (
    external_id_u64 BLOB NOT NULL,
    external_id_hex TEXT PRIMARY KEY,
    process_string_index INTEGER NOT NULL REFERENCES string_data(string_index),
    thread_string_index INTEGER NOT NULL REFERENCES string_data(string_index),
    process_name TEXT NOT NULL,
    thread_name TEXT NOT NULL
) WITHOUT ROWID;

CREATE TABLE thread_compression (
    domain TEXT NOT NULL,
    compressed_id INTEGER NOT NULL,
    thread_u64 BLOB NOT NULL,
    thread_hex TEXT NOT NULL,
    PRIMARY KEY(domain, compressed_id)
) WITHOUT ROWID;

CREATE TABLE source_locations_static (
    static_key_u64 BLOB NOT NULL,
    static_key_hex TEXT PRIMARY KEY,
    raw_payload BLOB NOT NULL,
    name_ref_u64 BLOB NOT NULL,
    name_ref_hex TEXT NOT NULL,
    name_active INTEGER NOT NULL,
    name_is_index INTEGER NOT NULL,
    function_ref_u64 BLOB NOT NULL,
    function_ref_hex TEXT NOT NULL,
    function_active INTEGER NOT NULL,
    function_is_index INTEGER NOT NULL,
    file_ref_u64 BLOB NOT NULL,
    file_ref_hex TEXT NOT NULL,
    file_active INTEGER NOT NULL,
    file_is_index INTEGER NOT NULL,
    name_text TEXT NOT NULL,
    function_text TEXT NOT NULL,
    file_text TEXT NOT NULL,
    line INTEGER NOT NULL,
    color INTEGER NOT NULL
) WITHOUT ROWID;

CREATE TABLE source_location_expand (
    source_location_id INTEGER PRIMARY KEY,
    static_key_u64 BLOB NOT NULL,
    static_key_hex TEXT NOT NULL,
    resolved INTEGER NOT NULL
);

CREATE TABLE source_locations_dynamic (
    payload_index INTEGER PRIMARY KEY,
    source_location_id INTEGER UNIQUE NOT NULL,
    raw_payload BLOB NOT NULL,
    name_ref_u64 BLOB NOT NULL,
    name_ref_hex TEXT NOT NULL,
    name_active INTEGER NOT NULL,
    name_is_index INTEGER NOT NULL,
    function_ref_u64 BLOB NOT NULL,
    function_ref_hex TEXT NOT NULL,
    function_active INTEGER NOT NULL,
    function_is_index INTEGER NOT NULL,
    file_ref_u64 BLOB NOT NULL,
    file_ref_hex TEXT NOT NULL,
    file_active INTEGER NOT NULL,
    file_is_index INTEGER NOT NULL,
    name_text TEXT NOT NULL,
    function_text TEXT NOT NULL,
    file_text TEXT NOT NULL,
    line INTEGER NOT NULL,
    color INTEGER NOT NULL
);

CREATE TABLE source_location_zone_counts (
    domain TEXT NOT NULL,
    source_location_id INTEGER NOT NULL,
    event_count INTEGER NOT NULL,
    PRIMARY KEY(domain, source_location_id)
) WITHOUT ROWID;

CREATE TABLE zone_extras (
    extra_id INTEGER PRIMARY KEY,
    raw_payload BLOB NOT NULL,
    callstack_id INTEGER NOT NULL,
    text_index INTEGER,
    text_text TEXT,
    name_index INTEGER,
    name_text TEXT,
    color INTEGER NOT NULL
);

CREATE TABLE locks (
    lock_id INTEGER PRIMARY KEY,
    custom_name_index INTEGER,
    custom_name_text TEXT,
    source_location_id INTEGER NOT NULL,
    lock_type INTEGER NOT NULL,
    valid INTEGER NOT NULL,
    time_announce_ns INTEGER NOT NULL,
    time_terminate_ns INTEGER NOT NULL,
    is_contended INTEGER NOT NULL,
    locking_thread_u64 BLOB NOT NULL,
    locking_thread_hex TEXT NOT NULL
);

CREATE TABLE lock_threads (
    lock_id INTEGER NOT NULL REFERENCES locks(lock_id),
    thread_ordinal INTEGER NOT NULL,
    thread_u64 BLOB NOT NULL,
    thread_hex TEXT NOT NULL,
    PRIMARY KEY(lock_id, thread_ordinal)
) WITHOUT ROWID;

CREATE TABLE lock_events (
    lock_id INTEGER NOT NULL REFERENCES locks(lock_id),
    event_index INTEGER NOT NULL,
    time_ns INTEGER NOT NULL,
    source_location_id INTEGER NOT NULL,
    lock_thread_index INTEGER NOT NULL,
    event_type INTEGER NOT NULL,
    locking_thread_index INTEGER NOT NULL,
    lock_count INTEGER NOT NULL,
    wait_list_u64 BLOB NOT NULL,
    wait_list_hex TEXT NOT NULL,
    wait_shared_u64 BLOB,
    wait_shared_hex TEXT,
    shared_list_u64 BLOB,
    shared_list_hex TEXT,
    PRIMARY KEY(lock_id, event_index)
) WITHOUT ROWID;

CREATE TABLE threads (
    thread_index INTEGER PRIMARY KEY,
    thread_u64 BLOB NOT NULL,
    thread_hex TEXT UNIQUE NOT NULL,
    thread_name TEXT NOT NULL,
    zone_count INTEGER NOT NULL,
    kernel_sample_count INTEGER NOT NULL,
    is_fiber INTEGER NOT NULL,
    group_hint INTEGER NOT NULL
);

CREATE TABLE messages (
    message_id INTEGER PRIMARY KEY,
    original_pointer_u64 BLOB NOT NULL,
    original_pointer_hex TEXT NOT NULL,
    time_ns INTEGER NOT NULL,
    string_ref_u64 BLOB NOT NULL,
    string_ref_hex TEXT NOT NULL,
    string_ref_active INTEGER NOT NULL,
    string_ref_is_index INTEGER NOT NULL,
    message_text TEXT NOT NULL,
    compressed_thread_id INTEGER NOT NULL,
    thread_u64 BLOB NOT NULL,
    thread_hex TEXT NOT NULL,
    color INTEGER NOT NULL,
    callstack_id INTEGER NOT NULL
);

CREATE TABLE thread_messages (
    thread_index INTEGER NOT NULL REFERENCES threads(thread_index),
    message_ordinal INTEGER NOT NULL,
    message_id INTEGER NOT NULL REFERENCES messages(message_id),
    original_pointer_u64 BLOB NOT NULL,
    original_pointer_hex TEXT NOT NULL,
    PRIMARY KEY(thread_index, message_ordinal)
) WITHOUT ROWID;

CREATE TABLE cpu_zones (
    zone_id INTEGER PRIMARY KEY,
    thread_index INTEGER NOT NULL REFERENCES threads(thread_index),
    parent_zone_id INTEGER REFERENCES cpu_zones(zone_id),
    depth INTEGER NOT NULL,
    sibling_ordinal INTEGER NOT NULL,
    start_ns INTEGER NOT NULL,
    end_ns INTEGER NOT NULL,
    inferred_end_ns INTEGER NOT NULL,
    end_valid INTEGER NOT NULL,
    source_location_id INTEGER NOT NULL,
    extra_id INTEGER NOT NULL,
    has_children INTEGER NOT NULL,
    child_vector_index INTEGER NOT NULL
);

CREATE TABLE thread_samples (
    thread_index INTEGER NOT NULL REFERENCES threads(thread_index),
    sample_kind TEXT NOT NULL,
    sample_index INTEGER NOT NULL,
    time_ns INTEGER NOT NULL,
    callstack_id INTEGER NOT NULL,
    PRIMARY KEY(thread_index, sample_kind, sample_index)
) WITHOUT ROWID;

CREATE TABLE gpu_contexts (
    gpu_context_id INTEGER PRIMARY KEY,
    owning_thread_u64 BLOB NOT NULL,
    owning_thread_hex TEXT NOT NULL,
    event_count INTEGER NOT NULL,
    period REAL NOT NULL,
    period_bits BLOB NOT NULL,
    context_type INTEGER NOT NULL,
    has_period INTEGER NOT NULL,
    has_calibration INTEGER NOT NULL,
    time_diff_ns INTEGER NOT NULL,
    calibrated_gpu_time_ns INTEGER NOT NULL,
    calibrated_cpu_time_ns INTEGER NOT NULL,
    calibration_mod REAL NOT NULL,
    calibration_mod_bits BLOB NOT NULL,
    last_gpu_time_ns INTEGER NOT NULL,
    overflow_u64 BLOB NOT NULL,
    overflow_hex TEXT NOT NULL,
    overflow_multiplier INTEGER NOT NULL,
    name_index INTEGER,
    name_text TEXT
);

CREATE TABLE gpu_context_note_names (
    gpu_context_id INTEGER NOT NULL REFERENCES gpu_contexts(gpu_context_id),
    note_id INTEGER NOT NULL,
    name_index INTEGER NOT NULL,
    name_text TEXT NOT NULL,
    PRIMARY KEY(gpu_context_id, note_id)
) WITHOUT ROWID;

CREATE TABLE gpu_context_threads (
    gpu_context_id INTEGER NOT NULL REFERENCES gpu_contexts(gpu_context_id),
    context_thread_index INTEGER NOT NULL,
    thread_u64 BLOB NOT NULL,
    thread_hex TEXT NOT NULL,
    PRIMARY KEY(gpu_context_id, context_thread_index)
) WITHOUT ROWID;

CREATE TABLE gpu_zones (
    gpu_zone_id INTEGER PRIMARY KEY,
    gpu_context_id INTEGER NOT NULL REFERENCES gpu_contexts(gpu_context_id),
    context_thread_index INTEGER NOT NULL,
    parent_gpu_zone_id INTEGER REFERENCES gpu_zones(gpu_zone_id),
    depth INTEGER NOT NULL,
    sibling_ordinal INTEGER NOT NULL,
    cpu_start_ns INTEGER NOT NULL,
    cpu_end_ns INTEGER NOT NULL,
    gpu_start_ns INTEGER NOT NULL,
    gpu_end_ns INTEGER NOT NULL,
    gpu_end_valid INTEGER NOT NULL,
    source_location_id INTEGER NOT NULL,
    callstack_id INTEGER NOT NULL,
    compressed_thread_id INTEGER NOT NULL,
    event_thread_u64 BLOB NOT NULL,
    event_thread_hex TEXT NOT NULL,
    query_id INTEGER NOT NULL,
    has_children INTEGER NOT NULL,
    child_vector_index INTEGER NOT NULL
);

CREATE TABLE gpu_notes (
    gpu_context_id INTEGER NOT NULL REFERENCES gpu_contexts(gpu_context_id),
    query_id INTEGER NOT NULL,
    note_id INTEGER NOT NULL,
    value REAL NOT NULL,
    value_bits BLOB NOT NULL,
    PRIMARY KEY(gpu_context_id, query_id, note_id)
) WITHOUT ROWID;

CREATE TABLE plots (
    plot_id INTEGER PRIMARY KEY,
    name_u64 BLOB NOT NULL,
    name_hex TEXT NOT NULL,
    name_text TEXT NOT NULL,
    plot_type INTEGER NOT NULL,
    value_format INTEGER NOT NULL,
    show_steps INTEGER NOT NULL,
    fill INTEGER NOT NULL,
    color INTEGER NOT NULL,
    min_value REAL NOT NULL,
    min_bits BLOB NOT NULL,
    max_value REAL NOT NULL,
    max_bits BLOB NOT NULL,
    sum_value REAL NOT NULL,
    sum_bits BLOB NOT NULL,
    running_min REAL NOT NULL,
    running_min_bits BLOB NOT NULL,
    running_max REAL NOT NULL,
    running_max_bits BLOB NOT NULL,
    running_num REAL NOT NULL,
    running_num_bits BLOB NOT NULL,
    origin TEXT NOT NULL
);

CREATE TABLE plot_samples (
    plot_id INTEGER NOT NULL REFERENCES plots(plot_id),
    sample_index INTEGER NOT NULL,
    time_ns INTEGER NOT NULL,
    value REAL NOT NULL,
    value_bits BLOB NOT NULL,
    PRIMARY KEY(plot_id, sample_index)
) WITHOUT ROWID;

CREATE TABLE memory_pools (
    memory_pool_id INTEGER PRIMARY KEY,
    map_key_u64 BLOB NOT NULL,
    map_key_hex TEXT NOT NULL,
    stored_name_u64 BLOB NOT NULL,
    stored_name_hex TEXT NOT NULL,
    name_text TEXT NOT NULL,
    high_u64 BLOB NOT NULL,
    high_hex TEXT NOT NULL,
    low_u64 BLOB NOT NULL,
    low_hex TEXT NOT NULL,
    usage_u64 BLOB NOT NULL,
    usage_hex TEXT NOT NULL,
    reconstruct INTEGER NOT NULL,
    plot_id INTEGER REFERENCES plots(plot_id)
);

CREATE TABLE memory_events (
    memory_pool_id INTEGER NOT NULL REFERENCES memory_pools(memory_pool_id),
    event_index INTEGER NOT NULL,
    pointer_u64 BLOB NOT NULL,
    pointer_hex TEXT NOT NULL,
    size_u64 BLOB NOT NULL,
    size_hex TEXT NOT NULL,
    allocation_callstack_id INTEGER NOT NULL,
    free_callstack_id INTEGER NOT NULL,
    allocation_time_ns INTEGER NOT NULL,
    free_time_ns INTEGER NOT NULL,
    allocation_compressed_thread_id INTEGER NOT NULL,
    allocation_thread_u64 BLOB NOT NULL,
    allocation_thread_hex TEXT NOT NULL,
    free_compressed_thread_id INTEGER NOT NULL,
    free_thread_u64 BLOB NOT NULL,
    free_thread_hex TEXT NOT NULL,
    is_active INTEGER NOT NULL,
    PRIMARY KEY(memory_pool_id, event_index)
) WITHOUT ROWID;

CREATE TABLE memory_active (
    memory_pool_id INTEGER NOT NULL REFERENCES memory_pools(memory_pool_id),
    pointer_u64 BLOB NOT NULL,
    pointer_hex TEXT NOT NULL,
    event_index INTEGER NOT NULL,
    PRIMARY KEY(memory_pool_id, pointer_hex)
) WITHOUT ROWID;

CREATE TABLE memory_frees (
    memory_pool_id INTEGER NOT NULL REFERENCES memory_pools(memory_pool_id),
    free_ordinal INTEGER NOT NULL,
    event_index INTEGER NOT NULL,
    PRIMARY KEY(memory_pool_id, free_ordinal)
) WITHOUT ROWID;

CREATE TABLE callstacks (
    callstack_id INTEGER PRIMARY KEY,
    frame_count INTEGER NOT NULL
);

CREATE TABLE callstack_items (
    callstack_id INTEGER NOT NULL REFERENCES callstacks(callstack_id),
    frame_ordinal INTEGER NOT NULL,
    frame_id_u64 BLOB NOT NULL,
    frame_id_hex TEXT NOT NULL,
    PRIMARY KEY(callstack_id, frame_ordinal)
) WITHOUT ROWID;

CREATE TABLE callstack_frame_definitions (
    frame_id_u64 BLOB NOT NULL,
    frame_id_hex TEXT PRIMARY KEY,
    inline_frame_count INTEGER NOT NULL,
    image_name_index INTEGER,
    image_name_text TEXT
) WITHOUT ROWID;

CREATE TABLE callstack_frame_entries (
    frame_id_hex TEXT NOT NULL REFERENCES callstack_frame_definitions(frame_id_hex),
    inline_ordinal INTEGER NOT NULL,
    name_index INTEGER,
    name_text TEXT,
    file_index INTEGER,
    file_text TEXT,
    line INTEGER NOT NULL,
    symbol_address_u64 BLOB NOT NULL,
    symbol_address_hex TEXT NOT NULL,
    PRIMARY KEY(frame_id_hex, inline_ordinal)
) WITHOUT ROWID;

CREATE TABLE app_info (
    app_info_index INTEGER PRIMARY KEY,
    string_ref_u64 BLOB NOT NULL,
    string_ref_hex TEXT NOT NULL,
    string_ref_active INTEGER NOT NULL,
    string_ref_is_index INTEGER NOT NULL,
    value_text TEXT NOT NULL
);

CREATE TABLE frame_image_dictionary (
    singleton INTEGER PRIMARY KEY CHECK(singleton = 1),
    byte_count INTEGER NOT NULL,
    sha256 TEXT NOT NULL,
    data BLOB NOT NULL
);

CREATE TABLE frame_images (
    frame_image_index INTEGER PRIMARY KEY,
    width INTEGER NOT NULL,
    height INTEGER NOT NULL,
    flip INTEGER NOT NULL,
    frame_reference INTEGER NOT NULL,
    packed_byte_count INTEGER NOT NULL,
    packed_sha256 TEXT NOT NULL,
    packed_data BLOB NOT NULL,
    unpacked_byte_count INTEGER NOT NULL,
    unpacked_sha256 TEXT NOT NULL,
    unpacked_data BLOB NOT NULL
);

CREATE TABLE context_switch_threads (
    thread_u64 BLOB NOT NULL,
    thread_hex TEXT PRIMARY KEY,
    event_count INTEGER NOT NULL,
    running_time_ns INTEGER NOT NULL,
    pending_wakeup_ns INTEGER NOT NULL,
    pending_wakeup_cpu INTEGER NOT NULL
) WITHOUT ROWID;

CREATE TABLE context_switches (
    thread_u64 BLOB NOT NULL,
    thread_hex TEXT NOT NULL,
    switch_index INTEGER NOT NULL,
    wakeup_ns INTEGER NOT NULL,
    start_ns INTEGER NOT NULL,
    end_ns INTEGER NOT NULL,
    end_valid INTEGER NOT NULL,
    cpu INTEGER NOT NULL,
    wakeup_cpu INTEGER NOT NULL,
    reason INTEGER NOT NULL,
    state INTEGER NOT NULL,
    related_compressed_thread_id INTEGER NOT NULL,
    related_thread_u64 BLOB NOT NULL,
    related_thread_hex TEXT NOT NULL,
    PRIMARY KEY(thread_hex, switch_index)
) WITHOUT ROWID;

CREATE TABLE cpu_context_switches (
    cpu INTEGER NOT NULL,
    switch_index INTEGER NOT NULL,
    start_ns INTEGER NOT NULL,
    end_ns INTEGER NOT NULL,
    end_valid INTEGER NOT NULL,
    compressed_thread_id INTEGER NOT NULL,
    thread_u64 BLOB NOT NULL,
    thread_hex TEXT NOT NULL,
    PRIMARY KEY(cpu, switch_index)
) WITHOUT ROWID;

CREATE TABLE tid_pid (
    tid_u64 BLOB NOT NULL,
    tid_hex TEXT PRIMARY KEY,
    pid_u64 BLOB NOT NULL,
    pid_hex TEXT NOT NULL
) WITHOUT ROWID;

CREATE TABLE cpu_thread_stats (
    tid_u64 BLOB NOT NULL,
    tid_hex TEXT PRIMARY KEY,
    running_time_ns INTEGER NOT NULL,
    running_regions INTEGER NOT NULL,
    migrations INTEGER NOT NULL
) WITHOUT ROWID;

CREATE TABLE symbols (
    symbol_address_u64 BLOB NOT NULL,
    symbol_address_hex TEXT PRIMARY KEY,
    name_index INTEGER,
    name_text TEXT,
    file_index INTEGER,
    file_text TEXT,
    line INTEGER NOT NULL,
    image_name_index INTEGER,
    image_name_text TEXT,
    call_file_index INTEGER,
    call_file_text TEXT,
    call_line INTEGER NOT NULL,
    is_inline INTEGER NOT NULL,
    size INTEGER NOT NULL,
    raw_payload BLOB NOT NULL
) WITHOUT ROWID;

CREATE TABLE symbol_locations (
    location_kind TEXT NOT NULL,
    location_index INTEGER NOT NULL,
    symbol_address_u64 BLOB NOT NULL,
    symbol_address_hex TEXT NOT NULL,
    size INTEGER,
    PRIMARY KEY(location_kind, location_index)
) WITHOUT ROWID;

CREATE TABLE symbol_code (
    symbol_address_u64 BLOB NOT NULL,
    symbol_address_hex TEXT PRIMARY KEY,
    byte_count INTEGER NOT NULL,
    sha256 TEXT NOT NULL,
    data BLOB NOT NULL
) WITHOUT ROWID;

CREATE TABLE code_symbol_map (
    code_address_u64 BLOB NOT NULL,
    code_address_hex TEXT PRIMARY KEY,
    symbol_address_u64 BLOB NOT NULL,
    symbol_address_hex TEXT NOT NULL
) WITHOUT ROWID;

CREATE TABLE hardware_samples (
    address_u64 BLOB NOT NULL,
    address_hex TEXT NOT NULL,
    sample_kind TEXT NOT NULL,
    sample_index INTEGER NOT NULL,
    time_ns INTEGER NOT NULL,
    PRIMARY KEY(address_hex, sample_kind, sample_index)
) WITHOUT ROWID;

CREATE TABLE source_cache (
    source_path_blob BLOB NOT NULL,
    source_path_text TEXT PRIMARY KEY,
    byte_count INTEGER NOT NULL,
    sha256 TEXT NOT NULL,
    data BLOB NOT NULL
) WITHOUT ROWID;

CREATE TABLE derived_cpu_usage (
    usage_index INTEGER PRIMARY KEY,
    time_ns INTEGER NOT NULL,
    other_cpu_count INTEGER NOT NULL,
    own_cpu_count INTEGER NOT NULL
);

CREATE TABLE derived_zone_statistics (
    domain TEXT NOT NULL,
    source_location_id INTEGER NOT NULL,
    event_count INTEGER NOT NULL,
    min_ns INTEGER NOT NULL,
    max_ns INTEGER NOT NULL,
    total_ns INTEGER NOT NULL,
    sum_sq REAL NOT NULL,
    sum_sq_bits BLOB NOT NULL,
    self_min_ns INTEGER,
    self_max_ns INTEGER,
    self_total_ns INTEGER,
    non_reentrant_count INTEGER,
    non_reentrant_min_ns INTEGER,
    non_reentrant_max_ns INTEGER,
    non_reentrant_total_ns INTEGER,
    PRIMARY KEY(domain, source_location_id)
) WITHOUT ROWID;

CREATE TABLE derived_zone_statistics_threads (
    source_location_id INTEGER NOT NULL,
    compressed_thread_id INTEGER NOT NULL,
    event_count INTEGER NOT NULL,
    PRIMARY KEY(source_location_id, compressed_thread_id)
) WITHOUT ROWID;

CREATE TABLE derived_symbol_statistics (
    symbol_address_u64 BLOB NOT NULL,
    symbol_address_hex TEXT PRIMARY KEY,
    inclusive_count INTEGER NOT NULL,
    exclusive_count INTEGER NOT NULL
) WITHOUT ROWID;

CREATE TABLE derived_symbol_stat_parents (
    symbol_address_hex TEXT NOT NULL REFERENCES derived_symbol_statistics(symbol_address_hex),
    parent_kind TEXT NOT NULL,
    parent_index INTEGER NOT NULL,
    event_count INTEGER NOT NULL,
    PRIMARY KEY(symbol_address_hex, parent_kind, parent_index)
) WITHOUT ROWID;

CREATE TABLE derived_symbol_samples (
    symbol_address_u64 BLOB NOT NULL,
    symbol_address_hex TEXT NOT NULL,
    sample_index INTEGER NOT NULL,
    time_ns INTEGER NOT NULL,
    compressed_thread_id INTEGER NOT NULL,
    thread_u64 BLOB NOT NULL,
    thread_hex TEXT NOT NULL,
    instruction_pointer_u64 BLOB NOT NULL,
    instruction_pointer_hex TEXT NOT NULL,
    PRIMARY KEY(symbol_address_hex, sample_index)
) WITHOUT ROWID;

CREATE TABLE derived_child_samples (
    parent_address_u64 BLOB NOT NULL,
    parent_address_hex TEXT NOT NULL,
    sample_index INTEGER NOT NULL,
    time_ns INTEGER NOT NULL,
    child_address_u64 BLOB NOT NULL,
    child_address_hex TEXT NOT NULL,
    PRIMARY KEY(parent_address_hex, sample_index)
) WITHOUT ROWID;

CREATE TABLE derived_instruction_pointers (
    symbol_address_u64 BLOB NOT NULL,
    symbol_address_hex TEXT NOT NULL,
    frame_id_u64 BLOB NOT NULL,
    frame_id_hex TEXT NOT NULL,
    event_count INTEGER NOT NULL,
    PRIMARY KEY(symbol_address_hex, frame_id_hex)
) WITHOUT ROWID;

CREATE TABLE fiber_thread_map (
    fiber_u64 BLOB NOT NULL,
    fiber_hex TEXT PRIMARY KEY,
    thread_u64 BLOB NOT NULL,
    thread_hex TEXT NOT NULL
) WITHOUT ROWID;
)SQL" );
}

void SqliteExporter::InsertMeta( const std::string& key, const std::string& value )
{
    Statement stmt( m_db->Handle(), "INSERT INTO meta(key, value) VALUES(?1, ?2)" );
    stmt.BindText( 1, key );
    stmt.BindText( 2, value );
    stmt.StepDone();
}

void SqliteExporter::Audit( const std::string& section, const std::string& table, uint64_t sourceCount, uint64_t exportedCount, const std::string& details )
{
    std::string quoted;
    quoted.reserve( table.size() + 2 );
    quoted.push_back( '"' );
    for( const char c : table )
    {
        if( c == '"' ) quoted.push_back( '"' );
        quoted.push_back( c );
    }
    quoted.push_back( '"' );

    const uint64_t databaseCount = static_cast<uint64_t>( m_db->ScalarInt64( ( "SELECT count(*) FROM " + quoted ).c_str() ) );
    const bool pass = sourceCount == exportedCount && exportedCount == databaseCount;

    Statement stmt( m_db->Handle(),
        "INSERT INTO export_audit(section, table_name, source_count, exported_count, database_count, status, details) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)" );
    stmt.BindText( 1, section );
    stmt.BindText( 2, table );
    stmt.BindInt64( 3, static_cast<int64_t>( sourceCount ) );
    stmt.BindInt64( 4, static_cast<int64_t>( exportedCount ) );
    stmt.BindInt64( 5, static_cast<int64_t>( databaseCount ) );
    stmt.BindText( 6, pass ? "PASS" : "MISMATCH" );
    stmt.BindText( 7, details );
    stmt.StepDone();

    if( !pass )
    {
        std::ostringstream ss;
        ss << "Audit failed for " << section << ": source=" << sourceCount
           << ", exported=" << exportedCount << ", database=" << databaseCount;
        Fail( ss.str() );
    }
}

void SqliteExporter::ExportRawCapture()
{
    constexpr size_t ChunkSize = 16 * 1024 * 1024;
    std::ifstream input( m_input, std::ios::binary );
    if( !input ) Fail( "Cannot reopen input trace for archival copy" );

    Statement insert( m_db->Handle(),
        "INSERT INTO capture_chunks(chunk_index, byte_offset, byte_count, sha256, data) VALUES(?1, ?2, ?3, ?4, ?5)" );

    Sha256 fullHash;
    std::vector<uint8_t> buffer( ChunkSize );
    uint64_t offset = 0;
    uint64_t chunkIndex = 0;
    for(;;)
    {
        input.read( reinterpret_cast<char*>( buffer.data() ), static_cast<std::streamsize>( buffer.size() ) );
        const auto count = static_cast<size_t>( input.gcount() );
        if( count == 0 ) break;

        fullHash.Update( buffer.data(), count );
        const auto chunkDigest = Sha256::Digest( buffer.data(), count );

        insert.BindInt64( 1, static_cast<int64_t>( chunkIndex ) );
        insert.BindInt64( 2, static_cast<int64_t>( offset ) );
        insert.BindInt64( 3, static_cast<int64_t>( count ) );
        insert.BindText( 4, Hex( chunkDigest.data(), chunkDigest.size() ) );
        insert.BindBlob( 5, buffer.data(), count );
        insert.StepDone();

        offset += count;
        chunkIndex++;
    }
    if( !input.eof() ) Fail( "Read error while archiving input trace" );

    const auto digest = fullHash.Finish();
    if( offset != m_frozenInput.size || digest != m_frozenInput.sha256 )
    {
        Fail( "Input trace changed between the initial freeze hash and archival export" );
    }
    InsertMeta( "input_path", m_input.string() );
    InsertMeta( "input_size", std::to_string( offset ) );
    InsertMeta( "input_sha256", Hex( digest.data(), digest.size() ) );
    InsertMeta( "capture_chunk_size", std::to_string( ChunkSize ) );
    Audit( "raw_capture_chunks", "capture_chunks", chunkIndex, chunkIndex,
        "Exact source bytes; reconstruction SHA-256 must match input_sha256." );
}

void SqliteExporter::ExportMetadata()
{
    Statement metadata( m_db->Handle(),
        "INSERT INTO capture_metadata("
        "singleton, trace_version, resolution, timer_mul, timer_mul_bits, first_time_ns, last_time_ns, "
        "frame_offset_u64, frame_offset_hex, pid_u64, pid_hex, sampling_period_ns, cpu_arch, cpu_id, "
        "cpu_manufacturer, on_demand, inconsistent_samples, capture_name, capture_name_text, "
        "capture_program, capture_program_text, capture_time_u64, capture_time_hex, executable_time_u64, "
        "executable_time_hex, host_info, host_info_text, load_time_ns) "
        "VALUES(1, ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, "
        "?17, ?18, ?19, ?20, ?21, ?22, ?23, ?24, ?25, ?26, ?27)" );
    metadata.BindInt( 1, m_worker.m_traceVersion );
    metadata.BindInt64( 2, m_worker.m_resolution );
    BindDoubleExact( metadata, 3, 4, m_worker.m_timerMul );
    metadata.BindInt64( 5, m_worker.GetFirstTime() );
    metadata.BindInt64( 6, m_worker.m_data.lastTime );
    BindU64( metadata, 7, 8, m_worker.m_data.frameOffset );
    BindU64( metadata, 9, 10, m_worker.m_pid );
    metadata.BindInt64( 11, m_worker.m_samplingPeriod );
    metadata.BindInt( 12, static_cast<int>( m_worker.m_data.cpuArch ) );
    metadata.BindInt( 13, static_cast<int>( m_worker.m_data.cpuId ) );
    metadata.BindBlob( 14, m_worker.m_data.cpuManufacturer, 12 );
    metadata.BindInt( 15, m_worker.m_onDemand ? 1 : 0 );
    metadata.BindInt( 16, m_worker.m_inconsistentSamples ? 1 : 0 );
    metadata.BindBlob( 17, m_worker.m_captureName.data(), m_worker.m_captureName.size() );
    metadata.BindText( 18, m_worker.m_captureName );
    metadata.BindBlob( 19, m_worker.m_captureProgram.data(), m_worker.m_captureProgram.size() );
    metadata.BindText( 20, m_worker.m_captureProgram );
    BindU64( metadata, 21, 22, m_worker.m_captureTime );
    BindU64( metadata, 23, 24, m_worker.m_executableTime );
    metadata.BindBlob( 25, m_worker.m_hostInfo.data(), m_worker.m_hostInfo.size() );
    metadata.BindText( 26, m_worker.m_hostInfo );
    metadata.BindInt64( 27, m_worker.m_loadTime );
    metadata.StepDone();
    Audit( "capture_metadata", "capture_metadata", 1, 1 );

    Statement topology( m_db->Handle(),
        "INSERT INTO cpu_topology(package_id, die_id, core_id, sibling_ordinal, hardware_thread) "
        "VALUES(?1, ?2, ?3, ?4, ?5)" );
    uint64_t topologyCount = 0;
    for( const auto& package : m_worker.m_data.cpuTopology )
    {
        for( const auto& die : package.second )
        {
            for( const auto& core : die.second )
            {
                for( size_t i = 0; i < core.second.size(); ++i )
                {
                    topology.BindInt64( 1, package.first );
                    topology.BindInt64( 2, die.first );
                    topology.BindInt64( 3, core.first );
                    topology.BindInt64( 4, static_cast<int64_t>( i ) );
                    topology.BindInt64( 5, core.second[i] );
                    topology.StepDone();
                    topologyCount++;
                }
            }
        }
    }
    Audit( "cpu_topology", "cpu_topology", topologyCount, topologyCount );

    const auto& crash = m_worker.m_data.crashEvent;
    Statement crashStmt( m_db->Handle(),
        "INSERT INTO crash_event(singleton, thread_u64, thread_hex, time_ns, message_u64, message_hex, callstack_id) "
        "VALUES(1, ?1, ?2, ?3, ?4, ?5, ?6)" );
    BindU64( crashStmt, 1, 2, crash.thread );
    crashStmt.BindInt64( 3, crash.time );
    BindU64( crashStmt, 4, 5, crash.message );
    crashStmt.BindInt64( 6, crash.callstack );
    crashStmt.StepDone();
    Audit( "crash_event", "crash_event", 1, 1 );
}

std::string SqliteExporter::StringRefText( const StringRef& ref ) const
{
    if( !ref.active ) return {};
    return Utf8OrEmpty( m_worker.GetString( ref ) );
}

std::string SqliteExporter::StringIdxText( const StringIdx& idx ) const
{
    if( !idx.Active() ) return {};
    return Utf8OrEmpty( m_worker.GetString( idx ) );
}

void SqliteExporter::BindOptionalStringIdx( Statement& stmt, int indexColumn, int textColumn, const StringIdx& idx ) const
{
    if( idx.Active() )
    {
        stmt.BindInt64( indexColumn, idx.Idx() );
        stmt.BindText( textColumn, StringIdxText( idx ) );
    }
    else
    {
        stmt.BindNull( indexColumn );
        stmt.BindNull( textColumn );
    }
}

uint64_t SqliteExporter::OriginalStringPointer( const char* value ) const
{
    const auto it = m_originalStringPointer.find( value );
    if( it == m_originalStringPointer.end() ) Fail( "A loaded string has no preserved original pointer identity" );
    return it->second;
}

uint32_t SqliteExporter::StringIndex( const char* value ) const
{
    const auto it = m_stringIndex.find( value );
    if( it == m_stringIndex.end() ) Fail( "A string reference does not resolve to string_data" );
    return it->second;
}

uint64_t SqliteExporter::DecompressLocalThread( uint16_t compressed ) const
{
    const auto& expand = m_worker.m_data.localThreadCompress.m_threadExpand;
    if( compressed >= expand.size() ) Fail( "Compressed local thread id is outside thread_compression" );
    return expand[compressed];
}

uint64_t SqliteExporter::DecompressExternalThread( uint16_t compressed ) const
{
    const auto& expand = m_worker.m_data.externalThreadCompress.m_threadExpand;
    if( compressed >= expand.size() ) Fail( "Compressed external thread id is outside thread_compression" );
    return expand[compressed];
}

void SqliteExporter::ExportStrings()
{
    const auto& strings = m_worker.m_data.stringData;
    if( strings.size() != m_worker.m_data.exportOriginalStringPointers.size() )
    {
        Fail( "Preserved original string pointer vector does not match string_data" );
    }

    Statement stringStmt( m_db->Handle(),
        "INSERT INTO string_data(string_index, original_pointer_u64, original_pointer_hex, byte_count, value_blob, value_text) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6)" );
    m_originalStringPointer.reserve( strings.size() );
    m_stringIndex.reserve( strings.size() );
    for( size_t i = 0; i < strings.size(); ++i )
    {
        const char* value = strings[i];
        const size_t len = strlen( value );
        const uint64_t original = m_worker.m_data.exportOriginalStringPointers[i];
        stringStmt.BindInt64( 1, static_cast<int64_t>( i ) );
        BindU64( stringStmt, 2, 3, original );
        stringStmt.BindInt64( 4, static_cast<int64_t>( len ) );
        stringStmt.BindBlob( 5, value, len );
        stringStmt.BindText( 6, std::string( value, len ) );
        stringStmt.StepDone();
        m_originalStringPointer.emplace( value, original );
        m_stringIndex.emplace( value, static_cast<uint32_t>( i ) );
    }
    Audit( "string_data", "string_data", strings.size(), strings.size() );

    Statement mapStmt( m_db->Handle(),
        "INSERT INTO string_map(string_key_u64, string_key_hex, string_index) VALUES(?1, ?2, ?3)" );
    uint64_t mapCount = 0;
    for( const auto& entry : m_worker.m_data.strings )
    {
        BindU64( mapStmt, 1, 2, entry.first );
        mapStmt.BindInt64( 3, StringIndex( entry.second ) );
        mapStmt.StepDone();
        ++mapCount;
    }
    Audit( "string_map", "string_map", m_worker.m_data.strings.size(), mapCount );

    Statement threadNameStmt( m_db->Handle(),
        "INSERT INTO thread_names(thread_u64, thread_hex, string_index, name_text) VALUES(?1, ?2, ?3, ?4)" );
    uint64_t threadNameCount = 0;
    for( const auto& entry : m_worker.m_data.threadNames )
    {
        BindU64( threadNameStmt, 1, 2, entry.first );
        threadNameStmt.BindInt64( 3, StringIndex( entry.second ) );
        threadNameStmt.BindText( 4, entry.second );
        threadNameStmt.StepDone();
        ++threadNameCount;
    }
    Audit( "thread_names", "thread_names", m_worker.m_data.threadNames.size(), threadNameCount );

    Statement externalNameStmt( m_db->Handle(),
        "INSERT INTO external_names(external_id_u64, external_id_hex, process_string_index, thread_string_index, "
        "process_name, thread_name) VALUES(?1, ?2, ?3, ?4, ?5, ?6)" );
    uint64_t externalNameCount = 0;
    for( const auto& entry : m_worker.m_data.externalNames )
    {
        BindU64( externalNameStmt, 1, 2, entry.first );
        externalNameStmt.BindInt64( 3, StringIndex( entry.second.first ) );
        externalNameStmt.BindInt64( 4, StringIndex( entry.second.second ) );
        externalNameStmt.BindText( 5, entry.second.first );
        externalNameStmt.BindText( 6, entry.second.second );
        externalNameStmt.StepDone();
        ++externalNameCount;
    }
    Audit( "external_names", "external_names", m_worker.m_data.externalNames.size(), externalNameCount );

    Statement compressionStmt( m_db->Handle(),
        "INSERT INTO thread_compression(domain, compressed_id, thread_u64, thread_hex) VALUES(?1, ?2, ?3, ?4)" );
    uint64_t compressionCount = 0;
    const auto exportCompression = [&]( const char* domain, const ThreadCompress& compression )
    {
        for( size_t i = 0; i < compression.m_threadExpand.size(); ++i )
        {
            compressionStmt.BindText( 1, domain );
            compressionStmt.BindInt64( 2, static_cast<int64_t>( i ) );
            BindU64( compressionStmt, 3, 4, compression.m_threadExpand[i] );
            compressionStmt.StepDone();
            ++compressionCount;
        }
    };
    exportCompression( "local", m_worker.m_data.localThreadCompress );
    exportCompression( "external", m_worker.m_data.externalThreadCompress );
    const uint64_t compressionSource = m_worker.m_data.localThreadCompress.m_threadExpand.size()
        + m_worker.m_data.externalThreadCompress.m_threadExpand.size();
    Audit( "thread_compression", "thread_compression", compressionSource, compressionCount,
        "Contains both local and external compression domains." );
}

void SqliteExporter::ExportFrames()
{
    Statement setStmt( m_db->Handle(),
        "INSERT INTO frame_sets(frame_set_id, name_u64, name_hex, name_text, continuous, is_base, "
        "min_ns, max_ns, total_ns, sum_sq, sum_sq_bits) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)" );
    Statement frameStmt( m_db->Handle(),
        "INSERT INTO frames(frame_set_id, frame_index, start_ns, stored_end_ns, effective_end_ns, frame_image_index) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6)" );

    uint64_t setCount = 0;
    uint64_t frameCount = 0;
    const auto& sets = m_worker.m_data.frames.Data();
    for( size_t i = 0; i < sets.size(); ++i )
    {
        const auto* set = sets[i];
        setStmt.BindInt64( 1, static_cast<int64_t>( i ) );
        BindU64( setStmt, 2, 3, set->name );
        setStmt.BindText( 4, set->name == 0 ? std::string() : Utf8OrEmpty( m_worker.GetString( set->name ) ) );
        setStmt.BindInt( 5, set->continuous ? 1 : 0 );
        setStmt.BindInt( 6, set == m_worker.m_data.framesBase ? 1 : 0 );
        setStmt.BindInt64( 7, set->min );
        setStmt.BindInt64( 8, set->max );
        setStmt.BindInt64( 9, set->total );
        BindDoubleExact( setStmt, 10, 11, set->sumSq );
        setStmt.StepDone();
        ++setCount;

        for( size_t j = 0; j < set->frames.size(); ++j )
        {
            const auto& frame = set->frames[j];
            frameStmt.BindInt64( 1, static_cast<int64_t>( i ) );
            frameStmt.BindInt64( 2, static_cast<int64_t>( j ) );
            frameStmt.BindInt64( 3, frame.start );
            frameStmt.BindInt64( 4, frame.end );
            frameStmt.BindInt64( 5, m_worker.GetFrameEnd( *set, j ) );
            frameStmt.BindInt64( 6, frame.frameImage );
            frameStmt.StepDone();
            ++frameCount;
        }
    }
    uint64_t sourceFrameCount = 0;
    for( const auto* set : sets ) sourceFrameCount += set->frames.size();
    Audit( "frame_sets", "frame_sets", sets.size(), setCount );
    Audit( "frames", "frames", sourceFrameCount, frameCount );
}

void SqliteExporter::ExportSourceLocations()
{
    const auto bindRef = [&]( Statement& stmt, int first, const StringRef& ref )
    {
        BindU64( stmt, first, first + 1, ref.str );
        stmt.BindInt( first + 2, ref.active ? 1 : 0 );
        stmt.BindInt( first + 3, ref.isidx ? 1 : 0 );
    };

    Statement staticStmt( m_db->Handle(),
        "INSERT INTO source_locations_static(static_key_u64, static_key_hex, raw_payload, "
        "name_ref_u64, name_ref_hex, name_active, name_is_index, "
        "function_ref_u64, function_ref_hex, function_active, function_is_index, "
        "file_ref_u64, file_ref_hex, file_active, file_is_index, "
        "name_text, function_text, file_text, line, color) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, "
        "?16, ?17, ?18, ?19, ?20)" );
    uint64_t staticCount = 0;
    for( const auto& entry : m_worker.m_data.sourceLocation )
    {
        const SourceLocationBase& source = entry.second;
        BindU64( staticStmt, 1, 2, entry.first );
        staticStmt.BindBlob( 3, &source, sizeof( source ) );
        bindRef( staticStmt, 4, source.name );
        bindRef( staticStmt, 8, source.function );
        bindRef( staticStmt, 12, source.file );
        staticStmt.BindText( 16, StringRefText( source.name ) );
        staticStmt.BindText( 17, StringRefText( source.function ) );
        staticStmt.BindText( 18, StringRefText( source.file ) );
        staticStmt.BindInt64( 19, source.line );
        staticStmt.BindInt64( 20, source.color );
        staticStmt.StepDone();
        ++staticCount;
    }
    Audit( "source_locations_static", "source_locations_static", m_worker.m_data.sourceLocation.size(), staticCount );

    Statement expandStmt( m_db->Handle(),
        "INSERT INTO source_location_expand(source_location_id, static_key_u64, static_key_hex, resolved) "
        "VALUES(?1, ?2, ?3, ?4)" );
    for( size_t i = 0; i < m_worker.m_data.sourceLocationExpand.size(); ++i )
    {
        const uint64_t key = m_worker.m_data.sourceLocationExpand[i];
        expandStmt.BindInt64( 1, static_cast<int64_t>( i ) );
        BindU64( expandStmt, 2, 3, key );
        expandStmt.BindInt( 4, m_worker.m_data.sourceLocation.find( key ) != m_worker.m_data.sourceLocation.end() ? 1 : 0 );
        expandStmt.StepDone();
    }
    Audit( "source_location_expand", "source_location_expand",
        m_worker.m_data.sourceLocationExpand.size(), m_worker.m_data.sourceLocationExpand.size() );

    Statement dynamicStmt( m_db->Handle(),
        "INSERT INTO source_locations_dynamic(payload_index, source_location_id, raw_payload, "
        "name_ref_u64, name_ref_hex, name_active, name_is_index, "
        "function_ref_u64, function_ref_hex, function_active, function_is_index, "
        "file_ref_u64, file_ref_hex, file_active, file_is_index, "
        "name_text, function_text, file_text, line, color) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, "
        "?16, ?17, ?18, ?19, ?20)" );
    for( size_t i = 0; i < m_worker.m_data.sourceLocationPayload.size(); ++i )
    {
        const SourceLocationBase& source = *m_worker.m_data.sourceLocationPayload[i];
        dynamicStmt.BindInt64( 1, static_cast<int64_t>( i ) );
        dynamicStmt.BindInt64( 2, -static_cast<int64_t>( i ) - 1 );
        dynamicStmt.BindBlob( 3, &source, sizeof( source ) );
        bindRef( dynamicStmt, 4, source.name );
        bindRef( dynamicStmt, 8, source.function );
        bindRef( dynamicStmt, 12, source.file );
        dynamicStmt.BindText( 16, StringRefText( source.name ) );
        dynamicStmt.BindText( 17, StringRefText( source.function ) );
        dynamicStmt.BindText( 18, StringRefText( source.file ) );
        dynamicStmt.BindInt64( 19, source.line );
        dynamicStmt.BindInt64( 20, source.color );
        dynamicStmt.StepDone();
    }
    Audit( "source_locations_dynamic", "source_locations_dynamic",
        m_worker.m_data.sourceLocationPayload.size(), m_worker.m_data.sourceLocationPayload.size() );

    Statement countStmt( m_db->Handle(),
        "INSERT INTO source_location_zone_counts(domain, source_location_id, event_count) VALUES(?1, ?2, ?3)" );
    uint64_t countRows = 0;
    for( const auto& entry : m_worker.m_data.sourceLocationZones )
    {
        countStmt.BindText( 1, "cpu" );
        countStmt.BindInt64( 2, entry.first );
        countStmt.BindInt64( 3, static_cast<int64_t>( entry.second.zones.size() ) );
        countStmt.StepDone();
        ++countRows;
    }
    for( const auto& entry : m_worker.m_data.gpuSourceLocationZones )
    {
        countStmt.BindText( 1, "gpu" );
        countStmt.BindInt64( 2, entry.first );
        countStmt.BindInt64( 3, static_cast<int64_t>( entry.second.zones.size() ) );
        countStmt.StepDone();
        ++countRows;
    }
    const uint64_t sourceCountRows =
        m_worker.m_data.sourceLocationZones.size() + m_worker.m_data.gpuSourceLocationZones.size();
    Audit( "source_location_zone_counts", "source_location_zone_counts", sourceCountRows, countRows );
}

void SqliteExporter::ExportLocks()
{
    Statement extraStmt( m_db->Handle(),
        "INSERT INTO zone_extras(extra_id, raw_payload, callstack_id, text_index, text_text, "
        "name_index, name_text, color) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)" );
    for( size_t i = 0; i < m_worker.m_data.zoneExtra.size(); ++i )
    {
        const auto& extra = m_worker.m_data.zoneExtra[i];
        extraStmt.BindInt64( 1, static_cast<int64_t>( i ) );
        extraStmt.BindBlob( 2, &extra, sizeof( extra ) );
        extraStmt.BindInt64( 3, extra.callstack.Val() );
        BindOptionalStringIdx( extraStmt, 4, 5, extra.text );
        BindOptionalStringIdx( extraStmt, 6, 7, extra.name );
        extraStmt.BindInt64( 8, extra.color.Val() );
        extraStmt.StepDone();
    }
    Audit( "zone_extras", "zone_extras", m_worker.m_data.zoneExtra.size(), m_worker.m_data.zoneExtra.size() );

    Statement lockStmt( m_db->Handle(),
        "INSERT INTO locks(lock_id, custom_name_index, custom_name_text, source_location_id, lock_type, "
        "valid, time_announce_ns, time_terminate_ns, is_contended, locking_thread_u64, locking_thread_hex) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)" );
    Statement threadStmt( m_db->Handle(),
        "INSERT INTO lock_threads(lock_id, thread_ordinal, thread_u64, thread_hex) VALUES(?1, ?2, ?3, ?4)" );
    Statement eventStmt( m_db->Handle(),
        "INSERT INTO lock_events(lock_id, event_index, time_ns, source_location_id, lock_thread_index, "
        "event_type, locking_thread_index, lock_count, wait_list_u64, wait_list_hex, "
        "wait_shared_u64, wait_shared_hex, shared_list_u64, shared_list_hex) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14)" );

    uint64_t lockCount = 0;
    uint64_t lockThreadCount = 0;
    uint64_t lockEventCount = 0;
    uint64_t sourceThreadCount = 0;
    uint64_t sourceEventCount = 0;
    for( const auto& entry : m_worker.m_data.lockMap )
    {
        const auto& lock = *entry.second;
        lockStmt.BindInt64( 1, entry.first );
        BindOptionalStringIdx( lockStmt, 2, 3, lock.customName );
        lockStmt.BindInt64( 4, lock.srcloc );
        lockStmt.BindInt( 5, static_cast<int>( lock.type ) );
        lockStmt.BindInt( 6, lock.valid ? 1 : 0 );
        lockStmt.BindInt64( 7, lock.timeAnnounce );
        lockStmt.BindInt64( 8, lock.timeTerminate );
        lockStmt.BindInt( 9, lock.isContended ? 1 : 0 );
        BindU64( lockStmt, 10, 11, lock.lockingThread );
        lockStmt.StepDone();
        ++lockCount;

        sourceThreadCount += lock.threadList.size();
        for( size_t i = 0; i < lock.threadList.size(); ++i )
        {
            threadStmt.BindInt64( 1, entry.first );
            threadStmt.BindInt64( 2, static_cast<int64_t>( i ) );
            BindU64( threadStmt, 3, 4, lock.threadList[i] );
            threadStmt.StepDone();
            ++lockThreadCount;
        }

        sourceEventCount += lock.timeline.size();
        for( size_t i = 0; i < lock.timeline.size(); ++i )
        {
            const auto& eventPtr = lock.timeline[i];
            const auto& event = *eventPtr.ptr;
            eventStmt.BindInt64( 1, entry.first );
            eventStmt.BindInt64( 2, static_cast<int64_t>( i ) );
            eventStmt.BindInt64( 3, event.Time() );
            eventStmt.BindInt64( 4, event.SrcLoc() );
            eventStmt.BindInt( 5, event.thread );
            eventStmt.BindInt( 6, static_cast<int>( event.type ) );
            eventStmt.BindInt( 7, eventPtr.lockingThread );
            eventStmt.BindInt( 8, eventPtr.lockCount );
            BindU64( eventStmt, 9, 10, eventPtr.waitList );
            if( lock.type == LockType::SharedLockable )
            {
                const auto& shared = static_cast<const LockEventShared&>( event );
                BindU64( eventStmt, 11, 12, shared.waitShared );
                BindU64( eventStmt, 13, 14, shared.sharedList );
            }
            else
            {
                eventStmt.BindNull( 11 );
                eventStmt.BindNull( 12 );
                eventStmt.BindNull( 13 );
                eventStmt.BindNull( 14 );
            }
            eventStmt.StepDone();
            ++lockEventCount;
        }
    }
    Audit( "locks", "locks", m_worker.m_data.lockMap.size(), lockCount );
    Audit( "lock_threads", "lock_threads", sourceThreadCount, lockThreadCount );
    Audit( "lock_events", "lock_events", sourceEventCount, lockEventCount );
}

void SqliteExporter::ExportMessagesAndThreads()
{
    Statement threadStmt( m_db->Handle(),
        "INSERT INTO threads(thread_index, thread_u64, thread_hex, thread_name, zone_count, "
        "kernel_sample_count, is_fiber, group_hint) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)" );
    std::unordered_map<const ThreadData*, int64_t> threadIndices;
    threadIndices.reserve( m_worker.m_data.threads.size() );
    uint64_t threadCount = 0;
    uint64_t declaredZoneCount = 0;
    for( size_t i = 0; i < m_worker.m_data.threads.size(); ++i )
    {
        const auto* thread = m_worker.m_data.threads[i];
        threadStmt.BindInt64( 1, static_cast<int64_t>( i ) );
        BindU64( threadStmt, 2, 3, thread->id );
        threadStmt.BindText( 4, Utf8OrEmpty( m_worker.GetThreadName( thread->id ) ) );
        threadStmt.BindInt64( 5, static_cast<int64_t>( thread->count ) );
        threadStmt.BindInt64( 6, static_cast<int64_t>( thread->kernelSampleCnt ) );
        threadStmt.BindInt( 7, thread->isFiber ? 1 : 0 );
        threadStmt.BindInt64( 8, thread->groupHint );
        threadStmt.StepDone();
        threadIndices.emplace( thread, static_cast<int64_t>( i ) );
        declaredZoneCount += thread->count;
        ++threadCount;
    }
    if( declaredZoneCount != m_worker.m_data.zonesCnt )
    {
        Fail( "Thread zone counts do not match Worker::zonesCnt" );
    }
    Audit( "threads", "threads", m_worker.m_data.threads.size(), threadCount );

    Statement messageStmt( m_db->Handle(),
        "INSERT INTO messages(message_id, original_pointer_u64, original_pointer_hex, time_ns, "
        "string_ref_u64, string_ref_hex, string_ref_active, string_ref_is_index, message_text, "
        "compressed_thread_id, thread_u64, thread_hex, color, callstack_id) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14)" );
    std::unordered_map<const MessageData*, int64_t> messageIds;
    messageIds.reserve( m_worker.m_data.messages.size() );
    uint64_t messageCount = 0;
    for( size_t i = 0; i < m_worker.m_data.messages.size(); ++i )
    {
        const MessageData* message = m_worker.m_data.messages[i];
        const auto originalIt = m_worker.m_data.exportOriginalMessagePointers.find( message );
        if( originalIt == m_worker.m_data.exportOriginalMessagePointers.end() )
        {
            Fail( "A global message has no preserved original pointer identity" );
        }
        const uint64_t thread = DecompressLocalThread( message->thread );
        messageStmt.BindInt64( 1, static_cast<int64_t>( i ) );
        BindU64( messageStmt, 2, 3, originalIt->second );
        messageStmt.BindInt64( 4, message->time );
        BindU64( messageStmt, 5, 6, message->ref.str );
        messageStmt.BindInt( 7, message->ref.active ? 1 : 0 );
        messageStmt.BindInt( 8, message->ref.isidx ? 1 : 0 );
        messageStmt.BindText( 9, StringRefText( message->ref ) );
        messageStmt.BindInt64( 10, message->thread );
        BindU64( messageStmt, 11, 12, thread );
        messageStmt.BindInt64( 13, message->color );
        messageStmt.BindInt64( 14, message->callstack.Val() );
        messageStmt.StepDone();
        messageIds.emplace( message, static_cast<int64_t>( i ) );
        ++messageCount;
    }
    Audit( "messages", "messages", m_worker.m_data.messages.size(), messageCount );

    Statement threadMessageStmt( m_db->Handle(),
        "INSERT INTO thread_messages(thread_index, message_ordinal, message_id, original_pointer_u64, "
        "original_pointer_hex) VALUES(?1, ?2, ?3, ?4, ?5)" );
    uint64_t sourceThreadMessages = 0;
    uint64_t exportedThreadMessages = 0;
    for( size_t i = 0; i < m_worker.m_data.threads.size(); ++i )
    {
        const auto* thread = m_worker.m_data.threads[i];
        sourceThreadMessages += thread->messages.size();
        for( size_t j = 0; j < thread->messages.size(); ++j )
        {
            const MessageData* message = thread->messages[j];
            const auto idIt = messageIds.find( message );
            const auto pointerIt = m_worker.m_data.exportOriginalMessagePointers.find( message );
            if( idIt == messageIds.end() || pointerIt == m_worker.m_data.exportOriginalMessagePointers.end() )
            {
                Fail( "A per-thread message does not resolve to the global message table" );
            }
            threadMessageStmt.BindInt64( 1, static_cast<int64_t>( i ) );
            threadMessageStmt.BindInt64( 2, static_cast<int64_t>( j ) );
            threadMessageStmt.BindInt64( 3, idIt->second );
            BindU64( threadMessageStmt, 4, 5, pointerIt->second );
            threadMessageStmt.StepDone();
            ++exportedThreadMessages;
        }
    }
    Audit( "thread_messages", "thread_messages", sourceThreadMessages, exportedThreadMessages );

    Statement zoneStmt( m_db->Handle(),
        "INSERT INTO cpu_zones(zone_id, thread_index, parent_zone_id, depth, sibling_ordinal, "
        "start_ns, end_ns, inferred_end_ns, end_valid, source_location_id, extra_id, has_children, "
        "child_vector_index) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13)" );
    uint64_t exportedZones = 0;
    std::function<void( const Vector<short_ptr<ZoneEvent>>&, int64_t, int64_t, int )> exportTimeline;
    exportTimeline = [&]( const Vector<short_ptr<ZoneEvent>>& timeline, int64_t threadIndex, int64_t parent, int depth )
    {
        const auto exportEvent = [&]( const ZoneEvent& event, size_t sibling )
        {
            const int64_t id = m_nextCpuZoneId++;
            zoneStmt.BindInt64( 1, id );
            zoneStmt.BindInt64( 2, threadIndex );
            if( parent == 0 ) zoneStmt.BindNull( 3 );
            else zoneStmt.BindInt64( 3, parent );
            zoneStmt.BindInt( 4, depth );
            zoneStmt.BindInt64( 5, static_cast<int64_t>( sibling ) );
            zoneStmt.BindInt64( 6, event.Start() );
            zoneStmt.BindInt64( 7, event.End() );
            zoneStmt.BindInt64( 8, m_worker.GetZoneEnd( event ) );
            zoneStmt.BindInt( 9, event.IsEndValid() ? 1 : 0 );
            zoneStmt.BindInt64( 10, event.SrcLoc() );
            zoneStmt.BindInt64( 11, event.extra );
            zoneStmt.BindInt( 12, event.HasChildren() ? 1 : 0 );
            zoneStmt.BindInt64( 13, event.Child() );
            zoneStmt.StepDone();
            ++exportedZones;

            if( event.HasChildren() )
            {
                const int32_t child = event.Child();
                if( child < 0 || static_cast<size_t>( child ) >= m_worker.m_data.zoneChildren.size() )
                {
                    Fail( "CPU zone child vector index is outside zoneChildren" );
                }
                exportTimeline( m_worker.m_data.zoneChildren[child], threadIndex, id, depth + 1 );
            }
        };

        if( timeline.is_magic() )
        {
            const auto& direct = *reinterpret_cast<const Vector<ZoneEvent>*>( &timeline );
            for( size_t i = 0; i < direct.size(); ++i ) exportEvent( direct[i], i );
        }
        else
        {
            for( size_t i = 0; i < timeline.size(); ++i ) exportEvent( *timeline[i], i );
        }
    };
    for( size_t i = 0; i < m_worker.m_data.threads.size(); ++i )
    {
        exportTimeline( m_worker.m_data.threads[i]->timeline, static_cast<int64_t>( i ), 0, 0 );
    }
    Audit( "cpu_zones", "cpu_zones", m_worker.m_data.zonesCnt, exportedZones,
        "Recursive traversal covers direct and magic-vector timelines." );

    Statement sampleStmt( m_db->Handle(),
        "INSERT INTO thread_samples(thread_index, sample_kind, sample_index, time_ns, callstack_id) "
        "VALUES(?1, ?2, ?3, ?4, ?5)" );
    uint64_t sourceSamples = 0;
    uint64_t exportedSamples = 0;
    for( size_t i = 0; i < m_worker.m_data.threads.size(); ++i )
    {
        const auto* thread = m_worker.m_data.threads[i];
        const auto exportSamples = [&]( const char* kind, const Vector<SampleData>& samples )
        {
            sourceSamples += samples.size();
            for( size_t j = 0; j < samples.size(); ++j )
            {
                sampleStmt.BindInt64( 1, static_cast<int64_t>( i ) );
                sampleStmt.BindText( 2, kind );
                sampleStmt.BindInt64( 3, static_cast<int64_t>( j ) );
                sampleStmt.BindInt64( 4, samples[j].time.Val() );
                sampleStmt.BindInt64( 5, samples[j].callstack.Val() );
                sampleStmt.StepDone();
                ++exportedSamples;
            }
        };
        exportSamples( "context_switch", thread->ctxSwitchSamples );
        exportSamples( "sample", thread->samples );
    }
    Audit( "thread_samples", "thread_samples", sourceSamples, exportedSamples,
        "Includes sampling-profiler and context-switch callstack samples." );
}

void SqliteExporter::ExportGpu()
{
    Statement contextStmt( m_db->Handle(),
        "INSERT INTO gpu_contexts(gpu_context_id, owning_thread_u64, owning_thread_hex, event_count, "
        "period, period_bits, context_type, has_period, has_calibration, time_diff_ns, "
        "calibrated_gpu_time_ns, calibrated_cpu_time_ns, calibration_mod, calibration_mod_bits, "
        "last_gpu_time_ns, overflow_u64, overflow_hex, overflow_multiplier, name_index, name_text) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, "
        "?16, ?17, ?18, ?19, ?20)" );
    Statement noteNameStmt( m_db->Handle(),
        "INSERT INTO gpu_context_note_names(gpu_context_id, note_id, name_index, name_text) "
        "VALUES(?1, ?2, ?3, ?4)" );
    Statement contextThreadStmt( m_db->Handle(),
        "INSERT INTO gpu_context_threads(gpu_context_id, context_thread_index, thread_u64, thread_hex) "
        "VALUES(?1, ?2, ?3, ?4)" );
    Statement zoneStmt( m_db->Handle(),
        "INSERT INTO gpu_zones(gpu_zone_id, gpu_context_id, context_thread_index, parent_gpu_zone_id, "
        "depth, sibling_ordinal, cpu_start_ns, cpu_end_ns, gpu_start_ns, gpu_end_ns, gpu_end_valid, "
        "source_location_id, callstack_id, compressed_thread_id, event_thread_u64, event_thread_hex, "
        "query_id, has_children, child_vector_index) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, "
        "?16, ?17, ?18, ?19)" );
    Statement noteStmt( m_db->Handle(),
        "INSERT INTO gpu_notes(gpu_context_id, query_id, note_id, value, value_bits) "
        "VALUES(?1, ?2, ?3, ?4, ?5)" );

    uint64_t contextCount = 0;
    uint64_t noteNameCount = 0;
    uint64_t sourceNoteNameCount = 0;
    uint64_t contextThreadCount = 0;
    uint64_t sourceContextThreadCount = 0;
    uint64_t zoneCount = 0;
    uint64_t noteCount = 0;
    uint64_t sourceNoteCount = 0;
    uint64_t declaredGpuCount = 0;

    for( size_t contextIndex = 0; contextIndex < m_worker.m_data.gpuData.size(); ++contextIndex )
    {
        const auto* context = m_worker.m_data.gpuData[contextIndex];
        contextStmt.BindInt64( 1, static_cast<int64_t>( contextIndex ) );
        BindU64( contextStmt, 2, 3, context->thread );
        contextStmt.BindInt64( 4, static_cast<int64_t>( context->count ) );
        BindFloatExact( contextStmt, 5, 6, context->period );
        contextStmt.BindInt( 7, static_cast<int>( context->type ) );
        contextStmt.BindInt( 8, context->hasPeriod ? 1 : 0 );
        contextStmt.BindInt( 9, context->hasCalibration ? 1 : 0 );
        contextStmt.BindInt64( 10, context->timeDiff );
        contextStmt.BindInt64( 11, context->calibratedGpuTime );
        contextStmt.BindInt64( 12, context->calibratedCpuTime );
        BindDoubleExact( contextStmt, 13, 14, context->calibrationMod );
        contextStmt.BindInt64( 15, context->lastGpuTime );
        BindU64( contextStmt, 16, 17, context->overflow );
        contextStmt.BindInt64( 18, context->overflowMul );
        BindOptionalStringIdx( contextStmt, 19, 20, context->name );
        contextStmt.StepDone();
        ++contextCount;
        declaredGpuCount += context->count;

        sourceNoteNameCount += context->noteNames.size();
        for( const auto& name : context->noteNames )
        {
            noteNameStmt.BindInt64( 1, static_cast<int64_t>( contextIndex ) );
            noteNameStmt.BindInt64( 2, name.first );
            noteNameStmt.BindInt64( 3, name.second.Idx() );
            noteNameStmt.BindText( 4, StringIdxText( name.second ) );
            noteNameStmt.StepDone();
            ++noteNameCount;
        }

        size_t contextThreadIndex = 0;
        sourceContextThreadCount += context->threadData.size();
        for( const auto& threadData : context->threadData )
        {
            contextThreadStmt.BindInt64( 1, static_cast<int64_t>( contextIndex ) );
            contextThreadStmt.BindInt64( 2, static_cast<int64_t>( contextThreadIndex ) );
            BindU64( contextThreadStmt, 3, 4, threadData.first );
            contextThreadStmt.StepDone();
            ++contextThreadCount;

            std::function<void( const Vector<short_ptr<GpuEvent>>&, int64_t, int )> exportTimeline;
            exportTimeline = [&]( const Vector<short_ptr<GpuEvent>>& timeline, int64_t parent, int depth )
            {
                const auto exportEvent = [&]( const GpuEvent& event, size_t sibling )
                {
                    const int64_t id = m_nextGpuZoneId++;
                    const uint64_t eventThread = DecompressLocalThread( event.Thread() );
                    const bool hasChildren = event.Child() >= 0;
                    zoneStmt.BindInt64( 1, id );
                    zoneStmt.BindInt64( 2, static_cast<int64_t>( contextIndex ) );
                    zoneStmt.BindInt64( 3, static_cast<int64_t>( contextThreadIndex ) );
                    if( parent == 0 ) zoneStmt.BindNull( 4 );
                    else zoneStmt.BindInt64( 4, parent );
                    zoneStmt.BindInt( 5, depth );
                    zoneStmt.BindInt64( 6, static_cast<int64_t>( sibling ) );
                    zoneStmt.BindInt64( 7, event.CpuStart() );
                    zoneStmt.BindInt64( 8, event.CpuEnd() );
                    zoneStmt.BindInt64( 9, event.GpuStart() );
                    zoneStmt.BindInt64( 10, event.GpuEnd() );
                    zoneStmt.BindInt( 11, event.GpuEnd() >= 0 ? 1 : 0 );
                    zoneStmt.BindInt64( 12, event.SrcLoc() );
                    zoneStmt.BindInt64( 13, event.callstack.Val() );
                    zoneStmt.BindInt64( 14, event.Thread() );
                    BindU64( zoneStmt, 15, 16, eventThread );
                    zoneStmt.BindInt64( 17, event.query_id );
                    zoneStmt.BindInt( 18, hasChildren ? 1 : 0 );
                    zoneStmt.BindInt64( 19, event.Child() );
                    zoneStmt.StepDone();
                    ++zoneCount;

                    if( hasChildren )
                    {
                        const int32_t child = event.Child();
                        if( static_cast<size_t>( child ) >= m_worker.m_data.gpuChildren.size() )
                        {
                            Fail( "GPU zone child vector index is outside gpuChildren" );
                        }
                        exportTimeline( m_worker.m_data.gpuChildren[child], id, depth + 1 );
                    }
                };

                if( timeline.is_magic() )
                {
                    const auto& direct = *reinterpret_cast<const Vector<GpuEvent>*>( &timeline );
                    for( size_t i = 0; i < direct.size(); ++i ) exportEvent( direct[i], i );
                }
                else
                {
                    for( size_t i = 0; i < timeline.size(); ++i ) exportEvent( *timeline[i], i );
                }
            };
            exportTimeline( threadData.second.timeline, 0, 0 );
            ++contextThreadIndex;
        }

        for( const auto& query : context->notes )
        {
            sourceNoteCount += query.second.size();
            for( const auto& note : query.second )
            {
                noteStmt.BindInt64( 1, static_cast<int64_t>( contextIndex ) );
                noteStmt.BindInt64( 2, query.first );
                noteStmt.BindInt64( 3, note.first );
                BindDoubleExact( noteStmt, 4, 5, note.second );
                noteStmt.StepDone();
                ++noteCount;
            }
        }
    }

    if( declaredGpuCount != m_worker.m_data.gpuCnt ) Fail( "GPU context event counts do not match Worker::gpuCnt" );
    Audit( "gpu_contexts", "gpu_contexts", m_worker.m_data.gpuData.size(), contextCount );
    Audit( "gpu_context_note_names", "gpu_context_note_names", sourceNoteNameCount, noteNameCount );
    Audit( "gpu_context_threads", "gpu_context_threads", sourceContextThreadCount, contextThreadCount );
    Audit( "gpu_zones", "gpu_zones", m_worker.m_data.gpuCnt, zoneCount,
        "Every zone retains its 16-bit query id and incomplete GPU end state." );
    Audit( "gpu_notes", "gpu_notes", sourceNoteCount, noteCount );
}

void SqliteExporter::ExportPlots()
{
    Statement plotStmt( m_db->Handle(),
        "INSERT INTO plots(plot_id, name_u64, name_hex, name_text, plot_type, value_format, show_steps, "
        "fill, color, min_value, min_bits, max_value, max_bits, sum_value, sum_bits, "
        "running_min, running_min_bits, running_max, running_max_bits, running_num, running_num_bits, origin) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, "
        "?16, ?17, ?18, ?19, ?20, ?21, ?22)" );
    Statement sampleStmt( m_db->Handle(),
        "INSERT INTO plot_samples(plot_id, sample_index, time_ns, value, value_bits) VALUES(?1, ?2, ?3, ?4, ?5)" );

    const auto& plots = m_worker.m_data.plots.Data();
    m_plotId.reserve( plots.size() );
    uint64_t plotCount = 0;
    uint64_t sourceSampleCount = 0;
    uint64_t sampleCount = 0;
    for( size_t i = 0; i < plots.size(); ++i )
    {
        const auto* plot = plots[i];
        m_plotId.emplace( plot, static_cast<int64_t>( i ) );
        plotStmt.BindInt64( 1, static_cast<int64_t>( i ) );
        BindU64( plotStmt, 2, 3, plot->name );
        plotStmt.BindText( 4, plot->name == 0 ? std::string() : Utf8OrEmpty( m_worker.GetString( plot->name ) ) );
        plotStmt.BindInt( 5, static_cast<int>( plot->type ) );
        plotStmt.BindInt( 6, static_cast<int>( plot->format ) );
        plotStmt.BindInt( 7, plot->showSteps );
        plotStmt.BindInt( 8, plot->fill );
        plotStmt.BindInt64( 9, plot->color );
        BindDoubleExact( plotStmt, 10, 11, plot->min );
        BindDoubleExact( plotStmt, 12, 13, plot->max );
        BindDoubleExact( plotStmt, 14, 15, plot->sum );
        BindDoubleExact( plotStmt, 16, 17, plot->rMin );
        BindDoubleExact( plotStmt, 18, 19, plot->rMax );
        BindDoubleExact( plotStmt, 20, 21, plot->num );
        plotStmt.BindText( 22, plot->type == PlotType::Memory ? "derived_memory" : "recorded" );
        plotStmt.StepDone();
        ++plotCount;

        sourceSampleCount += plot->data.size();
        for( size_t j = 0; j < plot->data.size(); ++j )
        {
            sampleStmt.BindInt64( 1, static_cast<int64_t>( i ) );
            sampleStmt.BindInt64( 2, static_cast<int64_t>( j ) );
            sampleStmt.BindInt64( 3, plot->data[j].time.Val() );
            BindDoubleExact( sampleStmt, 4, 5, plot->data[j].val );
            sampleStmt.StepDone();
            ++sampleCount;
        }
    }
    Audit( "plots", "plots", plots.size(), plotCount,
        "Includes persisted user/system/power plots and reconstructed memory plots; PlotConfig fields are explicit columns." );
    Audit( "plot_samples", "plot_samples", sourceSampleCount, sampleCount );
}

void SqliteExporter::ExportMemory()
{
    Statement poolStmt( m_db->Handle(),
        "INSERT INTO memory_pools(memory_pool_id, map_key_u64, map_key_hex, stored_name_u64, "
        "stored_name_hex, name_text, high_u64, high_hex, low_u64, low_hex, usage_u64, usage_hex, "
        "reconstruct, plot_id) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14)" );
    Statement eventStmt( m_db->Handle(),
        "INSERT INTO memory_events(memory_pool_id, event_index, pointer_u64, pointer_hex, size_u64, size_hex, "
        "allocation_callstack_id, free_callstack_id, allocation_time_ns, free_time_ns, "
        "allocation_compressed_thread_id, allocation_thread_u64, allocation_thread_hex, "
        "free_compressed_thread_id, free_thread_u64, free_thread_hex, is_active) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17)" );
    Statement activeStmt( m_db->Handle(),
        "INSERT INTO memory_active(memory_pool_id, pointer_u64, pointer_hex, event_index) VALUES(?1, ?2, ?3, ?4)" );
    Statement freeStmt( m_db->Handle(),
        "INSERT INTO memory_frees(memory_pool_id, free_ordinal, event_index) VALUES(?1, ?2, ?3)" );

    uint64_t poolCount = 0;
    uint64_t eventCount = 0;
    uint64_t sourceEventCount = 0;
    uint64_t activeCount = 0;
    uint64_t sourceActiveCount = 0;
    uint64_t freeCount = 0;
    uint64_t sourceFreeCount = 0;
    size_t poolIndex = 0;
    for( const auto& entry : m_worker.m_data.memNameMap )
    {
        const auto& memory = *entry.second;
        poolStmt.BindInt64( 1, static_cast<int64_t>( poolIndex ) );
        BindU64( poolStmt, 2, 3, entry.first );
        BindU64( poolStmt, 4, 5, memory.name );
        poolStmt.BindText( 6, memory.name == 0 ? std::string() : Utf8OrEmpty( m_worker.GetString( memory.name ) ) );
        BindU64( poolStmt, 7, 8, memory.high );
        BindU64( poolStmt, 9, 10, memory.low );
        BindU64( poolStmt, 11, 12, memory.usage );
        poolStmt.BindInt( 13, memory.reconstruct ? 1 : 0 );
        if( memory.plot )
        {
            const auto plotIt = m_plotId.find( memory.plot );
            if( plotIt == m_plotId.end() ) Fail( "Memory pool plot does not resolve to plots" );
            poolStmt.BindInt64( 14, plotIt->second );
        }
        else
        {
            poolStmt.BindNull( 14 );
        }
        poolStmt.StepDone();
        ++poolCount;

        sourceEventCount += memory.data.size();
        for( size_t i = 0; i < memory.data.size(); ++i )
        {
            const auto& event = memory.data[i];
            const uint64_t allocationThread = DecompressLocalThread( event.ThreadAlloc() );
            const uint64_t freeThread = DecompressLocalThread( event.ThreadFree() );
            const bool active = event.TimeFree() < 0;
            eventStmt.BindInt64( 1, static_cast<int64_t>( poolIndex ) );
            eventStmt.BindInt64( 2, static_cast<int64_t>( i ) );
            BindU64( eventStmt, 3, 4, event.Ptr() );
            BindU64( eventStmt, 5, 6, event.Size() );
            eventStmt.BindInt64( 7, event.CsAlloc() );
            eventStmt.BindInt64( 8, event.csFree.Val() );
            eventStmt.BindInt64( 9, event.TimeAlloc() );
            eventStmt.BindInt64( 10, event.TimeFree() );
            eventStmt.BindInt64( 11, event.ThreadAlloc() );
            BindU64( eventStmt, 12, 13, allocationThread );
            eventStmt.BindInt64( 14, event.ThreadFree() );
            BindU64( eventStmt, 15, 16, freeThread );
            eventStmt.BindInt( 17, active ? 1 : 0 );
            eventStmt.StepDone();
            ++eventCount;
        }

        sourceActiveCount += memory.active.size();
        for( const auto& active : memory.active )
        {
            activeStmt.BindInt64( 1, static_cast<int64_t>( poolIndex ) );
            BindU64( activeStmt, 2, 3, active.first );
            activeStmt.BindInt64( 4, static_cast<int64_t>( active.second ) );
            activeStmt.StepDone();
            ++activeCount;
        }

        sourceFreeCount += memory.frees.size();
        for( size_t i = 0; i < memory.frees.size(); ++i )
        {
            freeStmt.BindInt64( 1, static_cast<int64_t>( poolIndex ) );
            freeStmt.BindInt64( 2, static_cast<int64_t>( i ) );
            freeStmt.BindInt64( 3, memory.frees[i] );
            freeStmt.StepDone();
            ++freeCount;
        }
        ++poolIndex;
    }
    Audit( "memory_pools", "memory_pools", m_worker.m_data.memNameMap.size(), poolCount );
    Audit( "memory_events", "memory_events", sourceEventCount, eventCount );
    Audit( "memory_active", "memory_active", sourceActiveCount, activeCount );
    Audit( "memory_frees", "memory_frees", sourceFreeCount, freeCount );
}

void SqliteExporter::ExportCallstacks()
{
    Statement callstackStmt( m_db->Handle(),
        "INSERT INTO callstacks(callstack_id, frame_count) VALUES(?1, ?2)" );
    Statement itemStmt( m_db->Handle(),
        "INSERT INTO callstack_items(callstack_id, frame_ordinal, frame_id_u64, frame_id_hex) "
        "VALUES(?1, ?2, ?3, ?4)" );
    uint64_t callstackCount = 0;
    uint64_t itemCount = 0;
    uint64_t sourceItemCount = 0;
    if( m_worker.m_data.callstackPayload.empty() ) Fail( "callstackPayload is missing its sentinel entry" );
    for( size_t i = 1; i < m_worker.m_data.callstackPayload.size(); ++i )
    {
        const auto* callstack = static_cast<const VarArray<CallstackFrameId>*>( m_worker.m_data.callstackPayload[i] );
        callstackStmt.BindInt64( 1, static_cast<int64_t>( i ) );
        callstackStmt.BindInt64( 2, callstack->size() );
        callstackStmt.StepDone();
        ++callstackCount;
        sourceItemCount += callstack->size();
        for( size_t j = 0; j < callstack->size(); ++j )
        {
            itemStmt.BindInt64( 1, static_cast<int64_t>( i ) );
            itemStmt.BindInt64( 2, static_cast<int64_t>( j ) );
            BindU64( itemStmt, 3, 4, ( *callstack )[j].data );
            itemStmt.StepDone();
            ++itemCount;
        }
    }
    Audit( "callstacks", "callstacks", m_worker.m_data.callstackPayload.size() - 1, callstackCount );
    Audit( "callstack_items", "callstack_items", sourceItemCount, itemCount );

    Statement definitionStmt( m_db->Handle(),
        "INSERT INTO callstack_frame_definitions(frame_id_u64, frame_id_hex, inline_frame_count, "
        "image_name_index, image_name_text) VALUES(?1, ?2, ?3, ?4, ?5)" );
    Statement entryStmt( m_db->Handle(),
        "INSERT INTO callstack_frame_entries(frame_id_hex, inline_ordinal, name_index, name_text, "
        "file_index, file_text, line, symbol_address_u64, symbol_address_hex) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)" );
    uint64_t definitionSource = 0;
    uint64_t definitionCount = 0;
    uint64_t frameEntrySource = 0;
    uint64_t frameEntryCount = 0;
    for( const auto& entry : m_worker.m_data.callstackFrameMap )
    {
        if( !entry.second ) continue;
        ++definitionSource;
        const auto* frameData = entry.second;
        BindU64( definitionStmt, 1, 2, entry.first.data );
        definitionStmt.BindInt( 3, frameData->size );
        BindOptionalStringIdx( definitionStmt, 4, 5, frameData->imageName );
        definitionStmt.StepDone();
        ++definitionCount;

        frameEntrySource += frameData->size;
        for( uint8_t i = 0; i < frameData->size; ++i )
        {
            const auto& frame = frameData->data[i];
            entryStmt.BindText( 1, U64Hex( entry.first.data ) );
            entryStmt.BindInt( 2, i );
            BindOptionalStringIdx( entryStmt, 3, 4, frame.name );
            BindOptionalStringIdx( entryStmt, 5, 6, frame.file );
            entryStmt.BindInt64( 7, frame.line );
            BindU64( entryStmt, 8, 9, frame.symAddr );
            entryStmt.StepDone();
            ++frameEntryCount;
        }
    }
    if( definitionSource != m_worker.m_data.callstackFrameMap.size() - m_worker.m_pendingCallstackFrames )
    {
        Fail( "Resolved callstack frame count does not match pending-frame accounting" );
    }
    Audit( "callstack_frame_definitions", "callstack_frame_definitions", definitionSource, definitionCount );
    Audit( "callstack_frame_entries", "callstack_frame_entries", frameEntrySource, frameEntryCount );

    Statement appStmt( m_db->Handle(),
        "INSERT INTO app_info(app_info_index, string_ref_u64, string_ref_hex, string_ref_active, "
        "string_ref_is_index, value_text) VALUES(?1, ?2, ?3, ?4, ?5, ?6)" );
    for( size_t i = 0; i < m_worker.m_data.appInfo.size(); ++i )
    {
        const auto& info = m_worker.m_data.appInfo[i];
        appStmt.BindInt64( 1, static_cast<int64_t>( i ) );
        BindU64( appStmt, 2, 3, info.str );
        appStmt.BindInt( 4, info.active ? 1 : 0 );
        appStmt.BindInt( 5, info.isidx ? 1 : 0 );
        appStmt.BindText( 6, StringRefText( info ) );
        appStmt.StepDone();
    }
    Audit( "app_info", "app_info", m_worker.m_data.appInfo.size(), m_worker.m_data.appInfo.size() );
}

void SqliteExporter::ExportFrameImages()
{
    const auto& dictionary = m_worker.m_data.exportFrameImageDictionary;
    const auto dictionaryHash = Sha256::Digest( dictionary.data(), dictionary.size() );
    Statement dictionaryStmt( m_db->Handle(),
        "INSERT INTO frame_image_dictionary(singleton, byte_count, sha256, data) VALUES(1, ?1, ?2, ?3)" );
    dictionaryStmt.BindInt64( 1, static_cast<int64_t>( dictionary.size() ) );
    dictionaryStmt.BindText( 2, Hex( dictionaryHash.data(), dictionaryHash.size() ) );
    dictionaryStmt.BindBlob( 3, dictionary.data(), dictionary.size() );
    dictionaryStmt.StepDone();
    Audit( "frame_image_dictionary", "frame_image_dictionary", 1, 1,
        "Original Zstd dictionary bytes from the capture." );

    Statement imageStmt( m_db->Handle(),
        "INSERT INTO frame_images(frame_image_index, width, height, flip, frame_reference, "
        "packed_byte_count, packed_sha256, packed_data, unpacked_byte_count, unpacked_sha256, unpacked_data) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)" );
    uint64_t imageCount = 0;
    for( size_t i = 0; i < m_worker.m_data.frameImage.size(); ++i )
    {
        const auto* image = static_cast<const FrameImage*>( m_worker.m_data.frameImage[i] );
        const auto* packed = static_cast<const char*>( image->ptr );
        const size_t packedSize = image->csz;
        const size_t unpackedSize = static_cast<size_t>( image->w ) * image->h / 2;
        const char* unpacked = m_worker.UnpackFrameImage( *image );
        const auto packedHash = Sha256::Digest( packed, packedSize );
        const auto unpackedHash = Sha256::Digest( unpacked, unpackedSize );

        imageStmt.BindInt64( 1, static_cast<int64_t>( i ) );
        imageStmt.BindInt( 2, image->w );
        imageStmt.BindInt( 3, image->h );
        imageStmt.BindInt( 4, image->flip );
        imageStmt.BindInt64( 5, image->frameRef );
        imageStmt.BindInt64( 6, static_cast<int64_t>( packedSize ) );
        imageStmt.BindText( 7, Hex( packedHash.data(), packedHash.size() ) );
        imageStmt.BindBlob( 8, packed, packedSize );
        imageStmt.BindInt64( 9, static_cast<int64_t>( unpackedSize ) );
        imageStmt.BindText( 10, Hex( unpackedHash.data(), unpackedHash.size() ) );
        imageStmt.BindBlob( 11, unpacked, unpackedSize );
        imageStmt.StepDone();
        ++imageCount;
    }
    Audit( "frame_images", "frame_images", m_worker.m_data.frameImage.size(), imageCount,
        "Stores both Worker's recompressed representation and exact unpacked YCoCg frame bytes." );
}

void SqliteExporter::ExportContextSwitches()
{
    Statement threadStmt( m_db->Handle(),
        "INSERT INTO context_switch_threads(thread_u64, thread_hex, event_count, running_time_ns, "
        "pending_wakeup_ns, pending_wakeup_cpu) VALUES(?1, ?2, ?3, ?4, ?5, ?6)" );
    Statement switchStmt( m_db->Handle(),
        "INSERT INTO context_switches(thread_u64, thread_hex, switch_index, wakeup_ns, start_ns, end_ns, "
        "end_valid, cpu, wakeup_cpu, reason, state, related_compressed_thread_id, "
        "related_thread_u64, related_thread_hex) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14)" );
    uint64_t switchThreadCount = 0;
    uint64_t sourceSwitchCount = 0;
    uint64_t switchCount = 0;
    for( const auto& entry : m_worker.m_data.ctxSwitch )
    {
        const auto& context = *entry.second;
        BindU64( threadStmt, 1, 2, entry.first );
        threadStmt.BindInt64( 3, static_cast<int64_t>( context.v.size() ) );
        threadStmt.BindInt64( 4, context.runningTime );
        threadStmt.BindInt64( 5, context.pendingWakeUp.time );
        threadStmt.BindInt( 6, context.pendingWakeUp.cpu );
        threadStmt.StepDone();
        ++switchThreadCount;

        sourceSwitchCount += context.v.size();
        for( size_t i = 0; i < context.v.size(); ++i )
        {
            const auto& item = context.v[i];
            const uint64_t related = DecompressLocalThread( item.Thread() );
            BindU64( switchStmt, 1, 2, entry.first );
            switchStmt.BindInt64( 3, static_cast<int64_t>( i ) );
            switchStmt.BindInt64( 4, item.WakeupVal() );
            switchStmt.BindInt64( 5, item.Start() );
            switchStmt.BindInt64( 6, item.End() );
            switchStmt.BindInt( 7, item.IsEndValid() ? 1 : 0 );
            switchStmt.BindInt( 8, item.Cpu() );
            switchStmt.BindInt( 9, item.WakeupCpu() );
            switchStmt.BindInt( 10, static_cast<int>( item.Reason() ) );
            switchStmt.BindInt( 11, item.State() );
            switchStmt.BindInt( 12, item.Thread() );
            BindU64( switchStmt, 13, 14, related );
            switchStmt.StepDone();
            ++switchCount;
        }
    }
    Audit( "context_switch_threads", "context_switch_threads", m_worker.m_data.ctxSwitch.size(), switchThreadCount );
    Audit( "context_switches", "context_switches", sourceSwitchCount, switchCount );

    Statement cpuStmt( m_db->Handle(),
        "INSERT INTO cpu_context_switches(cpu, switch_index, start_ns, end_ns, end_valid, "
        "compressed_thread_id, thread_u64, thread_hex) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)" );
    uint64_t sourceCpuSwitches = 0;
    uint64_t cpuSwitches = 0;
    for( int cpu = 0; cpu < 256; ++cpu )
    {
        const auto& list = m_worker.m_data.cpuData[cpu].cs;
        sourceCpuSwitches += list.size();
        for( size_t i = 0; i < list.size(); ++i )
        {
            const auto& item = list[i];
            const uint64_t thread = DecompressExternalThread( item.Thread() );
            cpuStmt.BindInt( 1, cpu );
            cpuStmt.BindInt64( 2, static_cast<int64_t>( i ) );
            cpuStmt.BindInt64( 3, item.Start() );
            cpuStmt.BindInt64( 4, item.End() );
            cpuStmt.BindInt( 5, item.IsEndValid() ? 1 : 0 );
            cpuStmt.BindInt( 6, item.Thread() );
            BindU64( cpuStmt, 7, 8, thread );
            cpuStmt.StepDone();
            ++cpuSwitches;
        }
    }
    InsertMeta( "cpu_data_count", std::to_string( m_worker.m_data.cpuDataCount ) );
    Audit( "cpu_context_switches", "cpu_context_switches", sourceCpuSwitches, cpuSwitches );

    Statement pidStmt( m_db->Handle(),
        "INSERT INTO tid_pid(tid_u64, tid_hex, pid_u64, pid_hex) VALUES(?1, ?2, ?3, ?4)" );
    uint64_t pidCount = 0;
    for( const auto& entry : m_worker.m_data.tidToPid )
    {
        BindU64( pidStmt, 1, 2, entry.first );
        BindU64( pidStmt, 3, 4, entry.second );
        pidStmt.StepDone();
        ++pidCount;
    }
    Audit( "tid_pid", "tid_pid", m_worker.m_data.tidToPid.size(), pidCount );

    Statement statStmt( m_db->Handle(),
        "INSERT INTO cpu_thread_stats(tid_u64, tid_hex, running_time_ns, running_regions, migrations) "
        "VALUES(?1, ?2, ?3, ?4, ?5)" );
    uint64_t statCount = 0;
    for( const auto& entry : m_worker.m_data.cpuThreadData )
    {
        BindU64( statStmt, 1, 2, entry.first );
        statStmt.BindInt64( 3, entry.second.runningTime );
        statStmt.BindInt64( 4, entry.second.runningRegions );
        statStmt.BindInt64( 5, entry.second.migrations );
        statStmt.StepDone();
        ++statCount;
    }
    Audit( "cpu_thread_stats", "cpu_thread_stats", m_worker.m_data.cpuThreadData.size(), statCount );
}

void SqliteExporter::ExportSymbolsAndSamples()
{
    Statement symbolStmt( m_db->Handle(),
        "INSERT INTO symbols(symbol_address_u64, symbol_address_hex, name_index, name_text, file_index, "
        "file_text, line, image_name_index, image_name_text, call_file_index, call_file_text, call_line, "
        "is_inline, size, raw_payload) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15)" );
    uint64_t symbolCount = 0;
    for( const auto& entry : m_worker.m_data.symbolMap )
    {
        BindU64( symbolStmt, 1, 2, entry.first );
        BindOptionalStringIdx( symbolStmt, 3, 4, entry.second.name );
        BindOptionalStringIdx( symbolStmt, 5, 6, entry.second.file );
        symbolStmt.BindInt64( 7, entry.second.line );
        BindOptionalStringIdx( symbolStmt, 8, 9, entry.second.imageName );
        BindOptionalStringIdx( symbolStmt, 10, 11, entry.second.callFile );
        symbolStmt.BindInt64( 12, entry.second.callLine );
        symbolStmt.BindInt( 13, entry.second.isInline );
        symbolStmt.BindInt64( 14, entry.second.size.Val() );
        symbolStmt.BindBlob( 15, &entry.second, sizeof( entry.second ) );
        symbolStmt.StepDone();
        ++symbolCount;
    }
    Audit( "symbols", "symbols", m_worker.m_data.symbolMap.size(), symbolCount );

    Statement locationStmt( m_db->Handle(),
        "INSERT INTO symbol_locations(location_kind, location_index, symbol_address_u64, "
        "symbol_address_hex, size) VALUES(?1, ?2, ?3, ?4, ?5)" );
    uint64_t locationCount = 0;
    for( size_t i = 0; i < m_worker.m_data.symbolLoc.size(); ++i )
    {
        locationStmt.BindText( 1, "regular" );
        locationStmt.BindInt64( 2, static_cast<int64_t>( i ) );
        BindU64( locationStmt, 3, 4, m_worker.m_data.symbolLoc[i].addr );
        locationStmt.BindInt64( 5, m_worker.m_data.symbolLoc[i].len );
        locationStmt.StepDone();
        ++locationCount;
    }
    for( size_t i = 0; i < m_worker.m_data.symbolLocInline.size(); ++i )
    {
        locationStmt.BindText( 1, "inline" );
        locationStmt.BindInt64( 2, static_cast<int64_t>( i ) );
        BindU64( locationStmt, 3, 4, m_worker.m_data.symbolLocInline[i] );
        locationStmt.BindNull( 5 );
        locationStmt.StepDone();
        ++locationCount;
    }
    const uint64_t sourceLocationCount =
        m_worker.m_data.symbolLoc.size() + m_worker.m_data.symbolLocInline.size();
    Audit( "symbol_locations", "symbol_locations", sourceLocationCount, locationCount );

    Statement codeStmt( m_db->Handle(),
        "INSERT INTO symbol_code(symbol_address_u64, symbol_address_hex, byte_count, sha256, data) "
        "VALUES(?1, ?2, ?3, ?4, ?5)" );
    uint64_t codeCount = 0;
    uint64_t codeBytes = 0;
    for( const auto& entry : m_worker.m_data.symbolCode )
    {
        const auto digest = Sha256::Digest( entry.second.data, entry.second.len );
        BindU64( codeStmt, 1, 2, entry.first );
        codeStmt.BindInt64( 3, entry.second.len );
        codeStmt.BindText( 4, Hex( digest.data(), digest.size() ) );
        codeStmt.BindBlob( 5, entry.second.data, entry.second.len );
        codeStmt.StepDone();
        codeBytes += entry.second.len;
        ++codeCount;
    }
    if( codeBytes != m_worker.m_data.symbolCodeSize ) Fail( "Symbol code byte total does not match Worker accounting" );
    InsertMeta( "symbol_code_bytes", std::to_string( codeBytes ) );
    Audit( "symbol_code", "symbol_code", m_worker.m_data.symbolCode.size(), codeCount );

    Statement codeMapStmt( m_db->Handle(),
        "INSERT INTO code_symbol_map(code_address_u64, code_address_hex, symbol_address_u64, "
        "symbol_address_hex) VALUES(?1, ?2, ?3, ?4)" );
    uint64_t codeMapCount = 0;
    for( const auto& entry : m_worker.m_data.codeSymbolMap )
    {
        BindU64( codeMapStmt, 1, 2, entry.first );
        BindU64( codeMapStmt, 3, 4, entry.second );
        codeMapStmt.StepDone();
        ++codeMapCount;
    }
    Audit( "code_symbol_map", "code_symbol_map", m_worker.m_data.codeSymbolMap.size(), codeMapCount );

    Statement hardwareStmt( m_db->Handle(),
        "INSERT INTO hardware_samples(address_u64, address_hex, sample_kind, sample_index, time_ns) "
        "VALUES(?1, ?2, ?3, ?4, ?5)" );
    uint64_t sourceHardwareCount = 0;
    uint64_t hardwareCount = 0;
    for( const auto& entry : m_worker.m_data.hwSamples )
    {
        const auto exportVector = [&]( const char* kind, const auto& samples )
        {
            sourceHardwareCount += samples.size();
            for( size_t i = 0; i < samples.size(); ++i )
            {
                BindU64( hardwareStmt, 1, 2, entry.first );
                hardwareStmt.BindText( 3, kind );
                hardwareStmt.BindInt64( 4, static_cast<int64_t>( i ) );
                hardwareStmt.BindInt64( 5, samples[i].Val() );
                hardwareStmt.StepDone();
                ++hardwareCount;
            }
        };
        exportVector( "cycles", entry.second.cycles );
        exportVector( "retired", entry.second.retired );
        exportVector( "cache_reference", entry.second.cacheRef );
        exportVector( "cache_miss", entry.second.cacheMiss );
        exportVector( "branch_retired", entry.second.branchRetired );
        exportVector( "branch_miss", entry.second.branchMiss );
    }
    InsertMeta( "has_branch_retirement", m_worker.m_data.hasBranchRetirement ? "1" : "0" );
    Audit( "hardware_samples", "hardware_samples", sourceHardwareCount, hardwareCount );

    Statement sourceStmt( m_db->Handle(),
        "INSERT INTO source_cache(source_path_blob, source_path_text, byte_count, sha256, data) "
        "VALUES(?1, ?2, ?3, ?4, ?5)" );
    uint64_t sourceCacheCount = 0;
    for( const auto& entry : m_worker.m_data.sourceFileCache )
    {
        const size_t pathSize = strlen( entry.first );
        const auto digest = Sha256::Digest( entry.second.data, entry.second.len );
        sourceStmt.BindBlob( 1, entry.first, pathSize );
        sourceStmt.BindText( 2, std::string( entry.first, pathSize ) );
        sourceStmt.BindInt64( 3, entry.second.len );
        sourceStmt.BindText( 4, Hex( digest.data(), digest.size() ) );
        sourceStmt.BindBlob( 5, entry.second.data, entry.second.len );
        sourceStmt.StepDone();
        ++sourceCacheCount;
    }
    Audit( "source_cache", "source_cache", m_worker.m_data.sourceFileCache.size(), sourceCacheCount );
}

void SqliteExporter::ExportDerivedData()
{
    InsertMeta( "derived_source_location_zones_ready", m_worker.m_data.sourceLocationZonesReady ? "1" : "0" );
    InsertMeta( "derived_gpu_source_location_zones_ready", m_worker.m_data.gpuSourceLocationZonesReady ? "1" : "0" );
    InsertMeta( "derived_cpu_usage_ready", m_worker.m_data.ctxUsageReady ? "1" : "0" );
    InsertMeta( "derived_callstack_samples_ready", m_worker.m_data.callstackSamplesReady ? "1" : "0" );
    InsertMeta( "derived_ghost_zones_ready", m_worker.m_data.ghostZonesReady ? "1" : "0" );
    InsertMeta( "derived_symbol_samples_ready", m_worker.m_data.symbolSamplesReady ? "1" : "0" );

    Statement usageStmt( m_db->Handle(),
        "INSERT INTO derived_cpu_usage(usage_index, time_ns, other_cpu_count, own_cpu_count) "
        "VALUES(?1, ?2, ?3, ?4)" );
    for( size_t i = 0; i < m_worker.m_data.ctxUsage.size(); ++i )
    {
        usageStmt.BindInt64( 1, static_cast<int64_t>( i ) );
        usageStmt.BindInt64( 2, m_worker.m_data.ctxUsage[i].Time() );
        usageStmt.BindInt( 3, m_worker.m_data.ctxUsage[i].Other() );
        usageStmt.BindInt( 4, m_worker.m_data.ctxUsage[i].Own() );
        usageStmt.StepDone();
    }
    Audit( "derived_cpu_usage", "derived_cpu_usage",
        m_worker.m_data.ctxUsage.size(), m_worker.m_data.ctxUsage.size() );

    Statement zoneStatStmt( m_db->Handle(),
        "INSERT INTO derived_zone_statistics(domain, source_location_id, event_count, min_ns, max_ns, "
        "total_ns, sum_sq, sum_sq_bits, self_min_ns, self_max_ns, self_total_ns, non_reentrant_count, "
        "non_reentrant_min_ns, non_reentrant_max_ns, non_reentrant_total_ns) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15)" );
    Statement zoneThreadStmt( m_db->Handle(),
        "INSERT INTO derived_zone_statistics_threads(source_location_id, compressed_thread_id, event_count) "
        "VALUES(?1, ?2, ?3)" );
    uint64_t zoneStatCount = 0;
    uint64_t zoneThreadSource = 0;
    uint64_t zoneThreadCount = 0;
    for( const auto& entry : m_worker.m_data.sourceLocationZones )
    {
        const auto& stats = entry.second;
        zoneStatStmt.BindText( 1, "cpu" );
        zoneStatStmt.BindInt64( 2, entry.first );
        zoneStatStmt.BindInt64( 3, static_cast<int64_t>( stats.zones.size() ) );
        zoneStatStmt.BindInt64( 4, stats.min );
        zoneStatStmt.BindInt64( 5, stats.max );
        zoneStatStmt.BindInt64( 6, stats.total );
        BindDoubleExact( zoneStatStmt, 7, 8, stats.sumSq );
        zoneStatStmt.BindInt64( 9, stats.selfMin );
        zoneStatStmt.BindInt64( 10, stats.selfMax );
        zoneStatStmt.BindInt64( 11, stats.selfTotal );
        zoneStatStmt.BindInt64( 12, static_cast<int64_t>( stats.nonReentrantCount ) );
        zoneStatStmt.BindInt64( 13, stats.nonReentrantMin );
        zoneStatStmt.BindInt64( 14, stats.nonReentrantMax );
        zoneStatStmt.BindInt64( 15, stats.nonReentrantTotal );
        zoneStatStmt.StepDone();
        ++zoneStatCount;

        zoneThreadSource += stats.threadCnt.size();
        for( const auto& thread : stats.threadCnt )
        {
            zoneThreadStmt.BindInt64( 1, entry.first );
            zoneThreadStmt.BindInt64( 2, thread.first );
            zoneThreadStmt.BindInt64( 3, static_cast<int64_t>( thread.second ) );
            zoneThreadStmt.StepDone();
            ++zoneThreadCount;
        }
    }
    for( const auto& entry : m_worker.m_data.gpuSourceLocationZones )
    {
        const auto& stats = entry.second;
        zoneStatStmt.BindText( 1, "gpu" );
        zoneStatStmt.BindInt64( 2, entry.first );
        zoneStatStmt.BindInt64( 3, static_cast<int64_t>( stats.zones.size() ) );
        zoneStatStmt.BindInt64( 4, stats.min );
        zoneStatStmt.BindInt64( 5, stats.max );
        zoneStatStmt.BindInt64( 6, stats.total );
        BindDoubleExact( zoneStatStmt, 7, 8, stats.sumSq );
        for( int i = 9; i <= 15; ++i ) zoneStatStmt.BindNull( i );
        zoneStatStmt.StepDone();
        ++zoneStatCount;
    }
    const uint64_t zoneStatSource =
        m_worker.m_data.sourceLocationZones.size() + m_worker.m_data.gpuSourceLocationZones.size();
    Audit( "derived_zone_statistics", "derived_zone_statistics", zoneStatSource, zoneStatCount );
    Audit( "derived_zone_statistics_threads", "derived_zone_statistics_threads", zoneThreadSource, zoneThreadCount );

    Statement symbolStatStmt( m_db->Handle(),
        "INSERT INTO derived_symbol_statistics(symbol_address_u64, symbol_address_hex, inclusive_count, "
        "exclusive_count) VALUES(?1, ?2, ?3, ?4)" );
    Statement symbolParentStmt( m_db->Handle(),
        "INSERT INTO derived_symbol_stat_parents(symbol_address_hex, parent_kind, parent_index, event_count) "
        "VALUES(?1, ?2, ?3, ?4)" );
    uint64_t symbolStatCount = 0;
    uint64_t symbolParentSource = 0;
    uint64_t symbolParentCount = 0;
    for( const auto& entry : m_worker.m_data.symbolStats )
    {
        BindU64( symbolStatStmt, 1, 2, entry.first );
        symbolStatStmt.BindInt64( 3, entry.second.incl );
        symbolStatStmt.BindInt64( 4, entry.second.excl );
        symbolStatStmt.StepDone();
        ++symbolStatCount;

        const auto exportParents = [&]( const char* kind, const auto& parents )
        {
            symbolParentSource += parents.size();
            for( const auto& parent : parents )
            {
                symbolParentStmt.BindText( 1, U64Hex( entry.first ) );
                symbolParentStmt.BindText( 2, kind );
                symbolParentStmt.BindInt64( 3, parent.first );
                symbolParentStmt.BindInt64( 4, parent.second );
                symbolParentStmt.StepDone();
                ++symbolParentCount;
            }
        };
        exportParents( "callstack", entry.second.parents );
        exportParents( "base_callstack", entry.second.baseParents );
    }
    Audit( "derived_symbol_statistics", "derived_symbol_statistics",
        m_worker.m_data.symbolStats.size(), symbolStatCount );
    Audit( "derived_symbol_stat_parents", "derived_symbol_stat_parents",
        symbolParentSource, symbolParentCount );

    Statement symbolSampleStmt( m_db->Handle(),
        "INSERT INTO derived_symbol_samples(symbol_address_u64, symbol_address_hex, sample_index, time_ns, "
        "compressed_thread_id, thread_u64, thread_hex, instruction_pointer_u64, instruction_pointer_hex) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)" );
    uint64_t symbolSampleSource = 0;
    uint64_t symbolSampleCount = 0;
    for( const auto& entry : m_worker.m_data.symbolSamples )
    {
        symbolSampleSource += entry.second.size();
        for( size_t i = 0; i < entry.second.size(); ++i )
        {
            const auto& sample = entry.second[i];
            BindU64( symbolSampleStmt, 1, 2, entry.first );
            symbolSampleStmt.BindInt64( 3, static_cast<int64_t>( i ) );
            symbolSampleStmt.BindInt64( 4, sample.time.Val() );
            symbolSampleStmt.BindInt( 5, sample.thread );
            BindU64( symbolSampleStmt, 6, 7, DecompressLocalThread( sample.thread ) );
            BindU64( symbolSampleStmt, 8, 9, sample.ip.data );
            symbolSampleStmt.StepDone();
            ++symbolSampleCount;
        }
    }
    Audit( "derived_symbol_samples", "derived_symbol_samples", symbolSampleSource, symbolSampleCount );

    Statement childStmt( m_db->Handle(),
        "INSERT INTO derived_child_samples(parent_address_u64, parent_address_hex, sample_index, time_ns, "
        "child_address_u64, child_address_hex) VALUES(?1, ?2, ?3, ?4, ?5, ?6)" );
    uint64_t childSource = 0;
    uint64_t childCount = 0;
    for( const auto& entry : m_worker.m_data.childSamples )
    {
        childSource += entry.second.size();
        for( size_t i = 0; i < entry.second.size(); ++i )
        {
            BindU64( childStmt, 1, 2, entry.first );
            childStmt.BindInt64( 3, static_cast<int64_t>( i ) );
            childStmt.BindInt64( 4, entry.second[i].time.Val() );
            BindU64( childStmt, 5, 6, entry.second[i].addr );
            childStmt.StepDone();
            ++childCount;
        }
    }
    Audit( "derived_child_samples", "derived_child_samples", childSource, childCount );

    Statement ipStmt( m_db->Handle(),
        "INSERT INTO derived_instruction_pointers(symbol_address_u64, symbol_address_hex, frame_id_u64, "
        "frame_id_hex, event_count) VALUES(?1, ?2, ?3, ?4, ?5)" );
    uint64_t ipSource = 0;
    uint64_t ipCount = 0;
    for( const auto& symbol : m_worker.m_data.instructionPointersMap )
    {
        ipSource += symbol.second.size();
        for( const auto& ip : symbol.second )
        {
            BindU64( ipStmt, 1, 2, symbol.first );
            BindU64( ipStmt, 3, 4, ip.first.data );
            ipStmt.BindInt64( 5, ip.second );
            ipStmt.StepDone();
            ++ipCount;
        }
    }
    Audit( "derived_instruction_pointers", "derived_instruction_pointers", ipSource, ipCount );

    Statement fiberStmt( m_db->Handle(),
        "INSERT INTO fiber_thread_map(fiber_u64, fiber_hex, thread_u64, thread_hex) VALUES(?1, ?2, ?3, ?4)" );
    uint64_t fiberCount = 0;
    for( const auto& entry : m_worker.m_data.fiberToThreadMap )
    {
        BindU64( fiberStmt, 1, 2, entry.first );
        BindU64( fiberStmt, 3, 4, entry.second );
        fiberStmt.StepDone();
        ++fiberCount;
    }
    Audit( "fiber_thread_map", "fiber_thread_map", m_worker.m_data.fiberToThreadMap.size(), fiberCount );
}

void SqliteExporter::ValidateAndPublish()
{
    m_db->Exec( R"SQL(
CREATE INDEX idx_frames_start ON frames(frame_set_id, start_ns);
CREATE INDEX idx_messages_time ON messages(time_ns);
CREATE INDEX idx_messages_thread ON messages(thread_hex, time_ns);
CREATE INDEX idx_cpu_zones_thread_time ON cpu_zones(thread_index, start_ns);
CREATE INDEX idx_cpu_zones_source_time ON cpu_zones(source_location_id, start_ns);
CREATE INDEX idx_cpu_zones_parent ON cpu_zones(parent_zone_id);
CREATE INDEX idx_thread_samples_time ON thread_samples(time_ns);
CREATE INDEX idx_gpu_zones_context_time ON gpu_zones(gpu_context_id, context_thread_index, gpu_start_ns);
CREATE INDEX idx_gpu_zones_source_time ON gpu_zones(source_location_id, gpu_start_ns);
CREATE INDEX idx_gpu_zones_parent ON gpu_zones(parent_gpu_zone_id);
CREATE INDEX idx_gpu_zones_query ON gpu_zones(gpu_context_id, query_id);
CREATE INDEX idx_plot_samples_time ON plot_samples(plot_id, time_ns);
CREATE INDEX idx_memory_events_pointer ON memory_events(memory_pool_id, pointer_hex);
CREATE INDEX idx_memory_events_alloc_time ON memory_events(memory_pool_id, allocation_time_ns);
CREATE INDEX idx_memory_events_free_time ON memory_events(memory_pool_id, free_time_ns);
CREATE INDEX idx_context_switches_time ON context_switches(thread_hex, start_ns);
CREATE INDEX idx_cpu_context_switches_time ON cpu_context_switches(cpu, start_ns);
CREATE INDEX idx_hardware_samples_time ON hardware_samples(sample_kind, time_ns);
CREATE INDEX idx_derived_symbol_samples_time ON derived_symbol_samples(time_ns);
)SQL" );

    if( m_db->ScalarInt64( "SELECT count(*) FROM export_audit WHERE status <> 'PASS'" ) != 0 )
    {
        Fail( "One or more export audit rows are not PASS" );
    }

    Sha256 reconstructed;
    uint64_t expectedChunk = 0;
    uint64_t expectedOffset = 0;
    {
        Statement chunks( m_db->Handle(),
            "SELECT chunk_index, byte_offset, byte_count, sha256, data FROM capture_chunks ORDER BY chunk_index" );
        while( chunks.StepRow() )
        {
            const uint64_t chunkIndex = static_cast<uint64_t>( chunks.ColumnInt64( 0 ) );
            const uint64_t offset = static_cast<uint64_t>( chunks.ColumnInt64( 1 ) );
            const uint64_t declaredCount = static_cast<uint64_t>( chunks.ColumnInt64( 2 ) );
            const auto* declaredHashText = chunks.ColumnText( 3 );
            const void* data = chunks.ColumnBlob( 4 );
            const int bytes = chunks.ColumnBytes( 4 );
            if( chunkIndex != expectedChunk || offset != expectedOffset || bytes < 0
                || declaredCount != static_cast<uint64_t>( bytes ) )
            {
                Fail( "capture_chunks sequence, offset, or byte count validation failed" );
            }
            const auto digest = Sha256::Digest( data, static_cast<size_t>( bytes ) );
            const std::string digestText = Hex( digest.data(), digest.size() );
            if( !declaredHashText || digestText != reinterpret_cast<const char*>( declaredHashText ) )
            {
                Fail( "A capture_chunks per-chunk SHA-256 validation failed" );
            }
            reconstructed.Update( data, static_cast<size_t>( bytes ) );
            expectedOffset += static_cast<uint64_t>( bytes );
            ++expectedChunk;
        }
    }
    const auto reconstructedHash = reconstructed.Finish();
    if( expectedOffset != m_frozenInput.size || reconstructedHash != m_frozenInput.sha256 )
    {
        Fail( "Reconstructed capture SHA-256 does not match the frozen input" );
    }
    if( m_db->ScalarText( "SELECT value FROM meta WHERE key='input_sha256'" )
        != Hex( m_frozenInput.sha256.data(), m_frozenInput.sha256.size() ) )
    {
        Fail( "meta.input_sha256 does not match the reconstructed capture" );
    }

    const auto finalInput = DigestFile( m_input );
    if( finalInput.size != m_frozenInput.size || finalInput.sha256 != m_frozenInput.sha256 )
    {
        Fail( "Input trace changed while semantic sections were being exported" );
    }

    {
        Statement foreignKeys( m_db->Handle(), "PRAGMA foreign_key_check" );
        if( foreignKeys.StepRow() ) Fail( "PRAGMA foreign_key_check reported a violation" );
    }
    if( m_db->ScalarText( "PRAGMA integrity_check" ) != "ok" )
    {
        Fail( "PRAGMA integrity_check did not return ok" );
    }

    InsertMeta( "raw_capture_reconstruction_validated", "1" );
    InsertMeta( "input_unchanged_through_export", "1" );
    InsertMeta( "foreign_key_check", "ok" );
    InsertMeta( "integrity_check", "ok" );
    InsertMeta( "output_path", m_output.string() );
    m_db->Exec( "UPDATE meta SET value='1' WHERE key='export_complete'" );
    m_db->Exec( "COMMIT" );
    m_db->Close();
    std::error_code renameError;
    for( int attempt = 0; attempt < 50; ++attempt )
    {
        fs::rename( m_partial, m_output, renameError );
        if( !renameError ) return;
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    }
    Fail( "Cannot publish completed SQLite database: " + renameError.message() );
}

void SqliteExporter::Run()
{
    m_partial = m_output;
    m_partial += ".partial";
    if( fs::exists( m_partial ) ) Fail( "Partial output already exists: " + m_partial.string() );
    if( fs::exists( m_output ) ) Fail( "Output already exists: " + m_output.string() );

    m_db = std::make_unique<Database>( m_partial );
    CreateSchema();
    m_db->Exec( "BEGIN IMMEDIATE" );
    InsertMeta( "exporter_schema_version", "1" );
    InsertMeta( "tracy_version", "0.13.1" );
    InsertMeta( "tracy_source_commit", "05cceee0df3b8d7c6fa87e9638af311dbabc63cb" );
    InsertMeta( "export_complete", "0" );

    const auto phase = []( const char* name )
    {
        std::cerr << "[sqliteexport] " << name << std::endl;
    };
    phase( "raw capture bytes" );
    ExportRawCapture();
    phase( "capture metadata" );
    ExportMetadata();
    phase( "strings and identities" );
    ExportStrings();
    phase( "frame sets" );
    ExportFrames();
    phase( "source locations" );
    ExportSourceLocations();
    phase( "locks and zone extras" );
    ExportLocks();
    phase( "threads, messages, and CPU zones" );
    ExportMessagesAndThreads();
    phase( "GPU contexts, zones, queries, and notes" );
    ExportGpu();
    phase( "plots and samples" );
    ExportPlots();
    phase( "memory pools and events" );
    ExportMemory();
    phase( "callstacks and app info" );
    ExportCallstacks();
    phase( "frame images" );
    ExportFrameImages();
    phase( "context switches" );
    ExportContextSwitches();
    phase( "symbols, code, hardware samples, and source cache" );
    ExportSymbolsAndSamples();
    phase( "derived analysis data" );
    ExportDerivedData();
    phase( "indexes, byte reconstruction, integrity checks, and publish" );
    ValidateAndPublish();
    std::cerr << "[sqliteexport] complete: " << m_output.string() << std::endl;
}

} // namespace tracy

int main( int argc, char** argv )
{
    if( argc != 3 )
    {
        fprintf( stderr, "Usage: tracy-sqliteexport <input.tracy> <output.sqlite>\n" );
        return 2;
    }

    try
    {
        const fs::path input = fs::absolute( fs::path( argv[1] ) );
        const fs::path output = fs::absolute( fs::path( argv[2] ) );

        if( !fs::is_regular_file( input ) ) Fail( "Input trace does not exist: " + input.string() );
        if( fs::exists( output ) ) Fail( "Output already exists: " + output.string() );

        std::cerr << "[sqliteexport] freezing input SHA-256" << std::endl;
        const auto frozenInput = DigestFile( input );
        std::cerr << "[sqliteexport] loading Tracy 0.13.1 capture" << std::endl;
        auto file = std::unique_ptr<tracy::FileRead>( tracy::FileRead::Open( input.string().c_str() ) );
        if( !file ) Fail( "Tracy FileRead could not open input: " + input.string() );

        tracy::Worker worker( *file, tracy::EventType::All, true, false );
        if( worker.GetTraceVersion() != ( ( 0 << 16 ) | ( 13 << 8 ) | 1 ) )
        {
            Fail( "Strict exporter only accepts Tracy trace version 0.13.1" );
        }
        while( !worker.IsBackgroundDone() )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
        }
        worker.DoPostponedWorkAll();

        tracy::SqliteExporter exporter( worker, input, output, frozenInput );
        exporter.Run();
        return 0;
    }
    catch( const std::exception& e )
    {
        fprintf( stderr, "ERROR: %s\n", e.what() );
        return 1;
    }
}
