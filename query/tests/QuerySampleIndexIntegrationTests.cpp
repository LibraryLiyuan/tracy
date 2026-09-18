#include "TracyQueryIndex.hpp"
#include "TracyWorkerTraceSource.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace
{

void Require( bool condition, const char* message )
{
    if( !condition ) throw std::runtime_error( message );
}

}

int main( int argc, char** argv )
{
    try
    {
        if( argc != 3 ) throw std::runtime_error( "expected sample Trace and isolated output directory" );
        namespace fs = std::filesystem;
        const fs::path root = argv[2];
        if( fs::exists( root ) ) throw std::runtime_error( "refusing existing output directory" );
        fs::create_directories( root );
        const auto input = root / "sample.tracy";
        fs::copy_file( argv[1], input );

        auto original = tracy::analysis::WorkerTraceSource::Open( input );
        const auto originalInfo = original->GetTraceInfo();
        Require( originalInfo.counts.samples != 0, "fixture has no main samples" );
        tracy::analysis::ScanRange range;
        range.startNs = std::numeric_limits<int64_t>::min();
        range.endNs = std::numeric_limits<int64_t>::max();
        range.limit = size_t( originalInfo.counts.samples + originalInfo.counts.contextSwitchSamples );
        const auto expected = original->ScanSampleEvents( range );
        Require( expected.size() == range.limit, "fixture sample counts and scan disagree" );
        original.reset();

        const auto manifest = tracy::query::QueryIndex::Build( input );
        std::ifstream manifestInput( manifest.manifestPath );
        const auto json = nlohmann::json::parse( manifestInput );
        Require( json.at( "sections" ).contains( "sample" ), "Query index did not publish a sample sidecar" );
        const auto indexedMain = std::stoull( json.at( "counts" ).at( "samples" ).get<std::string>() );
        const auto indexedContext = std::stoull( json.at( "counts" ).at( "context_switch_samples" ).get<std::string>() );
        Require( indexedMain + indexedContext == expected.size(), "sample query count changed" );
        Require( std::stoull( json.at( "sections" ).at( "sample" ).at( "count" ).get<std::string>() ) >= indexedMain,
            "sample sidecar omitted main sample records" );
        const auto sampleRoot = manifest.manifestPath.parent_path() /
            json.at( "sections" ).at( "sample" ).at( "file" ).get<std::string>();
        Require( fs::is_regular_file( sampleRoot / "query-spans" / "header.bin" ),
            "Query index did not publish the derived sample logical view" );

        bool rejectedUnboundCompact = false;
        try { auto compact = tracy::analysis::WorkerTraceSource::Open( manifest.dataPath ); }
        catch( const tracy::analysis::TraceLoadError& error )
        { rejectedUnboundCompact = error.code == tracy::analysis::TraceLoadErrorCode::Corrupt; }
        Require( rejectedUnboundCompact, "manifest-bound compact payload opened without its external dictionary" );

        auto indexed = tracy::query::QueryIndex::Open( manifest );
        Require( !indexed->SampleDerivedDataReady(),
            "indexed trace silently claims Worker-derived Sample data is ready" );
        const auto indexedInfo = indexed->GetTraceInfo();
        Require( indexedInfo.counts.samples == originalInfo.counts.samples,
            "indexed trace changed main sample count" );
        Require( indexedInfo.counts.contextSwitchSamples == originalInfo.counts.contextSwitchSamples,
            "indexed trace changed context-switch sample count" );
        const auto actual = indexed->ScanSampleEvents( range );
        Require( actual.size() == expected.size(), "indexed trace lost sample events" );
        for( size_t index = 0; index < expected.size(); index++ )
        {
            Require( actual[index].ref == expected[index].ref && actual[index].threadRef == expected[index].threadRef &&
                actual[index].timeNs == expected[index].timeNs && actual[index].callstack == expected[index].callstack &&
                actual[index].callstackRef == expected[index].callstackRef && actual[index].kind == expected[index].kind,
                "indexed sample event changed" );
        }
        std::cout << "sample index integration passed records=" << actual.size() << '\n';
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
