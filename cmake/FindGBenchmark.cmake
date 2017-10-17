CMAKE_MINIMUM_REQUIRED(VERSION 3.2 FATAL_ERROR)

FUNCTION(BUILD_GBENCHMARK)
  INCLUDE(ExternalProject)
  SET(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "")
  ExternalProject_Add(google_benchmark
    URL https://github.com/google/benchmark/archive/v1.2.0.zip
    URL_HASH SHA256=cc463b28cb3701a35c0855fbcefb75b29068443f1952b64dd5f4f669272e95ea
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS <BINARY_DIR>/src/libbenchmark.a
  )

  ExternalProject_Get_Property(google_benchmark install_dir)

  ADD_LIBRARY(gbenchmark STATIC IMPORTED)
  ADD_DEPENDENCIES(gbenchmark google_benchmark)

  ExternalProject_Get_Property(google_benchmark source_dir)
  SET(GBENCHMARK_INCLUDE_DIRS ${source_dir}/include PARENT_SCOPE)

  ExternalProject_Get_Property(google_benchmark binary_dir)
  SET_TARGET_PROPERTIES(gbenchmark PROPERTIES IMPORTED_LOCATION ${binary_dir}/src/libbenchmark.a)

  SET(GBENCHMARK_FOUND TRUE PARENT_SCOPE)
  SET(GBENCHMARK_LIBRARIES gbenchmark PARENT_SCOPE)

  MARK_AS_ADVANCED(FORCE GBENCHMARK_FOUND)
  MARK_AS_ADVANCED(FORCE GBENCHMARK_INCLUDE_DIRS)
  MARK_AS_ADVANCED(FORCE GBENCHMARK_LIBRARIES)
ENDFUNCTION(BUILD_GBENCHMARK)

BUILD_GBENCHMARK()
