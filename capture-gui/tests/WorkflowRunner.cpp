#include "Controller.hpp"
#include <chrono>
#include <iostream>
#include <thread>
using namespace capturegui;
int wmain(int argc, wchar_t **argv)
{
    try
    {
        if (argc < 2)
            return 2;
        auto j = ReadJson(argv[1]);
        auto s = Settings::FromJson(j);
        Controller c;
        if (j.value("launch_only",false))
            c.Launch(s);
        else if (j.contains("recover"))
            c.Recover(Wide(j.at("recover").get<std::string>()));
        else
        {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, j.at("target_pid").get<DWORD>());
            if (!h)
                throw std::runtime_error(ErrorText());
            auto target = Identify(h);
            CloseHandle(h);
            s.ready = true;
            c.Start(s, target);
        }
        std::string last;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
        for (;;)
        {
            auto v = c.Snapshot();
            if (v.state != last)
            {
                std::cout << v.state << ": " << v.message << std::endl;
                last = v.state;
            }
            if (!v.busy)
            {
                if(j.value("launch_only",false)) {
                    AtomicJson(Wide(j.at("launch_result").get<std::string>()),{{"state",v.state},{"message",v.message},{"target",v.target.ToJson()}});
                    return v.state=="waiting_ready"?0:1;
                }
                std::cout << v.task.dump() << std::endl;
                return v.state == "complete" || v.state == "partial" ? 0 : 1;
            }
            if (std::chrono::steady_clock::now() > deadline)
            {
                c.ForceClose();
                throw std::runtime_error("test timeout");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
