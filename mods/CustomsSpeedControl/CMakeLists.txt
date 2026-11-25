add_library(CustomsSpeedControl SHARED
    src/customs_speed/main.cpp
    src/customs_speed/config.cpp
    src/customs_speed/customs_hook.cpp
    "${CMAKE_SOURCE_DIR}/src/overlay/mod_overlay.cpp"
)

set_target_properties(CustomsSpeedControl PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

target_include_directories(CustomsSpeedControl PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/overlay"
)

target_link_libraries(CustomsSpeedControl PRIVATE
    wrsr_common
    minhook_lib
    imgui_lib
    mini_lib
    d3d11
    dxgi
    d3dcompiler
)

target_compile_definitions(CustomsSpeedControl PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)

wrsr_configure_mod_version(CustomsSpeedControl "${CMAKE_CURRENT_SOURCE_DIR}")
wrsr_embed_offsets(CustomsSpeedControl "${CMAKE_CURRENT_SOURCE_DIR}/config/offsets.ini")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/CustomsSpeedControl.ini")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/CustomsSpeedControl.ini"
        "${CMAKE_CURRENT_BINARY_DIR}/CustomsSpeedControl.ini"
        COPYONLY
    )
endif()

if(MSVC)
    target_compile_options(CustomsSpeedControl PRIVATE
        /W4
        /WX-
        /wd4100
        /wd4201
        /MP
        /sdl-
        /GS-
    )
    target_link_options(CustomsSpeedControl PRIVATE
        /DEBUG:FULL
        /PDB:$<TARGET_FILE_DIR:CustomsSpeedControl>/CustomsSpeedControl.pdb
    )
endif()
