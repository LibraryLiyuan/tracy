#include <cstdio>
// P3 fixed-width mutable row backend.
#if __has_include("../../storage/TracyMutableRows.hpp")
#include "../../storage/TracyMutableRows.hpp"
#include <array>
#include <chrono>
static void Check(bool ok){if(!ok)throw std::runtime_error("mutable rows invariant");}
template<class F> static void Reject(F&& action,const char* reason){try{action();}catch(const std::exception& e){if(std::string(e.what()).find(reason)!=std::string::npos)return;throw;}throw std::runtime_error("expected mutable row refusal");}
int main(int argc,char** argv){try{
 if(argc!=2)return 2;auto root=std::filesystem::path(argv[1])/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());std::filesystem::create_directories(root);
 tracy::storage::ResourceLimits limits;limits.streamBytes=32ull<<20;limits.diskReserveBytes=0;
 tracy::storage::ResourceSample sample;sample.systemCommitLimit=64ull<<30;
 tracy::storage::ConversionResources budget(limits,[&]{return sample;},[]{return 1ull<<40;});
 tracy::storage::MutableRows store(root/"records.bin",40,budget,2*(16384+1024));
 std::array<uint64_t,5> row{};
 for(uint64_t i=0;i<10000;++i){row={i,i*3,UINT64_MAX-i,0,i};Check(store.Append(row.data())==i);}
 store.FreezeAppend();
 for(uint64_t i=9999;i>0;i-=137){store.Read(i,row.data());Check(row[0]==i);row[3]=42;store.Put(i,row.data());if(i<137)break;}
 store.Seal();uint64_t count=0;store.Visit([&](const void* data,uint64_t n){auto p=static_cast<const unsigned char*>(data);for(uint64_t i=0;i<n;++i){std::memcpy(row.data(),p+i*40,40);Check(row[0]==count&&row[2]==UINT64_MAX-count&&row[3]==((9999-count)%137==0?42:0));++count;}});Check(count==10000);
 bool refused=false;try{store.Put(0,row.data());}catch(const std::exception&){refused=true;}Check(refused);
 refused=false;try{store.Read(10000,row.data());}catch(const std::exception&){refused=true;}Check(refused);
 {tracy::storage::MutableRows empty(root/"empty.bin",16,budget);empty.Seal();uint64_t seen=0;empty.Visit([&](const void*,uint64_t n){seen+=n;});Check(seen==0);}
 for(const bool header:{false,true}){
     tracy::storage::ConversionResources guarded(limits,[&]{return sample;},[]{return 1ull<<40;});
     const auto path=root/(header?"bad-width.bin":"bad-hash.bin");tracy::storage::MutableRows damaged(path,40,guarded);damaged.Append(row.data());damaged.Seal();damaged.ClearCache();
     {std::fstream file(tracy::analysis::AnalysisIoPath(path),std::ios::in|std::ios::out|std::ios::binary);file.seekp(header?12:128);char byte=1;file.write(&byte,1);file.flush();Check(bool(file));}
     Reject([&]{damaged.Read(0,row.data());},header?"mutable_rows_header":"mutable_rows_checksum");
 }
 {auto tight=limits;tight.absoluteDiskLimit=4096;tracy::storage::ConversionResources denied(tight,[&]{return sample;},[]{return 1ull<<40;});tracy::storage::MutableRows bounded(root/"budget.bin",40,denied,16384+1024);bool failed=false;for(uint64_t i=0;i<1000;++i){try{bounded.Append(row.data());}catch(const std::exception& e){Check(std::string(e.what()).find("disk")!=std::string::npos&&bounded.Count()==i);failed=true;break;}}Check(failed);}
 {tracy::storage::ConversionResources cancelled(limits,[&]{return sample;},[]{return 1ull<<40;});tracy::storage::MutableRows bounded(root/"cancel.bin",40,cancelled);cancelled.Cancel();Reject([&]{bounded.Append(row.data());},"cancelled");Check(bounded.Count()==0);}
 puts("mutable rows append, update, eviction and sealed traversal passed");return 0;
 }catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
#else
int main(){puts("missing bounded mutable rows");return 1;}
#endif
