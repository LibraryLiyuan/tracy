#include "TracyQueryIndex.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
int main(int argc,char** argv)
{
    try {
        if(argc!=3) throw std::runtime_error("expected input trace and isolated output directory");
        const std::filesystem::path root=argv[2];
        if(std::filesystem::exists(root)) throw std::runtime_error("refusing existing output");
        std::filesystem::create_directories(root);
        const auto input=root/"fixture.tracy";
        std::filesystem::copy_file(argv[1],input);
        const auto manifest=tracy::query::QueryIndex::Build(input);
        const auto validation=tracy::query::QueryIndex::Validate(input,true);
        if(!validation.manifest) throw std::runtime_error(validation.reason);
        if(!manifest.cpuZones.count || manifest.cpuZones.bytes>=manifest.cpuZones.count*52/2)
            throw std::runtime_error("CPU sidecar did not compress the repeated real fixture records");
        auto source=tracy::query::QueryIndex::Open(manifest);
        const auto info=source->GetTraceInfo();
        if(info.counts.cpuZones!=manifest.cpuZones.count) throw std::runtime_error("indexed count changed");
        tracy::analysis::ScanRange range; range.limit=10;
        const auto zones=source->ScanCpuZones(range);
        if(zones.empty()) throw std::runtime_error("compressed indexed trace lost its zones");
        std::cout<<"compressed index passed cpu="<<manifest.cpuZones.count<<" stored="<<manifest.cpuZones.bytes<<'\n';
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
