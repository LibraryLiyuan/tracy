#include "TracyGpuAnalysisSpoolCheckpoint.hpp"

#include "TracyGpuAnalysisCache.hpp"
#include "TracyGpuAnalysisPath.hpp"
#include "TracyHash.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>

#ifdef _WIN32
#  include <Windows.h>
#endif

namespace tracy::analysis
{
namespace
{

constexpr uint64_t CatalogMagic = 0x3150435343474aull; // JGCSCP1
constexpr uint64_t PassMagic = 0x3150435350474aull;    // JGPSCP1
constexpr uint32_t CheckpointSchema = 1;
constexpr uint64_t MaxEntries = 16ull * 1024 * 1024;
constexpr uint64_t MaxStringBytes = 16ull * 1024 * 1024;

template<typename T>
bool WritePod( std::ofstream& out, const T& value )
{
    static_assert( std::is_trivially_copyable_v<T> );
    out.write( reinterpret_cast<const char*>( &value ), sizeof( value ) );
    return bool( out );
}

template<typename T>
bool ReadPod( std::ifstream& in, T& value )
{
    static_assert( std::is_trivially_copyable_v<T> );
    in.read( reinterpret_cast<char*>( &value ), sizeof( value ) );
    return bool( in );
}

bool WriteString( std::ofstream& out, std::string_view value )
{
    const uint64_t bytes = value.size();
    return WritePod( out, bytes ) &&
        ( bytes == 0 || bool( out.write( value.data(), std::streamsize( bytes ) ) ) );
}

bool ReadString( std::ifstream& in, std::string& value )
{
    uint64_t bytes = 0;
    if( !ReadPod( in, bytes ) || bytes > MaxStringBytes ) return false;
    value.resize( size_t( bytes ) );
    return bytes == 0 || bool( in.read( value.data(), std::streamsize( bytes ) ) );
}

bool AtomicReplaceCheckpoint( const std::filesystem::path& source,
    const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    if( MoveFileExW( GpuAnalysisIoPath( source ).c_str(),
        GpuAnalysisIoPath( target ).c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) ) return true;
    error = "gpu_spool_checkpoint_atomic_replace_failed:" +
        std::to_string( GetLastError() );
    return false;
#else
    std::error_code ec;
    std::filesystem::rename( source, target, ec );
    if( !ec ) return true;
    error = "gpu_spool_checkpoint_atomic_replace_failed:" + ec.message();
    return false;
#endif
}

bool SafeRelative( const std::filesystem::path& relative )
{
    if( relative.empty() || relative.is_absolute() ) return false;
    for( const auto& part : relative ) if( part == ".." ) return false;
    return true;
}

struct FileEvidence
{
    std::filesystem::path relative;
    uint64_t bytes = 0;
    std::string sha256;
};

bool CaptureFile( const std::filesystem::path& root,
    const std::filesystem::path& path, FileEvidence& value, std::string& error )
{
    std::error_code ec;
    value.relative = std::filesystem::relative( path, root, ec );
    if( ec || !SafeRelative( value.relative ) )
    { error = "gpu_spool_checkpoint_path_invalid"; return false; }
    value.bytes = std::filesystem::file_size( GpuAnalysisIoPath( path ), ec );
    if( ec ) { error = "gpu_spool_checkpoint_file_missing:" + ec.message(); return false; }
    value.sha256 = Sha256File( GpuAnalysisIoPath( path ) );
    if( value.sha256.size() != 64 )
    { error = "gpu_spool_checkpoint_file_hash_failed"; return false; }
    return true;
}

bool WriteFileEvidence( std::ofstream& out, const FileEvidence& value )
{
    return WriteString( out, value.relative.generic_string() ) &&
        WritePod( out, value.bytes ) && WriteString( out, value.sha256 );
}

bool ReadAndVerifyFileEvidence( std::ifstream& in,
    const std::filesystem::path& root, std::filesystem::path& path,
    std::string& error )
{
    std::string relative, expectedSha; uint64_t expectedBytes = 0;
    if( !ReadString( in, relative ) || !ReadPod( in, expectedBytes ) ||
        !ReadString( in, expectedSha ) )
    { error = "gpu_spool_checkpoint_file_record_invalid"; return false; }
    const std::filesystem::path relativePath( relative );
    if( !SafeRelative( relativePath ) || expectedSha.size() != 64 )
    { error = "gpu_spool_checkpoint_file_identity_invalid"; return false; }
    path = root / relativePath;
    std::error_code ec;
    const auto bytes = std::filesystem::file_size( GpuAnalysisIoPath( path ), ec );
    if( ec || bytes != expectedBytes || Sha256File( GpuAnalysisIoPath( path ) ) != expectedSha )
    { error = "gpu_spool_checkpoint_file_mismatch:" + relative; return false; }
    return true;
}

void AddUniquePath( std::vector<std::filesystem::path>& paths,
    const std::filesystem::path& value )
{
    if( value.empty() ) return;
    if( std::find( paths.begin(), paths.end(), value ) == paths.end() )
        paths.push_back( value );
}

bool WritePathVector( std::ofstream& out, const std::filesystem::path& root,
    const std::vector<std::filesystem::path>& values, std::string& error )
{
    const uint64_t count = values.size();
    if( !WritePod( out, count ) ) return false;
    for( const auto& value : values )
    {
        FileEvidence evidence;
        if( !CaptureFile( root, value, evidence, error ) ||
            !WriteFileEvidence( out, evidence ) ) return false;
    }
    return true;
}

bool ReadPathVector( std::ifstream& in, const std::filesystem::path& root,
    std::vector<std::filesystem::path>& values, std::string& error )
{
    uint64_t count = 0;
    if( !ReadPod( in, count ) || count > MaxEntries )
    { error = "gpu_spool_checkpoint_path_count_invalid"; return false; }
    values.clear(); values.reserve( size_t( count ) );
    for( uint64_t i = 0; i < count; ++i )
    {
        std::filesystem::path path;
        if( !ReadAndVerifyFileEvidence( in, root, path, error ) ) return false;
        values.push_back( std::move( path ) );
    }
    return true;
}

bool WriteIdentity( std::ofstream& out, uint64_t magic,
    const GpuAnalysisTraceIdentity& identity )
{
    return WritePod( out, magic ) && WritePod( out, CheckpointSchema ) &&
        WriteString( out, identity.sha256 ) && WritePod( out, identity.fileSize );
}

bool ReadIdentity( std::ifstream& in, uint64_t expectedMagic,
    const GpuAnalysisTraceIdentity& identity, std::string& error )
{
    uint64_t magic = 0, sourceSize = 0; uint32_t schema = 0;
    std::string sourceSha;
    if( !ReadPod( in, magic ) || !ReadPod( in, schema ) ||
        !ReadString( in, sourceSha ) || !ReadPod( in, sourceSize ) ||
        magic != expectedMagic || schema != CheckpointSchema ||
        sourceSha != identity.sha256 || sourceSize != identity.fileSize )
    { error = "gpu_spool_checkpoint_identity_mismatch"; return false; }
    return true;
}

bool WritePage( std::ofstream& out, const GpuAnalysisStorePage& page )
{
    const auto kind = uint8_t( page.kind );
    return WritePod( out, kind ) && WritePod( out, page.firstIndex ) &&
        WritePod( out, page.recordCount ) && WritePod( out, page.firstKey ) &&
        WritePod( out, page.lastKey ) && WritePod( out, page.fileBytes ) &&
        WritePod( out, page.checksum ) &&
        WriteString( out, page.relativePath.generic_string() );
}

bool ReadPage( std::ifstream& in, GpuAnalysisStorePage& page )
{
    uint8_t kind = 0; std::string relative;
    if( !ReadPod( in, kind ) || !ReadPod( in, page.firstIndex ) ||
        !ReadPod( in, page.recordCount ) || !ReadPod( in, page.firstKey ) ||
        !ReadPod( in, page.lastKey ) || !ReadPod( in, page.fileBytes ) ||
        !ReadPod( in, page.checksum ) || !ReadString( in, relative ) ) return false;
    page.kind = GpuAnalysisStorePageKind( kind );
    page.relativePath = relative;
    return kind <= uint8_t( GpuAnalysisStorePageKind::PassChildIndex ) &&
        SafeRelative( page.relativePath );
}

}

bool SaveGpuCatalogSpoolCheckpoint( const GpuAnalysisCatalogSpool& spool,
    const GpuAnalysisTraceIdentity& identity, std::string& error )
{
    error.clear();
    const GpuAnalysisCacheIdentity cacheIdentity { identity.sha256, identity.fileSize,
        std::string( GpuAnalysisAlgorithmId ) + "-session-catalog-spool1" };
    const auto overviewPath = spool.root / "overview.cache";
    if( !SaveGpuAnalysisCache( overviewPath, cacheIdentity, spool.overview, error ) ) return false;
    std::vector<std::filesystem::path> files;
    AddUniquePath( files, overviewPath ); AddUniquePath( files, spool.resourceLookupPath );
    AddUniquePath( files, spool.allocationLookupPath ); AddUniquePath( files, spool.pointerLifetimePath );
    AddUniquePath( files, spool.stringIndexPath ); AddUniquePath( files, spool.stringDataPath );
    for( const auto& page : spool.pages ) AddUniquePath( files, spool.root / page.relativePath );
    const auto temporary = spool.root / "spool.meta.tmp";
    std::ofstream out( GpuAnalysisIoPath( temporary ), std::ios::binary | std::ios::trunc );
    if( !out || !WriteIdentity( out, CatalogMagic, identity ) )
    { error = "gpu_catalog_spool_checkpoint_open_failed"; return false; }
    const uint64_t counters[] = { spool.resourceCount, spool.allocationCount,
        spool.residencyCount, spool.churnCount, spool.pageCount,
        spool.peakRecordsInMemory };
    for( const auto value : counters ) if( !WritePod( out, value ) ) return false;
    const uint64_t pageCount = spool.pages.size();
    if( !WritePod( out, pageCount ) ) return false;
    for( const auto& page : spool.pages ) if( !WritePage( out, page ) ) return false;
    const uint64_t typeCount = spool.typeSummaries.size();
    if( !WritePod( out, typeCount ) ) return false;
    for( const auto& value : spool.typeSummaries )
        if( !WritePod( out, value.primaryKind ) || !WritePod( out, value.liveResourceCount ) ||
            !WritePod( out, value.resourceCapacityBytes ) ) return false;
    if( !WritePathVector( out, spool.root, files, error ) ) return false;
    out.flush(); if( !out ) { error = "gpu_catalog_spool_checkpoint_write_failed"; return false; }
    out.close(); return AtomicReplaceCheckpoint( temporary, spool.root / "spool.meta", error );
}

bool LoadGpuCatalogSpoolCheckpoint( const std::filesystem::path& root,
    const GpuAnalysisTraceIdentity& identity, GpuAnalysisCatalogSpool& spool,
    std::string& error )
{
    error.clear(); spool = {}; spool.root = root;
    std::ifstream in( GpuAnalysisIoPath( root / "spool.meta" ), std::ios::binary );
    if( !in ) { error = "gpu_catalog_spool_checkpoint_not_found"; return false; }
    if( !ReadIdentity( in, CatalogMagic, identity, error ) ) return false;
    uint64_t* counters[] = { &spool.resourceCount, &spool.allocationCount,
        &spool.residencyCount, &spool.churnCount, &spool.pageCount,
        &spool.peakRecordsInMemory };
    for( auto* value : counters ) if( !ReadPod( in, *value ) )
    { error = "gpu_catalog_spool_checkpoint_counter_invalid"; return false; }
    uint64_t pageCount = 0;
    if( !ReadPod( in, pageCount ) || pageCount > MaxEntries )
    { error = "gpu_catalog_spool_checkpoint_page_count_invalid"; return false; }
    spool.pages.resize( size_t( pageCount ) );
    for( auto& page : spool.pages ) if( !ReadPage( in, page ) )
    { error = "gpu_catalog_spool_checkpoint_page_invalid"; return false; }
    uint64_t typeCount = 0;
    if( !ReadPod( in, typeCount ) || typeCount > MaxEntries )
    { error = "gpu_catalog_spool_checkpoint_type_count_invalid"; return false; }
    spool.typeSummaries.resize( size_t( typeCount ) );
    for( auto& value : spool.typeSummaries )
        if( !ReadPod( in, value.primaryKind ) || !ReadPod( in, value.liveResourceCount ) ||
            !ReadPod( in, value.resourceCapacityBytes ) )
        { error = "gpu_catalog_spool_checkpoint_type_invalid"; return false; }
    std::vector<std::filesystem::path> files;
    if( !ReadPathVector( in, root, files, error ) ) return false;
    const GpuAnalysisCacheIdentity cacheIdentity { identity.sha256, identity.fileSize,
        std::string( GpuAnalysisAlgorithmId ) + "-session-catalog-spool1" };
    auto overview = LoadGpuAnalysisCache( root / "overview.cache", cacheIdentity, error );
    if( !overview ) { error = "gpu_catalog_spool_overview_invalid:" + error; return false; }
    spool.overview = std::move( *overview );
    spool.resourceLookupPath = root / "resource-lookup.bin";
    spool.allocationLookupPath = root / "allocation-lookup.bin";
    spool.allocationResourceCountPath = spool.allocationLookupPath;
    spool.pointerLifetimePath = root / "pointer-lifetimes.bin";
    spool.stringIndexPath = root / "strings.index";
    spool.stringDataPath = root / "strings.data";
    uint64_t resources = 0, allocations = 0, churn = 0;
    for( const auto& page : spool.pages )
    {
        auto snapshot = LoadGpuAnalysisCache( root / page.relativePath, cacheIdentity, error );
        if( !snapshot ) { error = "gpu_catalog_spool_page_invalid:" + error; return false; }
        if( page.kind == GpuAnalysisStorePageKind::Resource ) resources += snapshot->resources.size();
        else if( page.kind == GpuAnalysisStorePageKind::Allocation ) allocations += snapshot->allocations.size();
        else if( page.kind == GpuAnalysisStorePageKind::Churn ) churn += snapshot->churnCandidates.size();
        else { error = "gpu_catalog_spool_page_kind_invalid"; return false; }
    }
    if( resources != spool.resourceCount || allocations != spool.allocationCount ||
        churn != spool.churnCount || spool.pageCount != spool.pages.size() )
    { error = "gpu_catalog_spool_checkpoint_count_mismatch"; return false; }
    return true;
}

bool SaveGpuPassSpoolCheckpoint( const GpuAnalysisPassSpool& spool,
    const std::vector<GpuResourceAnalysisRecord>& appendedResources,
    const GpuAnalysisTraceIdentity& identity, std::string& error )
{
    error.clear();
    GpuAnalysisSnapshot appended; appended.resources = appendedResources;
    const GpuAnalysisCacheIdentity cacheIdentity { identity.sha256, identity.fileSize,
        std::string( GpuAnalysisAlgorithmId ) + "-session-pass-spool1" };
    const auto appendedPath = spool.root / "appended-resources.cache";
    if( !SaveGpuAnalysisCache( appendedPath, cacheIdentity, appended, error ) ) return false;
    const auto temporary = spool.root / "spool.meta.tmp";
    std::ofstream out( GpuAnalysisIoPath( temporary ), std::ios::binary | std::ios::trunc );
    if( !out || !WriteIdentity( out, PassMagic, identity ) )
    { error = "gpu_pass_spool_checkpoint_open_failed"; return false; }
    const uint64_t counters[] = { spool.pageCount, spool.passCount,
        spool.directRelationCount, spool.inclusiveRelationCount, spool.rangeCount,
        spool.logicalCount, spool.catalogRelationCount, spool.viewCount,
        spool.partCount, spool.virtualGeometryCount, spool.sourceGapResourceCount,
        spool.sourceGapReferenceCount, spool.taxonomyCount, spool.stableSummaryCount };
    for( const auto value : counters ) if( !WritePod( out, value ) ) return false;
    const std::vector<const std::vector<std::filesystem::path>*> vectors = {
        &spool.rangeRuns, &spool.logicalRuns, &spool.catalogRelationRuns,
        &spool.viewRuns, &spool.partRuns, &spool.virtualGeometryRuns,
        &spool.taxonomyRuns, &spool.stableSummaryRuns };
    for( const auto* values : vectors )
        if( !WritePathVector( out, spool.root, *values, error ) ) return false;
    std::vector<std::filesystem::path> fixed;
    AddUniquePath( fixed, appendedPath );
    AddUniquePath( fixed, spool.root / "logical-lifetimes.bin" );
    for( uint64_t i = 0; i < spool.pageCount; ++i )
    {
        std::ostringstream name; name << std::setw( 6 ) << std::setfill( '0' ) << i << ".bin";
        AddUniquePath( fixed, spool.root / name.str() );
    }
    if( !WritePathVector( out, spool.root, fixed, error ) ) return false;
    out.flush(); if( !out ) { error = "gpu_pass_spool_checkpoint_write_failed"; return false; }
    out.close(); return AtomicReplaceCheckpoint( temporary, spool.root / "spool.meta", error );
}

bool LoadGpuPassSpoolCheckpoint( const std::filesystem::path& root,
    const GpuAnalysisTraceIdentity& identity, GpuAnalysisPassSpool& spool,
    std::vector<GpuResourceAnalysisRecord>& appendedResources,
    std::string& error )
{
    error.clear(); spool = {}; spool.root = root; appendedResources.clear();
    std::ifstream in( GpuAnalysisIoPath( root / "spool.meta" ), std::ios::binary );
    if( !in ) { error = "gpu_pass_spool_checkpoint_not_found"; return false; }
    if( !ReadIdentity( in, PassMagic, identity, error ) ) return false;
    uint64_t* counters[] = { &spool.pageCount, &spool.passCount,
        &spool.directRelationCount, &spool.inclusiveRelationCount, &spool.rangeCount,
        &spool.logicalCount, &spool.catalogRelationCount, &spool.viewCount,
        &spool.partCount, &spool.virtualGeometryCount, &spool.sourceGapResourceCount,
        &spool.sourceGapReferenceCount, &spool.taxonomyCount, &spool.stableSummaryCount };
    for( auto* value : counters ) if( !ReadPod( in, *value ) )
    { error = "gpu_pass_spool_checkpoint_counter_invalid"; return false; }
    std::vector<std::vector<std::filesystem::path>*> vectors = {
        &spool.rangeRuns, &spool.logicalRuns, &spool.catalogRelationRuns,
        &spool.viewRuns, &spool.partRuns, &spool.virtualGeometryRuns,
        &spool.taxonomyRuns, &spool.stableSummaryRuns };
    for( auto* values : vectors )
        if( !ReadPathVector( in, root, *values, error ) ) return false;
    std::vector<std::filesystem::path> fixed;
    if( !ReadPathVector( in, root, fixed, error ) ) return false;
    const GpuAnalysisCacheIdentity cacheIdentity { identity.sha256, identity.fileSize,
        std::string( GpuAnalysisAlgorithmId ) + "-session-pass-spool1" };
    auto appended = LoadGpuAnalysisCache( root / "appended-resources.cache",
        cacheIdentity, error );
    if( !appended ) { error = "gpu_pass_spool_appended_invalid:" + error; return false; }
    appendedResources = std::move( appended->resources );
    uint64_t passCount = 0;
    for( uint64_t i = 0; i < spool.pageCount; ++i )
    {
        std::ostringstream name; name << std::setw( 6 ) << std::setfill( '0' ) << i << ".bin";
        auto page = LoadGpuAnalysisCache( root / name.str(), cacheIdentity, error );
        if( !page ) { error = "gpu_pass_spool_page_invalid:" + error; return false; }
        passCount += page->passes.size();
    }
    if( passCount != spool.passCount )
    { error = "gpu_pass_spool_checkpoint_count_mismatch"; return false; }
    return true;
}

}
