#include "TracyStreamSnapshotMap.hpp"

#include <charconv>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>

namespace tracy::stream
{
namespace
{

constexpr std::string_view SnapshotMapMagic = "JNTRACY-STREAM-SNAPSHOT-MAP/1";
constexpr uint64_t MaximumSnapshotMapBytes = 64 * 1024;

int64_t FileWriteTime( const std::filesystem::path& path )
{
    return std::filesystem::last_write_time( path ).time_since_epoch().count();
}

bool SameParent( const std::filesystem::path& left, const std::filesystem::path& right )
{
    std::error_code error;
    const auto leftAbsolute = std::filesystem::weakly_canonical( left, error );
    if( error ) return false;
    const auto rightAbsolute = std::filesystem::weakly_canonical( right, error );
    return !error && leftAbsolute.parent_path() == rightAbsolute.parent_path();
}

template<typename T>
bool ParseUnsigned( std::string_view text, T& value )
{
    uint64_t parsed = 0;
    const auto result = std::from_chars( text.data(), text.data() + text.size(), parsed );
    if( result.ec != std::errc() || result.ptr != text.data() + text.size() || parsed > uint64_t( std::numeric_limits<T>::max() ) ) return false;
    value = T( parsed );
    return true;
}

bool ParseSigned( std::string_view text, int64_t& value )
{
    const auto result = std::from_chars( text.data(), text.data() + text.size(), value );
    return result.ec == std::errc() && result.ptr == text.data() + text.size();
}

bool ReadValue( std::ifstream& input, std::string_view expectedKey, std::string& value )
{
    std::string line;
    if( !std::getline( input, line ) ) return false;
    const auto separator = line.find( '=' );
    if( separator == std::string::npos || std::string_view( line.data(), separator ) != expectedKey ) return false;
    value.assign( line.data() + separator + 1, line.size() - separator - 1 );
    return true;
}

}

std::filesystem::path ConvertedSnapshotMapPath( const std::filesystem::path& streamPath )
{
    auto result = streamPath;
    result += ".snapshot-map";
    return result;
}

bool WriteConvertedSnapshotMap(
    const std::filesystem::path& streamPath,
    const ScanResult& scan,
    const std::filesystem::path& snapshotPath,
    std::string& error )
{
    error.clear();
    try
    {
        if( !scan.HasRecoverablePrefix() || !std::filesystem::is_regular_file( streamPath ) )
        {
            error = "stream does not have a recoverable committed prefix";
            return false;
        }
        if( !std::filesystem::is_regular_file( snapshotPath ) )
        {
            error = "converted snapshot does not exist";
            return false;
        }
        if( !SameParent( streamPath, snapshotPath ) || snapshotPath.filename().empty() )
        {
            error = "converted snapshot must be in the stream directory";
            return false;
        }

        const auto mapPath = ConvertedSnapshotMapPath( streamPath );
        auto temporary = mapPath;
        temporary += ".tmp";
        std::ofstream output( temporary, std::ios::binary | std::ios::trunc );
        if( !output )
        {
            error = "cannot create temporary snapshot map";
            return false;
        }
        output << SnapshotMapMagic << '\n'
            << "stream_bytes=" << std::filesystem::file_size( streamPath ) << '\n'
            << "stream_write_time=" << FileWriteTime( streamPath ) << '\n'
            << "protocol_version=" << scan.header.protocolVersion << '\n'
            << "revision=" << scan.lastSequence << '\n'
            << "valid_size=" << scan.validSize << '\n'
            << "record_count=" << scan.recordCount << '\n'
            << "watermark_ns=" << scan.lastMonotonicNs << '\n'
            << "prefix_crc32c=" << scan.prefixCrc32c << '\n'
            << "complete=" << ( scan.complete ? 1 : 0 ) << '\n'
            << "snapshot=" << std::quoted( snapshotPath.filename().string() ) << '\n'
            << "snapshot_bytes=" << std::filesystem::file_size( snapshotPath ) << '\n'
            << "snapshot_write_time=" << FileWriteTime( snapshotPath ) << '\n';
        output.close();
        if( !output )
        {
            error = "cannot flush temporary snapshot map";
            std::error_code ignored;
            std::filesystem::remove( temporary, ignored );
            return false;
        }

        std::error_code filesystemError;
        std::filesystem::remove( mapPath, filesystemError );
        filesystemError.clear();
        std::filesystem::rename( temporary, mapPath, filesystemError );
        if( filesystemError )
        {
            error = "cannot publish snapshot map: " + filesystemError.message();
            std::error_code ignored;
            std::filesystem::remove( temporary, ignored );
            return false;
        }
        return true;
    }
    catch( const std::exception& exception )
    {
        error = exception.what();
        return false;
    }
}

bool ReadConvertedSnapshotMap(
    const std::filesystem::path& streamPath,
    const JournalReadView& view,
    ConvertedSnapshotMap& result,
    std::string& error )
{
    error.clear();
    result = {};
    try
    {
        const auto mapPath = ConvertedSnapshotMapPath( streamPath );
        if( !std::filesystem::is_regular_file( mapPath ) )
        {
            error = "snapshot map is absent";
            return false;
        }
        if( std::filesystem::file_size( mapPath ) > MaximumSnapshotMapBytes )
        {
            error = "snapshot map is too large";
            return false;
        }
        std::ifstream input( mapPath, std::ios::binary );
        std::string line;
        if( !std::getline( input, line ) || line != SnapshotMapMagic )
        {
            error = "snapshot map magic or schema is invalid";
            return false;
        }

        uint64_t streamBytes = 0;
        int64_t streamWriteTime = 0;
        uint32_t protocolVersion = 0;
        uint64_t revision = 0;
        uint64_t validSize = 0;
        uint64_t recordCount = 0;
        uint64_t watermarkNs = 0;
        uint32_t prefixCrc32c = 0;
        uint32_t complete = 0;
        uint64_t snapshotBytes = 0;
        int64_t snapshotWriteTime = 0;
        std::string value;
        if( !ReadValue( input, "stream_bytes", value ) || !ParseUnsigned( value, streamBytes ) ||
            !ReadValue( input, "stream_write_time", value ) || !ParseSigned( value, streamWriteTime ) ||
            !ReadValue( input, "protocol_version", value ) || !ParseUnsigned( value, protocolVersion ) ||
            !ReadValue( input, "revision", value ) || !ParseUnsigned( value, revision ) ||
            !ReadValue( input, "valid_size", value ) || !ParseUnsigned( value, validSize ) ||
            !ReadValue( input, "record_count", value ) || !ParseUnsigned( value, recordCount ) ||
            !ReadValue( input, "watermark_ns", value ) || !ParseUnsigned( value, watermarkNs ) ||
            !ReadValue( input, "prefix_crc32c", value ) || !ParseUnsigned( value, prefixCrc32c ) ||
            !ReadValue( input, "complete", value ) || !ParseUnsigned( value, complete ) || complete > 1 )
        {
            error = "snapshot map stream identity is invalid";
            return false;
        }

        if( !ReadValue( input, "snapshot", value ) )
        {
            error = "snapshot map filename is absent";
            return false;
        }
        std::istringstream quoted( value );
        std::string filename;
        quoted >> std::quoted( filename );
        quoted >> std::ws;
        if( !quoted || !quoted.eof() )
        {
            error = "snapshot map filename is invalid";
            return false;
        }
        if( !ReadValue( input, "snapshot_bytes", value ) || !ParseUnsigned( value, snapshotBytes ) ||
            !ReadValue( input, "snapshot_write_time", value ) || !ParseSigned( value, snapshotWriteTime ) )
        {
            error = "snapshot map file identity is invalid";
            return false;
        }

        if( streamBytes != std::filesystem::file_size( streamPath ) || streamWriteTime != FileWriteTime( streamPath ) ||
            protocolVersion != view.header.protocolVersion || revision != view.revision || validSize != view.validSize ||
            recordCount != view.recordCount || watermarkNs != view.watermarkNs || prefixCrc32c != view.prefixCrc32c ||
            ( complete != 0 ) != view.complete )
        {
            error = "snapshot map does not match the committed stream revision";
            return false;
        }

        const auto filenamePath = std::filesystem::path( filename );
        if( filenamePath.empty() || filenamePath != filenamePath.filename() )
        {
            error = "snapshot map filename escapes the stream directory";
            return false;
        }
        const auto snapshotPath = streamPath.parent_path() / filenamePath;
        if( !SameParent( streamPath, snapshotPath ) || !std::filesystem::is_regular_file( snapshotPath ) ||
            std::filesystem::file_size( snapshotPath ) != snapshotBytes || FileWriteTime( snapshotPath ) != snapshotWriteTime )
        {
            error = "converted snapshot metadata changed after map publication";
            return false;
        }

        result.snapshotPath = snapshotPath;
        result.snapshotBytes = snapshotBytes;
        result.snapshotWriteTime = snapshotWriteTime;
        return true;
    }
    catch( const std::exception& exception )
    {
        error = exception.what();
        return false;
    }
}

}
