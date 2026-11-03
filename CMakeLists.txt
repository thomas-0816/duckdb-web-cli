cmake_minimum_required(VERSION 3.16)

project(duckdb_docs_search VERSION 1.0.0 LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)

# Use a portable vectorization baseline instead of -march=native so the binary
# still runs on older CPUs while using the wider instruction sets that the CPU
# backend of llama.cpp benefits from. These flags also apply to the llama.cpp
# and ggml targets added below.
set(DOCS_ARCH_FLAGS)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    list(APPEND DOCS_ARCH_FLAGS -march=x86-64-v3 -mtune=generic)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    if(APPLE)
      list(APPEND DOCS_ARCH_FLAGS -mcpu=apple-m1)
    else()
      list(APPEND DOCS_ARCH_FLAGS -march=armv8-a -mtune=generic)
    endif()
  endif()
  if(DOCS_ARCH_FLAGS)
    add_compile_options(${DOCS_ARCH_FLAGS})
  endif()
endif()

set(DOCS_RUNTIME_FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/duckdb-blog.md
  ${CMAKE_CURRENT_SOURCE_DIR}/duckdb-docs.md)

# The semantic index uses a Q8_0 GGUF embedding model, and the model is always
# embedded in the binary. scripts/fetch_models.sh downloads it into the source
# directory; the build fails if the file is missing.
set(DOCS_EMBED_MODEL_FILE ${CMAKE_CURRENT_SOURCE_DIR}/bge-small-en-v1.5-q8_0.gguf)
if(NOT EXISTS ${DOCS_EMBED_MODEL_FILE})
  message(FATAL_ERROR
    "Semantic embedding model not found: ${DOCS_EMBED_MODEL_FILE}\n"
    "Run scripts/fetch_models.sh before building.")
endif()

# Build the llama.cpp submodule as a static library. Its runtime is linked into
# the docs binary, so no separate runtime installation is needed.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# The common library renders the model's chat template with its Jinja engine,
# so the answer prompt follows the GGUF metadata instead of a hand-written copy.
set(LLAMA_BUILD_COMMON ON CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_APP OFF CACHE BOOL "" FORCE)
set(LLAMA_OPENSSL ON CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)

# GPU backends: Metal on Apple, Vulkan everywhere else (Linux and Windows).
# Embedding runs on the GPU when a compatible device and driver are present,
# and llama.cpp falls back to the CPU backend otherwise.
if(APPLE)
  set(GGML_METAL ON CACHE BOOL "" FORCE)
  set(GGML_METAL_EMBED_LIBRARY ON CACHE BOOL "" FORCE)
else()
  set(GGML_VULKAN ON CACHE BOOL "" FORCE)
  if(WIN32)
    if(NOT DEFINED ENV{VULKAN_SDK})
      message(STATUS "Vulkan SDK not found in VULKAN_SDK; relying on the CMake glslc lookup")
    endif()
  else()
    find_program(DOCS_GLSLC glslc)
    if(NOT DOCS_GLSLC)
      message(FATAL_ERROR
        "glslc is required to build the Vulkan backend.\n"
        "Install the Vulkan SDK or the glslc package and re-run CMake.")
    endif()
  endif()
endif()
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/llama.cpp/CMakeLists.txt)
  message(FATAL_ERROR
    "llama.cpp submodule is missing.\n"
    "Run: git submodule update --init --recursive")
endif()
add_subdirectory(third_party/llama.cpp EXCLUDE_FROM_ALL)

add_library(md4c STATIC
  third_party/md4c/md4c.c
  third_party/md4c/entity.c
)
target_include_directories(md4c PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/md4c)

# Missing Markdown corpora and the answer model are downloaded at startup
# through llama.cpp's common download helpers, the same code the llama CLI
# uses. The common library builds cpp-httplib with HTTPS support when OpenSSL
# is available.
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(docs_core STATIC
  src/docs_search.cpp
  src/docs_storage.cpp
  src/semantic.cpp
  src/generation.cpp
  src/cli/run.cpp
  src/cli/markdown.cpp
  src/cli/pager.cpp
)
target_link_libraries(docs_core PUBLIC md4c llama)
target_link_libraries(docs_core PRIVATE llama-common)

target_include_directories(docs_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/llama.cpp/vendor
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(docs_core PRIVATE -ffunction-sections -fdata-sections)
  set(DOCS_LINK_OPTIONS -Wl,--gc-sections -s)
endif()

add_executable(docs src/cli/docs_main.cpp)
target_link_libraries(docs PRIVATE docs_core)

# Embed the GGUF bytes directly into the executable. An assembly .incbin keeps
# the build fast and avoids generating a huge C array; no separate model file is
# needed at run time.
file(SHA1 ${DOCS_EMBED_MODEL_FILE} DOCS_EMBEDDED_MODEL_SHA1)
set(DOCS_MODEL_ASM ${CMAKE_CURRENT_BINARY_DIR}/embedded_model.S)
if(APPLE)
  set(DOCS_MODEL_SYMBOL_PREFIX "_")
  set(DOCS_MODEL_SECTION "__TEXT,__const")
  set(DOCS_ASM_NOTE "")
else()
  set(DOCS_MODEL_SYMBOL_PREFIX "")
  set(DOCS_MODEL_SECTION ".rodata")
  set(DOCS_ASM_NOTE "    .section .note.GNU-stack,\"\",@progbits\n")
endif()
file(WRITE ${DOCS_MODEL_ASM}
  "    .section ${DOCS_MODEL_SECTION}\n"
  "    .p2align 6\n"
  "    .globl ${DOCS_MODEL_SYMBOL_PREFIX}docs_model_gguf_start\n"
  "    .globl ${DOCS_MODEL_SYMBOL_PREFIX}docs_model_gguf_end\n"
  "${DOCS_MODEL_SYMBOL_PREFIX}docs_model_gguf_start:\n"
  "    .incbin \"${DOCS_EMBED_MODEL_FILE}\"\n"
  "${DOCS_MODEL_SYMBOL_PREFIX}docs_model_gguf_end:\n"
  "${DOCS_ASM_NOTE}")
target_sources(docs PRIVATE ${DOCS_MODEL_ASM})
set_source_files_properties(${DOCS_MODEL_ASM} PROPERTIES
  OBJECT_DEPENDS ${DOCS_EMBED_MODEL_FILE})
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${DOCS_EMBED_MODEL_FILE})
target_compile_definitions(docs_core PRIVATE
  DOCS_EMBEDDED_MODEL_SHA1="${DOCS_EMBEDDED_MODEL_SHA1}")
message(STATUS "Embedding semantic model ${DOCS_EMBED_MODEL_FILE}")

if(DOCS_LINK_OPTIONS)
  target_link_options(docs PRIVATE ${DOCS_LINK_OPTIONS})
endif()
set_target_properties(docs PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Keep the Markdown sources next to the binary so it works directly from the
# build tree. Generated index/data files are written next to the binary as well,
# so an installation stays self-contained.
add_custom_command(TARGET docs POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DOCS_RUNTIME_FILES} $<TARGET_FILE_DIR:docs>
  COMMENT "Copying DuckDB documentation sources next to the docs binary"
  VERBATIM)

install(TARGETS docs RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${DOCS_RUNTIME_FILES} DESTINATION ${CMAKE_INSTALL_BINDIR})
