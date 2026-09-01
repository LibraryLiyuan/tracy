#include "TracyTraceSessionPlots.hpp"

#include "TracyHash.hpp"
#include "TracyTraceSessionCanonical.hpp"
#include "TracyTraceSessionGpuCanonical.hpp"
#include "TracyProtocol.hpp"
#include "TracyQueue.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#  include <Windows.h>
#endif

namespace tracy::analysis
{
namespace
{

constexpr uint64_t PlotFileMagic = 0x31544c50534e4aull;     // JNSPLT1
constexpr uint64_t PlotManifestMagic = 0x31464d50534e4aull; // JNPMF1
constexpr const char* PlotFileName = "plots.bin";

#pragma pack( push, 1 )
struct PlotFileHeader
{
    uint64_t magic = PlotFileMagic;
    uint32_t schema = TraceSessionPlotIndexSchemaVersion;
    uint32_t endian = 0x01020304;
    uint64_t sourceSize = 0;
    uint64_t plotCount = 0;
    uint64_t pointCount = 0;
    uint64_t dataEvents = 0;
    uint64_t configEvents = 0;
    uint64_t nameEvents = 0;
    uint64_t plotsOffset = 0;
    uint64_t stringsOffset = 0;
    uint64_t pointsOffset = 0;
    uint32_t generationBytes = 0;
    uint32_t reserved = 0;
};

struct StoredPlot
{
    uint64_t nativeName = 0;
    uint64_t nameOffset = 0;
    uint64_t pointBegin = 0;
    uint64_t pointCount = 0;
    double minimum = 0;
    double maximum = 0;
    double sum = 0;
    uint32_t nameBytes = 0;
    uint32_t color = 0;
    uint8_t type = 0;
    uint8_t format = 0;
    uint8_t showSteps = 0;
    uint8_t fill = 1;
};

struct StoredPoint
{
    uint64_t plotIndex = 0;
    uint64_t pointIndex = 0;
    int64_t timeNs = 0;
    double value = 0;
};
#pragma pack( pop )

struct PlotManifest
{
    std::string sourceSha256;
    uint64_t sourceSize = 0;
    std::string generation;
    uint64_t fileBytes = 0;
    std::string fileSha256;
    TraceSessionPlotStats stats;
};

struct PlotRuntime
{
    StoredPlot stored;
    std::string name;
    std::filesystem::path workPath;
    std::unique_ptr<std::ofstream> work;
    uint64_t lastUse = 0;
};

struct BuildState
{
    std::filesystem::path root;
    TraceSessionTimeTransform transform;
    int64_t refTimeThread = 0;
    std::unordered_map<uint64_t, size_t> byName;
    std::unordered_map<uint64_t, std::string> pendingNames;
    std::vector<PlotRuntime> plots;
    uint64_t useClock = 0;
    size_t openFiles = 0;
    TraceSessionPlotStats stats;
};

bool AtomicReplace( const std::filesystem::path& source,
    const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    if( MoveFileExW( source.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) ) return true;
    error = "session_plot_atomic_replace_failed:" + std::to_string( GetLastError() );
    return false;
#else
    std::error_code ec;
    std::filesystem::rename( source, target, ec );
    if( !ec ) return true;
    error = "session_plot_atomic_replace_failed:" + ec.message();
    return false;
#endif
}

bool DecodeItem( const TraceSessionCanonicalRecord& record, QueueItem& item,
    std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ||
        record.type >= uint8_t( QueueType::NUM_TYPES ) ||
        record.payload.size() < QueueDataSize[record.type] )
    { error = "session_plot_protocol_record_invalid"; return false; }
    item = {};
    std::memcpy( &item, record.payload.data(),
        std::min<size_t>( record.payload.size(), sizeof( item ) ) );
    if( item.hdr.idx != record.type )
    { error = "session_plot_protocol_type_mismatch"; return false; }
    return true;
}

bool GetShortPayload( const TraceSessionCanonicalRecord& record,
    const uint8_t*& data, size_t& size, std::string& error )
{
    const auto fixed = size_t( QueueDataSize[record.type] );
    if( record.payload.size() < fixed + sizeof( uint16_t ) )
    { error = "session_plot_payload_truncated"; return false; }
    uint16_t bytes = 0;
    std::memcpy( &bytes, record.payload.data() + fixed, sizeof( bytes ) );
    if( bytes != record.variablePayloadBytes ||
        record.payload.size() != fixed + sizeof( bytes ) + bytes )
    { error = "session_plot_payload_mismatch"; return false; }
    data = record.payload.data() + fixed + sizeof( bytes );
    size = bytes;
    return true;
}

PlotRuntime& EnsurePlot( BuildState& state, uint64_t nativeName )
{
    const auto found = state.byName.find( nativeName );
    if( found != state.byName.end() ) return state.plots[found->second];
    const auto index = state.plots.size();
    state.byName.emplace( nativeName, index );
    PlotRuntime runtime;
    runtime.stored.nativeName = nativeName;
    runtime.stored.type = 0;
    const auto pendingName = state.pendingNames.find( nativeName );
    if( pendingName != state.pendingNames.end() )
    {
        runtime.name = std::move( pendingName->second );
        state.pendingNames.erase( pendingName );
    }
    runtime.workPath = state.root / ( "plot-" + std::to_string( index ) + ".work" );
    state.plots.emplace_back( std::move( runtime ) );
    return state.plots.back();
}

bool EnsureWork( BuildState& state, PlotRuntime& plot, std::string& error )
{
    plot.lastUse = ++state.useClock;
    if( plot.work && *plot.work ) return true;
    constexpr size_t MaximumOpenPlotFiles = 64;
    if( state.openFiles >= MaximumOpenPlotFiles )
    {
        PlotRuntime* oldest = nullptr;
        for( auto& candidate : state.plots )
            if( candidate.work && ( !oldest || candidate.lastUse < oldest->lastUse ) ) oldest = &candidate;
        if( oldest )
        {
            oldest->work->flush();
            if( !*oldest->work ) { error = "session_plot_work_flush_failed"; return false; }
            oldest->work.reset();
            --state.openFiles;
        }
    }
    plot.work = std::make_unique<std::ofstream>( plot.workPath,
        std::ios::binary | std::ios::app );
    if( !*plot.work ) { error = "session_plot_work_open_failed"; return false; }
    ++state.openFiles;
    return true;
}

bool AppendPoint( BuildState& state, uint64_t nativeName, int64_t delta,
    double value, std::string& error )
{
    if( !std::isfinite( value ) ) return true;
    auto& plot = EnsurePlot( state, nativeName );
    if( !EnsureWork( state, plot, error ) ) return false;
    state.refTimeThread += delta;
    StoredPoint point;
    point.plotIndex = size_t( &plot - state.plots.data() );
    point.pointIndex = plot.stored.pointCount;
    point.timeNs = state.transform.ToNanoseconds( state.refTimeThread );
    point.value = value;
    plot.work->write( reinterpret_cast<const char*>( &point ), sizeof( point ) );
    if( !*plot.work ) { error = "session_plot_work_write_failed"; return false; }
    if( plot.stored.pointCount == 0 )
    {
        plot.stored.minimum = value;
        plot.stored.maximum = value;
    }
    else
    {
        plot.stored.minimum = std::min( plot.stored.minimum, value );
        plot.stored.maximum = std::max( plot.stored.maximum, value );
    }
    plot.stored.sum += value;
    ++plot.stored.pointCount;
    ++state.stats.points;
    return true;
}

bool VisitPlotRecord( const TraceSessionCanonicalRecord& record,
    void* userData, std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ) return true;
    auto& state = *static_cast<BuildState*>( userData );
    QueueItem item {};
    if( !DecodeItem( record, item, error ) ) return false;
    switch( QueueType( record.type ) )
    {
    case QueueType::ThreadContext:
        state.refTimeThread = 0;
        break;
    case QueueType::PlotName:
    {
        const uint8_t* data = nullptr; size_t size = 0;
        if( !GetShortPayload( record, data, size, error ) ) return false;
        const auto found = state.byName.find( item.stringTransfer.ptr );
        if( found != state.byName.end() )
        {
            state.plots[found->second].name.assign(
                reinterpret_cast<const char*>( data ), size );
        }
        else
        {
            state.pendingNames.insert_or_assign( item.stringTransfer.ptr,
                std::string( reinterpret_cast<const char*>( data ), size ) );
        }
        ++state.stats.nameEvents;
        break;
    }
    case QueueType::PlotConfig:
    {
        auto& plot = EnsurePlot( state, item.plotConfig.name );
        plot.stored.format = item.plotConfig.type;
        plot.stored.showSteps = item.plotConfig.step;
        plot.stored.fill = item.plotConfig.fill;
        plot.stored.color = item.plotConfig.color & 0xFFFFFF;
        ++state.stats.configEvents;
        break;
    }
    case QueueType::PlotDataInt:
        ++state.stats.dataEvents;
        return AppendPoint( state, item.plotDataInt.name, item.plotDataInt.time,
            double( item.plotDataInt.val ), error );
    case QueueType::PlotDataFloat:
        ++state.stats.dataEvents;
        return AppendPoint( state, item.plotDataFloat.name, item.plotDataFloat.time,
            double( item.plotDataFloat.val ), error );
    case QueueType::PlotDataDouble:
        ++state.stats.dataEvents;
        return AppendPoint( state, item.plotDataDouble.name, item.plotDataDouble.time,
            item.plotDataDouble.val, error );
    default:
        break;
    }
    return true;
}

