cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# 拓竹监控固件统一命名,产物为 bambumonitor.bin
# 版本号由 git tag vX.Y.Z 管理,此处跟随主版本
project(bambumonitor VERSION 1.0.0)
