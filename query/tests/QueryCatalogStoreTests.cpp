#include "TracyOfflineCatalog.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#if __has_include( "TracyQueryCatalogStore.hpp" )
#  include "TracyQueryCatalogStore.hpp"
#  define TRACY_HAS_QUERY_CATALOG_STORE 1
#else
#  define TRACY_HAS_QUERY_CATALOG_STORE 0
#endif

namespace
{

void Require( bool condition, const char* message )
{
    if( !condition ) throw std::runtime_error( message );
}

}

int main()
{
    try
    {
#if !TRACY_HAS_QUERY_CATALOG_STORE
        throw std::runtime_error( "Query Catalog store is not implemented" );
#else
        namespace fs = std::filesystem;
        using namespace tracy;
        using namespace tracy::query;
        const auto root = fs::temp_directory_path() / "tracy-query-catalog-store-test";
        std::error_code ignored;
        fs::remove_all( root, ignored );
        const std::string identity( 64, 'd' );

        QueryCatalogStoreWriter writer( root, identity );
        writer.SetMetadata( { 1, 1, JnGpuCatalogSchemaVersion, JnGpuDetailedEvidenceSchemaVersion } );
        JnGpuCatalogResourceRecordV1 resources[3] {};
        resources[0].time = 10; resources[0].resourceId = 100; resources[0].capacityBytes = 4096;
        resources[1].time = 20; resources[1].resourceId = 101; resources[1].capacityBytes = 8192;
        resources[2].time = 30; resources[2].resourceId = 102; resources[2].capacityBytes = 16384;
        writer.Begin( OfflineCatalogDomain::CatalogResource, 3, sizeof( resources[0] ) );
        writer.Append( OfflineCatalogDomain::CatalogResource, 0, resources, 2, sizeof( resources[0] ) );
        writer.Append( OfflineCatalogDomain::CatalogResource, 2, resources + 2, 1, sizeof( resources[0] ) );
        writer.End( OfflineCatalogDomain::CatalogResource );

        JnGpuCatalogStringRecordHeaderV1 header {};
        header.stringId = 7; header.originalLength = 4; header.byteLength = 4;
        header.hash = JnGpuCatalogChecksum64( "main", 4 );
        writer.BeginStrings( 1 );
        writer.CatalogString( 0, 9, header, "main" );
        writer.EndStrings();
        writer.Finish();

        QueryCatalogStoreReader reader( root, identity );
        const auto metadata = reader.Metadata();
        Require( metadata[0] == 1 && metadata[1] == 1 && metadata[2] == JnGpuCatalogSchemaVersion,
            "Catalog metadata changed" );
        Require( reader.Count( OfflineCatalogDomain::CatalogResource ) == 3,
            "Catalog resource count changed" );
        Require( reader.Count( OfflineCatalogDomain::CatalogAllocation ) == 0,
            "missing Catalog domain was not published empty" );
        JnGpuCatalogResourceRecordV1 value {};
        reader.Read( OfflineCatalogDomain::CatalogResource, 1, &value, 1, sizeof( value ) );
        Require( value.resourceId == 101 && value.capacityBytes == 8192,
            "Catalog random read changed" );
        const auto text = reader.String( 0 );
        Require( text.generation == 9 && text.header.stringId == 7 && text.value == "main",
            "Catalog string changed" );
        reader.ValidateAll();

        bool identityRejected = false;
        try { QueryCatalogStoreReader mismatch( root, std::string( 64, 'e' ) ); }
        catch( const std::exception& ) { identityRejected = true; }
        Require( identityRejected, "Catalog store accepted another Trace identity" );
        fs::remove_all( root, ignored );
        std::cout << "query Catalog store passed\n";
        return 0;
#endif
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
