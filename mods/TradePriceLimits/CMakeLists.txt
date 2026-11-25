add_library(TradePriceLimits SHARED
    src/main.cpp
    src/core/rules.cpp
    src/hooks/dx11_overlay.cpp
    src/hooks/save_tracker.cpp
    src/hooks/game_hooks.cpp
)

set_target_properties(TradePriceLimits PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

target_include_directories(TradePriceLimits PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(TradePriceLimits PRIVATE
    wrsr_common
    minhook_lib
    imgui_lib
    mini_lib
    d3d11
    dxgi
    d3dcompiler
)

target_compile_definitions(TradePriceLimits PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)

wrsr_configure_mod_version(TradePriceLimits "${CMAKE_CURRENT_SOURCE_DIR}")
wrsr_embed_offsets(TradePriceLimits "${CMAKE_CURRENT_SOURCE_DIR}/config/offsets.ini")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/TradePriceLimits.ini")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/TradePriceLimits.ini"
        "${CMAKE_CURRENT_BINARY_DIR}/TradePriceLimits.ini"
        COPYONLY
    )
endif()

if(MSVC)
    target_compile_options(TradePriceLimits PRIVATE
        /W4
        /WX-
        /wd4100
        /wd4201
        /MP
        /sdl-
        /GS-
    )
    target_link_options(TradePriceLimits PRIVATE
        /DEBUG:FULL
        /PDB:$<TARGET_FILE_DIR:TradePriceLimits>/TradePriceLimits.pdb
    )
endif()

# Stamped TradePriceLimits.rml.json is generated directly into ${CMAKE_CURRENT_BINARY_DIR}
# by wrsr_configure_mod_version().
