//
// Created by yyz on 2019/9/29.
//
#include <boost/thread.hpp>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <folly/init/Init.h>

void link_boost_thread_tag() {
    boost::thread t;
    return;
    link_boost_thread_tag();
}

int main(int argc,char* argv[]) {
    testing::InitGoogleTest(&argc, argv);
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    folly::init(&argc, &argv, true);
    FLAGS_logtostderr=true;
    FLAGS_v = 3;

    return RUN_ALL_TESTS();
}