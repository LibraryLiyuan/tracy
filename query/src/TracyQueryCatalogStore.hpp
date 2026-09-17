#ifndef __TRACYQUERYCATALOGSTORE_HPP__
#define __TRACYQUERYCATALOGSTORE_HPP__

#include "TracyOfflineCatalog.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace tracy::query
{

class QueryCatalogStoreWriter
{
public:
    QueryCatalogStoreWriter( std::filesystem::path root, std::string sourceFingerprint );
    ~QueryCatalogStoreWriter();
    QueryCatalogStoreWriter( const QueryCatalogStoreWriter& ) = delete;
    QueryCatalogStoreWriter& operator=( const QueryCatalogStoreWriter& ) = delete;

    void SetMetadata( OfflineCatalogQuality metadata );
    void Begin( OfflineCatalogDomain domain, uint64_t count, uint32_t recordBytes );
    void Append( OfflineCatalogDomain domain, uint64_t offset, const void* records, uint64_t count, uint32_t recordBytes );
    void End( OfflineCatalogDomain domain );
    void BeginStrings( uint64_t count );
    void CatalogString( uint64_t index, uint64_t generation,
        const JnGpuCatalogStringRecordHeaderV1& header, const void* bytes );
    void EndStrings();
    void Finish();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class QueryCatalogStoreReader
{
public:
    QueryCatalogStoreReader( std::filesystem::path root, std::string sourceFingerprint );
    ~QueryCatalogStoreReader();
    QueryCatalogStoreReader( const QueryCatalogStoreReader& ) = delete;
    QueryCatalogStoreReader& operator=( const QueryCatalogStoreReader& ) = delete;

    OfflineCatalogQuality Metadata() const;
    uint64_t Count( OfflineCatalogDomain domain ) const;
    void Read( OfflineCatalogDomain domain, uint64_t ordinal, void* output, uint64_t count, uint32_t recordBytes ) const;
    JnGpuCatalogStringData String( uint64_t ordinal ) const;
    void ValidateAll() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}

#endif
