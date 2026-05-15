cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME sorted_set)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include directories (this is the important part you asked about)
include_directories(src/include)

# Source files
set(EXTENSION_SOURCES
    src/sorted_set_extension.cpp
)

# Build both static and loadable extension
build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Optional: compile flags
target_compile_options(${EXTENSION_NAME} PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
target_compile_options(${LOADABLE_EXTENSION_NAME} PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)

# Install rules (good practice)
install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")