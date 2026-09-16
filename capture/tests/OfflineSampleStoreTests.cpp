#include "../../storage/TracyOfflineSampleStore.hpp"
#include <chrono>
#include <cstdio>
#include <vector>
static void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(int argc,char** argv){try{
 if(argc!=2)return 2;auto root=std::filesystem::path(argv[1])/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());std::filesystem::create_directories(root);tracy::storage::ResourceLimits limits;limits.streamBytes=64ull<<20;limits.diskReserveBytes=0;tracy::storage::ResourceSample sample;sample.systemCommitLimit=64ull<<30;tracy::storage::ConversionResources budget(limits,[&]{return sample;},[]{return 1ull<<40;});
 tracy::storage::OfflineSampleStore store(root/"samples",std::string(64,'7'),budget);using Row=tracy::storage::OfflineSampleRow;
 store.Append(10,0,{10,1});store.Append(20,0,{11,7});store.Append(10,0,{20,2});store.Append(10,0,{30,3});store.Append(10,1,{15,8});
 const auto duplicate=store.Find(10,0,20);Check(duplicate&&duplicate->second.callstack==2,"offline_sample_find");auto merged=duplicate->second;merged.callstack=22;store.Put(duplicate->first,merged);Check(!store.Find(10,0,25),"offline_sample_missing");store.Append(10,0,{25,4});Check(!store.Find(10,0,5),"offline_sample_earlier_missing");store.Append(10,0,{5,5});store.Append(10,0,{0,6});store.Append(10,0,{0,9});
 for(uint64_t i=0;i<10000;++i)store.Append(30,0,{int64_t(i+1),uint32_t(i)});Check(store.Count(30,0)==10000,"offline_sample_ordered_count");store.Seal();
 std::vector<Row> rows;store.Visit(10,0,[&](const Row& row){rows.push_back(row);});Check(rows.size()==7,"offline_sample_visit_count");const int64_t times[]={0,0,5,10,20,25,30};for(size_t i=0;i<rows.size();++i)Check(rows[i].time==times[i],"offline_sample_order");Check(rows[4].callstack==22,"offline_sample_merge");
 rows.clear();store.Visit(10,1,[&](const Row& row){rows.push_back(row);});Check(rows.size()==1&&rows[0].time==15,"offline_sample_kind");rows.clear();store.Visit(20,0,[&](const Row& row){rows.push_back(row);});Check(rows.size()==1&&rows[0].time==11,"offline_sample_thread");Check(store.TotalCount()==10009&&store.ThreadCount()==3&&store.HasTimeIndex(),"offline_sample_totals");
 std::puts("offline sample append, disorder index, duplicate merge, zero-time and ordered visit tests passed");return 0;
 }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
