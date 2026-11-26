add_library(CODispatchControl SHARED
    src/co_dispatch_control/main.cpp
    src/co_dispatch_control/config.cpp
    src/co_dispatch_control/co_dispatch_hook.cpp
    "${CMAKE_SOURCE_DIR}/src/overlay/mod_overlay.cpp"
)

set_target_properties(CODispatchControl PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

target_include_directories(CODispatchControl PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/co_dispatch_control"
    "${CMAKE_SOURCE_DIR}/src/overlay"
)

target_link_libraries(CODispatchControl PRIVATE
    wrsr_common
    minhook_lib
    imgui_lib
    mini_lib
    d3d11
    dxgi
    d3dcompiler
)

target_compile_definitions(CODispatchControl PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)

wrsr_configure_mod_version(CODispatchControl "${CMAKE_CURRENT_SOURCE_DIR}")
wrsr_embed_offsets(CODispatchControl "${CMAKE_CURRENT_SOURCE_DIR}/config/offsets.ini")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/CODispatchControl.ini")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/CODispatchControl.ini"
        "${CMAKE_CURRENT_BINARY_DIR}/CODispatchControl.ini"
        COPYONLY
    )
endif()

if(MSVC)
    target_compile_options(CODispatchControl PRIVATE
        /W4
        /WX-
        /wd4100
        /wd4201
        /MP
        /sdl-
        /GS-
    )
    target_link_options(CODispatchControl PRIVATE
        /DEBUG:FULL
        /PDB:$<TARGET_FILE_DIR:CODispatchControl>/CODispatchControl.pdb
    )
endif()
