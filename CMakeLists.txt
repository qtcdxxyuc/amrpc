cmake_minimum_required(VERSION 3.15)
project(AMRPC)

#########################################################
# 基本通用编译选项
#########################################################
set(CMAKE_VERBOSE_MAKEFILE OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(THREADS_PREFER_PTHREAD_FLAG ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_SKIP_BUILD_RPATH ON)
add_compile_options(-Wno-deprecated-declarations)
add_compile_options(-DECVAPI_EXPORTS)
add_compile_options(-fPIC -Wall -Wno-unused-function)
add_compile_options(-msse2 -msse3 -msse4 -msse4.1 -msse4.2 -mssse3 -mavx -mf16c -mavx2 -mfma -mpclmul)
add_definitions(-DECV_DETAIL=1)
add_definitions(-DECV_VERBOSE=2)
add_definitions(-DECV_DTRACE=3)
add_definitions(-DECV_VLOG_LEVEL=4)
#########################################################
# 第三方库
#########################################################

set(CMAKE_MODULE_PATH "/opt/Devs")

#for ecv5
find_package(ECV5)
include_directories(${ECV_INCLUDE_DIRS})
set(LIBS ${LIBS} ${ECV_LIBS})

#for folly
find_package(Folly REQUIRED Benchmark)
include_directories(${Folly_INCLUDE_DIRS})
set(LIBS ${LIBS} ${Folly_LIBS} Folly::follybenchmark)

# for folly iberty
set(LIBS ${LIBS} iberty)
# for folly libevent
set(LIBS ${LIBS} event)
set(LIBS ${LIBS} double-conversion)
set(LIBS ${LIBS} ssl)
set(LIBS ${LIBS} crypto)

#for boost
find_package(Boost REQUIRED filesystem thread regex coroutine fiber context program_options serialization)
include_directories(${Boost_INCLUDE_DIRS})
set(LIBS ${LIBS} ${Boost_LIBRARIES})

#for glog
set(LIBS ${LIBS} glog)

#for gflags
set(LIBS ${LIBS} gflags)

# for benchmark
find_package(Benchmark REQUIRED)
include_directories(${Benchmark_INCLUDE_DIRS})
set(LIBS ${LIBS} ${Benchmark_LIBS})

# for messagepack
find_package(Msgpack REQUIRED)
include_directories(${Msgpack_INCLUDE_DIRS})

set(LIBS ${LIBS} pthread)
set(LIBS ${LIBS} stdc++)
set(LIBS ${LIBS} dl)
set(LIBS ${LIBS} unwind)
set(LIBS ${LIBS} m)
set(LIBS ${LIBS} z)

#########################################################
# amrpc
#########################################################

# lib property
set(LIBRARY_OUTPUT_PATH ./lib)
string(TOLOWER ${CMAKE_BUILD_TYPE} BUILD_TYPE_POSTFIX)
set(CMAKE_DEBUG_POSTFIX _${BUILD_TYPE_POSTFIX})
set(CMAKE_RELEASE_POSTFIX _${BUILD_TYPE_POSTFIX})

include_directories(include)

# libarpc
aux_source_directory(src source_files)
#add_library(arpc ${source_files})

#########################################################
# other
#########################################################
add_subdirectory(Benchmark)
add_subdirectory(TEST)
