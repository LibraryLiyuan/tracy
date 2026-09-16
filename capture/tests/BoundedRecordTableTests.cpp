#include <cstdio>
// Catalog tables share a fixed small cache and compressed immutable storage.
#if __has_include("../../storage/TracyBoundedRecordTable.hpp")
#include "../../storage/TracyBoundedRecordTable.hpp"
#include <array>
#include <chrono>
static void Check(bool ok){if(!ok)throw std::runtime_error("bounded record table invariant");}
int main(int argc,char** argv){try{
 if(argc!=2)return 2;auto root=std::filesystem::path(argv[1])/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());std::filesystem::create_directories(root);
 tracy::storage::ResourceLimits limits;limits.streamBytes=32ull<<20;limits.diskReserveBytes=0;
 tracy::storage::ResourceSample sample;sample.systemCommitLimit=64ull<<30;
 tracy::storage::ConversionResources budget(limits,[&]{return sample;},[]{return 1ull<<40;});
 std::array<uint64_t,5> row{};
 {tracy::storage::BoundedRecordTable small(root/"small",std::string(64,'e'),40,false,budget);small.Append(row.data());small.FreezeAppend();small.Seal();Check(budget.DiskCurrent()==0);small.Read(0,row.data());Check(row[0]==0);}
 tracy::storage::BoundedRecordTable table(root/"large",std::string(64,'e'),40,false,budget);
 for(uint64_t i=0;i<30000;++i){row={i,i,0,0,0};table.Append(row.data());}
 table.FreezeAppend();table.Seal();uint64_t seen=0;table.Visit([&](const void* data,uint64_t n){auto bytes=static_cast<const unsigned char*>(data);for(uint64_t i=0;i<n;++i){std::memcpy(row.data(),bytes+i*40,40);Check(row[0]==seen++);}});Check(seen==30000&&budget.DiskCurrent()>0);
 bool rejected=false;try{table.Put(0,row.data());}catch(const std::exception&){rejected=true;}Check(rejected);
 const auto diskBefore=budget.DiskCurrent();tracy::storage::BoundedRecordTable mutableTable(root/"mutable",std::string(64,'e'),40,true,budget);row={1,2,3,4,5};mutableTable.Append(row.data());mutableTable.FreezeAppend();row[4]=99;mutableTable.Put(0,row.data());mutableTable.Seal();row={};mutableTable.Read(0,row.data());Check(row[4]==99&&budget.DiskCurrent()==diskBefore);
 puts("bounded tables retain small rows and spill immutable history to compressed storage");return 0;
 }catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
#else
int main(){puts("missing bounded record table");return 1;}
#endif
