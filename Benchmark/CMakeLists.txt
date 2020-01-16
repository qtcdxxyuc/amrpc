project(AMRPC_benchmark)

include_directories(../include)

# for Locate GTest
find_package(Benchmark REQUIRED)
include_directories(${BENCHMARK_INCLUDE_DIRS})

unset(BM_SOURCE)

SET(BM_SOURCE ${BM_SOURCE} bm_amrpc.cpp)
aux_source_directory(../src BM_SOURCE)

add_executable(AMRPC_benchmark bm_main.cpp ${BM_SOURCE})
target_link_libraries(AMRPC_benchmark ${LIBS} ${BENCHMARK_LIBRARIES} )