bool CopyFile( const std::filesystem::path& path, std::ofstream& out,
    std::string& error )
{
    std::ifstream in( path, std::ios::binary );
    if( !in ) { error = "session_plot_work_read_failed"; return false; }
    // The production converter uses the Windows default 1 MiB thread stack.
    // Keep the large sequential-copy buffer on the heap; a 1 MiB local array
    // leaves no room for the caller and terminates the process with
    // STATUS_STACK_OVERFLOW before an error can be reported.
    std::vector<char> buffer( 1024 * 1024 );
    while( in )
    {
        in.read( buffer.data(), std::streamsize( buffer.size() ) );
        const auto bytes = in.gcount();
        if( bytes > 0 ) out.write( buffer.data(), bytes );
    }
    if( !in.eof() || !out ) { error = "session_plot_work_copy_failed"; return false; }
    return true;
}

bool SaveManifest( const std::filesystem::path& root,
    const PlotManifest& value, std::string& error )
{
    const auto temporary = root / "manifest.tmp";
    const auto target = root / "manifest";
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_plot_manifest_open_failed"; return false; }
    out << std::hex << PlotManifestMagic << std::dec << '\n'
        << TraceSessionPlotIndexSchemaVersion << '\n'
        << std::quoted( value.sourceSha256 ) << '\n' << value.sourceSize << '\n'
        << std::quoted( value.generation ) << '\n' << value.fileBytes << '\n'
        << std::quoted( value.fileSha256 ) << '\n'
        << value.stats.plots << ' ' << value.stats.points << ' '
        << value.stats.dataEvents << ' ' << value.stats.configEvents << ' '
        << value.stats.nameEvents << '\n';
    out.flush();
    if( !out ) { error = "session_plot_manifest_write_failed"; return false; }
    out.close();
    return AtomicReplace( temporary, target, error );
}

