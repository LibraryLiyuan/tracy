#include "../src/OfflineValidationSink.hpp"
#include <fstream>
#include <cstdio>
using namespace tracy;
static void Check(bool v){if(!v)throw std::runtime_error("relation storage invariant");}
template<class F> static void Refuses(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}Check(failed);}
static void Zones(conversion::OfflineValidationSink& sink){ZoneExtra zero{};sink.ZoneExtrasBegin(1);sink.ZoneExtrasRawBlock(0,&zero,1);sink.ZoneExtrasEnd();}
int main(int argc,char** argv){try{
 StringIdx largest(0xFFFFFE);Check(largest.Active()&&largest.Idx()==0xFFFFFE);
 Refuses([]{StringIdx invalid(0xFFFFFF);});
 Refuses([]{StringIdx invalid(UINT32_MAX);});
 if(argc!=2)return 2;auto root=std::filesystem::path(argv[1]);std::filesystem::create_directories(root);
 storage::ResourceLimits limits;limits.streamBytes=64ull<<20;limits.diskReserveBytes=0;
 storage::ResourceSample sample;sample.systemCommitLimit=64ull<<30;
 storage::ConversionResources resources(limits,[&]{return sample;},[]{return 1ull<<40;});
 const std::string identity(64,'e');using Domain=SerializedZoneSink::JnDomain;
 {
 conversion::OfflineConversionStorage generic(root/"generic",identity,resources);
 for(size_t i=0;i<OfflineAppendDomainCount;++i){auto domain=OfflineAppendDomain(i);auto width=OfflineAppendInfo(domain).width;std::vector<char> row(width,char(i+1));for(int j=0;j<3;++j)generic.AppendRecord(domain,row.data(),width);Check(generic.RecordCount(domain)==3);}
 generic.Seal();
 for(size_t i=0;i<OfflineAppendDomainCount;++i){auto domain=OfflineAppendDomain(i);uint64_t seen=0;generic.VisitRecords(domain,[&](const void* p,uint64_t n){auto bytes=static_cast<const char*>(p);for(uint64_t j=0;j<n*OfflineAppendInfo(domain).width;++j)Check(bytes[j]==char(i+1));seen+=n;});Check(seen==3);}
 }
 {
 conversion::OfflineConversionStorage empty(root/"empty",identity,resources);empty.Seal();
 Check(empty.RelationCount()==0&&!std::filesystem::exists(root/"empty/relations"));
 conversion::OfflineValidationSink sink(empty,resources);Zones(sink);sink.Finish();
 conversion::OfflineValidationSink begun(empty,resources);Zones(begun);begun.JnRecordsBegin(Domain::Relation,0,37);Refuses([&]{begun.Finish();});begun.JnRecordsEnd(Domain::Relation);begun.Finish();
 }
 {
 conversion::OfflineConversionStorage store(root/"rows",identity,resources);
 JnRelationData rows[3]={{10,1,2,3,4,5,6,7,8},{10,1,2,3,4,5,6,7,8},{-11,9,10,11,255,254,253,252,251}};
 for(const auto& row:rows)store.AppendRelation(row);Check(store.RelationCount()==3);
 Refuses([&]{store.RelationHash();});store.Seal();Refuses([&]{store.AppendRelation(rows[0]);});
 {conversion::OfflineValidationSink sink(store,resources);Zones(sink);Refuses([&]{sink.Finish();});sink.JnRecordsBegin(Domain::Relation,3,37);sink.JnRecordsBlock(Domain::Relation,0,rows,3,37);sink.JnRecordsEnd(Domain::Relation);sink.Finish();Refuses([&]{sink.JnRecordsEnd(Domain::Relation);});}
 {conversion::OfflineValidationSink sink(store,resources);Check(!sink.AllowsLargeJnRecords(Domain::Relation,100000001,37));Refuses([&]{sink.JnRecordsBegin(Domain::Relation,4,37);});Refuses([&]{sink.JnRecordsBegin(Domain::Relation,3,38);});sink.JnRecordsBegin(Domain::Relation,3,37);Refuses([&]{sink.JnRecordsBlock(Domain::Relation,1,rows,1,37);});Refuses([&]{sink.JnRecordsEnd(Domain::Relation);});}
 {conversion::OfflineValidationSink sink(store,resources);sink.JnRecordsBegin(Domain::Relation,3,37);rows[2].flags^=1;sink.JnRecordsBlock(Domain::Relation,0,rows,3,37);Refuses([&]{sink.JnRecordsEnd(Domain::Relation);});}
 {std::fstream file(root/"rows/relations/data.bin",std::ios::in|std::ios::out|std::ios::binary);Check(bool(file));char c;file.read(&c,1);c^=1;file.seekp(0);file.write(&c,1);}
 Refuses([&]{store.VisitRelations([](const auto*,uint64_t){});});
 }
 Check(resources.CleanupOwned());Check(resources.DiskCurrent()==0);
 limits.absoluteDiskLimit=8192;
 storage::ConversionResources tight(limits,[&]{return sample;},[]{return 1ull<<40;});
 {conversion::OfflineConversionStorage rejected(root/"reject",identity,tight);Refuses([&]{rejected.AppendRelation({});});Check(rejected.RelationCount()==0);Check(std::string(rejected.RelationState())=="failed");Refuses([&]{rejected.Seal();});}
 Check(tight.CleanupOwned());
 analysis::PagedRecordOptions options;options.beforeMemoryAllocation=[](uint64_t){throw std::runtime_error("injected commit refusal");};
 Refuses([&]{analysis::PagedRecordWriter writer(root/"allocation-refusal",identity,37,options);});Check(!std::filesystem::exists(root/"allocation-refusal"));
 std::puts("Relation empty/missing/count/order/hash/corruption/budget tests passed");
 }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
