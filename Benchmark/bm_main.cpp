#include <benchmark/benchmark.h>
#include <boost/thread.hpp>

void link_boost_thread_tag() {
    boost::thread t;
    return;
    link_boost_thread_tag();
}

BENCHMARK_MAIN();
