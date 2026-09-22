#include "TracyQueryStringStore.hpp"

#include "TracyAnalysisExternalSort.hpp"
#include "TracyAnalysisIoPath.hpp"
#include "TracyHash.hpp"
#include "TracyAnalysisWorkspaceBudget.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace tracy::query
{
analysis::PagedRecordOptions QueryDictionaryReadOptions(analysis::PagedRecordOptions options, bool broadOrdinals)
{
    if(options.readerCacheBytes==0) {
        // The primary Worker dictionary serves broad ordinal lookups during
        // validation and queries. Reserve from the shared budget; secondary
        // count-only readers keep their existing small default.
        uint64_t cap=broadOrdinals ? (1ull<<30) : (64ull<<20);
        if(options.workspace) cap=std::min(cap,std::max<uint64_t>(options.blockBytes,options.workspace->Snapshot().maximumBytes/4));
        options.readerCacheBytes=cap;
    }
    return options;
}
namespace
{
constexpr uint64_t StoreVersion = 1;
constexpr size_t PointerKindCount = 3;
struct StringRecord { uint64_t token, offset, bytes; };
struct PointerRecord { uint64_t id, firstToken, secondToken; };
struct StoreMetadata
{
    char magic[8]; uint64_t version, stringCount, stringBytes, arenaBytes; uint64_t pointerCounts[PointerKindCount]; char arenaHash[64];
};
static_assert( sizeof( StringRecord ) == 24 && sizeof( PointerRecord ) == 24 && sizeof( StoreMetadata ) == 128 );

class ReadOnlyMapping
{
public:
    explicit ReadOnlyMapping( const std::filesystem::path& path, uint64_t expectedBytes ) : m_bytes( expectedBytes )
    {
        const auto native = analysis::AnalysisIoPath( path );
        if( std::filesystem::file_size( native ) != expectedBytes ) throw std::runtime_error( "query_string_arena_size" );
        if( expectedBytes == 0 ) return;
        if( expectedBytes > SIZE_MAX ) throw std::runtime_error( "query_string_arena_address_space" );
#ifdef _WIN32
        m_file = CreateFileW( native.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr );
        if( m_file == INVALID_HANDLE_VALUE ) throw std::runtime_error( "query_string_arena_open" );
        m_mapping = CreateFileMappingW( m_file, nullptr, PAGE_READONLY, 0, 0, nullptr );
        if( !m_mapping ) { CloseHandle( m_file ); m_file = INVALID_HANDLE_VALUE; throw std::runtime_error( "query_string_arena_map" ); }
        m_data = static_cast<const char*>( MapViewOfFile( m_mapping, FILE_MAP_READ, 0, 0, size_t( expectedBytes ) ) );
        if( !m_data ) { CloseHandle( m_mapping ); m_mapping = nullptr; CloseHandle( m_file ); m_file = INVALID_HANDLE_VALUE; throw std::runtime_error( "query_string_arena_view" ); }
#else
        m_file = open( native.c_str(), O_RDONLY );
        if( m_file < 0 ) throw std::runtime_error( "query_string_arena_open" );
        const auto view = mmap( nullptr, size_t( expectedBytes ), PROT_READ, MAP_SHARED, m_file, 0 );
        if( view == MAP_FAILED ) { close( m_file ); m_file = -1; throw std::runtime_error( "query_string_arena_view" ); }
        m_data = static_cast<const char*>( view );
#endif
    }
    ~ReadOnlyMapping()
    {
#ifdef _WIN32
        if( m_data ) UnmapViewOfFile( m_data );
        if( m_mapping ) CloseHandle( m_mapping );
        if( m_file != INVALID_HANDLE_VALUE ) CloseHandle( m_file );
#else
        if( m_data ) munmap( const_cast<char*>( m_data ), size_t( m_bytes ) );
        if( m_file >= 0 ) close( m_file );
#endif
    }
    const char* At( uint64_t offset, uint64_t bytes ) const
    {
        if( offset > m_bytes || bytes > m_bytes - offset ) throw std::runtime_error( "query_string_arena_range" );
        return m_data ? m_data + offset : nullptr;
    }
private:
    const char* m_data = nullptr; uint64_t m_bytes = 0;
#ifdef _WIN32
    HANDLE m_file = INVALID_HANDLE_VALUE; HANDLE m_mapping = nullptr;
#else
    int m_file = -1;
#endif
};

size_t KindIndex( QueryStringPointerKind kind )
{
    const auto value = size_t( kind );
    if( value >= PointerKindCount ) throw std::runtime_error( "query_string_pointer_kind" );
    return value;
}
std::string Identity( std::string_view source, std::string_view domain )
{
    if( source.size() != 64 ) throw std::runtime_error( "query_string_identity" );
    analysis::Sha256Builder hash; hash.Update( source.data(), source.size() ); hash.Update( domain.data(), domain.size() );
    return hash.FinalHex();
}
std::string PointerDomain( size_t kind ) { return "query-global-strings-v1-pointer-" + std::to_string( kind ); }
std::filesystem::path PointerRecordsPath( const std::filesystem::path& root, size_t kind ) { return root / ( "pointer-" + std::to_string( kind ) + "-records" ); }
std::filesystem::path PointerSourcePath( const std::filesystem::path& root, size_t kind ) { return root / ( "pointer-" + std::to_string( kind ) + "-index-source.bin" ); }
std::filesystem::path PointerIndexPath( const std::filesystem::path& root, size_t kind ) { return root / ( "pointer-" + std::to_string( kind ) + "-index.bin" ); }
void RemoveAccounted( const std::filesystem::path& path, const std::shared_ptr<analysis::AnalysisDiskBudget>& disk )
{
    std::error_code error; analysis::AnalysisDiskRemove( path, disk, error );
    if( error ) throw std::runtime_error( "query_string_cleanup:" + error.message() );
}
}

struct QueryStringStoreWriter::Impl
{
    Impl( std::filesystem::path output, std::string identity, analysis::PagedRecordOptions storage, uint64_t maximum )
        : root( std::move( output ) ), source( std::move( identity ) ), options( std::move( storage ) )
        , maximumBuffered( std::max<uint64_t>( 1, maximum ) )
        , pairWorkspace( options.workspace, maximumBuffered * sizeof( analysis::AnalysisUInt64Pair ) )
    {
        if( source.size() != 64 ) throw std::runtime_error( "query_string_identity" );
        if( std::filesystem::exists( root ) && !std::filesystem::is_empty( root ) ) throw std::runtime_error( "query_string_store_exists" );
        std::filesystem::create_directories( root );
        pairs.reserve( size_t( std::min<uint64_t>( maximumBuffered, std::numeric_limits<size_t>::max() ) ) );
    }
    void OpenPairSource( const std::filesystem::path& path )
    {
        pairSource.open( path, std::ios::binary | std::ios::trunc );
        if( !pairSource ) throw std::runtime_error( "query_string_index_open" );
    }
    void FlushPairs()
    {
        if( pairs.empty() ) return;
        const auto bytes = uint64_t( pairs.size() ) * sizeof( analysis::AnalysisUInt64Pair );
        analysis::AnalysisDiskGrow( options.disk, bytes );
        pairSource.write( reinterpret_cast<const char*>( pairs.data() ), std::streamsize( bytes ) );
        if( !pairSource ) throw std::runtime_error( "query_string_index_write" );
        pairs.clear();
    }
    void AppendPair( uint64_t key, uint64_t value )
    {
        pairs.push_back( { key, value } ); if( pairs.size() == maximumBuffered ) FlushPairs();
    }
    void FinishPairs( const std::filesystem::path& sourcePath, const std::filesystem::path& indexPath,
        const std::string& prefix, uint64_t count )
    {
        FlushPairs(); pairSource.flush();
        if( !pairSource ) throw std::runtime_error( "query_string_index_flush" );
        pairSource.close();
        std::string error;
        if( !analysis::SortAnalysisUInt64Pairs( sourcePath, indexPath, root, prefix, count,
            maximumBuffered, error, options.disk, options.workspace ) ) throw std::runtime_error( error );
        RemoveAccounted( sourcePath, options.disk );
    }
    std::filesystem::path root; std::string source; analysis::PagedRecordOptions options;
    uint64_t maximumBuffered; analysis::AnalysisWorkspaceReservation pairWorkspace;
    std::vector<analysis::AnalysisUInt64Pair> pairs; std::ofstream pairSource, arena;
    std::unique_ptr<analysis::PagedRecordWriter> strings; analysis::Sha256Builder arenaHash;
    std::array<std::unique_ptr<analysis::PagedRecordWriter>, PointerKindCount> pointers;
    std::array<uint64_t, PointerKindCount> expectedPointers {}, seenPointers {};
    std::array<bool, PointerKindCount> pointerStarted {}, pointerEnded {};
    uint64_t expectedStrings=0, seenStrings=0, totalBytes=0, arenaBytes=0, currentToken=0, currentBytes=0, currentWritten=0;
    bool stringsStarted=false, stringActive=false, stringsEnded=false, committed=false;
};

QueryStringStoreWriter::QueryStringStoreWriter( std::filesystem::path root, std::string sourceFingerprint,
    analysis::PagedRecordOptions options, uint64_t maximumBufferedPairs )
    : m_impl( std::make_unique<Impl>( std::move( root ), std::move( sourceFingerprint ), std::move( options ), maximumBufferedPairs ) ) {}
QueryStringStoreWriter::~QueryStringStoreWriter() = default;
void QueryStringStoreWriter::StringsBegin( uint64_t count )
{
    auto& v=*m_impl; if(v.stringsStarted||v.committed) throw std::runtime_error("query_string_strings_begin");
    v.stringsStarted=true; v.expectedStrings=count;
    v.strings=std::make_unique<analysis::PagedRecordWriter>(v.root/"records",Identity(v.source,"query-global-strings-v1-records"),uint32_t(sizeof(StringRecord)),v.options);
    v.arena.open(analysis::AnalysisIoPath(v.root/"arena.bin"),std::ios::binary|std::ios::trunc);if(!v.arena)throw std::runtime_error("query_string_arena_open");
    if(v.options.beforeFileGrowth)v.options.beforeFileGrowth(v.root/"arena.bin",0);
    v.OpenPairSource(v.root/"token-index-source.bin");
}
void QueryStringStoreWriter::StringBegin(uint64_t ordinal,uint64_t token,uint64_t bytes)
{
    auto& v=*m_impl; if(!v.stringsStarted||v.stringsEnded||v.stringActive||ordinal!=v.seenStrings||ordinal>=v.expectedStrings||bytes>UINT64_MAX-v.totalBytes) throw std::runtime_error("query_string_string_begin");
    v.currentToken=token;v.currentBytes=bytes;v.currentWritten=0;v.stringActive=true;
}
void QueryStringStoreWriter::StringBytes(uint64_t ordinal,uint64_t offset,const void* data,uint64_t bytes)
{
    auto& v=*m_impl; if(!v.stringActive||ordinal!=v.seenStrings||offset!=v.currentWritten||bytes>v.currentBytes-v.currentWritten||(bytes&&data==nullptr)) throw std::runtime_error("query_string_string_bytes");
    if(bytes){analysis::AnalysisDiskGrow(v.options.disk,bytes);if(v.options.beforeFileGrowth)v.options.beforeFileGrowth(v.root/"arena.bin",v.arenaBytes+bytes);v.arena.write(static_cast<const char*>(data),std::streamsize(bytes));if(!v.arena)throw std::runtime_error("query_string_arena_write");v.arenaHash.Update(data,size_t(bytes));v.arenaBytes+=bytes;}v.currentWritten+=bytes;
}
void QueryStringStoreWriter::StringEnd(uint64_t ordinal)
{
    auto& v=*m_impl; if(!v.stringActive||ordinal!=v.seenStrings||v.currentWritten!=v.currentBytes) throw std::runtime_error("query_string_string_end");
    const StringRecord record{v.currentToken,v.arenaBytes-v.currentBytes,v.currentBytes};v.strings->Append(&record,1);v.AppendPair(v.currentToken,ordinal);v.totalBytes+=v.currentBytes;
    const char terminator=0;analysis::AnalysisDiskGrow(v.options.disk,1);if(v.options.beforeFileGrowth)v.options.beforeFileGrowth(v.root/"arena.bin",v.arenaBytes+1);v.arena.write(&terminator,1);if(!v.arena)throw std::runtime_error("query_string_arena_write");v.arenaHash.Update(&terminator,1);++v.arenaBytes;++v.seenStrings;v.stringActive=false;
}
void QueryStringStoreWriter::StringsEnd()
{
    auto& v=*m_impl; if(!v.stringsStarted||v.stringsEnded||v.stringActive||v.seenStrings!=v.expectedStrings) throw std::runtime_error("query_string_strings_end");
    v.strings->Commit();v.strings.reset();v.arena.flush();if(!v.arena)throw std::runtime_error("query_string_arena_flush");v.arena.close();v.FinishPairs(v.root/"token-index-source.bin",v.root/"token-index.bin","query-string-token",v.seenStrings);v.stringsEnded=true;
}
void QueryStringStoreWriter::PointerTableBegin(QueryStringPointerKind kind,uint64_t count)
{
    auto& v=*m_impl;const auto i=KindIndex(kind);if(!v.stringsEnded||v.pointerStarted[i]||v.committed) throw std::runtime_error("query_string_pointer_begin");
    v.pointerStarted[i]=true;v.expectedPointers[i]=count;v.pointers[i]=std::make_unique<analysis::PagedRecordWriter>(PointerRecordsPath(v.root,i),Identity(v.source,PointerDomain(i)),uint32_t(sizeof(struct PointerRecord)),v.options);v.OpenPairSource(PointerSourcePath(v.root,i));
}
void QueryStringStoreWriter::PointerRecord(QueryStringPointerKind kind,uint64_t ordinal,uint64_t id,uint64_t firstToken,uint64_t secondToken)
{
    auto& v=*m_impl;const auto i=KindIndex(kind);if(!v.pointerStarted[i]||v.pointerEnded[i]||ordinal!=v.seenPointers[i]||ordinal>=v.expectedPointers[i]) throw std::runtime_error("query_string_pointer_record");
    const struct PointerRecord record{id,firstToken,secondToken};v.pointers[i]->Append(&record,1);v.AppendPair(id,ordinal);++v.seenPointers[i];
}
void QueryStringStoreWriter::PointerTableEnd(QueryStringPointerKind kind)
{
    auto& v=*m_impl;const auto i=KindIndex(kind);if(!v.pointerStarted[i]||v.pointerEnded[i]||v.seenPointers[i]!=v.expectedPointers[i]) throw std::runtime_error("query_string_pointer_end");
    v.pointers[i]->Commit();v.pointers[i].reset();v.FinishPairs(PointerSourcePath(v.root,i),PointerIndexPath(v.root,i),"query-string-pointer-"+std::to_string(i),v.seenPointers[i]);v.pointerEnded[i]=true;
}
void QueryStringStoreWriter::Commit()
{
    auto& v=*m_impl;if(v.committed||!v.stringsEnded||std::find(v.pointerEnded.begin(),v.pointerEnded.end(),false)!=v.pointerEnded.end()) throw std::runtime_error("query_string_commit");
    StoreMetadata m{};std::memcpy(m.magic,"JNQSTR1",7);m.version=StoreVersion;m.stringCount=v.seenStrings;m.stringBytes=v.totalBytes;m.arenaBytes=v.arenaBytes;std::copy(v.seenPointers.begin(),v.seenPointers.end(),m.pointerCounts);const auto arenaHash=v.arenaHash.FinalHex();std::memcpy(m.arenaHash,arenaHash.data(),arenaHash.size());
    analysis::PagedRecordWriter output(v.root/"metadata",Identity(v.source,"query-global-strings-v1-metadata"),sizeof(m),v.options);output.Append(&m,1);output.Commit();v.committed=true;
}

struct QueryStringStoreReader::Impl
{
    Impl(std::filesystem::path input,std::string identity,analysis::PagedRecordOptions storage)
        :root(std::move(input)),source(std::move(identity)),options(std::move(storage)),
        metadataReader(root/"metadata",Identity(source,"query-global-strings-v1-metadata"),sizeof(StoreMetadata),options),
        strings(root/"records",Identity(source,"query-global-strings-v1-records"),sizeof(StringRecord),options)
    {
        if(metadataReader.Count()!=1) throw std::runtime_error("query_string_metadata_count");metadataReader.Read(0,&metadata,1);
        if(std::memcmp(metadata.magic,"JNQSTR1",7)!=0||metadata.version!=StoreVersion||metadata.stringCount!=strings.Count()) throw std::runtime_error("query_string_metadata");
        arena=std::make_unique<ReadOnlyMapping>(root/"arena.bin",metadata.arenaBytes);
        OpenIndex(tokenIndex,root/"token-index.bin",metadata.stringCount);
        for(size_t i=0;i<PointerKindCount;i++){pointers[i]=std::make_unique<analysis::PagedRecordReader>(PointerRecordsPath(root,i),Identity(source,PointerDomain(i)),uint32_t(sizeof(PointerRecord)),options);if(pointers[i]->Count()!=metadata.pointerCounts[i]) throw std::runtime_error("query_string_pointer_count");OpenIndex(pointerIndexes[i],PointerIndexPath(root,i),metadata.pointerCounts[i]);}
    }
    static void OpenIndex(std::ifstream& input,const std::filesystem::path& path,uint64_t count)
    {
        const auto size=std::filesystem::file_size(path);if(count>UINT64_MAX/sizeof(analysis::AnalysisUInt64Pair)||size!=count*sizeof(analysis::AnalysisUInt64Pair)) throw std::runtime_error("query_string_index_size");input.open(path,std::ios::binary);if(!input) throw std::runtime_error("query_string_index_read_open");
    }
    static bool Find(std::ifstream& input,uint64_t count,uint64_t key,uint64_t& value)
    {
        uint64_t first=0,length=count;while(length){const auto half=length/2,middle=first+half;analysis::AnalysisUInt64Pair pair;input.clear();input.seekg(std::streamoff(middle*sizeof(pair)),std::ios::beg);input.read(reinterpret_cast<char*>(&pair),sizeof(pair));if(!input) throw std::runtime_error("query_string_index_read");if(pair.key<key){first=middle+1;length-=half+1;}else length=half;}
        if(first>=count)return false;analysis::AnalysisUInt64Pair pair;input.clear();input.seekg(std::streamoff(first*sizeof(pair)),std::ios::beg);input.read(reinterpret_cast<char*>(&pair),sizeof(pair));if(!input)throw std::runtime_error("query_string_index_read");if(pair.key!=key)return false;value=pair.value;return true;
    }
    StringRecord Record(uint64_t ordinal)const{if(ordinal>=metadata.stringCount)throw std::out_of_range("query_string_ordinal");StringRecord r;strings.Read(ordinal,&r,1);if(r.offset>metadata.arenaBytes||r.bytes>=metadata.arenaBytes-r.offset)throw std::runtime_error("query_string_record_range");return r;}
    QueryStableStringView Stable(uint64_t ordinal)const{const auto r=Record(ordinal);const auto* data=arena->At(r.offset,r.bytes+1);if(data[r.bytes]!=0)throw std::runtime_error("query_string_terminator");return {data,r.bytes};}
    std::string Read(uint64_t ordinal,uint64_t maximumBytes)const{const auto view=Stable(ordinal);if(view.bytes>maximumBytes||view.bytes>SIZE_MAX)throw std::length_error("query_string_value_limit");return std::string(view.data,size_t(view.bytes));}
    std::optional<uint64_t> TokenOrdinal(uint64_t token)const{uint64_t ordinal=0;if(!Find(tokenIndex,metadata.stringCount,token,ordinal))return std::nullopt;if(ordinal>=metadata.stringCount)throw std::runtime_error("query_string_token_ordinal");return ordinal;}
    std::optional<struct PointerRecord> Pointer(size_t kind,uint64_t id)const{uint64_t ordinal=0;if(!Find(pointerIndexes[kind],metadata.pointerCounts[kind],id,ordinal))return std::nullopt;if(ordinal>=metadata.pointerCounts[kind])throw std::runtime_error("query_string_pointer_ordinal");struct PointerRecord r;pointers[kind]->Read(ordinal,&r,1);if(r.id!=id)throw std::runtime_error("query_string_pointer_index");return r;}
    static void ValidateIndex(std::ifstream& input,uint64_t count,uint64_t recordCount,const std::function<uint64_t(uint64_t)>& expectedKey)
    {
        input.clear();input.seekg(0,std::ios::beg);analysis::AnalysisUInt64Pair previous{};for(uint64_t ordinal=0;ordinal<count;ordinal++){analysis::AnalysisUInt64Pair pair;input.read(reinterpret_cast<char*>(&pair),sizeof(pair));if(!input||pair.value>=recordCount||(ordinal&&(pair.key<previous.key||(pair.key==previous.key&&pair.value<previous.value)))||expectedKey(pair.value)!=pair.key)throw std::runtime_error("query_string_index_validation");previous=pair;}
    }
    std::filesystem::path root;std::string source;analysis::PagedRecordOptions options;StoreMetadata metadata{};
    mutable analysis::PagedRecordReader metadataReader,strings;std::unique_ptr<ReadOnlyMapping> arena;std::array<std::unique_ptr<analysis::PagedRecordReader>,PointerKindCount> pointers;
    mutable std::ifstream tokenIndex;mutable std::array<std::ifstream,PointerKindCount> pointerIndexes;mutable std::mutex mutex;
};

QueryStringStoreReader::QueryStringStoreReader(std::filesystem::path root,std::string sourceFingerprint,analysis::PagedRecordOptions options):m_impl(std::make_unique<Impl>(std::move(root),std::move(sourceFingerprint),std::move(options))){}
QueryStringStoreReader::~QueryStringStoreReader()=default;
analysis::PagedRecordReader::ReadStats QueryStringStoreReader::RecordReadStatistics()const { std::lock_guard lock(m_impl->mutex); return m_impl->strings.Statistics(); }
uint64_t QueryStringStoreReader::StringCount()const{return m_impl->metadata.stringCount;}
uint64_t QueryStringStoreReader::PointerCount(QueryStringPointerKind kind)const{return m_impl->metadata.pointerCounts[KindIndex(kind)];}
uint64_t QueryStringStoreReader::StringLength(uint64_t ordinal)const{std::lock_guard lock(m_impl->mutex);return m_impl->Record(ordinal).bytes;}
QueryStableStringView QueryStringStoreReader::StableOrdinal(uint64_t ordinal)const{std::lock_guard lock(m_impl->mutex);return m_impl->Stable(ordinal);}
std::optional<QueryStableStringView> QueryStringStoreReader::StableToken(uint64_t token)const{std::lock_guard lock(m_impl->mutex);const auto ordinal=m_impl->TokenOrdinal(token);return ordinal?std::optional<QueryStableStringView>(m_impl->Stable(*ordinal)):std::nullopt;}
std::optional<QueryStableStringView> QueryStringStoreReader::StablePointer(QueryStringPointerKind kind,uint64_t id)const
{
    const auto i=KindIndex(kind);if(kind==QueryStringPointerKind::External)throw std::invalid_argument("query_string_external_pair");std::lock_guard lock(m_impl->mutex);const auto pointer=m_impl->Pointer(i,id);if(!pointer)return std::nullopt;const auto ordinal=m_impl->TokenOrdinal(pointer->firstToken);return ordinal?std::optional<QueryStableStringView>(m_impl->Stable(*ordinal)):std::nullopt;
}
std::optional<std::pair<QueryStableStringView,QueryStableStringView>> QueryStringStoreReader::StableExternal(uint64_t id)const
{
    std::lock_guard lock(m_impl->mutex);const auto pointer=m_impl->Pointer(KindIndex(QueryStringPointerKind::External),id);if(!pointer)return std::nullopt;const auto first=m_impl->TokenOrdinal(pointer->firstToken),second=m_impl->TokenOrdinal(pointer->secondToken);if(!first||!second)return std::nullopt;return std::make_pair(m_impl->Stable(*first),m_impl->Stable(*second));
}
bool QueryStringStoreReader::HasPointer(QueryStringPointerKind kind,uint64_t id)const
{
    std::lock_guard lock(m_impl->mutex);const auto pointer=m_impl->Pointer(KindIndex(kind),id);if(!pointer)return false;if(!m_impl->TokenOrdinal(pointer->firstToken))return false;return kind!=QueryStringPointerKind::External||m_impl->TokenOrdinal(pointer->secondToken).has_value();
}
void QueryStringStoreReader::VisitPointerIds(QueryStringPointerKind kind,const std::function<void(uint64_t)>& visitor)const
{
    if(!visitor)throw std::invalid_argument("query_string_pointer_visitor");std::lock_guard lock(m_impl->mutex);const auto i=KindIndex(kind);auto& input=m_impl->pointerIndexes[i];input.clear();input.seekg(0,std::ios::beg);uint64_t previous=0;bool first=true;for(uint64_t ordinal=0;ordinal<m_impl->metadata.pointerCounts[i];ordinal++){analysis::AnalysisUInt64Pair pair;input.read(reinterpret_cast<char*>(&pair),sizeof(pair));if(!input)throw std::runtime_error("query_string_index_read");if(first||pair.key!=previous){if(pair.value>=m_impl->metadata.pointerCounts[i])throw std::runtime_error("query_string_pointer_ordinal");struct PointerRecord pointer;m_impl->pointers[i]->Read(pair.value,&pointer,1);const bool valid=pointer.id==pair.key&&m_impl->TokenOrdinal(pointer.firstToken).has_value()&&(kind!=QueryStringPointerKind::External||m_impl->TokenOrdinal(pointer.secondToken).has_value());if(valid)visitor(pair.key);}previous=pair.key;first=false;if(m_impl->options.cancelled&&m_impl->options.cancelled())throw std::runtime_error("query_string_cancelled");}
}
std::string QueryStringStoreReader::ReadOrdinal(uint64_t ordinal,uint64_t maximumBytes)const{std::lock_guard lock(m_impl->mutex);return m_impl->Read(ordinal,maximumBytes);}
std::optional<std::string> QueryStringStoreReader::ResolveToken(uint64_t token,uint64_t maximumBytes)const{std::lock_guard lock(m_impl->mutex);const auto ordinal=m_impl->TokenOrdinal(token);return ordinal?std::optional<std::string>(m_impl->Read(*ordinal,maximumBytes)):std::nullopt;}
std::optional<std::string> QueryStringStoreReader::ResolvePointer(QueryStringPointerKind kind,uint64_t id,uint64_t maximumBytes)const
{
    const auto i=KindIndex(kind);if(kind==QueryStringPointerKind::External)throw std::invalid_argument("query_string_external_pair");std::lock_guard lock(m_impl->mutex);const auto pointer=m_impl->Pointer(i,id);if(!pointer)return std::nullopt;const auto ordinal=m_impl->TokenOrdinal(pointer->firstToken);return ordinal?std::optional<std::string>(m_impl->Read(*ordinal,maximumBytes)):std::nullopt;
}
std::optional<std::pair<std::string,std::string>> QueryStringStoreReader::ResolveExternal(uint64_t id,uint64_t maximumBytesPerValue)const
{
    std::lock_guard lock(m_impl->mutex);const auto pointer=m_impl->Pointer(KindIndex(QueryStringPointerKind::External),id);if(!pointer)return std::nullopt;const auto first=m_impl->TokenOrdinal(pointer->firstToken),second=m_impl->TokenOrdinal(pointer->secondToken);if(!first||!second)return std::nullopt;return std::make_pair(m_impl->Read(*first,maximumBytesPerValue),m_impl->Read(*second,maximumBytesPerValue));
}
void QueryStringStoreReader::VisitOrdinalBytes(uint64_t ordinal,uint64_t maximumChunkBytes,const std::function<void(const void*,uint64_t)>& visitor)const
{
    if(!maximumChunkBytes||!visitor)throw std::invalid_argument("query_string_visit");std::lock_guard lock(m_impl->mutex);const auto view=m_impl->Stable(ordinal);for(uint64_t offset=0;offset<view.bytes;){const auto count=std::min<uint64_t>(maximumChunkBytes,view.bytes-offset);visitor(view.data+offset,count);offset+=count;if(m_impl->options.cancelled&&m_impl->options.cancelled())throw std::runtime_error("query_string_cancelled");}
}
void QueryStringStoreReader::ValidateAll()const
{
    std::lock_guard lock(m_impl->mutex);m_impl->metadataReader.ValidateAll();m_impl->strings.ValidateAll();for(auto& table:m_impl->pointers)table->ValidateAll();uint64_t nextOffset=0,payloadBytes=0;for(uint64_t ordinal=0;ordinal<m_impl->metadata.stringCount;ordinal++){const auto record=m_impl->Record(ordinal);if(record.offset!=nextOffset)throw std::runtime_error("query_string_record_order");m_impl->Stable(ordinal);payloadBytes+=record.bytes;nextOffset+=record.bytes+1;}if(nextOffset!=m_impl->metadata.arenaBytes||payloadBytes!=m_impl->metadata.stringBytes)throw std::runtime_error("query_string_byte_count");if(analysis::Sha256File(m_impl->root/"arena.bin")!=std::string(m_impl->metadata.arenaHash,64))throw std::runtime_error("query_string_arena_hash");Impl::ValidateIndex(m_impl->tokenIndex,m_impl->metadata.stringCount,m_impl->metadata.stringCount,[&](uint64_t ordinal){return m_impl->Record(ordinal).token;});for(size_t kind=0;kind<PointerKindCount;kind++)Impl::ValidateIndex(m_impl->pointerIndexes[kind],m_impl->metadata.pointerCounts[kind],m_impl->metadata.pointerCounts[kind],[&](uint64_t ordinal){struct PointerRecord record;m_impl->pointers[kind]->Read(ordinal,&record,1);return record.id;});
}
}
