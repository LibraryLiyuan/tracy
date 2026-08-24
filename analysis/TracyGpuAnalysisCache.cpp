#include "TracyGpuAnalysisCache.hpp"

#include <fstream>
#include <limits>
#include <type_traits>

namespace tracy::analysis
{
namespace
{

constexpr uint64_t CacheMagic = 0x314548434143475aull; // ZGCACHE1
constexpr uint64_t MaximumCacheItems = 100000000;
constexpr uint64_t MaximumCacheString = 16 * 1024 * 1024;

template<typename T> bool WritePod( std::ofstream& out, const T& value )
{
    static_assert( std::is_trivially_copyable_v<T> );
    out.write( reinterpret_cast<const char*>( &value ), sizeof( value ) );
    return bool( out );
}

template<typename T> bool ReadPod( std::ifstream& in, T& value )
{
    static_assert( std::is_trivially_copyable_v<T> );
    in.read( reinterpret_cast<char*>( &value ), sizeof( value ) );
    return bool( in );
}

bool WriteString( std::ofstream& out, const std::string& value )
{
    const uint64_t size = value.size();
    if( !WritePod( out, size ) ) return false;
    out.write( value.data(), std::streamsize( value.size() ) );
    return bool( out );
}

bool ReadString( std::ifstream& in, std::string& value )
{
    uint64_t size = 0;
    if( !ReadPod( in, size ) || size > MaximumCacheString ) return false;
    value.resize( size_t( size ) );
    in.read( value.data(), std::streamsize( size ) );
    return bool( in );
}

template<typename T> bool WriteVector( std::ofstream& out, const std::vector<T>& value )
{
    static_assert( std::is_trivially_copyable_v<T> );
    const uint64_t size = value.size();
    if( !WritePod( out, size ) ) return false;
    if( size != 0 ) out.write( reinterpret_cast<const char*>( value.data() ), std::streamsize( sizeof( T ) * size ) );
    return bool( out );
}

template<typename T> bool ReadVector( std::ifstream& in, std::vector<T>& value )
{
    static_assert( std::is_trivially_copyable_v<T> );
    uint64_t size = 0;
    if( !ReadPod( in, size ) || size > MaximumCacheItems || size > uint64_t( std::numeric_limits<size_t>::max() / sizeof( T ) ) ) return false;
    value.resize( size_t( size ) );
    if( size != 0 ) in.read( reinterpret_cast<char*>( value.data() ), std::streamsize( sizeof( T ) * size ) );
    return bool( in );
}

#define W( field ) if( !WritePod( out, value.field ) ) return false
#define R( field ) if( !ReadPod( in, value.field ) ) return false

bool WriteManifest( std::ofstream& out, const GpuAnalysisManifest& value )
{
    W( state ); W( catalogSchema ); W( evidenceSchema ); W( generationCount ); W( resourceRecordCount );
    W( allocationRecordCount ); W( viewRecordCount ); W( logicalRecordCount ); W( partRecordCount ); W( relationRecordCount );
    W( rangeRecordCount ); W( vgRecordCount ); W( evidenceRecordCount ); W( unresolvedCount ); W( invalidRecordCount );
    W( payloadBytes ); W( transportValid ); W( complete );
    return WriteString( out, value.reason );
}

bool ReadManifest( std::ifstream& in, GpuAnalysisManifest& value )
{
    R( state ); R( catalogSchema ); R( evidenceSchema ); R( generationCount ); R( resourceRecordCount );
    R( allocationRecordCount ); R( viewRecordCount ); R( logicalRecordCount ); R( partRecordCount ); R( relationRecordCount );
    R( rangeRecordCount ); R( vgRecordCount ); R( evidenceRecordCount ); R( unresolvedCount ); R( invalidRecordCount );
    R( payloadBytes ); R( transportValid ); R( complete );
    return ReadString( in, value.reason );
}

bool WriteResource( std::ofstream& out, const GpuResourceAnalysisRecord& value )
{
    W( generation ); W( resourceId ); W( familyId ); W( allocationId ); W( capacityBytes ); W( allocationOffsetBytes );
    W( createTime ); W( destroyTime ); W( lastUpdateTime ); W( nameHash ); W( createCallsiteId ); W( definitionRevision );
    W( declaredUsageMask ); W( observedUsageMask ); W( format ); W( width ); W( height ); W( depthOrArraySize ); W( mipLevels );
    W( primaryKind ); W( resourceClass ); W( dimension ); W( memoryDomain ); W( allocationKind ); W( classificationProvenance );
    W( nameProvenance ); W( stackProvenance ); W( exactness ); W( openBoundary ); W( aliveAtEnd ); W( invalid );
    return WriteString( out, value.name ) && WriteVector( out, value.history ) && WriteVector( out, value.views ) &&
        WriteVector( out, value.parts ) && WriteVector( out, value.relations ) && WriteVector( out, value.ranges );
}

bool ReadResource( std::ifstream& in, GpuResourceAnalysisRecord& value )
{
    R( generation ); R( resourceId ); R( familyId ); R( allocationId ); R( capacityBytes ); R( allocationOffsetBytes );
    R( createTime ); R( destroyTime ); R( lastUpdateTime ); R( nameHash ); R( createCallsiteId ); R( definitionRevision );
    R( declaredUsageMask ); R( observedUsageMask ); R( format ); R( width ); R( height ); R( depthOrArraySize ); R( mipLevels );
    R( primaryKind ); R( resourceClass ); R( dimension ); R( memoryDomain ); R( allocationKind ); R( classificationProvenance );
    R( nameProvenance ); R( stackProvenance ); R( exactness ); R( openBoundary ); R( aliveAtEnd ); R( invalid );
    return ReadString( in, value.name ) && ReadVector( in, value.history ) && ReadVector( in, value.views ) &&
        ReadVector( in, value.parts ) && ReadVector( in, value.relations ) && ReadVector( in, value.ranges );
}

bool WriteAllocation( std::ofstream& out, const GpuAllocationAnalysisRecord& value )
{
    W( generation ); W( allocationId ); W( heapId ); W( parentAllocationId ); W( sizeBytes ); W( offsetBytes ); W( residentBytes );
    W( createTime ); W( destroyTime ); W( lastUpdateTime ); W( primaryKind ); W( memoryDomain ); W( allocationKind );
    W( residencyState ); W( exactness ); W( openBoundary ); W( aliveAtEnd ); W( invalid );
    return WriteVector( out, value.history ) && WriteVector( out, value.resources );
}

bool ReadAllocation( std::ifstream& in, GpuAllocationAnalysisRecord& value )
{
    R( generation ); R( allocationId ); R( heapId ); R( parentAllocationId ); R( sizeBytes ); R( offsetBytes ); R( residentBytes );
    R( createTime ); R( destroyTime ); R( lastUpdateTime ); R( primaryKind ); R( memoryDomain ); R( allocationKind );
    R( residencyState ); R( exactness ); R( openBoundary ); R( aliveAtEnd ); R( invalid );
    return ReadVector( in, value.history ) && ReadVector( in, value.resources );
}

bool WritePass( std::ofstream& out, const GpuPassWorkingSet& value )
{
    W( passId ); W( parentPassId ); W( frameId ); W( commandListId ); W( startNs ); W( endNs ); W( directRangeBytes );
    W( directPhysicalBytes ); W( inclusivePhysicalBytes ); W( unknownRangeResourceCount ); W( complete ); W( truncated );
    return WriteString( out, value.name ) && WriteVector( out, value.directResources ) && WriteVector( out, value.inclusiveResources );
}

bool ReadPass( std::ifstream& in, GpuPassWorkingSet& value )
{
    R( passId ); R( parentPassId ); R( frameId ); R( commandListId ); R( startNs ); R( endNs ); R( directRangeBytes );
    R( directPhysicalBytes ); R( inclusivePhysicalBytes ); R( unknownRangeResourceCount ); R( complete ); R( truncated );
    return ReadString( in, value.name ) && ReadVector( in, value.directResources ) && ReadVector( in, value.inclusiveResources );
}

bool WriteCandidate( std::ofstream& out, const GpuChurnCandidate& value )
{
    W( kind ); W( resourceId ); W( allocationId ); W( bytes ); W( eventCount ); W( score );
    return WriteString( out, value.reason );
}

bool ReadCandidate( std::ifstream& in, GpuChurnCandidate& value )
{
    R( kind ); R( resourceId ); R( allocationId ); R( bytes ); R( eventCount ); R( score );
    return ReadString( in, value.reason );
}

#undef W
#undef R

}

bool SaveGpuAnalysisCache( const std::filesystem::path& path, const GpuAnalysisCacheIdentity& identity,
    const GpuAnalysisSnapshot& snapshot, std::string& error )
{
    error.clear();
    try
    {
        std::filesystem::create_directories( path.parent_path() );
        auto temporary = path; temporary += ".tmp";
        std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
        if( !out ) { error = "cache_open_failed"; return false; }
        if( !WritePod( out, CacheMagic ) || !WritePod( out, GpuAnalysisCacheSchemaVersion ) || !WritePod( out, GpuAnalysisSchemaVersion ) ||
            !WritePod( out, identity.traceSize ) || !WriteString( out, identity.traceSha256 ) || !WriteString( out, identity.guiBuild ) ||
            !WriteManifest( out, snapshot.manifest ) ) { error = "cache_write_failed"; return false; }
#define WS( field ) if( !WritePod( out, snapshot.field ) ) { error = "cache_write_failed"; return false; }
        WS( engineKnownPhysicalBytes ); WS( engineKnownPhysicalPeakBytes ); WS( engineKnownPhysicalPeakTimeNs );
        WS( allocationCreateCount ); WS( allocationDestroyCount ); WS( allocatedPhysicalBytes ); WS( freedPhysicalBytes );
        WS( residentPhysicalBytes ); WS( logicalCapacityBytes );
#undef WS
        const auto writeComplex = [&]( const auto& items, const auto& writer )
        {
            const uint64_t count = items.size(); if( !WritePod( out, count ) ) return false;
            for( const auto& item : items ) if( !writer( out, item ) ) return false;
            return true;
        };
        if( !writeComplex( snapshot.resources, WriteResource ) || !writeComplex( snapshot.allocations, WriteAllocation ) ||
            !writeComplex( snapshot.passes, WritePass ) || !WriteVector( out, snapshot.residency ) ||
            !writeComplex( snapshot.churnCandidates, WriteCandidate ) ) { error = "cache_write_failed"; return false; }
        out.close();
        if( !out ) { error = "cache_flush_failed"; return false; }
        std::error_code ec; std::filesystem::remove( path, ec ); ec.clear(); std::filesystem::rename( temporary, path, ec );
        if( ec ) { error = "cache_publish_failed:" + ec.message(); return false; }
        return true;
    }
    catch( const std::exception& e ) { error = e.what(); return false; }
}

std::optional<GpuAnalysisSnapshot> LoadGpuAnalysisCache( const std::filesystem::path& path,
    const GpuAnalysisCacheIdentity& identity, std::string& error )
{
    error.clear();
    try
    {
        std::ifstream in( path, std::ios::binary );
        if( !in ) { error = "cache_not_found"; return std::nullopt; }
        uint64_t magic = 0, traceSize = 0; uint32_t cacheSchema = 0, analysisSchema = 0; std::string sha, build;
        if( !ReadPod( in, magic ) || !ReadPod( in, cacheSchema ) || !ReadPod( in, analysisSchema ) || !ReadPod( in, traceSize ) ||
            !ReadString( in, sha ) || !ReadString( in, build ) || magic != CacheMagic || cacheSchema != GpuAnalysisCacheSchemaVersion ||
            analysisSchema != GpuAnalysisSchemaVersion || traceSize != identity.traceSize || sha != identity.traceSha256 || build != identity.guiBuild )
        { error = "cache_identity_mismatch"; return std::nullopt; }
        GpuAnalysisSnapshot snapshot;
        if( !ReadManifest( in, snapshot.manifest ) ) { error = "cache_manifest_corrupt"; return std::nullopt; }
#define RS( field ) if( !ReadPod( in, snapshot.field ) ) { error = "cache_payload_corrupt"; return std::nullopt; }
        RS( engineKnownPhysicalBytes ); RS( engineKnownPhysicalPeakBytes ); RS( engineKnownPhysicalPeakTimeNs );
        RS( allocationCreateCount ); RS( allocationDestroyCount ); RS( allocatedPhysicalBytes ); RS( freedPhysicalBytes );
        RS( residentPhysicalBytes ); RS( logicalCapacityBytes );
#undef RS
        const auto readComplex = [&]( auto& items, const auto& reader )
        {
            uint64_t count = 0; if( !ReadPod( in, count ) || count > MaximumCacheItems ) return false;
            items.resize( size_t( count ) ); for( auto& item : items ) if( !reader( in, item ) ) return false;
            return true;
        };
        if( !readComplex( snapshot.resources, ReadResource ) || !readComplex( snapshot.allocations, ReadAllocation ) ||
            !readComplex( snapshot.passes, ReadPass ) || !ReadVector( in, snapshot.residency ) ||
            !readComplex( snapshot.churnCandidates, ReadCandidate ) ) { error = "cache_payload_corrupt"; return std::nullopt; }
        char trailing = 0; if( in.read( &trailing, 1 ) ) { error = "cache_trailing_data"; return std::nullopt; }
        if( !in.eof() ) { error = "cache_read_failed"; return std::nullopt; }
        for( size_t i = 0; i < snapshot.resources.size(); ++i )
        {
            if( snapshot.resources[i].resourceId == 0 || !snapshot.resourceById.emplace( snapshot.resources[i].resourceId, i ).second )
            { error = "cache_duplicate_resource"; return std::nullopt; }
        }
        for( size_t i = 0; i < snapshot.allocations.size(); ++i )
        {
            if( snapshot.allocations[i].allocationId == 0 || !snapshot.allocationById.emplace( snapshot.allocations[i].allocationId, i ).second )
            { error = "cache_duplicate_allocation"; return std::nullopt; }
        }
        for( size_t i = 0; i < snapshot.passes.size(); ++i )
        {
            if( snapshot.passes[i].passId == 0 || !snapshot.passById.emplace( snapshot.passes[i].passId, i ).second )
            { error = "cache_duplicate_pass"; return std::nullopt; }
        }
        return snapshot;
    }
    catch( const std::exception& e ) { error = e.what(); return std::nullopt; }
}

}
