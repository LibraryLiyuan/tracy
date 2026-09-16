#include "../../analysis/TracyHash.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>

static void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}

int main()
{
    tracy::analysis::Sha256Builder empty;
    Check(empty.FinalHex()=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","sha256_empty");
    const std::string value="abc";
    tracy::analysis::Sha256Builder whole;whole.Update(value.data(),value.size());
    tracy::analysis::Sha256Builder chunked;for(const auto ch:value)chunked.Update(&ch,1);
    Check(whole.FinalHex()=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","sha256_abc");
    Check(chunked.FinalHex()==whole.FinalHex(),"sha256_chunked");
#ifdef _WIN32
    Check(std::string(tracy::analysis::Sha256Backend())=="windows-cng","sha256_backend");
#else
    Check(std::string(tracy::analysis::Sha256Backend())=="portable","sha256_backend");
#endif
    std::puts("SHA-256 vectors, streaming and backend passed");
}
