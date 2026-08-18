#include "TracyStreamJournal.hpp"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#ifndef TRACY_STREAM_VERSION
#  define TRACY_STREAM_VERSION "development"
#endif

namespace
{

void Usage()
{
    std::cerr << "Usage:\n"
                 "  tracy-stream --version\n"
                 "  tracy-stream inspect <capture.tracy-stream> [--records <count>] [--output file.json]\n"
                 "  tracy-stream recover <capture.tracy-stream> --truncate [--output file.json]\n";
}

std::string JsonEscape( std::string_view value )
{
    std::string result;
    result.reserve( value.size() + 8 );
    for( const unsigned char ch : value )
    {
        switch( ch )
        {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if( ch < 0x20 )
            {
                constexpr char Hex[] = "0123456789abcdef";
                result += "\\u00";
                result += Hex[ch >> 4];
                result += Hex[ch & 0xF];
            }
            else
            {
                result += char( ch );
            }
            break;
        }
    }
    return result;
}

void PrintScan( const std::filesystem::path& path, const tracy::stream::ScanResult& scan, bool repaired )
{
    const auto pathBytes = path.u8string();
    const std::string pathUtf8( reinterpret_cast<const char*>( pathBytes.data() ), pathBytes.size() );
    std::cout << "{\n"
              << "  \"path\": \"" << JsonEscape( pathUtf8 ) << "\",\n"
              << "  \"status\": \"" << tracy::stream::ScanCodeName( scan.code ) << "\",\n"
              << "  \"message\": \"" << JsonEscape( scan.message ) << "\",\n"
              << "  \"file_size\": " << scan.fileSize << ",\n"
              << "  \"valid_size\": " << scan.validSize << ",\n"
              << "  \"record_count\": " << scan.recordCount << ",\n"
              << "  \"last_sequence\": " << scan.lastSequence << ",\n"
              << "  \"watermark_ns\": " << scan.lastMonotonicNs << ",\n"
              << "  \"prefix_crc32c\": " << scan.prefixCrc32c << ",\n"
              << "  \"complete\": " << ( scan.complete ? "true" : "false" ) << ",\n"
              << "  \"recoverable_prefix\": " << ( scan.HasRecoverablePrefix() ? "true" : "false" ) << ",\n"
              << "  \"repaired\": " << ( repaired ? "true" : "false" );
    if( scan.HasValidHeader() )
    {
        std::cout << ",\n  \"format_version\": \"" << tracy::stream::JournalVersionMajor << "." << tracy::stream::JournalVersionMinor << "\",\n"
                  << "  \"protocol_version\": " << scan.header.protocolVersion;
    }
    if( !scan.records.empty() )
    {
        std::cout << ",\n  \"records\": [";
        for( size_t i = 0; i < scan.records.size(); i++ )
        {
            const auto& record = scan.records[i];
            if( i != 0 ) std::cout << ',';
            std::cout << "\n    {\"sequence\": " << record.sequence
                      << ", \"offset\": " << record.offset
                      << ", \"type\": \"" << tracy::stream::RecordTypeName( record.type )
                      << "\", \"flags\": " << record.flags
                      << ", \"monotonic_ns\": " << record.monotonicNs
                      << ", \"payload_size\": " << record.payloadSize << "}";
        }
        std::cout << "\n  ]";
    }
    std::cout << "\n}\n";
}

int ExitCode( const tracy::stream::ScanResult& scan )
{
    using tracy::stream::ScanCode;
    switch( scan.code )
    {
    case ScanCode::Ok: return 0;
    case ScanCode::TruncatedTail:
    case ScanCode::CorruptTail: return 2;
    case ScanCode::TruncatedFileHeader:
    case ScanCode::InvalidFileHeader: return 3;
    case ScanCode::IoError: return 4;
    default: return 4;
    }
}

bool ParseCount( std::string_view text, size_t& value )
{
    uint64_t parsed = 0;
    const auto result = std::from_chars( text.data(), text.data() + text.size(), parsed );
    if( result.ec != std::errc() || result.ptr != text.data() + text.size() || parsed > 1000000 ) return false;
    value = size_t( parsed );
    return true;
}

}

int main( int argc, char** argv )
{
    if( argc == 2 && std::string_view( argv[1] ) == "--version" )
    {
        std::cout << "tracy-stream " << TRACY_STREAM_VERSION << " (journal "
                  << tracy::stream::JournalVersionMajor << "." << tracy::stream::JournalVersionMinor << ")\n";
        return 0;
    }
    if( argc < 3 )
    {
        Usage();
        return 1;
    }

    const std::string_view command = argv[1];
    const std::filesystem::path path = std::filesystem::u8path( argv[2] );
    tracy::stream::ScanOptions options;
    options.maxCollectedRecords = 0;

    bool truncate = false;
    std::filesystem::path outputPath;
    for( int i = 3; i < argc; i++ )
    {
        const std::string_view argument = argv[i];
        if( argument == "--truncate" )
        {
            truncate = true;
        }
        else if( argument == "--records" && i + 1 < argc )
        {
            if( !ParseCount( argv[++i], options.maxCollectedRecords ) )
            {
                std::cerr << "Invalid --records count.\n";
                return 1;
            }
        }
        else if( argument == "--output" && i + 1 < argc )
        {
            outputPath = std::filesystem::u8path( argv[++i] );
        }
        else
        {
            std::cerr << "Unknown or incomplete argument: " << argument << '\n';
            Usage();
            return 1;
        }
    }

    std::ofstream output;
    if( !outputPath.empty() )
    {
        output.open( outputPath, std::ios::binary | std::ios::trunc );
        if( !output )
        {
            std::cerr << "Unable to open output file.\n";
            return 1;
        }
        std::cout.rdbuf( output.rdbuf() );
    }

    if( command == "inspect" )
    {
        if( truncate )
        {
            std::cerr << "--truncate is only valid with recover.\n";
            return 1;
        }
        const auto scan = tracy::stream::ScanJournal( path, options );
        PrintScan( path, scan, false );
        return ExitCode( scan );
    }

    if( command == "recover" )
    {
        if( !truncate )
        {
            std::cerr << "recover requires the explicit --truncate option.\n";
            return 1;
        }
        const auto before = tracy::stream::ScanJournal( path, options );
        if( before.code == tracy::stream::ScanCode::Ok )
        {
            PrintScan( path, before, false );
            return 0;
        }
        std::string error;
        if( !tracy::stream::TruncateToValidPrefix( path, before, error ) )
        {
            std::cerr << "Recovery refused: " << error << '\n';
            PrintScan( path, before, false );
            return ExitCode( before );
        }
        const auto after = tracy::stream::ScanJournal( path, options );
        PrintScan( path, after, true );
        return ExitCode( after );
    }

    std::cerr << "Unknown command: " << command << '\n';
    Usage();
    return 1;
}
