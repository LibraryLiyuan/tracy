#include <cstdio>
#if __has_include("../../storage/TracyMutableRecordStore.hpp")
#include "../../storage/TracyMutableRecordStore.hpp"
#include <stdexcept>
using namespace tracy::storage;
void Require(bool x){if(!x)throw std::runtime_error("mutable record invariant");}
template<class F> void Refuses(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}Require(failed);}
int main(int argc,char** argv){
 if(argc!=2)return 2;auto root=std::filesystem::path(argv[1]);std::filesystem::create_directories(root);
 ResourceLimits l;l.streamBytes=1ull<<26;l.diskReserveBytes=0;
 ResourceSample s;s.physicalTotal=64ull<<30;s.physicalAvailable=32ull<<30;s.systemCommitLimit=64ull<<30;
 ConversionResources r(l,[&]{return s;},[]{return 1ull<<40;});
 {
 MutableRecordStore store(root/"records",r,2);MutableRecordStore::Record row{};
 Require(store.Count()==0);
 for(uint32_t i=0;i<20000;++i){row[0]=i;row[1]=i*3;row[2]=i^123;Require(store.Append(row)==i);}
 for(uint32_t i=0;i<20000;i+=137){row=store.Read(i);Require(row[0]==i);row[1]=42;store.Put(i,row);}
 for(uint32_t i=0;i<20000;++i){row=store.Read(i);Require(row[0]==i&&row[1]==(i%137?i*3:42)&&row[2]==(i^123));}
 Refuses([&]{store.Read(20000);});store.Seal();uint64_t seen=0;
 store.Visit([&](const auto* rows,uint64_t n){for(uint64_t i=0;i<n;++i)Require(rows[i][0]==seen++);});Require(seen==20000);
 Refuses([&]{store.Append(row);});Refuses([&]{store.Put(0,row);});
 }
 Require(r.CleanupOwned());Require(r.DiskCurrent()==0);
 {
 MutableRecordStore empty(root/"empty",r,1);empty.Seal();uint64_t seen=0;empty.Visit([&](auto*,uint64_t n){seen+=n;});Require(seen==0);
 }
 Require(r.CleanupOwned());
 {
 MutableRecordStore corrupt(root/"corrupt",r,1);corrupt.Append({1,2,3});corrupt.Seal();
 {std::fstream file(root/"corrupt",std::ios::binary|std::ios::in|std::ios::out);file.seekp(4096);char byte=7;file.write(&byte,1);file.flush();Require(bool(file));}
 Refuses([&]{corrupt.Visit([](auto*,uint64_t){});});
 }
 Require(r.CleanupOwned());
 l.absoluteDiskLimit=4096;ConversionResources small(l,[&]{return s;},[]{return 1ull<<40;});
 {MutableRecordStore store(root/"reject",small,2);Refuses([&]{store.Append({});});Require(store.Count()==0);}
 Require(small.CleanupOwned());puts("mutable records passed");
}
#else
int main(){puts("missing mutable record store");return 1;}
#endif
