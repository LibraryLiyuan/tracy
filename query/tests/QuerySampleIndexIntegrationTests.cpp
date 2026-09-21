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
        const auto expectedGhostZones = original->ScanGhostZones( range );
        const auto expectedSymbols = original->GetSymbols();
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
        Require( indexed->SampleDerivedDataReady(),
            "indexed trace did not publish complete Sample derived data" );
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
        const auto actualGhostZones = indexed->ScanGhostZones( range );
        const auto actualSymbols = indexed->GetSymbols();
        Require( actualGhostZones.size() == expectedGhostZones.size(),
            "indexed compact Worker changed ghost-zone availability" );
        for( size_t index = 0; index < expectedGhostZones.size(); index++ )
        {
            const auto& expectedZone = expectedGhostZones[index];
            const auto& actualZone = actualGhostZones[index];
            Require( actualZone.ref == expectedZone.ref && actualZone.threadRef == expectedZone.threadRef &&
                actualZone.parentRef == expectedZone.parentRef && actualZone.startNs == expectedZone.startNs &&
                actualZone.endNs == expectedZone.endNs && actualZone.name == expectedZone.name &&
                actualZone.file == expectedZone.file && actualZone.line == expectedZone.line &&
                actualZone.address == expectedZone.address && actualZone.depth == expectedZone.depth &&
                actualZone.childCount == expectedZone.childCount && actualZone.inlineFrame == expectedZone.inlineFrame,
                "indexed ghost zone changed" );
        }
        if( expectedGhostZones.size() >= 2 )
        {
            auto pageRange = range;
            pageRange.offset = 1;
            pageRange.limit = 1;
            const auto page = indexed->ScanGhostZones( pageRange );
            Require( page.size() == 1 && page.front().ref == expectedGhostZones[1].ref,
                "indexed ghost-zone ordinal pagination changed" );
        }
        if( !expectedGhostZones.empty() )
        {
            auto timeRange = range;
            timeRange.startNs = expectedGhostZones.front().startNs;
            timeRange.endNs = expectedGhostZones.front().startNs + 1;
            timeRange.offset = 0;
            timeRange.limit = expectedGhostZones.size();
            const auto expectedTime = originalInfo.counts.ghostZones == 0 ? std::vector<tracy::analysis::GhostZoneDto>() :
                expectedGhostZones;
            size_t intersections = 0;
            for( const auto& zone : expectedTime )
                if( zone.startNs < timeRange.endNs && zone.endNs > timeRange.startNs ) intersections++;
            Require( indexed->ScanGhostZones( timeRange ).size() == intersections,
                "indexed ghost-zone time pagination changed" );
        }
        Require( actualSymbols.size() == expectedSymbols.size(),
            "indexed compact Worker changed symbol-statistics availability" );
        for( size_t index = 0; index < expectedSymbols.size(); index++ )
        {
            const auto& expectedSymbol = expectedSymbols[index];
            const auto& actualSymbol = actualSymbols[index];
            Require( actualSymbol.ref == expectedSymbol.ref && actualSymbol.address == expectedSymbol.address &&
                actualSymbol.name == expectedSymbol.name && actualSymbol.file == expectedSymbol.file &&
                actualSymbol.line == expectedSymbol.line && actualSymbol.size == expectedSymbol.size &&
                actualSymbol.inclusiveSamples == expectedSymbol.inclusiveSamples &&
                actualSymbol.exclusiveSamples == expectedSymbol.exclusiveSamples &&
                actualSymbol.childSamples == expectedSymbol.childSamples && actualSymbol.hasCode == expectedSymbol.hasCode &&
                actualSymbol.imageName == expectedSymbol.imageName && actualSymbol.callFile == expectedSymbol.callFile &&
                actualSymbol.callLine == expectedSymbol.callLine && actualSymbol.inlineFrame == expectedSymbol.inlineFrame,
                "indexed symbol statistics changed" );
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
