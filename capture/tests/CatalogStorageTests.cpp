#include <cstdio>
// Catalog backend domain and finalize contract.
#if __has_include("../../server/TracyOfflineCatalog.hpp")
#include "../src/OfflineConversionStorage.hpp"
#include "../../server/TracyOfflineCatalog.hpp"
#include <chrono>
static void Check(bool ok){if(!ok)throw std::runtime_error("Catalog storage invariant");}
int main(int argc,char** argv){try{
 if(argc!=2)return 2;auto root=std::filesystem::path(argv[1])/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());std::filesystem::create_directories(root);
 tracy::storage::ResourceLimits limits;limits.streamBytes=64ull<<20;limits.diskReserveBytes=0;tracy::storage::ResourceSample sample;sample.systemCommitLimit=64ull<<30;
 tracy::storage::ConversionResources resources(limits,[&]{return sample;},[]{return 1ull<<40;});tracy::conversion::OfflineConversionStorage storage(root/"store",std::string(64,'a'),resources);
 std::array<std::vector<unsigned char>,tracy::OfflineCatalogDomainCount> expected;
#define X(name,field,type,mut) {tracy::type row{};const auto id=size_t(tracy::OfflineCatalogDomain::name);expected[id].assign(sizeof(row),uint8_t(id+1));std::memcpy(&row,expected[id].data(),sizeof(row));Check(storage.AppendCatalog(tracy::OfflineCatalogDomain::name,&row,sizeof(row))==0);Check(storage.CatalogCount(tracy::OfflineCatalogDomain::name)==1);}
 TRACY_OFFLINE_CATALOG_FIXED_DOMAINS(X)
#undef X
 tracy::OfflineCatalogString row{};row.generation=1;row.header.stringId=2;row.header.byteLength=3;row.bytes[0]='a';row.bytes[1]='b';row.bytes[2]='c';storage.AppendCatalog(tracy::OfflineCatalogDomain::CatalogString,&row,sizeof(row));
 storage.Seal();
 tracy::JnGpuCatalogBatchData batch{};batch.storedChecksum=123;storage.PutCatalog(tracy::OfflineCatalogDomain::CatalogBatch,0,&batch,sizeof(batch));
 std::memcpy(expected[size_t(tracy::OfflineCatalogDomain::CatalogBatch)].data(),&batch,sizeof(batch));
 storage.SealCatalog({1,1,1,1});tracy::JnGpuCatalogBatchData read{};storage.ReadCatalog(tracy::OfflineCatalogDomain::CatalogBatch,0,&read,sizeof(read));Check(read.storedChecksum==123);
#define X(name,field,type,mut) {const auto& bytes=expected[size_t(tracy::OfflineCatalogDomain::name)];uint64_t seen=0;storage.VisitCatalog(tracy::OfflineCatalogDomain::name,[&](const void* p,uint64_t n){Check(n==1&&std::memcmp(p,bytes.data(),bytes.size())==0);seen+=n;});Check(seen==1);tracy::analysis::Sha256Builder hash;hash.Update(bytes.data(),bytes.size());Check(storage.CatalogHash(tracy::OfflineCatalogDomain::name)==hash.FinalHex());}
 TRACY_OFFLINE_CATALOG_FIXED_DOMAINS(X)
#undef X
 Check(storage.CatalogHash(tracy::OfflineCatalogDomain::CatalogString).size()==64);
 puts("Catalog fixed domains, mutable finalize and string output hash passed");return 0;
 }catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
#else
int main(){puts("missing Catalog storage domains");return 1;}
#endif
