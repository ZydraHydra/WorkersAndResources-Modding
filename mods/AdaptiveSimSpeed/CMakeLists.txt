add_library(AdaptiveSimSpeed SHARED
    src/adaptive_sim_speed/main.cpp
    src/adaptive_sim_speed/config.cpp
    src/adaptive_sim_speed/sim_speed_hook.cpp
    "${CMAKE_SOURCE_DIR}/src/overlay/mod_overlay.cpp"
)

set_target_properties(AdaptiveSimSpeed PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

target_include_directories(AdaptiveSimSpeed PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/adaptive_sim_speed"
    "${CMAKE_SOURCE_DIR}/src/overlay"
)

target_link_libraries(AdaptiveSimSpeed PRIVATE
    wrsr_common
    minhook_lib
    imgui_lib
    mini_lib
    d3d11
    dxgi
    d3dcompiler
)

target_compile_definitions(AdaptiveSimSpeed PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)

wrsr_configure_mod_version(AdaptiveSimSpeed "${CMAKE_CURRENT_SOURCE_DIR}")
wrsr_embed_offsets(AdaptiveSimSpeed "${CMAKE_CURRENT_SOURCE_DIR}/config/offsets.ini")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/AdaptiveSimSpeed.ini")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/AdaptiveSimSpeed.ini"
        "${CMAKE_CURRENT_BINARY_DIR}/AdaptiveSimSpeed.ini"
        COPYONLY
    )
endif()

if(MSVC)
    target_compile_options(AdaptiveSimSpeed PRIVATE
        /W4
        /WX-
        /wd4100
        /wd4201
        /MP
        /sdl-
        /GS-
    )
    target_link_options(AdaptiveSimSpeed PRIVATE
        /DEBUG:FULL
        /PDB:$<TARGET_FILE_DIR:AdaptiveSimSpeed>/AdaptiveSimSpeed.pdb
    )
endif()