bool LoadManifest( const std::filesystem::path& root,
    PlotManifest& value, std::string& error )
{
    std::ifstream in( root / "manifest", std::ios::binary );
    uint64_t magic = 0; uint32_t schema = 0;
    if( !( in >> std::hex >> magic >> std::dec >> schema >> std::quoted( value.sourceSha256 )
        >> value.sourceSize >> std::quoted( value.generation ) >> value.fileBytes
        >> std::quoted( value.fileSha256 ) >> value.stats.plots >> value.stats.points
        >> value.stats.dataEvents >> value.stats.configEvents >> value.stats.nameEvents ) )
    { error = "session_plot_manifest_parse_failed"; return false; }
    value.stats.fileBytes = value.fileBytes;
    if( magic != PlotManifestMagic || schema != TraceSessionPlotIndexSchemaVersion ||
        value.sourceSha256.size() != 64 || value.fileSha256.size() != 64 ||
        value.stats.points > value.stats.dataEvents )
    { error = "session_plot_manifest_invalid"; return false; }
    return true;
}

std::string MakeRef( const std::string& fingerprint, const char* kind, uint64_t id )
{
    std::ostringstream out;
    out << "tracy:v1:" << fingerprint.substr( 0, 16 ) << ':' << kind << ':' << std::hex << id;
    return out.str();
}

}

