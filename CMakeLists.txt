CMAKE_MINIMUM_REQUIRED(VERSION 2.8.12 FATAL_ERROR)

INCLUDE(GNUInstallDirs)

# ---[ Project
PROJECT(pthreadpool LANGUAGES C CXX)

# ---[ Options.
OPTION(PTHREADPOOL_BUILD_TESTS "Build pthreadpool unit tests" ON)
OPTION(PTHREADPOOL_BUILD_BENCHMARKS "Build pthreadpool micro-benchmarks" ON)

# ---[ CMake options
IF(PTHREADPOOL_BUILD_TESTS)
  ENABLE_TESTING()
ENDIF()

# ---[ Download deps
SET(CONFU_DEPENDENCIES_SOURCE_DIR ${CMAKE_SOURCE_DIR}/deps
  CACHE PATH "Confu-style dependencies source directory")
SET(CONFU_DEPENDENCIES_BINARY_DIR ${CMAKE_BINARY_DIR}/deps
  CACHE PATH "Confu-style dependencies binary directory")

IF(NOT TARGET fxdiv)
  CONFIGURE_FILE(cmake/DownloadFXdiv.cmake fxdiv-download/CMakeLists.txt)
  EXECUTE_PROCESS(COMMAND "${CMAKE_COMMAND}" -G "${CMAKE_GENERATOR}" .
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/fxdiv-download")
  EXECUTE_PROCESS(COMMAND "${CMAKE_COMMAND}" --build .
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/fxdiv-download")
ENDIF()

IF(NOT TARGET gtest)
  CONFIGURE_FILE(cmake/DownloadGoogleTest.cmake googletest-download/CMakeLists.txt)
  EXECUTE_PROCESS(COMMAND "${CMAKE_COMMAND}" -G "${CMAKE_GENERATOR}" .
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/googletest-download")
  EXECUTE_PROCESS(COMMAND "${CMAKE_COMMAND}" --build .
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/googletest-download")
ENDIF()

IF(NOT TARGET benchmark)
  CONFIGURE_FILE(cmake/DownloadGoogleBenchmark.cmake googlebenchmark-download/CMakeLists.txt)
  EXECUTE_PROCESS(COMMAND "${CMAKE_COMMAND}" -G "${CMAKE_GENERATOR}" .
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/googlebenchmark-download")
  EXECUTE_PROCESS(COMMAND "${CMAKE_COMMAND}" --build .
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/googlebenchmark-download")
ENDIF()

# ---[ Build flags
IF(NOT MSVC)
  SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c99")
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
ENDIF()

# ---[ pthreadpool library
IF(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  SET(PTHREADPOOL_SRCS src/threadpool-shim.c)
ELSE()
  SET(PTHREADPOOL_SRCS src/threadpool-pthreads.c)
ENDIF()

ADD_LIBRARY(pthreadpool ${PTHREADPOOL_SRCS})
SET_TARGET_PROPERTIES(pthreadpool PROPERTIES PUBLIC_HEADER include/pthreadpool.h)
TARGET_INCLUDE_DIRECTORIES(pthreadpool PUBLIC include)
IF(NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  SET(CMAKE_THREAD_PREFER_PTHREAD TRUE)
  SET(THREADS_PREFER_PTHREAD_FLAG TRUE)
  FIND_PACKAGE(Threads REQUIRED)
  TARGET_LINK_LIBRARIES(pthreadpool PUBLIC Threads::Threads)
ENDIF()

# ---[ Configure FXdiv
IF(NOT TARGET fxdiv)
  SET(FXDIV_BUILD_TESTS OFF CACHE BOOL "")
  SET(FXDIV_BUILD_BENCHMARKS OFF CACHE BOOL "")
  ADD_SUBDIRECTORY(
    "${CONFU_DEPENDENCIES_SOURCE_DIR}/fxdiv"
    "${CONFU_DEPENDENCIES_BINARY_DIR}/fxdiv")
ENDIF()
TARGET_LINK_LIBRARIES(pthreadpool PRIVATE fxdiv)

INSTALL(TARGETS pthreadpool
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

IF(PTHREADPOOL_BUILD_TESTS)
  # ---[ Build google test
  IF(NOT TARGET gtest)
    SET(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    ADD_SUBDIRECTORY(
      "${CONFU_DEPENDENCIES_SOURCE_DIR}/googletest"
      "${CONFU_DEPENDENCIES_BINARY_DIR}/googletest")
  ENDIF()

  ADD_EXECUTABLE(pthreadpool-test test/pthreadpool.cc)
  TARGET_LINK_LIBRARIES(pthreadpool-test pthreadpool gtest gtest_main)
  ADD_TEST(pthreadpool pthreadpool-test)
ENDIF()

IF(PTHREADPOOL_BUILD_BENCHMARKS)
  # ---[ Build google benchmark
  IF(NOT TARGET benchmark)
    SET(BENCHMARK_ENABLE_TESTING ON CACHE BOOL "")
    ADD_SUBDIRECTORY(
      "${CONFU_DEPENDENCIES_SOURCE_DIR}/googlebenchmark"
      "${CONFU_DEPENDENCIES_BINARY_DIR}/googlebenchmark")
  ENDIF()

  ADD_EXECUTABLE(latency-bench bench/latency.cc)
  TARGET_LINK_LIBRARIES(latency-bench pthreadpool benchmark)

  ADD_EXECUTABLE(throughput-bench bench/throughput.cc)
  TARGET_LINK_LIBRARIES(throughput-bench pthreadpool benchmark)
ENDIF()
