add_library(TechServiceControl SHARED
    src/tech_service_control/main.cpp
    src/tech_service_control/config.cpp
    src/tech_service_control/tech_service_hook.cpp
    "${CMAKE_SOURCE_DIR}/src/overlay/mod_overlay.cpp"
)

set_target_properties(TechServiceControl PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

target_include_directories(TechServiceControl PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/tech_service_control"
    "${CMAKE_SOURCE_DIR}/src/overlay"
)

target_link_libraries(TechServiceControl PRIVATE
    wrsr_common
    minhook_lib
    imgui_lib
    mini_lib
    d3d11
    dxgi
    d3dcompiler
)

target_compile_definitions(TechServiceControl PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)

wrsr_configure_mod_version(TechServiceControl "${CMAKE_CURRENT_SOURCE_DIR}")
wrsr_embed_offsets(TechServiceControl "${CMAKE_CURRENT_SOURCE_DIR}/config/offsets.ini")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/TechServiceControl.ini")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/TechServiceControl.ini"
        "${CMAKE_CURRENT_BINARY_DIR}/TechServiceControl.ini"
        COPYONLY
    )
endif()

if(MSVC)
    target_compile_options(TechServiceControl PRIVATE
        /W4
        /WX-
        /wd4100
        /wd4201
        /MP
        /sdl-
        /GS-
    )
    target_link_options(TechServiceControl PRIVATE
        /DEBUG:FULL
        /PDB:$<TARGET_FILE_DIR:TechServiceControl>/TechServiceControl.pdb
    )
endif()
