cmake_minimum_required(VERSION 3.16)

# C is enabled only because LLVMConfig.cmake probes libedit with a C
# try_compile(); nothing here is C.
project(mbcheck C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# LLVM.  Override with -DLLVM_DIR=<prefix>/lib/cmake/llvm.
if(NOT LLVM_DIR AND EXISTS "/usr/lib/llvm-18/lib/cmake/llvm")
    set(LLVM_DIR "/usr/lib/llvm-18/lib/cmake/llvm")
endif()

# LLVM's version file is exact-match, so check the major version by hand.
find_package(LLVM REQUIRED CONFIG)
if(NOT LLVM_VERSION_MAJOR EQUAL 18)
    message(FATAL_ERROR
        "mbcheck requires LLVM 18; found ${LLVM_PACKAGE_VERSION} in "
        "${LLVM_DIR}.  Pass -DLLVM_DIR=<prefix>/lib/cmake/llvm for an LLVM 18 "
        "installation.  See README.md.")
endif()
message(STATUS "LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM)

separate_arguments(_LLVM_DEFS NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${_LLVM_DEFS})

# SVF.  SVF_DIR is the source root (holding svf/, svf-llvm/ and the build
# directory), which is also the variable SVF's own runtime reads.
if(NOT SVF_DIR AND DEFINED ENV{SVF_DIR})
    set(SVF_DIR "$ENV{SVF_DIR}")
endif()
set(SVF_DIR "${SVF_DIR}" CACHE PATH "SVF source root (contains svf/, svf-llvm/)")
set(SVF_BUILD_DIR "${SVF_DIR}/Release-build"
    CACHE PATH "SVF build directory (contains lib/libSvfCore.a and lib/extapi.bc)")

if(NOT SVF_DIR)
    message(FATAL_ERROR
        "SVF_DIR is not set.  Pass -DSVF_DIR=/path/to/SVF (the source root, "
        "not the build directory) or export SVF_DIR.  See README.md.")
endif()

set(SVF_INCLUDE_DIRS
    "${SVF_BUILD_DIR}/include"
    "${SVF_DIR}/svf/include"
    "${SVF_DIR}/svf-llvm/include"
)
foreach(_dir IN LISTS SVF_INCLUDE_DIRS)
    if(NOT IS_DIRECTORY "${_dir}")
        message(FATAL_ERROR
            "SVF include directory not found: ${_dir}\n"
            "SVF_DIR=${SVF_DIR} SVF_BUILD_DIR=${SVF_BUILD_DIR}\n"
            "Check that SVF 3.2 is built and that SVF_DIR points at its "
            "source root.  See README.md.")
    endif()
endforeach()

find_library(SVF_CORE_LIB SvfCore HINTS "${SVF_BUILD_DIR}/lib" REQUIRED)
find_library(SVF_LLVM_LIB SvfLLVM HINTS "${SVF_BUILD_DIR}/lib" REQUIRED)

# SVF resolves extapi.bc relative to cwd or $SVF_DIR; bake the path in so
# mbcheck runs from anywhere.
find_file(SVF_EXTAPI_BC extapi.bc HINTS "${SVF_BUILD_DIR}/lib" REQUIRED)

message(STATUS "SVF core lib : ${SVF_CORE_LIB}")
message(STATUS "SVF LLVM lib : ${SVF_LLVM_LIB}")
message(STATUS "SVF extapi   : ${SVF_EXTAPI_BC}")

set(mbcheck_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/driver/mbcheck.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/pta.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/phase_timer.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/allocatorPre.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/anchor_pass.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/causal_detect.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/function_pairs.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/fn_heuristics.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/herd7_transpiler.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/litmus_verifier.cpp"
)

add_executable(mbcheck ${mbcheck_SOURCES})

target_include_directories(mbcheck SYSTEM PRIVATE
    ${LLVM_INCLUDE_DIRS}
    ${SVF_INCLUDE_DIRS}
)
target_include_directories(mbcheck PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_options(mbcheck PRIVATE -fno-exceptions -fno-rtti)

target_compile_definitions(mbcheck PRIVATE
    MBCHECK_SVF_EXTAPI_BC="${SVF_EXTAPI_BC}")

if(LLVM_LINK_LLVM_DYLIB)
    set(LLVM_LINK_LIBS LLVM)
else()
    llvm_map_components_to_libnames(LLVM_LINK_LIBS
        analysis bitwriter core instcombine instrumentation ipo
        irreader linker passes scalaropts support target transformutils)
endif()

target_link_libraries(mbcheck PRIVATE
    ${SVF_LLVM_LIB}
    ${SVF_CORE_LIB}
    ${LLVM_LINK_LIBS}
)

set_target_properties(mbcheck PROPERTIES
    BUILD_RPATH "${SVF_BUILD_DIR}/lib"
    INSTALL_RPATH "${SVF_BUILD_DIR}/lib"
)
