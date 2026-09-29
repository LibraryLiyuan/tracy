#ifndef TRACY_JN_RECORD_READER_HPP
#define TRACY_JN_RECORD_READER_HPP
#include "TracyWorker.hpp"
#include "TracyFileRead.hpp"
namespace tracy
{
template<class T> void ReadJnRecordsToSink(FileRead& file,std::vector<T>& target,const char* domain,
    SerializedZoneSink* sink,SerializedZoneSink::JnDomain kind)
{
    uint64_t count;file.Read(count);
    const bool external=sink&&sink->WantsJnRecords(kind,uint32_t(sizeof(T)));
    if(count>SIZE_MAX/sizeof(T) || (count>JnTraceMaxRecordsPerDomain &&
        (!external||!sink->AllowsLargeJnRecords(kind,count,uint32_t(sizeof(T)))))) {
        char message[128];snprintf(message,sizeof(message),"JN trace %s record count is invalid.",domain);throw LoadFailure(message);
    }
    if(!external){target.resize(size_t(count));if(count)file.Read(target.data(),size_t(count)*sizeof(T));return;}
    sink->CheckRead();sink->JnRecordsBegin(kind,count,uint32_t(sizeof(T)));
    const auto capacity=std::min<uint64_t>(count,65536);
    struct Lease {
        SerializedZoneSink* sink;
        Lease(SerializedZoneSink* s,uint64_t bytes):sink(s){sink->JnBufferBytes(bytes);}
        ~Lease(){try{sink->JnBufferBytes(0);}catch(...){}}
    } lease(sink,capacity*sizeof(T));
    std::vector<T> block(size_t(capacity),T{});
    for(uint64_t offset=0;offset<count;) {
        sink->CheckRead();const auto n=std::min<uint64_t>(capacity,count-offset);file.Read(block.data(),size_t(n)*sizeof(T));
        sink->JnRecordsBlock(kind,offset,block.data(),n,uint32_t(sizeof(T)));offset+=n;
    }
    sink->JnRecordsEnd(kind);
}
}
#endif