struct TraceSessionPlotReader::Impl
{
    std::filesystem::path path;
    std::string fingerprint;
    uint64_t pointsOffset = 0;
    uint64_t pointCount = 0;
};

TraceSessionPlotReader::TraceSessionPlotReader( std::shared_ptr<Impl> impl )
    : m_impl( std::move( impl ) )
{}

std::filesystem::path TraceSessionPlotIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest )
{
    return sessionRoot / "generations" / manifest.generation / "derived" /
        "plot-index" / std::to_string( TraceSessionPlotIndexSchemaVersion ) / "exact";
}

bool BuildTraceSessionPlotDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& session, TraceSessionPlotStats& stats, std::string& error )
{
    error.clear(); stats = {};
    BuildState state;
    state.root = TraceSessionPlotIndexRoot( sessionRoot, session );
    std::error_code ec; std::filesystem::create_directories( state.root, ec );
    if( ec ) { error = "session_plot_directory_failed:" + ec.message(); return false; }
    for( std::filesystem::directory_iterator it( state.root, ec ), end; it != end; it.increment( ec ) )
    {
        if( ec ) { error = "session_plot_work_cleanup_scan_failed:" + ec.message(); return false; }
        if( it->is_regular_file( ec ) && it->path().extension() == ".work" )
        {
            std::filesystem::remove( it->path(), ec );
            if( ec ) { error = "session_plot_work_cleanup_failed:" + ec.message(); return false; }
        }
    }
    if( !LoadTraceSessionTimeTransform( sessionRoot, session, state.transform, error ) ) return false;
    if( !VisitTraceSessionCanonicalOrdered( sessionRoot, session,
        VisitPlotRecord, &state, error ) ) return false;
    for( auto& plot : state.plots ) if( plot.work )
    {
        plot.work->flush();
        if( !*plot.work ) { error = "session_plot_work_flush_failed"; return false; }
        plot.work.reset();
    }
    state.openFiles = 0;

    std::vector<uint8_t> strings;
    uint64_t pointBegin = 0;
    for( size_t index = 0; index < state.plots.size(); ++index )
    {
        auto& plot = state.plots[index];
        if( plot.name.empty() )
        {
            std::ostringstream generated;
            generated << "Plot 0x" << std::hex << plot.stored.nativeName;
            plot.name = generated.str();
        }
        plot.stored.nameOffset = strings.size();
        plot.stored.nameBytes = uint32_t( std::min<size_t>( plot.name.size(),
            std::numeric_limits<uint32_t>::max() ) );
        strings.insert( strings.end(), plot.name.begin(), plot.name.begin() + plot.stored.nameBytes );
        plot.stored.pointBegin = pointBegin;
        pointBegin += plot.stored.pointCount;
    }

    PlotFileHeader header;
    header.sourceSize = session.source.fileSize;
    header.plotCount = state.plots.size();
    header.pointCount = state.stats.points;
    header.dataEvents = state.stats.dataEvents;
    header.configEvents = state.stats.configEvents;
    header.nameEvents = state.stats.nameEvents;
    header.generationBytes = uint32_t( session.generation.size() );
    header.plotsOffset = sizeof( header ) + session.source.sha256.size() + session.generation.size();
    header.stringsOffset = header.plotsOffset + header.plotCount * sizeof( StoredPlot );
    header.pointsOffset = header.stringsOffset + strings.size();
    const auto temporary = state.root / "plots.bin.tmp";
    const auto target = state.root / PlotFileName;
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_plot_file_open_failed"; return false; }
    out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
    out.write( session.source.sha256.data(), std::streamsize( session.source.sha256.size() ) );
    out.write( session.generation.data(), std::streamsize( session.generation.size() ) );
    for( const auto& plot : state.plots )
        out.write( reinterpret_cast<const char*>( &plot.stored ), sizeof( plot.stored ) );
    if( !strings.empty() ) out.write( reinterpret_cast<const char*>( strings.data() ),
        std::streamsize( strings.size() ) );
    for( const auto& plot : state.plots )
        if( plot.stored.pointCount != 0 && !CopyFile( plot.workPath, out, error ) ) return false;
    out.flush();
    if( !out ) { error = "session_plot_file_write_failed"; return false; }
    out.close();
    if( !AtomicReplace( temporary, target, error ) ) return false;
    for( const auto& plot : state.plots )
    {
        std::filesystem::remove( plot.workPath, ec );
        ec.clear();
    }
    PlotManifest manifest;
    manifest.sourceSha256 = session.source.sha256;
    manifest.sourceSize = session.source.fileSize;
    manifest.generation = session.generation;
    manifest.fileBytes = std::filesystem::file_size( target, ec );
    if( ec ) { error = "session_plot_file_size_failed:" + ec.message(); return false; }
    manifest.fileSha256 = Sha256File( target );
    manifest.stats = state.stats;
    manifest.stats.plots = state.plots.size();
    manifest.stats.fileBytes = manifest.fileBytes;
    if( !SaveManifest( state.root, manifest, error ) ) return false;
    stats = manifest.stats;
    return true;
}

bool AuditTraceSessionPlotDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& session, TraceSessionPlotStats& stats, std::string& error )
{
    error.clear(); stats = {};
    const auto root = TraceSessionPlotIndexRoot( sessionRoot, session );
    PlotManifest manifest;
    if( !LoadManifest( root, manifest, error ) ) return false;
    if( manifest.sourceSha256 != session.source.sha256 || manifest.sourceSize != session.source.fileSize ||
        manifest.generation != session.generation )
    { error = "session_plot_identity_mismatch"; return false; }
    const auto path = root / PlotFileName;
    std::error_code ec;
    if( std::filesystem::file_size( path, ec ) != manifest.fileBytes || ec )
    { error = "session_plot_file_size_mismatch"; return false; }
    if( Sha256File( path ) != manifest.fileSha256 )
    { error = "session_plot_file_sha256_mismatch"; return false; }
    stats = manifest.stats;
    return true;
}

std::shared_ptr<TraceSessionPlotReader> TraceSessionPlotReader::Open(
    const std::filesystem::path& sessionRoot, const TraceSessionManifest& session,
    std::string& error )
{
    error.clear();
    TraceSessionPlotStats stats;
    if( !AuditTraceSessionPlotDerived( sessionRoot, session, stats, error ) ) return {};
    const auto path = TraceSessionPlotIndexRoot( sessionRoot, session ) / PlotFileName;
    std::ifstream in( path, std::ios::binary );
    PlotFileHeader header;
    if( !in.read( reinterpret_cast<char*>( &header ), sizeof( header ) ) ||
        header.magic != PlotFileMagic || header.schema != TraceSessionPlotIndexSchemaVersion ||
        header.endian != 0x01020304 || header.sourceSize != session.source.fileSize ||
        header.plotCount != stats.plots || header.pointCount != stats.points ||
        header.dataEvents != stats.dataEvents || header.configEvents != stats.configEvents ||
        header.nameEvents != stats.nameEvents || header.generationBytes != session.generation.size() )
    { error = "session_plot_file_header_invalid"; return {}; }
    std::string sha( 64, '\0' ), generation( header.generationBytes, '\0' );
    if( !in.read( sha.data(), std::streamsize( sha.size() ) ) ||
        !in.read( generation.data(), std::streamsize( generation.size() ) ) ||
        sha != session.source.sha256 || generation != session.generation ||
        header.plotsOffset != uint64_t( in.tellg() ) ||
        header.stringsOffset != header.plotsOffset + header.plotCount * sizeof( StoredPlot ) ||
        header.pointsOffset > stats.fileBytes ||
        header.pointsOffset + header.pointCount * sizeof( StoredPoint ) != stats.fileBytes )
    { error = "session_plot_file_identity_or_bounds_invalid"; return {}; }
    std::vector<StoredPlot> stored( size_t( header.plotCount ) );
    if( !stored.empty() && !in.read( reinterpret_cast<char*>( stored.data() ),
        std::streamsize( stored.size() * sizeof( StoredPlot ) ) ) )
    { error = "session_plot_definition_read_failed"; return {}; }
    const auto stringBytes = header.pointsOffset - header.stringsOffset;
    std::vector<char> strings( static_cast<size_t>( stringBytes ), char {} );
    if( !strings.empty() && !in.read( strings.data(), std::streamsize( strings.size() ) ) )
    { error = "session_plot_string_read_failed"; return {}; }
    auto impl = std::make_shared<Impl>();
    impl->path = path; impl->fingerprint = session.source.sha256;
    impl->pointsOffset = header.pointsOffset; impl->pointCount = header.pointCount;
    auto reader = std::shared_ptr<TraceSessionPlotReader>( new TraceSessionPlotReader( impl ) );
    reader->m_stats = stats;
    reader->m_plots.reserve( stored.size() );
    uint64_t expectedPointBegin = 0;
    for( size_t index = 0; index < stored.size(); ++index )
    {
        const auto& value = stored[index];
        if( value.nameOffset > strings.size() || value.nameBytes > strings.size() - value.nameOffset ||
            value.pointBegin != expectedPointBegin || value.pointCount > header.pointCount - expectedPointBegin )
        { error = "session_plot_definition_bounds_invalid"; return {}; }
        expectedPointBegin += value.pointCount;
        PlotDto dto;
        dto.ref = MakeRef( session.source.sha256, "plot", index );
        dto.index = index;
        dto.name.assign( strings.data() + value.nameOffset, value.nameBytes );
        dto.type = value.type; dto.format = value.format;
        dto.pointCount = value.pointCount; dto.min = value.minimum;
        dto.max = value.maximum; dto.sum = value.sum;
        dto.showSteps = value.showSteps != 0; dto.fill = value.fill;
        dto.color = value.color;
        reader->m_plots.emplace_back( std::move( dto ) );
    }
    if( expectedPointBegin != header.pointCount )
    { error = "session_plot_point_count_mismatch"; return {}; }
    return reader;
}

std::vector<PlotPointDto> TraceSessionPlotReader::Scan( const ScanRange& range ) const
{
    std::vector<PlotPointDto> result;
    std::ifstream in( m_impl->path, std::ios::binary );
    if( !in ) return result;
    in.seekg( std::streamoff( m_impl->pointsOffset ) );
    size_t skipped = 0;
    for( uint64_t ordinal = 0; ordinal < m_impl->pointCount; ++ordinal )
    {
        StoredPoint value;
        if( !in.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) ) return result;
        if( value.plotIndex >= m_plots.size() ||
            value.timeNs < range.startNs || value.timeNs >= range.endNs ) continue;
        if( skipped++ < range.offset ) continue;
        result.push_back( {
            MakeRef( m_impl->fingerprint, "plot-point",
                ( value.plotIndex << 40 ) | value.pointIndex ),
            m_plots[size_t( value.plotIndex )].ref, value.timeNs, value.value
        } );
        if( result.size() >= range.limit ) break;
    }
    return result;
}

}
