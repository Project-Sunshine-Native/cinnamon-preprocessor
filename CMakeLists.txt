cmake_minimum_required(VERSION 3.21)
project(cinnamon_preprocessor LANGUAGES C)

if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    set(_default_cli OFF)
    set(_default_wasm ON)
else()
    set(_default_cli ON)
    set(_default_wasm OFF)
endif()
option(CINNAMON_PREPROCESSOR_BUILD_CLI "Build the native command-line front end" ${_default_cli})
option(CINNAMON_PREPROCESSOR_BUILD_WASM "Build the Emscripten front end" ${_default_wasm})
if(CINNAMON_PREPROCESSOR_BUILD_CLI AND CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    message(FATAL_ERROR
        "The CLI front end requires a native toolchain; use CINNAMON_PREPROCESSOR_BUILD_WASM instead")
endif()
if(CINNAMON_PREPROCESSOR_BUILD_WASM AND NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    message(FATAL_ERROR "The WASM front end requires an Emscripten toolchain")
endif()

option(ENABLE_WAD14 "Enable support for WAD Version 14 and below in cinnamon-preprocessor" ON)
option(ENABLE_WAD16 "Enable support for WAD Version 15/16 in cinnamon-preprocessor" ON)
option(ENABLE_WAD17 "Enable support for WAD Version 17 in cinnamon-preprocessor" ON)
if(NOT ENABLE_WAD14 AND NOT ENABLE_WAD16 AND NOT ENABLE_WAD17)
    message(FATAL_ERROR "cinnamon-preprocessor needs at least one WAD version enabled")
endif()

set(CINNAMON_PREPROCESSOR_VENDOR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vendor/cinnamon")
# GameMaker's compressed QOI textures only need bzip2 decompression.
set(CINNAMON_PREPROCESSOR_BZIP2_SOURCES
    "${CINNAMON_PREPROCESSOR_VENDOR_ROOT}/vendor/bzip2/bz_internal_error.c"
    "${CINNAMON_PREPROCESSOR_VENDOR_ROOT}/vendor/bzip2/bzlib.c"
    "${CINNAMON_PREPROCESSOR_VENDOR_ROOT}/vendor/bzip2/crctable.c"
    "${CINNAMON_PREPROCESSOR_VENDOR_ROOT}/vendor/bzip2/decompress.c"
    "${CINNAMON_PREPROCESSOR_VENDOR_ROOT}/vendor/bzip2/huffman.c"
    "${CINNAMON_PREPROCESSOR_VENDOR_ROOT}/vendor/bzip2/randtable.c"
)

add_library(cinnamon_core STATIC
    src/core/cinnamon_core.c
    src/core/game_profiles.c
    src/core/preprocess_config.c
    src/core/preprocess_context.c
    src/core/preprocess_datawin.c
    src/core/preprocess_options.c
    src/core/preprocess_run.c
    src/core/preprocess_staging.c
    src/audio/audio_bank.c
    src/audio/audio_convert.c
    src/audio/audio_decode.c
    src/audio/audio_encode.c
    src/audio/audio_sources.c
    src/audio/audio_streamed_bank.c
    src/audio/common/sound_bank.c
    src/texture/texture_atlas.c
    src/texture/texture_atlas_packer.c
    src/texture/texture_cache.c
    src/texture/texture_convert.c
    src/texture/texture_encode.c
    src/texture/texture_encode_3ds.c
    src/texture/texture_encode_wii.c
    src/texture/texture_inputs.c
    src/texture/texture_lz11.c
    src/texture/texture_pages.c
    src/texture/texture_pixels.c
    src/target/companion_files.c
    src/target/preprocess_target.c
    src/target/target_paths.c
    src/common/byteio.c
    src/common/fs_utils.c
    src/common/host_log.c
    src/common/path_utils.c
    src/common/string_utils.c
    src/common/stb_impl.c
    "${CINNAMON_PREPROCESSOR_VENDOR_ROOT}/src/binary_reader.c"
    "${CINNAMON_PREPROCESSOR_VENDOR_ROOT}/src/data_win.c"
    "${CINNAMON_PREPROCESSOR_VENDOR_ROOT}/src/image/image_decoder.c"
    "${CINNAMON_PREPROCESSOR_VENDOR_ROOT}/src/json_writer.c"
    "${CINNAMON_PREPROCESSOR_VENDOR_ROOT}/src/log.c"
    "${CINNAMON_PREPROCESSOR_VENDOR_ROOT}/src/string_builder.c"
    ${CINNAMON_PREPROCESSOR_BZIP2_SOURCES}
)

target_compile_features(cinnamon_core PUBLIC c_std_99)
target_include_directories(cinnamon_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CINNAMON_PREPROCESSOR_VENDOR_ROOT}/src"
    "${CINNAMON_PREPROCESSOR_VENDOR_ROOT}/vendor"
    PRIVATE
    "${CINNAMON_PREPROCESSOR_VENDOR_ROOT}/vendor/bzip2"
    "${CINNAMON_PREPROCESSOR_VENDOR_ROOT}/vendor/stb/ds"
    "${CINNAMON_PREPROCESSOR_VENDOR_ROOT}/vendor/stb/image"
)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    target_compile_definitions(cinnamon_core PRIVATE
        CINNAMON_PREPROCESSOR_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()

foreach(version 14 16 17)
    if(ENABLE_WAD${version})
        target_compile_definitions(cinnamon_core PUBLIC ENABLE_WAD${version})
    endif()
endforeach()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(cinnamon_core PUBLIC m)
endif()

set(CINNAMON_PREPROCESSOR_TARGETS cinnamon_core)
if(CINNAMON_PREPROCESSOR_BUILD_CLI OR CINNAMON_PREPROCESSOR_BUILD_WASM)
    add_executable(cinnamon-preprocessor
        src/frontend/preprocess_frontend.c
        src/frontend/cli/cli_args.c
    )
    target_link_libraries(cinnamon-preprocessor PRIVATE cinnamon_core)
    list(APPEND CINNAMON_PREPROCESSOR_TARGETS cinnamon-preprocessor)
    if(CINNAMON_PREPROCESSOR_BUILD_CLI)
        target_sources(cinnamon-preprocessor PRIVATE src/frontend/cli/main.c)
    else()
        target_sources(cinnamon-preprocessor PRIVATE
            src/frontend/wasm/wasm_main.c
            src/frontend/wasm/preprocess_wasm.c)
        set_target_properties(cinnamon-preprocessor PROPERTIES SUFFIX ".mjs")
        target_link_options(cinnamon-preprocessor PRIVATE
            "-sMODULARIZE=1"
            "-sEXPORT_ES6=1"
            "-sENVIRONMENT=web,worker,node"
            "-sINVOKE_RUN=0"
            "-sEXIT_RUNTIME=1"
            "-sSTACK_SIZE=2097152"
            "-sALLOW_MEMORY_GROWTH=1"
            "-sFORCE_FILESYSTEM=1"
            "-sEXPORTED_FUNCTIONS=['_main','_malloc','_free']"
            "-sEXPORTED_RUNTIME_METHODS=['FS','callMain','ccall','cwrap','HEAPU8','UTF8ToString']"
        )
    endif()

    set(CINNAMON_PREPROCESSOR_ASSETS
        game_profiles.cfg
        resources/wii/icon.png
        resources/wii/meta.xml
    )
    if(CINNAMON_PREPROCESSOR_BUILD_WASM)
        set(CINNAMON_PREPROCESSOR_WEBSITE_ASSET_DIR
            "${CMAKE_CURRENT_BINARY_DIR}/website-wasm-assets" CACHE PATH
            "Destination for browser preprocessor assets")
    endif()

    set(asset_sources)
    set(runtime_asset_commands
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:cinnamon-preprocessor>/resources/wii")
    set(website_asset_commands)
    if(CINNAMON_PREPROCESSOR_BUILD_WASM)
        list(APPEND website_asset_commands
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CINNAMON_PREPROCESSOR_WEBSITE_ASSET_DIR}/resources/wii")
    endif()
    foreach(asset IN LISTS CINNAMON_PREPROCESSOR_ASSETS)
        set(source "${CMAKE_CURRENT_SOURCE_DIR}/${asset}")
        list(APPEND asset_sources "${source}")
        list(APPEND runtime_asset_commands
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${source}" "$<TARGET_FILE_DIR:cinnamon-preprocessor>/${asset}")
        if(CINNAMON_PREPROCESSOR_BUILD_WASM)
            list(APPEND website_asset_commands
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${source}" "${CINNAMON_PREPROCESSOR_WEBSITE_ASSET_DIR}/${asset}")
        endif()
    endforeach()
    add_custom_target(cinnamon-preprocessor-assets
        ${runtime_asset_commands}
        DEPENDS ${asset_sources}
        VERBATIM)
    add_dependencies(cinnamon-preprocessor cinnamon-preprocessor-assets)

    if(CINNAMON_PREPROCESSOR_BUILD_WASM)
        add_custom_target(cinnamon-preprocessor-website-assets
            ${website_asset_commands}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:cinnamon-preprocessor>"
                "$<TARGET_FILE_DIR:cinnamon-preprocessor>/cinnamon-preprocessor.wasm"
                "${CINNAMON_PREPROCESSOR_WEBSITE_ASSET_DIR}"
            DEPENDS cinnamon-preprocessor ${asset_sources}
            VERBATIM)
    endif()
endif()

option(CINNAMON_PREPROCESSOR_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(CINNAMON_PREPROCESSOR_CLANG_TIDY "Run clang-tidy when compiling preprocessor sources" OFF)
option(CINNAMON_PREPROCESSOR_CPPCHECK "Run cppcheck when compiling preprocessor sources" OFF)

if(CINNAMON_PREPROCESSOR_SANITIZERS)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" OR CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
        message(FATAL_ERROR "Preprocessor sanitizers require a native GCC or Clang build")
    endif()
    target_link_options(cinnamon_core INTERFACE -fsanitize=address,undefined)
endif()
if(CINNAMON_PREPROCESSOR_CLANG_TIDY)
    find_program(CINNAMON_CLANG_TIDY clang-tidy REQUIRED)
endif()
if(CINNAMON_PREPROCESSOR_CPPCHECK)
    find_program(CINNAMON_CPPCHECK cppcheck REQUIRED)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(${CINNAMON_PREPROCESSOR_BZIP2_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-w")
    if(APPLE)
        target_link_options(cinnamon_core INTERFACE -Wl,-dead_strip)
    else()
        target_link_options(cinnamon_core INTERFACE -Wl,--gc-sections)
    endif()
endif()

foreach(target IN LISTS CINNAMON_PREPROCESSOR_TARGETS)
    set_target_properties(${target} PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -ffunction-sections -fdata-sections)
    endif()
    if(CINNAMON_PREPROCESSOR_SANITIZERS)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
    if(CINNAMON_PREPROCESSOR_CLANG_TIDY)
        set_target_properties(${target} PROPERTIES
            C_CLANG_TIDY "${CINNAMON_CLANG_TIDY};--header-filter=${CMAKE_CURRENT_SOURCE_DIR}/src/.*")
    endif()
    if(CINNAMON_PREPROCESSOR_CPPCHECK)
        set_target_properties(${target} PROPERTIES
            C_CPPCHECK "${CINNAMON_CPPCHECK};--enable=warning,performance,portability;--error-exitcode=1")
    endif()
endforeach()
