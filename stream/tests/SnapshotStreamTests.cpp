#include "../src/TracySnapshotStream.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace tracy::stream;
void Check( bool value, const char* error )
{
    if( !value ) throw std::runtime_error( error );
}
uint8_t Pattern( uint64_t i ) { return uint8_t( ( i >> 4 ) ^ ( i * 13 ) ^ ( i >> 16 ) ); }
int main( int argc, char** argv )
{
    if( argc == 4 && std::string_view( argv[1] ) == "--native" )
    {
        try
        {
            SnapshotStreamReader reader( std::filesystem::u8path( argv[2] ) );
            const auto total = std::stoull( argv[3] );
            std::array<uint8_t, 1003> bytes;
            uint64_t offset = 0;
            for( ;; )
            {
                const auto n = reader.Read( bytes );
                if( !n ) break;
                for( size_t i = 0; i < n; ++i ) Check( bytes[i] == Pattern( offset + i ), "native writer/bounded reader byte mismatch" );
                offset += n;
            }
            Check( offset == total && reader.Finish().decodedBytes == total, "native writer payload size" );
            std::cout << "Native writer / bounded reader passed: " << total << " bytes.\n";
            return 0;
        }
        catch( const std::exception& e )
        {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
    const auto root = std::filesystem::temp_directory_path() / ( "tracy-snapshot-stream-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        std::filesystem::create_directory( root );
        {
            const auto path=root/"known-digest.tracy";SnapshotStreamWriter writer(path);const uint8_t a='a';const uint8_t bc[]={'b','c'};writer.Write({&a,1});writer.Write(bc);
            const std::array<uint8_t,32> expected={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
            Check(writer.Finish().sha256==expected,"known SHA256 checks partial final block and tiny writes");SnapshotStreamReader reader(path);Check(reader.Finish().sha256==expected,"known SHA256 from bounded reader");
        }
        const uint64_t total = argc > 1 ? std::stoull( argv[1] ) : 4 * 1024 * 1024 + 123;
        for( uint8_t streams : { 1, 4, 16 } )
        {
            const auto path = root / ( "streams-" + std::to_string( streams ) + ".tracy" );
            SnapshotStreamOptions options;
            options.streams = streams;
            SnapshotStreamWriter writer( path, options );
            uint64_t offset = 0;
            std::vector<uint8_t> data( 70001 );
            while( offset < total )
            {
                const auto n = size_t( std::min<uint64_t>( data.size(), total - offset ) );
                for( size_t i = 0; i < n; ++i ) data[i] = Pattern( offset + i );
                writer.Write( { data.data(), n } );
                offset += n;
            }
            const auto written = writer.Finish();
            Check( written.decodedBytes == total && written.fileBytes == std::filesystem::file_size( path ), "writer byte counts" );
            Check( writer.Finish().sha256 == written.sha256, "finish is idempotent" );
            SnapshotStreamReader reader( path );
            std::array<uint8_t, 1003> output{};
            offset = 0;
            for( ;; )
            {
                const auto n = reader.Read( output );
                if( !n ) break;
                for( size_t i = 0; i < n; ++i ) Check( output[i] == Pattern( offset + i ), "round-robin stream dictionary and partial-block bytes" );
                offset += n;
            }
            const auto read = reader.Finish();
            Check( offset == total && read.decodedBytes == total && read.sha256 == written.sha256 && read.fileBytes == written.fileBytes, "streaming verification hash and counts" );
            bool protectedFile = false;
            try
            {
                SnapshotStreamWriter duplicate( path );
            }
            catch( const std::exception& )
            {
                protectedFile = true;
            }
            Check( protectedFile, "existing output cannot be overwritten" );
        }
        const auto original = root / "streams-4.tracy";
        const auto truncated = root / "truncated.tracy";
        std::filesystem::copy_file( original, truncated );
        std::filesystem::resize_file( truncated, std::filesystem::file_size( truncated ) - 1 );
        bool failed = false;
        try
        {
            SnapshotStreamReader reader( truncated );
            reader.Finish();
        }
        catch( const std::exception& )
        {
            failed = true;
        }
        Check( failed, "truncated block is not EOF" );
        const auto corrupt = root / "corrupt.tracy";
        std::filesystem::copy_file( original, corrupt );
        {
            std::fstream file( corrupt, std::ios::binary | std::ios::in | std::ios::out );
            file.seekp( 6 );
            const uint32_t size = 0xffffffffu;
            file.write( reinterpret_cast<const char*>( &size ), 4 );
        }
        failed = false;
        try
        {
            SnapshotStreamReader reader( corrupt );
            reader.Finish();
        }
        catch( const std::exception& )
        {
            failed = true;
        }
        Check( failed, "oversized compressed block rejected before allocation" );
        SnapshotStreamOptions tiny;
        tiny.maxFileBytes = 6;
        failed = false;
        try
        {
            SnapshotStreamWriter writer( root / "disk-budget.tracy", tiny );
            const uint8_t byte = 1;
            writer.Write( { &byte, 1 } );
            writer.Finish();
        }
        catch( const std::exception& )
        {
            failed = true;
        }
        Check( failed, "disk budget failure propagates" );
        failed = false;
        try
        {
            SnapshotStreamReader reader( original, tiny );
            reader.Finish();
        }
        catch( const std::exception& )
        {
            failed = true;
        }
        Check( failed, "input budget failure propagates" );
        SnapshotStreamOptions decodedBudget;
        decodedBudget.maxDecodedBytes = 1024;
        failed = false;
        try
        {
            SnapshotStreamReader reader( original, decodedBudget );
            reader.Finish();
        }
        catch( const std::exception& )
        {
            failed = true;
        }
        Check( failed, "decoded input byte budget" );
        failed = false;
        try
        {
            SnapshotStreamWriter writer( root / "decoded-budget.tracy", decodedBudget );
            const std::array<uint8_t, 1025> bytes{};
            writer.Write( bytes );
        }
        catch( const std::exception& )
        {
            failed = true;
        }
        Check( failed, "decoded output byte budget" );
        bool cancel = false;
        SnapshotStreamOptions cancellable;
        cancellable.cancelled = [&] { return cancel; };
        {
            SnapshotStreamWriter writer( root / "cancelled.tracy", cancellable );
            cancel = true;
            failed = false;
            try
            {
                const uint8_t byte = 1;
                writer.Write( { &byte, 1 } );
            }
            catch( const std::exception& )
            {
                failed = true;
            }
            Check( failed, "writer cancellation" );
        }
        cancel = false;
        {
            SnapshotStreamReader reader( original, cancellable );
            cancel = true;
            failed = false;
            try
            {
                std::array<uint8_t, 1> out;
                reader.Read( out );
            }
            catch( const std::exception& )
            {
                failed = true;
            }
            Check( failed, "reader cancellation" );
        }
        SnapshotStreamOptions invalid;
        invalid.streams = 0;
        failed = false;
        try
        {
            SnapshotStreamWriter writer( root / "invalid.tracy", invalid );
        }
        catch( const std::exception& )
        {
            failed = true;
        }
        Check( failed && !std::filesystem::exists( root / "invalid.tracy" ), "invalid options have no filesystem side effect" );
        SnapshotStreamOptions interrupted;
        unsigned polls=0;
        interrupted.cancelled=[&]{return ++polls>=2;};
        failed=false;
        try { SnapshotStreamWriter writer(root/"initialization-cancelled.tracy",interrupted); }
        catch(const std::exception&) { failed=true; }
        Check(failed && !std::filesystem::exists(root/"initialization-cancelled.tracy"),"failed writer initialization removes its newly created file");
        std::cout << "Snapshot stream tests passed: " << total << " bytes per stream configuration. Evidence: " << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << "FAIL: " << e.what() << "\nEvidence: " << root.string() << '\n';
        return 1;
    }
}
