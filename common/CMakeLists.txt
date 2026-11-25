add_library(wrsr_common STATIC
    src/game_signatures.cpp
    src/logger.cpp
    src/offsets.cpp
    src/pe_utils.cpp
)

target_include_directories(wrsr_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

wrsr_embed_offsets(wrsr_common "${CMAKE_CURRENT_SOURCE_DIR}/config/offsets.ini"
    HEADER_NAME "embedded_common_offsets.hpp"
    SYMBOL_NAME "kCommonOffsetsIni"
)

target_compile_definitions(wrsr_common PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)

if(MSVC)
    target_compile_options(wrsr_common PRIVATE
        /W4
        /WX-
        /MP
    )
endif()

set_target_properties(wrsr_common PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
