#ifndef __TRACYFILEWRITE_HPP__
#define __TRACYFILEWRITE_HPP__
#include <algorithm>
#include <assert.h>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <stdio.h>
#include <stdexcept>
#include <string.h>
#include <thread>
#include <vector>
#ifndef ZSTD_STATIC_LINKING_ONLY
#define ZSTD_STATIC_LINKING_ONLY
#endif
#include <zstd.h>
#include "TracyFileHeader.hpp"
#include "TracyFileMeta.hpp"
#include "TracyFileWriteHooks.hpp"
#include "../analysis/TracyAnalysisIoPath.hpp"
#include "../public/common/tracy_lz4.hpp"
#include "../public/common/tracy_lz4hc.hpp"
#include "../public/common/TracyForceInline.hpp"
namespace tracy
{
enum class FileCompression { Fast,Slow,Extreme,Zstd };
class WriteStream
{
public:
    WriteStream(FileCompression comp,int level)
        :m_buf(new char[FileBufSize]),m_second(new char[FileBufSize]),m_compressed(new char[FileBoundSize])
    {
        switch(comp) {
        case FileCompression::Fast:m_stream=LZ4_createStream();if(!m_stream)throw std::bad_alloc();break;
        case FileCompression::Slow:m_streamHC=LZ4_createStreamHC();if(!m_streamHC)throw std::bad_alloc();break;
        case FileCompression::Extreme:m_streamHC=LZ4_createStreamHC();if(!m_streamHC)throw std::bad_alloc();LZ4_resetStreamHC(m_streamHC,LZ4HC_CLEVEL_MAX);break;
        case FileCompression::Zstd:
            m_streamZstd=ZSTD_createCStream();if(!m_streamZstd)throw std::bad_alloc();
            if(ZSTD_isError(ZSTD_CCtx_setParameter(m_streamZstd,ZSTD_c_compressionLevel,level)) ||
                ZSTD_isError(ZSTD_CCtx_setParameter(m_streamZstd,ZSTD_c_contentSizeFlag,0))) {
                ZSTD_freeCStream(m_streamZstd);m_streamZstd=nullptr;throw std::runtime_error("file_compression_configuration");
            }break;
        }
    }
    ~WriteStream(){if(m_stream)LZ4_freeStream(m_stream);if(m_streamHC)LZ4_freeStreamHC(m_streamHC);if(m_streamZstd)ZSTD_freeCStream(m_streamZstd);}
    char* GetInputBuffer(){return m_buf.get();}
    const char* GetCompressedData() const{return m_compressed.get();}
    uint32_t GetSize() const{return m_size;}
    void Compress(uint32_t size)
    {
        if(m_stream) {
            const auto n=LZ4_compress_fast_continue(m_stream,m_buf.get(),m_compressed.get(),size,FileBoundSize,1);
            if(n<=0)throw std::runtime_error("file_lz4_compression");m_size=uint32_t(n);
        }else if(m_streamZstd) {
            ZSTD_outBuffer out={m_compressed.get(),FileBoundSize,0};ZSTD_inBuffer in={m_buf.get(),size,0};
            const auto code=ZSTD_compressStream2(m_streamZstd,&out,&in,ZSTD_e_flush);
            if(ZSTD_isError(code)||code!=0||in.pos!=in.size)throw std::runtime_error("file_zstd_compression");m_size=uint32_t(out.pos);
        }else {
            const auto n=LZ4_compress_HC_continue(m_streamHC,m_buf.get(),m_compressed.get(),size,FileBoundSize);
            if(n<=0)throw std::runtime_error("file_lz4hc_compression");m_size=uint32_t(n);
        }
        std::swap(m_buf,m_second);
    }
private:
    LZ4_stream_t* m_stream=nullptr;LZ4_streamHC_t* m_streamHC=nullptr;ZSTD_CStream* m_streamZstd=nullptr;
    std::unique_ptr<char[]> m_buf,m_second,m_compressed;
    uint32_t m_size=0;
};
class FileWrite
{
    struct StreamHandle
    {
        StreamHandle(FileCompression comp,int level,std::shared_ptr<FileWriteHooks> callbacks):stream(comp,level),hooks(std::move(callbacks)){}
        WriteStream stream;std::shared_ptr<FileWriteHooks> hooks;
        std::mutex signalLock;std::condition_variable signal;
        bool inputReady=false,outputReady=false,exit=false;uint32_t size=0;
        std::exception_ptr error;std::thread thread;
    };
public:
    static FileWrite* Open(const char* filename,FileCompression comp=FileCompression::Fast,int level=1,int streams=-1,
        std::shared_ptr<FileWriteHooks> hooks={})
    {return OpenWith([&](){return fopen(filename,"wb");},comp,level,streams,std::move(hooks));}
    static FileWrite* Open(const std::filesystem::path& filename,FileCompression comp=FileCompression::Fast,int level=1,int streams=-1,
        std::shared_ptr<FileWriteHooks> hooks={})
    {return OpenWith([&](){
#ifdef _WIN32
        return _wfopen(analysis::AnalysisIoPath(filename).c_str(),L"wb");
#else
        return fopen(filename.c_str(),"wb");
#endif
    },comp,level,streams,std::move(hooks));}
    template<class Opener> static FileWrite* OpenWith(Opener open,FileCompression comp,int level,int streams,
        std::shared_ptr<FileWriteHooks> hooks={})
    {
        if(streams<=0)streams=std::max<int>(1,int(std::thread::hardware_concurrency()));streams=std::min(streams,255);
        FILE* file=nullptr;
        try {
            if(hooks&&hooks->check)hooks->check();
            if(hooks&&hooks->reserveMemory) {
                const auto codec=comp==FileCompression::Zstd?ZSTD_estimateCStreamSize(level):size_t(comp==FileCompression::Fast?LZ4_sizeofState():LZ4_sizeofStateHC());
                if(ZSTD_isError(codec))throw std::runtime_error("file_compression_memory_estimate");
                hooks->reserveMemory(uint64_t(streams)*(2*FileBufSize+FileBoundSize+codec+sizeof(StreamHandle)+(1ull<<20)));
            }
            if(hooks&&hooks->beforeWrite)hooks->beforeWrite(sizeof(TracyHeader)+2);
            file=open();if(!file){if(hooks&&hooks->reserveMemory)hooks->reserveMemory(0);return nullptr;}
            return new FileWrite(file,comp,level,streams,hooks);
        }catch(const std::exception& e){if(file)fclose(file);Notify(hooks,e.what());Release(hooks);throw;}
        catch(...){if(file)fclose(file);Notify(hooks,"file_write_open_failure");Release(hooks);throw;}
    }
    ~FileWrite() noexcept
    {
        if(!m_finished&&!m_failed){try{Finish();}catch(...){}}
        StopThreads();if(m_file){fclose(m_file);m_file=nullptr;}Release(m_hooks);
    }
    void Finish()
    {
        if(m_finished)return;if(m_failed)throw std::runtime_error("file_writer_failed");
        try {
            Check();if(m_offset)WriteBlock();while(m_streamPending)ProcessPending();
            StopThreads();
            if(fflush(m_file)!=0||ferror(m_file))throw std::runtime_error("file_write_flush");
            auto* file=m_file;m_file=nullptr;if(fclose(file)!=0)throw std::runtime_error("file_write_close");
            m_finished=true;Release(m_hooks);
        }catch(const std::exception& e){m_failed=true;Notify(m_hooks,e.what());StopThreads();throw;}
        catch(...){m_failed=true;Notify(m_hooks,"file_write_finish_failure");StopThreads();throw;}
    }
    tracy_force_inline void Write(const void* ptr,size_t size)
    {
        if(m_finished||m_failed)throw std::runtime_error("file_writer_closed");
        try {
            Check();const auto* src=static_cast<const char*>(ptr);
            while(size) {
                const auto take=std::min(size,FileBufSize-m_offset);memcpy(m_buf+m_offset,src,take);
                src+=take;size-=take;m_offset+=take;if(m_offset==FileBufSize)WriteBlock();
            }
        }catch(const std::exception& e){m_failed=true;Notify(m_hooks,e.what());throw;}
        catch(...){m_failed=true;Notify(m_hooks,"file_write_failure");throw;}
    }
    std::pair<size_t,size_t> GetCompressionStatistics() const{return {m_srcBytes,m_dstBytes};}
private:
    static void Notify(const std::shared_ptr<FileWriteHooks>& hooks,const char* message) noexcept
    {try{if(hooks&&hooks->failed)hooks->failed(message);}catch(...) {}}
    static void Release(const std::shared_ptr<FileWriteHooks>& hooks) noexcept
    {try{if(hooks&&hooks->reserveMemory)hooks->reserveMemory(0);}catch(...) {}}
    void Check(){if(m_hooks&&m_hooks->check)m_hooks->check();}
    FileWrite(FILE* file,FileCompression comp,int level,int streams,std::shared_ptr<FileWriteHooks> hooks):m_file(file),m_hooks(std::move(hooks))
    {
        try {
            CheckedWrite(TracyHeader,sizeof(TracyHeader));uint8_t b=comp==FileCompression::Zstd?1:0;CheckedWrite(&b,1);b=uint8_t(streams);CheckedWrite(&b,1);
            m_streams.reserve(streams);
            for(int i=0;i<streams;++i){m_streams.emplace_back(std::make_unique<StreamHandle>(comp,level,m_hooks));auto* p=m_streams.back().get();p->thread=std::thread([p]{Worker(p);});}
            m_buf=m_streams[0]->stream.GetInputBuffer();
        }catch(...){StopThreads();throw;}
    }
    void CheckedWrite(const void* data,size_t size)
    {
        Check();if(size>UINT64_MAX-m_writtenBytes)throw std::runtime_error("file_write_size_overflow");
        if(m_hooks&&m_hooks->beforeWrite)m_hooks->beforeWrite(m_writtenBytes+size);
        const auto written=m_hooks&&m_hooks->write?m_hooks->write(data,size,m_file):fwrite(data,1,size,m_file);
        m_writtenBytes+=written;if(written!=size)throw std::runtime_error("file_write_short_write");
    }
    void WriteBlock()
    {
        Check();m_srcBytes+=m_offset;auto& h=*m_streams[m_streamId];
        {std::lock_guard lock(h.signalLock);h.inputReady=true;h.size=uint32_t(m_offset);h.signal.notify_one();}
        ++m_streamPending;m_streamId=(m_streamId+1)%int(m_streams.size());m_offset=0;
        if(m_streamPending==int(m_streams.size()))ProcessPending();
        m_buf=m_streams[m_streamId]->stream.GetInputBuffer();
    }
    void ProcessPending()
    {
        const int id=(m_streamId+int(m_streams.size())-m_streamPending)%int(m_streams.size());
        auto& h=*m_streams[id];std::unique_lock lock(h.signalLock);h.signal.wait(lock,[&]{return h.outputReady;});
        const auto error=h.error;h.outputReady=false;lock.unlock();--m_streamPending;
        if(error)std::rethrow_exception(error);
        const uint32_t size=h.stream.GetSize();CheckedWrite(&size,sizeof(size));CheckedWrite(h.stream.GetCompressedData(),size);m_dstBytes+=size;
    }
    void StopThreads() noexcept
    {
        for(auto& h:m_streams){std::lock_guard lock(h->signalLock);h->exit=true;h->signal.notify_one();}
        for(auto& h:m_streams)if(h->thread.joinable())h->thread.join();m_streams.clear();
    }
    static void Worker(StreamHandle* h)
    {
        std::unique_lock lock(h->signalLock);
        for(;;) {
            h->signal.wait(lock,[&]{return h->inputReady||h->exit;});if(h->exit)return;lock.unlock();
            std::exception_ptr failure;
            try{if(h->hooks&&h->hooks->check)h->hooks->check();h->stream.Compress(h->size);}catch(...){failure=std::current_exception();}
            lock.lock();h->error=failure;h->inputReady=false;h->outputReady=true;h->signal.notify_one();
        }
    }
    char* m_buf=nullptr;size_t m_offset=0;int m_streamId=0,m_streamPending=0;
    std::vector<std::unique_ptr<StreamHandle>> m_streams;FILE* m_file;
    std::shared_ptr<FileWriteHooks> m_hooks;
    size_t m_srcBytes=0,m_dstBytes=0;uint64_t m_writtenBytes=0;bool m_finished=false,m_failed=false;
};
}
#endif
