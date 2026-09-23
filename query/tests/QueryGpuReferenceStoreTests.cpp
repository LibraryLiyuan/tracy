#include "TracyQueryGpuReferenceStore.hpp"
#include "TracyQueryCatalogStore.hpp"
#include "TracyAnalysisExternalSort.hpp"
#include "TracyAnalysisDiskBudget.hpp"
#include "TracyAnalysisWorkspaceBudget.hpp"
#include "TracyHash.hpp"
#include "TracyPagedRecordStore.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{

void Require( bool condition, const char* message )
{
    if( !condition ) throw std::runtime_error( message );
}

class Input final : public tracy::query::QueryGpuReferenceInput
{
public:
    uint64_t PassCount() const override { return passes.size(); }
    uint64_t UseCount() const override { return uses.size(); }
    uint64_t EndCount() const override { return ends.size(); }
    tracy::JnGpuReferencePassData Pass( uint64_t ordinal ) const override { return passes.at( size_t( ordinal ) ); }
    tracy::JnGpuReferenceUseData Use( uint64_t ordinal ) const override { return uses.at( size_t( ordinal ) ); }
    tracy::JnGpuReferenceEndData End( uint64_t ordinal ) const override { return ends.at( size_t( ordinal ) ); }
    uint64_t ResolveCatalogResource( uint64_t pointerToken, int64_t time ) const override
    {
        if( pointerToken != 100 ) return 0;
        return time <= 20 ? 200 : 201;
    }

    std::vector<tracy::JnGpuReferencePassData> passes;
    std::vector<tracy::JnGpuReferenceUseData> uses;
    std::vector<tracy::JnGpuReferenceEndData> ends;
};

class GeneratedInput final : public tracy::query::QueryGpuReferenceInput
{
public:
    GeneratedInput( uint64_t useCount, uint64_t resourceCount )
        : m_uses( useCount ), m_resources( resourceCount ) {}

    uint64_t PassCount() const override { return ( m_uses + 31 ) / 32; }
    uint64_t UseCount() const override { return m_uses; }
    uint64_t EndCount() const override { return PassCount(); }
    tracy::JnGpuReferencePassData Pass( uint64_t ordinal ) const override
    { return { int64_t( ordinal * 100 ), ordinal + 1, ordinal, 1, 1, 0, 0 }; }
    tracy::JnGpuReferenceUseData Use( uint64_t ordinal ) const override
    {
        const auto passId = ordinal % PassCount() + 1;
        return { int64_t( ( passId - 1 ) * 100 + 1 ), passId,
            ordinal % m_resources + 1, 1, 1, 1, 0, 1 };
    }
    tracy::JnGpuReferenceEndData End( uint64_t ordinal ) const override
    { return { int64_t( ordinal * 100 + 99 ), ordinal + 1, 0, 1, 32, 0, 0 }; }
    uint64_t ResolveCatalogResource( uint64_t pointerToken, int64_t ) const override
    { return pointerToken; }

private:
    uint64_t m_uses;
    uint64_t m_resources;
};

