cmake_minimum_required(VERSION 3.22)
project(burstpipe_app CXX)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)
add_subdirectory(${ROOT} ${CMAKE_BINARY_DIR}/bp)
# 디버그 variant에서도 처리 시간이 의미 있도록 코어는 항상 최적화
target_compile_options(bpcore PRIVATE -O2)
add_library(burstpipe_jni SHARED ${ROOT}/android/jni.cc)
target_compile_options(burstpipe_jni PRIVATE -O2)
target_link_libraries(burstpipe_jni bpcore)
