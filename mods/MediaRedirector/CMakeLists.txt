add_library(MediaRedirector SHARED
    src/media_redirector/main.cpp
    src/media_redirector/config.cpp
    src/media_redirector/vfs_index.cpp
    src/media_redirector/redirect_hook.cpp
    src/media_redirector/zip_reader.cpp
    "${CMAKE_SOURCE_DIR}/src/overlay/mod_overlay.cpp"
)

set_target_properties(MediaRedirector PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

target_include_directories(MediaRedirector PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/overlay"
)

target_link_libraries(MediaRedirector PRIVATE
    wrsr_common
    minhook_lib
    imgui_lib
    mini_lib
    d3d11
    dxgi
    d3dcompiler
    advapi32
)

target_compile_definitions(MediaRedirector PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)

wrsr_configure_mod_version(MediaRedirector "${CMAKE_CURRENT_SOURCE_DIR}")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/MediaRedirector.ini")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/MediaRedirector.ini"
        "${CMAKE_CURRENT_BINARY_DIR}/MediaRedirector.ini"
        COPYONLY
    )
endif()

if(MSVC)
    target_compile_options(MediaRedirector PRIVATE
        /W4
        /WX-
        /wd4100
        /wd4201
        /MP
        /sdl-
        /GS-
    )
    target_link_options(MediaRedirector PRIVATE
        /DEBUG:FULL
        /PDB:$<TARGET_FILE_DIR:MediaRedirector>/MediaRedirector.pdb
    )
endif()