int CapacityProbe( uint64_t useCount, uint64_t resourceCount, uint64_t maximumBytes,
    bool inventory = false )
{
    namespace fs = std::filesystem;
    using namespace tracy;
    using namespace tracy::analysis;
    using namespace tracy::query;
    if( useCount < 100000 || useCount > 10000000 || resourceCount == 0 ||
        resourceCount > useCount || maximumBytes == 0 || maximumBytes > ( 20ull << 30 ) ) return 2;
    const auto root = fs::temp_directory_path() /
        ( "query-gpu-reference-capacity-" +
            std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        fs::create_directories( root );
        auto usage = std::make_shared<AnalysisDiskUsage>( std::vector<fs::path>{ root } );
        PagedRecordOptions options;
        options.disk = std::make_shared<AnalysisDiskBudget>(
            AnalysisDiskBudget{ usage, maximumBytes, {} } );
        options.workspace = std::make_shared<AnalysisWorkspaceBudget>( 4ull << 30, 2ull << 30 );
        const auto started = std::chrono::steady_clock::now();
        options.cancelled = [started] {
            return std::chrono::steady_clock::now() - started > std::chrono::minutes( 10 );
        };
        std::vector<std::string> inventoryRows;
        uint64_t nextSample = 16ull << 20;
        if( inventory ) options.disk->checkSpace = [&] {
            const auto current = usage->Current();
            if( current < nextSample ) return;
            nextSample = current + ( 16ull << 20 );
            const auto scratch = root / "reverse-scratch";
            uint64_t source = 0, sorted = 0, runs = 0, other = 0;
            if( fs::exists( scratch ) )
                for( const auto& item : fs::directory_iterator( scratch ) )
                {
                    if( !item.is_regular_file() ) continue;
                    const auto bytes = item.file_size();
                    const auto name = item.path().filename().string();
                    if( name.ends_with( "-source.work" ) ) source += bytes;
                    else if( name.ends_with( "-sorted.work" ) ) sorted += bytes;
                    else if( name.find( "-run-" ) != std::string::npos ) runs += bytes;
                    else other += bytes;
                }
            std::ostringstream line;
            line << "gpu_reference_inventory ledger=" << current << " source=" << source
                << " sorted=" << sorted << " runs=" << runs << " other=" << other;
            inventoryRows.emplace_back( line.str() );
        };
        GeneratedInput input( useCount, resourceCount );
        const std::string identity( 64, 'g' );
        const auto buildStarted = std::chrono::steady_clock::now();
        QueryGpuReferenceStore::Build( root, identity, input, options );
        const auto buildMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - buildStarted ).count();
        {
            QueryGpuReferenceStoreReader reader( root, identity, options );
            Require( reader.UseCount() == useCount, "GPU reference capacity use count changed" );
            const auto first = reader.UsesByPass( 1, 0, 1 );
            const auto last = reader.UsesByPass( input.PassCount(), 0, 1 );
            Require( first.total != 0 && last.total != 0 &&
                first.values.front().passId == 1 && last.values.front().passId == input.PassCount(),
                "GPU reference capacity pass identity changed" );
            if( useCount <= 1000000 )
                reader.ValidateAll( [&]( uint64_t token, int64_t time ) {
                    return input.ResolveCatalogResource( token, time );
                } );
        }
        const auto observed = usage->Snapshot();
        std::cout << "gpu_reference_uses=" << useCount << " passes=" << input.PassCount()
            << " resource_keys=" << resourceCount << " peak_bytes=" << observed.peakBytes
            << " final_bytes=" << observed.currentBytes
            << " build_elapsed_ms=" << buildMilliseconds << '\n';
        if( inventory )
            for( const auto& line : inventoryRows ) std::cout << line << '\n';
        std::error_code cleanup;
        fs::remove_all( root, cleanup );
        if( cleanup ) throw std::runtime_error( "GPU reference capacity cleanup: " + cleanup.message() );
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        fs::remove_all( root, ignored );
        return 1;
    }
}

}

