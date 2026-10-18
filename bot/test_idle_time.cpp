#include "idle_time.hpp"

#include <cassert>
using namespace std::chrono_literals;

int main()
{
    IdleTime idle;
    assert(idle.Record(400ms, true) == 0);
    assert(idle.Record(700ms, true) == 1);
    assert(idle.Record(900ms, true) == 1);
    assert(idle.Record(60s, false) == 0);
    assert(idle.Record(10s, true) == 10);
    assert(idle.Record(0s, true) == 0);
    assert(idle.Record(500ms, true) == 0);
    assert(idle.Record(10s, false) == 0);
    assert(idle.Record(500ms, true) == 1);
}
