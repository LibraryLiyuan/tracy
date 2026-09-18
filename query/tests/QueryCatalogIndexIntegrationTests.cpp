#include "TracyQueryCatalogStore.hpp"
#include "TracyQueryIndex.hpp"
#include "TracyWorkerTraceSource.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

#ifdef _WIN32
#  include <windows.h>
#endif

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
        if( argc != 3 ) throw std::runtime_error( "expected Catalog Trace and isolated output directory" );
        namespace fs = std::filesystem;
        using namespace tracy;
        const fs::path root = argv[2];
        if( fs::exists( root ) ) throw std::runtime_error( "refusing existing output directory" );
        fs::create_directories( root );
        const auto input = root / "catalog.tracy";
        fs::copy_file( argv[1], input );

        auto original = analysis::WorkerTraceSource::Open( input );
        auto catalog = original->GetGpuCatalogData();
        Require( catalog && catalog->gpuCatalogPresent && !catalog->gpuCatalogResources.empty(),
            "fixture has no GPU Catalog resources" );
        const auto expectedResources = catalog->gpuCatalogResources.size();
        const auto expectedBatches = catalog->gpuCatalogBatches.size();
        const auto expectedStrings = catalog->gpuCatalogStrings.size();
        original.reset(); catalog.reset();

        const auto manifest = query::QueryIndex::Build( input );
        std::ifstream manifestInput( manifest.manifestPath );
        const auto json = nlohmann::json::parse( manifestInput );
        manifestInput.close();
        Require( json.at( "sections" ).contains( "gpu_catalog" ),
            "Query index did not publish a GPU Catalog sidecar" );
        const auto catalogRoot = manifest.manifestPath.parent_path() /
            json.at( "sections" ).at( "gpu_catalog" ).at( "file" ).get<std::string>();
        {
            query::QueryCatalogStoreReader reader( catalogRoot, manifest.sourceFingerprint );
            Require( reader.Count( OfflineCatalogDomain::CatalogResource ) == expectedResources,
                "Catalog resource sidecar count changed" );
            Require( reader.Count( OfflineCatalogDomain::CatalogBatch ) == expectedBatches,
                "Catalog batch sidecar count changed" );
            Require( reader.Count( OfflineCatalogDomain::CatalogString ) == expectedStrings,
                "Catalog string sidecar count changed" );
        }

        bool rejectedUnboundCompact = false;
        try { auto compact = analysis::WorkerTraceSource::Open( manifest.dataPath ); }
        catch( const analysis::TraceLoadError& error )
        { rejectedUnboundCompact = error.code == analysis::TraceLoadErrorCode::Corrupt; }
        Require( rejectedUnboundCompact, "manifest-bound compact payload opened without its external dictionary" );
        auto indexed = query::QueryIndex::Open( manifest );
        Require( !indexed->CatalogDerivedDataReady(),
            "indexed trace silently claims GPU Catalog indices are ready" );
        const auto* statusSource = dynamic_cast<const query::QueryCatalogStatusSource*>( indexed.get() );
        Require( statusSource != nullptr, "indexed trace does not expose bounded Catalog status" );
        const auto status = statusSource->GetQueryCatalogStatus( 64, 16 );
        const auto& validation = statusSource->GetQueryCatalogValidation();
        Require( validation.complete && validation.healthy && status.metadata[0] == 1 &&
            status.counts[size_t( OfflineCatalogDomain::CatalogResource )] == expectedResources &&
            status.counts[size_t( OfflineCatalogDomain::CatalogBatch )] == expectedBatches,
            "indexed bounded Catalog status changed" );
        const auto activeData = manifest.dataPath;
        const std::string generationPrefix = "jnidx." + manifest.sourceFingerprint.substr( 0, 16 ) +
            ".s" + std::to_string( query::QueryIndexSchemaVersion ) + ".g";
        const auto orphanFinal = root / ( generationPrefix + "1.jnidx.cpu" );
        std::ofstream( orphanFinal ) << "orphan";
        auto cleanupReport = manifest.manifestPath;
        cleanupReport += ".cleanup.json";
#ifdef _WIN32
        HANDLE blockedOrphan = CreateFileW( orphanFinal.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );
        Require( blockedOrphan != INVALID_HANDLE_VALUE, "could not hold orphan generation file" );
        bool cleanupRejected = false;
        try { query::QueryIndex::Build( input ); }
        catch( const std::exception& error )
        { cleanupRejected = std::string( error.what() ).find( "generation cleanup failed" ) != std::string::npos; }
        CloseHandle( blockedOrphan );
        Require( cleanupRejected && fs::is_regular_file( cleanupReport ) && fs::exists( activeData ),
            "occupied orphan generation did not fail before replacing the active manifest" );
        {
            std::ifstream cleanupInput( cleanupReport );
            const auto cleanupJson = nlohmann::json::parse( cleanupInput );
            Require( cleanupJson.at( "state" ) == "failed" &&
                std::stoull( cleanupJson.at( "retained_count" ).get<std::string>() ) != 0,
                "occupied orphan generation failure report lost its retained files" );
        }
#endif
        bool observedWorkerLoad = false;
        bool orphanAbsentAtWorkerLoad = false;
        const auto second = query::QueryIndex::Build( input, [&]( analysis::TraceSourceState ) {
            if( observedWorkerLoad ) return;
            observedWorkerLoad = true;
            orphanAbsentAtWorkerLoad = !fs::exists( orphanFinal );
        } );
        Require( observedWorkerLoad && orphanAbsentAtWorkerLoad && fs::exists( activeData ) && !fs::exists( orphanFinal ),
            "build-start cleanup removed an active generation or retained an unreferenced final file" );
        Require( fs::is_regular_file( cleanupReport ), "deferred generation cleanup did not persist a report" );
        {
            std::ifstream cleanupInput( cleanupReport );
            const auto cleanupJson = nlohmann::json::parse( cleanupInput );
            Require( cleanupJson.at( "state" ) == "deferred" &&
                std::stoull( cleanupJson.at( "retained_count" ).get<std::string>() ) != 0,
                "deferred generation cleanup report lost its retained files" );
        }
        indexed.reset();
        auto currentBase = second.dataPath.filename().string();
        const std::string dataSuffix = ".jnidx.data";
        currentBase.erase( currentBase.size() - dataSuffix.size() );
        std::vector<fs::path> retained;
        std::string cleanupError;
        Require( query::QueryIndex::CleanupUnreferencedGenerationsForTesting(
                root, generationPrefix, currentBase, retained, cleanupError ) &&
            !fs::exists( activeData ) && fs::exists( second.dataPath ) && retained.empty(),
            "unused Query generations were not reclaimed after their reader lease ended" );
        const auto third = query::QueryIndex::Build( input );
        Require( !fs::exists( cleanupReport ) && !fs::exists( second.dataPath ) && fs::exists( third.dataPath ),
            "successful generation cleanup retained a stale cleanup report or old generation" );
        std::cout << "GPU Catalog index integration passed resources=" << expectedResources << '\n';
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
