# examples/mir-pass-nop-inserter/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(MIRNopInserter)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

include_directories(${LLVM_INCLUDE_DIRS})
# AMDGPU target headers (for AMDGPU:: opcodes)
include_directories(${LLVM_BUILD_DIR}/lib/Target/AMDGPU)
include_directories(${LLVM_MAIN_SRC_DIR}/lib/Target/AMDGPU)

separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

# Build as MODULE (not SHARED) -- loaded at runtime, not linked
add_library(mir-nop-inserter MODULE MIRNopInserter.cpp)
target_compile_options(mir-nop-inserter PRIVATE -fno-rtti -fPIC)

# No need to link LLVM libs -- symbols resolved at load time from flexclang
set_target_properties(mir-nop-inserter PROPERTIES
  PREFIX ""           # produce "mir-nop-inserter.so", not "libmir-nop-inserter.so"
  SUFFIX ".so"
)
