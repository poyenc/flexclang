cmake_minimum_required(VERSION 3.20)
project(flexclang VERSION 0.1.0)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

add_executable(flexclang
  src/main.cpp
  src/FlexConfig.cpp
  src/FlexPassNameRegistry.cpp
  src/FlexPassLoader.cpp
  src/FlexPassConfigCallback.cpp
)

llvm_map_components_to_libnames(LLVM_LIBS
  AMDGPUCodeGen AMDGPUAsmParser AMDGPUDesc AMDGPUInfo
  X86CodeGen X86AsmParser X86Desc X86Info
  CodeGen Core Support Target Passes Analysis
  TransformUtils ScalarOpts InstCombine MC MCParser Option ipo
)

target_link_libraries(flexclang PRIVATE
  clangFrontend clangFrontendTool clangCodeGen
  clangDriver clangBasic clangSerialization clangOptions
  ${LLVM_LIBS}
)

target_compile_features(flexclang PRIVATE cxx_std_17)
install(TARGETS flexclang DESTINATION bin)
