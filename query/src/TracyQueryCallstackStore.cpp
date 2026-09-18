#include "TracyQueryCallstackStore.hpp"

#include "TracyAnalysisIoPath.hpp"
#include "TracyHash.hpp"

#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace tracy::query
{
namespace
{
constexpr uint64_t StoreVersion=1;
struct CallstackRecord { uint64_t offset; uint32_t count; uint32_t reserved; };
struct StoreMetadata { char magic[8]; uint64_t version,count,arenaBytes; char arenaHash[64]; };
static_assert(sizeof(CallstackRecord)==16&&sizeof(StoreMetadata)==96);

std::string Identity(std::string_view source,std::string_view domain)
{
    if(source.size()!=64)throw std::runtime_error("query_callstack_identity");
    analysis::Sha256Builder hash;hash.Update(source.data(),source.size());hash.Update(domain.data(),domain.size());return hash.FinalHex();
}

class ReadOnlyMapping
{
public:
    ReadOnlyMapping(const std::filesystem::path& path,uint64_t expected):m_bytes(expected)
    {
        const auto native=analysis::AnalysisIoPath(path);if(std::filesystem::file_size(native)!=expected)throw std::runtime_error("query_callstack_arena_size");if(!expected)return;if(expected>SIZE_MAX)throw std::runtime_error("query_callstack_arena_address_space");
#ifdef _WIN32
        m_file=CreateFileW(native.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(m_file==INVALID_HANDLE_VALUE)throw std::runtime_error("query_callstack_arena_open");m_mapping=CreateFileMappingW(m_file,nullptr,PAGE_READONLY,0,0,nullptr);if(!m_mapping){CloseHandle(m_file);m_file=INVALID_HANDLE_VALUE;throw std::runtime_error("query_callstack_arena_map");}m_data=static_cast<const char*>(MapViewOfFile(m_mapping,FILE_MAP_READ,0,0,size_t(expected)));if(!m_data){CloseHandle(m_mapping);m_mapping=nullptr;CloseHandle(m_file);m_file=INVALID_HANDLE_VALUE;throw std::runtime_error("query_callstack_arena_view");}
#else
        m_file=open(native.c_str(),O_RDONLY);if(m_file<0)throw std::runtime_error("query_callstack_arena_open");const auto view=mmap(nullptr,size_t(expected),PROT_READ,MAP_SHARED,m_file,0);if(view==MAP_FAILED){close(m_file);m_file=-1;throw std::runtime_error("query_callstack_arena_view");}m_data=static_cast<const char*>(view);
#endif
    }
    ~ReadOnlyMapping()
    {
#ifdef _WIN32
        if(m_data)UnmapViewOfFile(m_data);if(m_mapping)CloseHandle(m_mapping);if(m_file!=INVALID_HANDLE_VALUE)CloseHandle(m_file);
#else
        if(m_data)munmap(const_cast<char*>(m_data),size_t(m_bytes));if(m_file>=0)close(m_file);
#endif
    }
    const CallstackFrameId* Frames(uint64_t offset,uint32_t count)const
    {
        const auto bytes=uint64_t(count)*sizeof(CallstackFrameId);if(offset>m_bytes||bytes>m_bytes-offset||offset%alignof(CallstackFrameId))throw std::runtime_error("query_callstack_arena_range");return reinterpret_cast<const CallstackFrameId*>(m_data+offset);
    }
private:
    const char* m_data=nullptr;uint64_t m_bytes=0;
#ifdef _WIN32
    HANDLE m_file=INVALID_HANDLE_VALUE;HANDLE m_mapping=nullptr;
#else
    int m_file=-1;
#endif
};
}

struct QueryCallstackStoreWriter::Impl
{
    Impl(std::filesystem::path output,std::string identity,analysis::PagedRecordOptions storage)
        :root(std::move(output)),source(std::move(identity)),options(std::move(storage))
    {
        if(source.size()!=64)throw std::runtime_error("query_callstack_identity");if(std::filesystem::exists(root)&&!std::filesystem::is_empty(root))throw std::runtime_error("query_callstack_store_exists");std::filesystem::create_directories(root);
    }
    std::filesystem::path root;std::string source;analysis::PagedRecordOptions options;
    std::unique_ptr<analysis::PagedRecordWriter> records;std::ofstream arena;analysis::Sha256Builder arenaHash;
    uint64_t expected=0,seen=0,arenaBytes=0;uint32_t currentId=0,currentCount=0,currentWritten=0;bool started=false,active=false,committed=false;
};

QueryCallstackStoreWriter::QueryCallstackStoreWriter(std::filesystem::path root,std::string sourceFingerprint,analysis::PagedRecordOptions options):m_impl(std::make_unique<Impl>(std::move(root),std::move(sourceFingerprint),std::move(options))){}
QueryCallstackStoreWriter::~QueryCallstackStoreWriter()=default;
void QueryCallstackStoreWriter::Begin(uint64_t count)
{
    auto& v=*m_impl;if(v.started||v.committed||count>UINT32_MAX-1)throw std::runtime_error("query_callstack_begin");v.started=true;v.expected=count;v.records=std::make_unique<analysis::PagedRecordWriter>(v.root/"records",Identity(v.source,"query-callstacks-v1-records"),uint32_t(sizeof(CallstackRecord)),v.options);v.arena.open(analysis::AnalysisIoPath(v.root/"arena.bin"),std::ios::binary|std::ios::trunc);if(!v.arena)throw std::runtime_error("query_callstack_arena_open");if(v.options.beforeFileGrowth)v.options.beforeFileGrowth(v.root/"arena.bin",0);
}
void QueryCallstackStoreWriter::CallstackBegin(uint32_t id,uint32_t count)
{
    auto& v=*m_impl;if(!v.started||v.active||v.committed||id!=v.seen+1||v.seen>=v.expected||!count||count>UINT16_MAX)throw std::runtime_error("query_callstack_record_begin");v.currentId=id;v.currentCount=count;v.currentWritten=0;v.active=true;
}
void QueryCallstackStoreWriter::CallstackFrames(uint32_t id,uint32_t offset,const CallstackFrameId* frames,uint32_t count)
{
    auto& v=*m_impl;if(!v.active||id!=v.currentId||offset!=v.currentWritten||count>v.currentCount-v.currentWritten||(count&&!frames))throw std::runtime_error("query_callstack_frames");if(v.options.cancelled&&v.options.cancelled())throw std::runtime_error("query_callstack_cancelled");const auto bytes=uint64_t(count)*sizeof(CallstackFrameId);if(bytes){analysis::AnalysisDiskGrow(v.options.disk,bytes);if(v.options.beforeFileGrowth)v.options.beforeFileGrowth(v.root/"arena.bin",v.arenaBytes+bytes);v.arena.write(reinterpret_cast<const char*>(frames),std::streamsize(bytes));if(!v.arena)throw std::runtime_error("query_callstack_arena_write");v.arenaHash.Update(frames,size_t(bytes));v.arenaBytes+=bytes;}v.currentWritten+=count;
}
void QueryCallstackStoreWriter::CallstackEnd(uint32_t id)
{
    auto& v=*m_impl;if(!v.active||id!=v.currentId||v.currentWritten!=v.currentCount)throw std::runtime_error("query_callstack_record_end");const CallstackRecord record{v.arenaBytes-uint64_t(v.currentCount)*sizeof(CallstackFrameId),v.currentCount,0};v.records->Append(&record,1);++v.seen;v.active=false;
}
void QueryCallstackStoreWriter::Commit()
{
    auto& v=*m_impl;if(!v.started||v.active||v.committed||v.seen!=v.expected)throw std::runtime_error("query_callstack_commit");v.records->Commit();v.records.reset();v.arena.flush();if(!v.arena)throw std::runtime_error("query_callstack_arena_flush");v.arena.close();StoreMetadata metadata{};std::memcpy(metadata.magic,"JNQCS1",6);metadata.version=StoreVersion;metadata.count=v.seen;metadata.arenaBytes=v.arenaBytes;const auto hash=v.arenaHash.FinalHex();std::memcpy(metadata.arenaHash,hash.data(),hash.size());analysis::PagedRecordWriter output(v.root/"metadata",Identity(v.source,"query-callstacks-v1-metadata"),uint32_t(sizeof(metadata)),v.options);output.Append(&metadata,1);output.Commit();v.committed=true;
}

struct QueryCallstackStoreReader::Impl
{
    Impl(std::filesystem::path input,std::string identity,analysis::PagedRecordOptions storage)
        :root(std::move(input)),source(std::move(identity)),options(std::move(storage)),metadataReader(root/"metadata",Identity(source,"query-callstacks-v1-metadata"),uint32_t(sizeof(StoreMetadata)),options),records(root/"records",Identity(source,"query-callstacks-v1-records"),uint32_t(sizeof(CallstackRecord)),options)
    {
        if(metadataReader.Count()!=1)throw std::runtime_error("query_callstack_metadata_count");metadataReader.Read(0,&metadata,1);if(std::memcmp(metadata.magic,"JNQCS1",6)!=0||metadata.version!=StoreVersion||metadata.count!=records.Count()||metadata.arenaBytes%sizeof(CallstackFrameId))throw std::runtime_error("query_callstack_metadata");arena=std::make_unique<ReadOnlyMapping>(root/"arena.bin",metadata.arenaBytes);
    }
    CallstackRecord Record(uint32_t id)const{if(!id||id>metadata.count)throw std::out_of_range("query_callstack_id");CallstackRecord record;records.Read(id-1,&record,1);if(!record.count||record.reserved)throw std::runtime_error("query_callstack_record");arena->Frames(record.offset,record.count);return record;}
    std::filesystem::path root;std::string source;analysis::PagedRecordOptions options;StoreMetadata metadata{};mutable analysis::PagedRecordReader metadataReader,records;std::unique_ptr<ReadOnlyMapping> arena;mutable std::mutex mutex;
};

QueryCallstackStoreReader::QueryCallstackStoreReader(std::filesystem::path root,std::string sourceFingerprint,analysis::PagedRecordOptions options):m_impl(std::make_unique<Impl>(std::move(root),std::move(sourceFingerprint),std::move(options))){}
QueryCallstackStoreReader::~QueryCallstackStoreReader()=default;
uint64_t QueryCallstackStoreReader::Count()const{return m_impl->metadata.count;}
std::span<const CallstackFrameId> QueryCallstackStoreReader::Frames(uint32_t id)const{std::lock_guard lock(m_impl->mutex);const auto record=m_impl->Record(id);return {m_impl->arena->Frames(record.offset,record.count),record.count};}
void QueryCallstackStoreReader::VisitFrames(uint32_t id,uint32_t maximumFramesPerChunk,const std::function<void(const CallstackFrameId*,uint32_t)>& visitor)const
{
    if(!maximumFramesPerChunk||!visitor)throw std::invalid_argument("query_callstack_visit");std::lock_guard lock(m_impl->mutex);const auto record=m_impl->Record(id);const auto* frames=m_impl->arena->Frames(record.offset,record.count);for(uint32_t offset=0;offset<record.count;){const auto count=std::min(maximumFramesPerChunk,record.count-offset);visitor(frames+offset,count);offset+=count;if(m_impl->options.cancelled&&m_impl->options.cancelled())throw std::runtime_error("query_callstack_cancelled");}
}
void QueryCallstackStoreReader::ValidateAll()const
{
    std::lock_guard lock(m_impl->mutex);m_impl->metadataReader.ValidateAll();m_impl->records.ValidateAll();uint64_t offset=0;for(uint32_t id=1;id<=m_impl->metadata.count;++id){const auto record=m_impl->Record(id);if(record.offset!=offset)throw std::runtime_error("query_callstack_record_order");offset+=uint64_t(record.count)*sizeof(CallstackFrameId);}if(offset!=m_impl->metadata.arenaBytes)throw std::runtime_error("query_callstack_byte_count");if(analysis::Sha256File(m_impl->root/"arena.bin")!=std::string(m_impl->metadata.arenaHash,64))throw std::runtime_error("query_callstack_arena_hash");
}
}
