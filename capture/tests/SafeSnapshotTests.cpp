#include <cstdio>
#if __has_include("../../server/TracySafeSnapshot.hpp")
#include "../../server/TracySafeSnapshot.hpp"
#include "../../server/TracyFileRead.hpp"
#include "../../server/TracyTraceFormat.hpp"
#include <fstream>
int main(int argc,char** argv){try{
 if(argc!=2)return 2;auto root=std::filesystem::path(argv[1]);std::filesystem::create_directories(root);auto target=root/"out.tracy";
 {std::ofstream old(target);old<<"historical trace";}
 auto failed=tracy::SaveSnapshotAtomically(target,tracy::FileCompression::Zstd,1,1,[](tracy::FileWrite&){throw std::runtime_error("injected save failure");});
 if(failed.success||std::string(failed.error.data()).find("injected save failure")==std::string::npos)throw std::runtime_error("missing failure");
 {std::ifstream old(target);std::string value;std::getline(old,value);if(value!="historical trace")throw std::runtime_error("old trace overwritten on failure");}
 auto good=tracy::SaveSnapshotAtomically(target,tracy::FileCompression::Zstd,1,1,[](tracy::FileWrite& file){auto h=tracy::EncodeTraceHeader();file.Write(h.data(),h.size());});
 if(!good.success)throw std::runtime_error(good.error.data());
 std::unique_ptr<tracy::FileRead> in(tracy::FileRead::Open(target));tracy::ReadExtendedTraceHeader(*in);
 for(const auto& p:std::filesystem::directory_iterator(root))if(p.path()!=target)throw std::runtime_error("candidate residue");
 puts("atomic snapshot success and failure passed");return 0;
 }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
#else
int main(){puts("missing atomic snapshot helper");return 1;}
#endif
