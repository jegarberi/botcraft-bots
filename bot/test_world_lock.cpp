// The shared world's lock: a writer gets in quickly while 16 threads read nonstop (each reading again inside its
// read, as World methods can), and nothing deadlocks.
#include "botcraft/Game/World/World.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

int main()
{
    Botcraft::WritePreferringMutex m;
    std::atomic<bool> stop = false;
    std::vector<std::thread> readers;
    for (int i = 0; i < 16; ++i)
    {
        readers.emplace_back([&]()
            {
                while (!stop)
                {
                    std::shared_lock<Botcraft::WritePreferringMutex> outer(m);
                    std::shared_lock<Botcraft::WritePreferringMutex> inner(m);   // a read inside a read
                    volatile int spin = 0;
                    for (int k = 0; k < 200; ++k) spin = spin + k;
                }
            });
    }
    long long worst = 0;
    for (int i = 0; i < 200; ++i)
    {
        const auto start = std::chrono::steady_clock::now();
        {
            std::unique_lock<Botcraft::WritePreferringMutex> write(m);
            worst = std::max<long long>(worst, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop = true;
    for (std::thread& t : readers) t.join();
    std::printf("world lock OK: worst write wait %lld us with 16 readers\n", worst);
    assert(worst < 50000);
}
