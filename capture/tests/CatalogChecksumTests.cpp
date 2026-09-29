#include "../../public/common/TracyJnGpuCatalog.hpp"
#include <cstdio>
#include <stdexcept>
int main(){try{
 const auto expected=0x85944171f73967e8ull;
 if(tracy::JnGpuCatalogChecksum64("foobar",6)!=expected)throw std::runtime_error("checksum golden mismatch");
 auto hash=tracy::JnGpuCatalogChecksum64(nullptr,0);
 hash=tracy::JnGpuCatalogChecksum64Update(hash,"foo",3);
 hash=tracy::JnGpuCatalogChecksum64Update(hash,nullptr,0);
 hash=tracy::JnGpuCatalogChecksum64Update(hash,"bar",3);
 if(hash!=expected)throw std::runtime_error("fragmented checksum mismatch");
 puts("Catalog checksum fragments preserve protocol bytes");return 0;
 }catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
