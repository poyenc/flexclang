cmake_minimum_required(VERSION 3.20)
project(IRInstCounter)

find_package(LLVM REQUIRED CONFIG)
include_directories(${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

add_library(ir-inst-counter MODULE IRInstCounter.cpp)
target_compile_options(ir-inst-counter PRIVATE -fno-rtti -fPIC)
set_target_properties(ir-inst-counter PROPERTIES PREFIX "" SUFFIX ".so")
