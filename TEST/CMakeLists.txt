project(AMRPC_test)

include_directories(../include)

# for Locate GTest
find_package(GTEST REQUIRED)
include_directories(${GTEST_INCLUDE_DIRS})

unset(TEST_SOURCE)

SET(TEST_SOURCE ${TEST_SOURCE} ../src/amrpc.cpp)
SET(TEST_SOURCE ${TEST_SOURCE} ../src/conversion.cpp)
SET(TEST_SOURCE ${TEST_SOURCE} base.cpp)
SET(TEST_SOURCE ${TEST_SOURCE} serverTest.cpp)

add_executable(AMRPC_test test_main.cpp ${TEST_SOURCE})
target_link_libraries(AMRPC_test ${LIBS} ${GTEST_LIBRARIES})