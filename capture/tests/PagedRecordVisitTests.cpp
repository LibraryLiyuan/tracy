#include "../../analysis/TracyPagedRecordStore.hpp"
#include <cstdio>
#include <fstream>
#include <source_location>
#include <stdexcept>
static void Check(bool value,std::source_location at=std::source_location::current()){if(!value)throw std::runtime_error("paged visit invariant at "+std::to_string(at.line()));}
template<class F> static void Refuses(F&& f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}Check(failed);}
int main(int argc,char** argv){try{
 if(argc!=2)return 2;const auto root=std::filesystem::path(argv[1]);std::filesystem::create_directories(root);tracy::analysis::PagedRecordOptions options;options.blockBytes=4096;
 {tracy::analysis::PagedRecordWriter writer(root/"valid",std::string(64,'a'),sizeof(uint64_t),options);for(uint64_t i=0;i<10000;++i)writer.Append(&i,1);writer.Commit();}
 tracy::analysis::PagedRecordReader reader(root/"valid",std::string(64,'a'),sizeof(uint64_t),options);uint64_t expected=0;
 const auto stats=reader.VisitValidated([&](const void* data,uint64_t count){const auto* rows=static_cast<const uint64_t*>(data);for(uint64_t i=0;i<count;++i)Check(rows[i]==expected++);});
 Check(expected==10000&&stats.records==10000&&stats.decodedBytes==80000&&stats.blocks>1&&stats.storedBytes>0);
 {tracy::analysis::PagedRecordWriter writer(root/"corrupt",std::string(64,'c'),sizeof(uint64_t),options);uint64_t value=9;for(unsigned i=0;i<2000;++i)writer.Append(&value,1);writer.Commit();}
 {std::fstream file(root/"corrupt"/"data.bin",std::ios::binary|std::ios::in|std::ios::out);char byte=0;file.read(&byte,1);file.seekp(0);byte^=0x5a;file.write(&byte,1);file.flush();Check(bool(file));}
 tracy::analysis::PagedRecordReader corrupt(root/"corrupt",std::string(64,'c'),sizeof(uint64_t),options);Refuses([&]{corrupt.VisitValidated([](const void*,uint64_t){});});
 puts("Paged records validate and visit each decoded block once");return 0;
 }catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
