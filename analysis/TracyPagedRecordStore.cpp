#include "TracyPagedRecordStore.hpp"
#include "TracyHash.hpp"
#include "TracyAnalysisIoPath.hpp"
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>
#include <map>
#include <list>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace tracy::analysis
{
namespace
{
[[noreturn]] void Fail(const char* reason) { throw std::runtime_error(std::string("paged_record_")+reason); }
#pragma pack(push,1)
struct Header
{
    char magic[8]={'J','N','P','A','G','E','1',0};
    uint32_t recordBytes=0,blockRecords=0;
    uint64_t records=0,blocks=0;
    char identity[64]{},contentHash[64]{};
};
struct Entry
{
    uint64_t first=0,offset=0;
    uint32_t records=0,stored=0,decoded=0,codec=0;
    char hash[64]{};
};
#pragma pack(pop)
static_assert(sizeof(Header)==160 && sizeof(Entry)==96);
std::string Hash(const void* data,size_t bytes) { Sha256Builder h; h.Update(data,bytes); return h.FinalHex(); }
bool Digest(const std::string& value)
{ return value.size()==64 && value.find_first_not_of("0123456789abcdefABCDEF")==std::string::npos; }
void Check(const PagedRecordOptions& options)
{ if(options.cancelled && options.cancelled()) Fail("cancelled"); }
void Validate(const std::string& identity,uint32_t recordBytes,const PagedRecordOptions& options)
{
    if(!Digest(identity) || !recordBytes || recordBytes>65536 ||
        options.blockBytes<recordBytes || options.blockBytes>4*1024*1024) Fail("configuration");
    Check(options);
}
template<class T> void Write(std::ostream& out,const T& value)
{ out.write(reinterpret_cast<const char*>(&value),sizeof(value)); if(!out) Fail("write"); }
template<class T> T ReadObject(std::istream& in)
{ T value{}; in.read(reinterpret_cast<char*>(&value),sizeof(value)); if(!in) Fail("read"); return value; }
void FlushFile(const std::filesystem::path& path)
{
#ifdef _WIN32
    const auto file=CreateFileW(AnalysisIoPath(path).c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) Fail("flush_open");
    const bool ok=FlushFileBuffers(file)!=0; CloseHandle(file); if(!ok) Fail("flush");
#else
    (void)path;
#endif
}
void Seek(std::istream& in,uint64_t offset)
{
    if(offset>uint64_t(std::numeric_limits<std::streamoff>::max())) Fail("offset");
    in.clear(); in.seekg(std::streamoff(offset)); if(!in) Fail("seek");
}
}
PagedRecordScratch::PagedRecordScratch(std::shared_ptr<AnalysisWorkspaceBudget> workspace):m_memory(std::move(workspace)){}
PagedRecordScratch::~PagedRecordScratch()=default;
void PagedRecordScratch::Ensure(size_t bytes,size_t arenaBytes,const std::function<void(uint64_t)>& beforeAllocation)
{
    if(arenaBytes>SIZE_MAX-7)Fail("scratch_size");const auto arenaWords=(arenaBytes+7)/8;
    if(bytes>UINT64_MAX-arenaWords*sizeof(uint64_t)-4096)Fail("scratch_size");
    const auto required=uint64_t(bytes)+uint64_t(arenaWords)*sizeof(uint64_t)+4096;
    if(required>m_memory.Bytes()){if(beforeAllocation)beforeAllocation(required);m_memory.Resize(required);++m_stats.grows;m_stats.peakBytes=required;}
    if(m_bytes.capacity()<bytes)m_bytes.reserve(bytes);m_bytes.resize(bytes);
    if(m_arena.capacity()<arenaWords)m_arena.reserve(arenaWords);m_arena.resize(arenaWords);
}
struct PagedRecordWriter::Impl
{
    std::filesystem::path root;
    PagedRecordOptions options;
    AnalysisWorkspaceReservation memory;
    Header header;
    std::ofstream data,directory;
    std::ifstream readData,readDirectory;
    std::vector<char> buffer;
    std::shared_ptr<PagedRecordScratch> scratch;
    Sha256Builder content;
    uint64_t offset=0;
    struct CachedBlock {
        AnalysisWorkspaceReservation memory;
        std::vector<char> bytes;
        std::list<uint64_t>::iterator lru;
        CachedBlock(const PagedRecordOptions& o,size_t size):memory(o.workspace,size+256),bytes(size){}
        void Resize(size_t size){if(size+256>memory.Bytes())memory.Resize(size+256);bytes.resize(size);}
    };
    std::map<uint64_t,std::unique_ptr<CachedBlock>> cache;
    std::list<uint64_t> cacheLru;
    uint64_t cacheBytes=0;
    bool outputDirty=false,committed=false,failed=false;
    Impl(std::filesystem::path path,std::string identity,uint32_t width,PagedRecordOptions config)
        :root(std::move(path)),options(std::move(config)),memory(options.workspace),scratch(options.scratch?options.scratch:std::make_shared<PagedRecordScratch>(options.workspace))
    {
        Validate(identity,width,options);
        if(std::filesystem::exists(AnalysisIoPath(root))) Fail("already_exists");
        if(options.beforeMemoryAllocation)options.beforeMemoryAllocation(options.blockBytes+4096); memory.Resize(options.blockBytes+4096);
        header.recordBytes=width; header.blockRecords=options.blockBytes/width;
        std::memcpy(header.identity,identity.data(),64);
        buffer.reserve(size_t(header.blockRecords)*width);
        if(options.beforeFileGrowth) {
            options.beforeFileGrowth(root,0);options.beforeFileGrowth(root/"data.bin",0);options.beforeFileGrowth(root/"directory.bin",0);
        }
        std::filesystem::create_directories(AnalysisIoPath(root));
        data.open(AnalysisIoPath(root/"data.bin"),std::ios::binary); directory.open(AnalysisIoPath(root/"directory.bin"),std::ios::binary);
        if(!data || !directory) Fail("open");
    }
    void Flush()
    {
        if(buffer.empty()) return;
        Check(options);
        Entry entry; entry.first=header.records-buffer.size()/header.recordBytes; entry.offset=offset;
        entry.records=uint32_t(buffer.size()/header.recordBytes); entry.decoded=uint32_t(buffer.size());
        const auto bound=ZSTD_compressBound(buffer.size()),arenaBytes=ZSTD_estimateCCtxSize(1);
        if(ZSTD_isError(bound)||ZSTD_isError(arenaBytes)) Fail("codec");
        scratch->Ensure(bound,arenaBytes,options.beforeMemoryAllocation);
        auto* context=ZSTD_initStaticCCtx(scratch->Arena(),scratch->ArenaSize()); if(!context) Fail("codec");
        const auto bytes=ZSTD_compressCCtx(context,scratch->Bytes(),scratch->BytesSize(),buffer.data(),buffer.size(),1);
        if(ZSTD_isError(bytes)) Fail("compress");
        const char* payload=buffer.data(); entry.stored=entry.decoded;
        if(bytes<buffer.size()) { entry.codec=1; entry.stored=uint32_t(bytes); payload=scratch->Bytes(); }
        const auto digest=Hash(payload,entry.stored); std::memcpy(entry.hash,digest.data(),64);
        if(entry.stored>UINT64_MAX-offset) Fail("offset");
        AnalysisDiskGrow(options.disk,entry.stored);
        if(options.beforeFileGrowth) options.beforeFileGrowth(root/"data.bin",offset+entry.stored);
        data.write(payload,entry.stored); if(!data) Fail("write");
        AnalysisDiskGrow(options.disk,sizeof(entry));
        if(options.beforeFileGrowth) options.beforeFileGrowth(root/"directory.bin",(header.blocks+1)*sizeof(entry));
        Write(directory,entry);
        outputDirty=true;
        content.Update(buffer.data(),buffer.size());
        offset+=entry.stored; ++header.blocks; buffer.clear();
    }
};
PagedRecordWriter::PagedRecordWriter(std::filesystem::path root,std::string identity,uint32_t width,PagedRecordOptions options)
    :m_impl(std::make_unique<Impl>(std::move(root),std::move(identity),width,std::move(options))) {}
PagedRecordWriter::~PagedRecordWriter()=default;
uint64_t PagedRecordWriter::Count() const { return m_impl->header.records; }
void PagedRecordWriter::Append(const void* records,uint64_t count)
{
    auto& w=*m_impl;
    if(w.failed || w.committed) Fail("writer_closed");
    try {
        Check(w.options);
        if(count>UINT64_MAX-w.header.records || count>SIZE_MAX/w.header.recordBytes || (count && !records)) Fail("count");
        auto* bytes=static_cast<const char*>(records);
        while(count) {
            const auto room=(uint64_t(w.header.blockRecords)*w.header.recordBytes-w.buffer.size())/w.header.recordBytes;
            const auto take=std::min(count,room),length=take*w.header.recordBytes;
            w.buffer.insert(w.buffer.end(),bytes,bytes+length);
            w.header.records+=take; bytes+=length; count-=take;
            if(w.buffer.size()==uint64_t(w.header.blockRecords)*w.header.recordBytes) w.Flush();
        }
    } catch(...) { w.failed=true; throw; }
}
void PagedRecordWriter::Commit()
{
    auto& w=*m_impl; if(w.failed || w.committed) Fail("writer_closed");
    try {
        Check(w.options); w.Flush();
        w.data.flush(); w.directory.flush(); if(!w.data||!w.directory) Fail("flush");
        w.data.close(); w.directory.close(); FlushFile(w.root/"data.bin"); FlushFile(w.root/"directory.bin");
        const auto digest=w.content.FinalHex(); std::memcpy(w.header.contentHash,digest.data(),64);
        const auto headerHash=Hash(&w.header,sizeof(w.header));
        AnalysisDiskGrow(w.options.disk,sizeof(w.header)+64);
        const auto temporary=w.root/"header.tmp",final=w.root/"header.bin";
        if(w.options.beforeFileGrowth) w.options.beforeFileGrowth(temporary,sizeof(w.header)+64);
        { std::ofstream out(AnalysisIoPath(temporary),std::ios::binary); Write(out,w.header); out.write(headerHash.data(),64); out.flush(); if(!out) Fail("write"); }
        FlushFile(temporary); Check(w.options);
        if(w.options.renameFile) w.options.renameFile(temporary,final);
        else {
#ifdef _WIN32
        if(!MoveFileExW(AnalysisIoPath(temporary).c_str(),AnalysisIoPath(final).c_str(),MOVEFILE_WRITE_THROUGH)) Fail("publish");
#else
        std::filesystem::rename(temporary,final);
#endif
        }
        w.committed=true;
    } catch(...) { w.failed=true; throw; }
}
void PagedRecordWriter::ClearReadCache()
{
    auto& w=*m_impl;w.cache.clear();w.cacheLru.clear();w.cacheBytes=0;
    if(w.data.is_open()&&w.outputDirty) { w.data.flush();w.directory.flush();if(!w.data||!w.directory)Fail("flush");w.outputDirty=false; }
}
void PagedRecordWriter::LimitReadCache(uint64_t bytes)
{ClearReadCache();m_impl->options.activeReadCacheBytes=bytes;}
std::string PagedRecordWriter::ContentHash() const
{ const auto& w=*m_impl;if(!w.committed)Fail("not_committed");return std::string(w.header.contentHash,64); }
void PagedRecordWriter::Read(uint64_t ordinal,void* output,uint64_t count)
{
    auto& w=*m_impl;Check(w.options);
    if(w.failed || ordinal>w.header.records || count>w.header.records-ordinal || count>SIZE_MAX/w.header.recordBytes || (count&&!output)) Fail("ordinal");
    auto* target=static_cast<char*>(output);
    const auto tailFirst=w.header.records-w.buffer.size()/w.header.recordBytes;
    while(count) {
        Check(w.options);
        if(ordinal>=tailFirst && !w.buffer.empty()) {
            std::memcpy(target,w.buffer.data()+(ordinal-tailFirst)*w.header.recordBytes,size_t(count*w.header.recordBytes));return;
        }
        const auto block=ordinal/w.header.blockRecords;
        auto found=w.cache.find(block);
        if(found==w.cache.end()) {
            if(w.data.is_open()&&w.outputDirty) { w.data.flush();w.directory.flush();if(!w.data||!w.directory)Fail("flush");w.outputDirty=false; }
            if(!w.readData.is_open()){w.readData.open(AnalysisIoPath(w.root/"data.bin"),std::ios::binary);w.readDirectory.open(AnalysisIoPath(w.root/"directory.bin"),std::ios::binary);if(!w.readData||!w.readDirectory)Fail("active_open");}
            Seek(w.readDirectory,block*sizeof(Entry));const auto e=ReadObject<Entry>(w.readDirectory);
            const auto expected=std::min<uint64_t>(w.header.blockRecords,w.header.records-block*w.header.blockRecords);
            if(e.first!=block*w.header.blockRecords || e.records!=expected || uint64_t(e.decoded)!=expected*w.header.recordBytes ||
                !e.stored || e.stored>e.decoded || e.codec>1 || (!e.codec&&e.stored!=e.decoded) || e.offset>w.offset || e.stored>w.offset-e.offset)
                Fail("active_block_bounds");
            const auto limit=std::max<uint64_t>(w.options.blockBytes+256,w.options.activeReadCacheBytes);
            std::unique_ptr<Impl::CachedBlock> page;
            while(!w.cache.empty() && w.cacheBytes+e.decoded+256>limit) {
                const auto victimId=w.cacheLru.front();w.cacheLru.pop_front();auto victim=w.cache.find(victimId);if(victim==w.cache.end())Fail("active_lru");
                w.cacheBytes-=victim->second->bytes.size()+256;page=std::move(victim->second);w.cache.erase(victim);
            }
            const auto arenaBytes=ZSTD_estimateDCtxSize();
            if(ZSTD_isError(arenaBytes))Fail("codec");w.scratch->Ensure(e.stored,arenaBytes,w.options.beforeMemoryAllocation);
            Seek(w.readData,e.offset);w.readData.read(w.scratch->Bytes(),e.stored);
            if(!w.readData || Hash(w.scratch->Bytes(),e.stored)!=std::string(e.hash,64)) Fail("active_block_checksum");
            if(!page)page=std::make_unique<Impl::CachedBlock>(w.options,e.decoded);else page->Resize(e.decoded);
            if(e.codec) {
                if(ZSTD_getFrameContentSize(w.scratch->Bytes(),e.stored)!=e.decoded || ZSTD_findFrameCompressedSize(w.scratch->Bytes(),e.stored)!=e.stored) Fail("active_frame_size");
                auto* context=ZSTD_initStaticDCtx(w.scratch->Arena(),w.scratch->ArenaSize());if(!context)Fail("codec");
                const auto bytes=ZSTD_decompressDCtx(context,page->bytes.data(),page->bytes.size(),w.scratch->Bytes(),e.stored);
                if(ZSTD_isError(bytes)||bytes!=e.decoded)Fail("active_decode");
            }else std::memcpy(page->bytes.data(),w.scratch->Bytes(),e.stored);
            w.cacheLru.push_back(block);page->lru=std::prev(w.cacheLru.end());w.cacheBytes+=page->bytes.size()+256;found=w.cache.emplace(block,std::move(page)).first;
        }
        else{w.cacheLru.splice(w.cacheLru.end(),w.cacheLru,found->second->lru);found->second->lru=std::prev(w.cacheLru.end());}
        auto& page=*found->second;
        const auto start=ordinal%w.header.blockRecords;
        const auto take=std::min<uint64_t>(count,page.bytes.size()/w.header.recordBytes-start);
        if(!take)Fail("active_block_count");
        const auto bytes=take*w.header.recordBytes;std::memcpy(target,page.bytes.data()+start*w.header.recordBytes,size_t(bytes));
        ordinal+=take;count-=take;target+=bytes;
    }
}
struct PagedRecordReader::Impl
{
    std::filesystem::path root;
    PagedRecordOptions options;
    AnalysisWorkspaceReservation memory;
    Header header;
    std::ifstream data,directory;
    std::vector<char> buffer;
    std::shared_ptr<PagedRecordScratch> scratch;
    uint64_t loaded=UINT64_MAX,dataBytes=0;
    Entry loadedEntry;
    PagedRecordReader::ReadStats stats;
    Impl(std::filesystem::path path,std::string identity,uint32_t width,PagedRecordOptions config)
        :root(std::move(path)),options(std::move(config)),memory(options.workspace),scratch(options.scratch?options.scratch:std::make_shared<PagedRecordScratch>(options.workspace))
    {
        Validate(identity,width,options); if(options.beforeMemoryAllocation)options.beforeMemoryAllocation(options.blockBytes+4096); memory.Resize(options.blockBytes+4096);
        std::ifstream in(AnalysisIoPath(root/"header.bin"),std::ios::binary); header=ReadObject<Header>(in);
        std::string digest(64,0); in.read(digest.data(),64);
        if(!in || std::filesystem::file_size(AnalysisIoPath(root/"header.bin"))!=sizeof(Header)+64 || digest!=Hash(&header,sizeof(header))) Fail("header_checksum");
        const Header expected;
        if(std::memcmp(header.magic,expected.magic,8) || header.recordBytes!=width ||
            !header.blockRecords || uint64_t(header.blockRecords)*width>4*1024*1024 ||
            std::min<uint64_t>(header.blockRecords,header.records)*width>options.blockBytes ||
            std::string(header.identity,64)!=identity || !Digest(std::string(header.contentHash,64))) Fail("identity_or_format");
        if(header.blocks!=header.records/header.blockRecords+(header.records%header.blockRecords!=0) ||
            header.blocks>UINT64_MAX/sizeof(Entry) ||
            std::filesystem::file_size(AnalysisIoPath(root/"directory.bin"))!=header.blocks*sizeof(Entry)) Fail("directory_size");
        dataBytes=std::filesystem::file_size(AnalysisIoPath(root/"data.bin"));
        if(!header.blocks && dataBytes) Fail("data_size");
        data.open(AnalysisIoPath(root/"data.bin"),std::ios::binary); directory.open(AnalysisIoPath(root/"directory.bin"),std::ios::binary);
        if(!data||!directory) Fail("open");
        buffer.reserve(options.blockBytes);
    }
    Entry Load(uint64_t block)
    {
        Check(options); if(block>=header.blocks) Fail("ordinal");
        if(block==loaded) return loadedEntry;
        Seek(directory,block*sizeof(Entry)); const auto entry=ReadObject<Entry>(directory);
        const auto expectedCount=std::min<uint64_t>(header.blockRecords,header.records-block*header.blockRecords);
        if(entry.first!=block*header.blockRecords || entry.records!=expectedCount ||
            uint64_t(entry.decoded)!=expectedCount*header.recordBytes || !entry.stored || entry.stored>entry.decoded ||
            entry.codec>1 || (!entry.codec && entry.stored!=entry.decoded) ||
            entry.offset>dataBytes || entry.stored>dataBytes-entry.offset ||
            (block+1==header.blocks && entry.offset+entry.stored!=dataBytes)) Fail("block_bounds");
        const auto arenaBytes=ZSTD_estimateDCtxSize();if(ZSTD_isError(arenaBytes)) Fail("codec");
        scratch->Ensure(entry.stored,arenaBytes,options.beforeMemoryAllocation);
        Seek(data,entry.offset); data.read(scratch->Bytes(),entry.stored);
        if(!data || Hash(scratch->Bytes(),entry.stored)!=std::string(entry.hash,64)) Fail("block_checksum");
        buffer.resize(entry.decoded);
        if(entry.codec) {
            if(ZSTD_getFrameContentSize(scratch->Bytes(),entry.stored)!=entry.decoded ||
                ZSTD_findFrameCompressedSize(scratch->Bytes(),entry.stored)!=entry.stored) Fail("frame_size");
            auto* context=ZSTD_initStaticDCtx(scratch->Arena(),scratch->ArenaSize()); if(!context) Fail("codec");
            const auto size=ZSTD_decompressDCtx(context,buffer.data(),buffer.size(),scratch->Bytes(),entry.stored);
            if(ZSTD_isError(size)||size!=entry.decoded) Fail("decode");
        } else std::memcpy(buffer.data(),scratch->Bytes(),entry.stored);
        ++stats.blockLoads;stats.storedBytes+=entry.stored;stats.decodedBytes+=entry.decoded;
        stats.checksumBytes+=entry.stored;if(entry.codec)++stats.decompressions;
        loadedEntry=entry; loaded=block; return entry;
    }
};
PagedRecordReader::PagedRecordReader(std::filesystem::path root,std::string identity,uint32_t width,PagedRecordOptions options)
    :m_impl(std::make_unique<Impl>(std::move(root),std::move(identity),width,std::move(options))) {}
PagedRecordReader::~PagedRecordReader()=default;
uint64_t PagedRecordReader::Count() const { return m_impl->header.records; }
PagedRecordReader::ReadStats PagedRecordReader::Statistics() const { return m_impl->stats; }
void PagedRecordReader::Read(uint64_t ordinal,void* output,uint64_t count)
{
    auto& r=*m_impl;
    if(ordinal>r.header.records || count>r.header.records-ordinal || count>SIZE_MAX/r.header.recordBytes || (count&&!output)) Fail("ordinal");
    auto* target=static_cast<char*>(output);
    while(count) {
        const auto entry=r.Load(ordinal/r.header.blockRecords);
        const auto start=ordinal-entry.first,take=std::min<uint64_t>(count,entry.records-start),bytes=take*r.header.recordBytes;
        std::memcpy(target,r.buffer.data()+start*r.header.recordBytes,size_t(bytes));
        target+=bytes; ordinal+=take; count-=take;
    }
}
void PagedRecordReader::ValidateAll()
{
    auto& r=*m_impl; r.loaded=UINT64_MAX; Sha256Builder content; uint64_t offset=0;
    for(uint64_t block=0;block<r.header.blocks;++block) {
        const auto entry=r.Load(block); if(entry.offset!=offset) Fail("block_continuity");
        offset+=entry.stored; content.Update(r.buffer.data(),r.buffer.size());
    }
    if(offset!=r.dataBytes || content.FinalHex()!=std::string(r.header.contentHash,64)) Fail("content_checksum");
}
PagedRecordReader::VisitStats PagedRecordReader::VisitValidated(const std::function<void(const void*,uint64_t)>& visitor)
{
    if(!visitor)Fail("visitor");auto& r=*m_impl;r.loaded=UINT64_MAX;Sha256Builder content;uint64_t offset=0;VisitStats stats;
    for(uint64_t block=0;block<r.header.blocks;++block){const auto entry=r.Load(block);if(entry.offset!=offset)Fail("block_continuity");offset+=entry.stored;content.Update(r.buffer.data(),r.buffer.size());visitor(r.buffer.data(),entry.records);++stats.blocks;stats.records+=entry.records;stats.decodedBytes+=entry.decoded;stats.storedBytes+=entry.stored;}
    if(offset!=r.dataBytes||stats.records!=r.header.records||content.FinalHex()!=std::string(r.header.contentHash,64))Fail("content_checksum");return stats;
}
}
