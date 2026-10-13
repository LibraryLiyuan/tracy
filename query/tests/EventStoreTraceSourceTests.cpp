#include "TracyEventStoreTraceSource.hpp"
#include "TracyBoundedScanCursor.hpp"
#include "TracyWindowedTraceSource.hpp"
#include "TracyEventStore.hpp"
#include "TracyStreamJournal.hpp"
#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracyQueue.hpp"
#include "../../public/common/tracy_lz4.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <vector>

using namespace tracy;

template<class T> static void AppendBytes( std::vector<uint8_t>& bytes, const T& value )
{
    const auto* begin = reinterpret_cast<const uint8_t*>( &value );
    bytes.insert( bytes.end(), begin, begin + sizeof( value ) );
}

static void CreateJournal( const std::filesystem::path& path, bool cleanEnd )
{
    std::string error;
    auto writer = stream::JournalWriter::CreateFileJournal( path,
        stream::MakeFileHeader( ProtocolVersion ), false, {}, error );
    if( !writer ) throw std::runtime_error( error );
    uint64_t sequence = 0;
    const auto append = [&]( stream::RecordType type, uint32_t flags, std::span<const uint8_t> bytes ) {
        if( !writer->Append( type, flags, bytes, ++sequence, error ) ) throw std::runtime_error( error );
    };
    std::vector<uint8_t> bytes( 24 );
    bytes[0] = 2; bytes[2] = 24;
    const uint32_t protocol = ProtocolVersion;
    std::memcpy( bytes.data() + 4, &protocol, sizeof( protocol ) );
    append( stream::RecordType::SessionBegin, 0, bytes );
    const uint8_t status = HandshakeWelcome;
    append( stream::RecordType::ClientToServer, stream::RecordFlagHandshake, { &status, 1 } );
    WelcomeMessage welcome {};
    welcome.timerMul = 1; welcome.initBegin = 1000; welcome.initEnd = 1010;
    const char program[] = "Query prefix fixture";
    std::memcpy( welcome.programName, program, sizeof( program ) );
    append( stream::RecordType::ClientToServer, stream::RecordFlagHandshake,
        { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
    bytes.clear();
    for( int64_t time : { 1100, 1200, 1300 } )
    {
        bytes.push_back( uint8_t( QueueType::FrameMarkMsg ) );
        AppendBytes( bytes, QueueFrameMark { time, 0 } );
    }
    std::vector<uint8_t> compressed( LZ4Size + 4 );
    const int size = LZ4_compress_default( reinterpret_cast<const char*>( bytes.data() ),
        reinterpret_cast<char*>( compressed.data() + 4 ), int( bytes.size() ), LZ4Size );
    assert( size > 0 );
    std::memcpy( compressed.data(), &size, sizeof( size ) );
    compressed.resize( size + 4 );
    append( stream::RecordType::ClientToServer, stream::RecordFlagCompressedFrame, compressed );
    if( cleanEnd )
    {
        bytes.assign( 32, 0 ); bytes[0] = 1; bytes[2] = 32; bytes[4] = 3;
        append( stream::RecordType::SessionEnd, stream::RecordFlagTerminal, bytes );
    }
}

int main()
{
    const auto root = std::filesystem::temp_directory_path() / ( "tracy-query-prefix-" +
        std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    std::filesystem::create_directories( root );
    for( const bool cleanEnd : { false, true } )
    {
        const auto path = root / ( cleanEnd ? "complete.tracy-stream" : "partial.tracy-stream" );
        CreateJournal( path, cleanEnd );
        const auto info = stream::EventStore::Build( path );
        assert( info.cleanEnd == cleanEnd );
        assert( query::EventStoreTraceSource::Available( path ) );
        {
            const auto source = std::shared_ptr<analysis::TraceSource>( query::EventStoreTraceSource::Open( path ) );
            const auto view = source->AcquireReadView();
            assert( view.sourceKind == analysis::TraceSourceKind::Segment );
            assert( view.revision == info.revision );
            assert( view.complete == cleanEnd );
            assert( source->GetTraceInfo().fingerprint == info.sourceSha256 );
            assert( source->GetTraceInfo().counts.frames == info.frames );
            const auto validation = source->ValidateZoneIndex( []( size_t count ) { return count; } );
            assert( validation );
            const bool warned = std::any_of( validation->findings.begin(), validation->findings.end(),
                []( const auto& finding ) { return finding.code == "INCOMPLETE_CAPTURE"; } );
            assert( warned == !cleanEnd );
            analysis::BoundedScanRequest request;
            request.domain = analysis::BoundedScanDomain::Frame;
            request.limit = 1;
            assert( analysis::BoundedTraceScanner( *source ).Read( request ).Count() == 1 );
            const auto windowed = analysis::MakeWindowedTraceSource( source, { { 0, 1000 } } );
            assert( windowed->AcquireReadView().complete == cleanEnd );
            assert( analysis::BoundedTraceScanner( *windowed ).Read( request ).Count() == 1 );
        }
    }
    std::filesystem::remove_all( root );
}