int main( int argc, char** argv )
{
    if( argc == 5 && std::string_view( argv[1] ) == "--capacity" )
        return CapacityProbe( std::stoull( argv[2] ), std::stoull( argv[3] ), std::stoull( argv[4] ) );
    if( argc == 5 && std::string_view( argv[1] ) == "--capacity-inventory" )
        return CapacityProbe( std::stoull( argv[2] ), std::stoull( argv[3] ), std::stoull( argv[4] ), true );
    try
    {
        namespace fs = std::filesystem;
        if( argc == 6 && std::string_view( argv[1] ) == "--validate-existing" )
        {
            const auto referenceRoot = fs::path( argv[2] );
            const auto catalogRoot = fs::path( argv[3] );
            const std::string source = argv[4];
            const auto scratch = fs::path( argv[5] );
            if( source.size() != 64 || fs::exists( scratch ) )
                throw std::runtime_error( "GPU reference diagnostic identity or scratch path" );
            tracy::analysis::PagedRecordOptions options;
            options.workspace = std::make_shared<tracy::analysis::AnalysisWorkspaceBudget>( 4ull << 30, 2ull << 30 );
            options.disk = std::make_shared<tracy::analysis::AnalysisDiskBudget>();
            options.disk->usage = std::make_shared<tracy::analysis::AnalysisDiskUsage>(
                std::vector<fs::path> { scratch } );
            options.disk->maximumBytes = 120ull << 30;
            tracy::query::QueryCatalogStoreReader catalog( catalogRoot, source, options );
            if( !catalog.PreparePointerResolutionFastPath() )
                throw std::runtime_error( "GPU reference diagnostic pointer fast path was unavailable" );
            tracy::query::QueryGpuReferenceStoreReader reader( referenceRoot, source, options );
            const auto start = std::chrono::steady_clock::now();
            const auto stats = reader.ValidateAll( [&]( uint64_t pointerToken, int64_t time ) {
                return catalog.ResolveResourcePointer( pointerToken, time );
            }, scratch );
            const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start ).count();
            std::cout << "gpu_reference_real_validation elapsed_ms=" << milliseconds
                << " use_block_loads=" << stats.useBlockLoads
                << " use_decoded_bytes=" << stats.useDecodedBytes
                << " sorted_references=" << stats.sortedReferenceCount
                << " scratch_peak_bytes=" << options.disk->usage->Snapshot().peakBytes
                << " scratch_remaining_bytes=" << options.disk->usage->Snapshot().currentBytes << '\n';
            return 0;
        }
        if( argc == 5 && std::string_view( argv[1] ) == "--validation-probe" )
        {
            const auto root = fs::path( argv[2] );
            const std::string source = argv[3];
            const auto count = std::stoull( argv[4] );
            if( source.size() != 64 || count == 0 || count > 4096 )
                throw std::runtime_error( "GPU reference validation probe arguments" );
            const auto identity = [&]( std::string_view kind ) {
                tracy::analysis::Sha256Builder hash;
                hash.Update( source.data(), source.size() );
                hash.Update( kind.data(), kind.size() );
                constexpr std::string_view version = "query-gpu-reference-v1";
                hash.Update( version.data(), version.size() );
                return hash.FinalHex();
            };
            for( const uint64_t cacheBytes : { uint64_t( 0 ), uint64_t( 64 ) << 20 } )
            {
                tracy::analysis::PagedRecordOptions options;
                options.readerCacheBytes = cacheBytes;
                options.workspace = std::make_shared<tracy::analysis::AnalysisWorkspaceBudget>( 4ull << 30, 2ull << 30 );
                tracy::analysis::PagedRecordReader lookup( root / "by-resource", identity( "by-resource" ),
                    sizeof( tracy::analysis::AnalysisKeyMetadataRecord ), options );
                tracy::analysis::PagedRecordReader uses( root / "resolved-uses", identity( "resolved-uses" ),
                    sizeof( tracy::query::QueryGpuReferenceResolvedUseRecord ), options );
                const auto start = std::chrono::steady_clock::now();
                uint64_t hash = 14695981039346656037ull;
                uint64_t minimum = UINT64_MAX, maximum = 0;
                for( uint64_t ordinal = 0; ordinal < count; ordinal++ )
                {
                    tracy::analysis::AnalysisKeyMetadataRecord row {};
                    lookup.Read( ordinal, &row, 1 );
                    if( row.metadata >= uses.Count() ) throw std::runtime_error( "GPU reference probe ordinal" );
                    tracy::query::QueryGpuReferenceResolvedUseRecord use {};
                    uses.Read( row.metadata, &use, 1 );
                    if( use.resourceId != row.keyHigh ) throw std::runtime_error( "GPU reference probe identity" );
                    minimum = std::min( minimum, row.metadata );
                    maximum = std::max( maximum, row.metadata );
                    hash ^= use.resourceId ^ use.passId ^ row.metadata;
                    hash *= 1099511628211ull;
                }
                const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start ).count();
                const auto stats = uses.Statistics();
                std::cout << "gpu_reference_validation_probe cache_bytes=" << cacheBytes
                    << " count=" << count << " elapsed_ms=" << milliseconds
                    << " minimum_use_ordinal=" << minimum << " maximum_use_ordinal=" << maximum
                    << " use_block_loads=" << stats.blockLoads << " use_decoded_bytes=" << stats.decodedBytes
                    << " use_decompressions=" << stats.decompressions << " hash=" << hash << '\n';
            }
            return 0;
        }
        const auto root = fs::temp_directory_path() / "tracy-query-gpu-reference-store";
        std::error_code ignored;
        fs::remove_all( root, ignored );
        fs::create_directories( root );
        Input input;
        input.passes = { { 10, 1, 0, 1, 0, 0, 0 }, { 30, 2, 0, 1, 0, 0, 0 } };
        input.ends = { { 20, 1, 0, 1, 0, 0, 0 } };
        input.uses = {
            { 30, 2, 100, 1, 5, 8, 0, 1 },
            { 10, 1, 100, 1, 3, 7, 0, 2 },
            { 10, 1, 0, 1, 1, 0, 0, 1 },
            { 40, 999, 100, 1, 1, 0, 0, 1 }
        };
        const std::string identity( 64, 'a' );
        tracy::query::QueryGpuReferenceStore::Build( root, identity, input );
        Require( !fs::exists( root / "reverse-scratch" ), "GPU reference scratch was retained" );
        tracy::query::QueryGpuReferenceStoreReader reader( root, identity );
        reader.ValidateAll( [&]( uint64_t pointerToken, int64_t time ) {
            return input.ResolveCatalogResource( pointerToken, time );
        } );
        const auto first = reader.UsesByResource( 100, 0, 1 );
        const auto second = reader.UsesByResource( 100, 1, 1 );
        Require( first.total == 2 && first.values.size() == 1 && first.values[0].passId == 2 &&
            first.values[0].passStart == 30 && first.values[0].passEnd == 30 &&
            first.values[0].usageMask == 5 && first.values[0].resourceSetId == 8,
            "GPU reference first resource page changed" );
        Require( second.total == 2 && second.values.size() == 1 && second.values[0].passId == 1 &&
            second.values[0].passStart == 10 && second.values[0].passEnd == 20 &&
            second.values[0].encoding == 2,
            "GPU reference continuation or open pass boundary changed" );
        Require( reader.UsesByResource( 999, 0, 10 ).total == 0,
            "GPU reference missing resource behavior changed" );
        const auto catalog200 = reader.UsesByCatalogResource( 200, 0, 10 );
        const auto catalog201 = reader.UsesByCatalogResource( 201, 0, 10 );
        Require( catalog200.total == 1 && catalog200.values.size() == 1 &&
            catalog200.values[0].passId == 1 && catalog200.values[0].resourceId == 100 &&
            catalog201.total == 1 && catalog201.values.size() == 1 &&
            catalog201.values[0].passId == 2 && catalog201.values[0].resourceId == 100,
            "GPU reference Catalog resource lifetime resolution changed" );
        const auto pass1 = reader.UsesByPass( 1, 0, 10 );
        const auto pass2 = reader.UsesByPass( 2, 0, 10 );
        Require( pass1.total == 1 && pass1.values.size() == 1 && pass1.values[0].passId == 1 &&
            pass2.total == 1 && pass2.values.size() == 1 && pass2.values[0].passId == 2 &&
            reader.UsesByPass( 999, 0, 10 ).total == 0,
            "GPU reference pass paging changed" );
        const auto scaleRoot = root.parent_path() / "tracy-query-gpu-reference-validation-scale";
        const auto scaleScratch = root.parent_path() / "tracy-query-gpu-reference-validation-scale-scratch";
        fs::remove_all( scaleRoot, ignored );
        fs::remove_all( scaleScratch, ignored );
        tracy::analysis::PagedRecordOptions scaleOptions;
        scaleOptions.blockBytes = 4096;
        scaleOptions.workspace = std::make_shared<tracy::analysis::AnalysisWorkspaceBudget>( 64ull << 20, 32ull << 20 );
        Input scaleInput;
        scaleInput.passes = { { 10, 1, 0, 1, 0, 0, 0 } };
        scaleInput.ends = { { 20, 1, 0, 1, 0, 0, 0 } };
        for( unsigned i = 0; i < 4096; i++ )
            scaleInput.uses.push_back( { 10, 1, 100 + i % 8, 1, 1, 0, 0, 1 } );
        tracy::query::QueryGpuReferenceStore::Build( scaleRoot, identity, scaleInput, scaleOptions );
        tracy::query::QueryGpuReferenceStoreReader scaleReader( scaleRoot, identity, scaleOptions );
        const auto validation = scaleReader.ValidateAll( [&]( uint64_t pointerToken, int64_t time ) {
            return scaleInput.ResolveCatalogResource( pointerToken, time );
        }, scaleScratch );
        Require( validation.sortedReferenceCount == scaleInput.uses.size() + scaleInput.uses.size() / 8 &&
            validation.useDecodedBytes <= scaleInput.uses.size() *
                sizeof( tracy::query::QueryGpuReferenceResolvedUseRecord ) * 5 &&
            !fs::exists( scaleScratch ),
            "GPU reference deep validation repeated compressed random reads or retained scratch" );
        const auto deniedScratch = root.parent_path() / "tracy-query-gpu-reference-validation-denied";
        fs::remove_all( deniedScratch, ignored );
        auto deniedOptions = scaleOptions;
        auto diskUsage = std::make_shared<tracy::analysis::AnalysisDiskUsage>(
            std::vector<fs::path> { deniedScratch } );
        deniedOptions.disk = std::make_shared<tracy::analysis::AnalysisDiskBudget>();
        deniedOptions.disk->usage = diskUsage;
        deniedOptions.disk->maximumBytes = 1;
        bool diskRejected = false;
        try
        {
            tracy::query::QueryGpuReferenceStoreReader denied( scaleRoot, identity, deniedOptions );
            denied.ValidateAll( {}, deniedScratch );
        }
        catch( const std::exception& error )
        { diskRejected = std::string_view( error.what() ).find( "analysis_scan_cache_disk_budget" ) != std::string_view::npos; }
        Require( diskRejected && !fs::exists( deniedScratch ),
            "GPU reference validation did not reject disk growth and clean its owned scratch" );
        const auto cancelledScratch = root.parent_path() / "tracy-query-gpu-reference-validation-cancelled";
        fs::remove_all( cancelledScratch, ignored );
        auto cancelledOptions = scaleOptions;
        cancelledOptions.cancelled = [&]() {
            std::error_code error;
            const auto source = cancelledScratch / "resource-source.work";
            return fs::exists( source, error ) && !error &&
                fs::file_size( source, error ) == scaleInput.uses.size() *
                    sizeof( tracy::analysis::AnalysisKeyMetadataRecord );
        };
        bool cancelledDuringSort = false;
        try
        {
            tracy::query::QueryGpuReferenceStoreReader cancelled( scaleRoot, identity, cancelledOptions );
            cancelled.ValidateAll( {}, cancelledScratch );
        }
        catch( const std::exception& error )
        { cancelledDuringSort = std::string_view( error.what() ).find( "analysis_metadata_sort_cancelled" ) != std::string_view::npos; }
        Require( cancelledDuringSort && !fs::exists( cancelledScratch ),
            "GPU reference validation did not cancel external sorting and clean scratch" );
        const auto corruptedRoot = root.parent_path() / "tracy-query-gpu-reference-validation-corrupt";
        const auto corruptedScratch = root.parent_path() / "tracy-query-gpu-reference-validation-corrupt-scratch";
        fs::remove_all( corruptedRoot, ignored );
        fs::remove_all( corruptedScratch, ignored );
        tracy::query::QueryGpuReferenceStore::Build( corruptedRoot, identity, scaleInput, scaleOptions );
        tracy::analysis::Sha256Builder lookupHash;
        lookupHash.Update( identity.data(), identity.size() );
        constexpr std::string_view lookupKind = "by-resource";
        lookupHash.Update( lookupKind.data(), lookupKind.size() );
        constexpr std::string_view lookupVersion = "query-gpu-reference-v1";
        lookupHash.Update( lookupVersion.data(), lookupVersion.size() );
        const auto lookupIdentity = lookupHash.FinalHex();
        const auto replacement = corruptedRoot / "by-resource-replacement";
        {
            tracy::analysis::PagedRecordReader original( corruptedRoot / "by-resource", lookupIdentity,
                sizeof( tracy::analysis::AnalysisKeyMetadataRecord ), scaleOptions );
            tracy::analysis::PagedRecordWriter changed( replacement, lookupIdentity,
                sizeof( tracy::analysis::AnalysisKeyMetadataRecord ), scaleOptions );
            for( uint64_t ordinal = 0; ordinal < original.Count(); ordinal++ )
            {
                tracy::analysis::AnalysisKeyMetadataRecord row {};
                original.Read( ordinal, &row, 1 );
                if( ordinal == 0 ) row.metadata = 1;
                changed.Append( &row, 1 );
            }
            changed.Commit();
        }
        fs::remove_all( corruptedRoot / "by-resource" );
        fs::rename( replacement, corruptedRoot / "by-resource" );
        bool rejectedCorruption = false;
        try
        {
            tracy::query::QueryGpuReferenceStoreReader corrupted( corruptedRoot, identity, scaleOptions );
            corrupted.ValidateAll( [&]( uint64_t pointerToken, int64_t time ) {
                return scaleInput.ResolveCatalogResource( pointerToken, time );
            }, corruptedScratch );
        }
        catch( const std::exception& error )
        { rejectedCorruption = std::string_view( error.what() ).find( "resource lookup ordinal mismatch" ) != std::string_view::npos; }
        Require( rejectedCorruption && !fs::exists( corruptedScratch ),
            "GPU reference validation accepted a valid-checksum but missing Use ordinal" );
        fs::remove_all( corruptedRoot, ignored );
        fs::remove_all( scaleRoot, ignored );
        fs::remove_all( root, ignored );
        std::cout << "query GPU reference store passed\n";
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
