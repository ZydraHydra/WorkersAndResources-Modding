add_library(SmartBayAccess SHARED
    src/smart_bay_access/main.cpp
    src/smart_bay_access/config.cpp
    src/smart_bay_access/bay_access_hook.cpp
    "${CMAKE_SOURCE_DIR}/src/overlay/mod_overlay.cpp"
)

set_target_properties(SmartBayAccess PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

target_include_directories(SmartBayAccess PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/smart_bay_access"
    "${CMAKE_SOURCE_DIR}/src/overlay"
)

target_link_libraries(SmartBayAccess PRIVATE
    wrsr_common
    minhook_lib
    imgui_lib
    mini_lib
    d3d11
    dxgi
    d3dcompiler
)

target_compile_definitions(SmartBayAccess PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)

wrsr_configure_mod_version(SmartBayAccess "${CMAKE_CURRENT_SOURCE_DIR}")
wrsr_embed_offsets(SmartBayAccess "${CMAKE_CURRENT_SOURCE_DIR}/config/offsets.ini")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/SmartBayAccess.ini")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/SmartBayAccess.ini"
        "${CMAKE_CURRENT_BINARY_DIR}/SmartBayAccess.ini"
        COPYONLY
    )
endif()

if(MSVC)
    target_compile_options(SmartBayAccess PRIVATE
        /W4
        /WX-
        /wd4100
        /wd4201
        /MP
        /sdl-
        /GS-
    )
    target_link_options(SmartBayAccess PRIVATE
        /DEBUG:FULL
        /PDB:$<TARGET_FILE_DIR:SmartBayAccess>/SmartBayAccess.pdb
    )
endif()
