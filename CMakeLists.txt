cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME sorted_set)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

# ←←← ADD ALL YOUR SOURCE FILES HERE
set(EXTENSION_SOURCES
    src/sorted_set_extension.cpp
    src/set_search_core.cpp  
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

target_compile_options(${EXTENSION_NAME} PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
target_compile_options(${LOADABLE_EXTENSION_NAME} PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")