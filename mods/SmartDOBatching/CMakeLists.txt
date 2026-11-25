add_library(SmartDOBatching SHARED
    src/smart_do_batching/main.cpp
    src/smart_do_batching/config.cpp
    src/smart_do_batching/batch_hook.cpp
    "${CMAKE_SOURCE_DIR}/src/overlay/mod_overlay.cpp"
)

set_target_properties(SmartDOBatching PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

target_include_directories(SmartDOBatching PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/smart_do_batching"
    "${CMAKE_SOURCE_DIR}/src/overlay"
)

target_link_libraries(SmartDOBatching PRIVATE
    wrsr_common
    minhook_lib
    imgui_lib
    mini_lib
    d3d11
    dxgi
    d3dcompiler
)

target_compile_definitions(SmartDOBatching PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)

wrsr_configure_mod_version(SmartDOBatching "${CMAKE_CURRENT_SOURCE_DIR}")
wrsr_embed_offsets(SmartDOBatching "${CMAKE_CURRENT_SOURCE_DIR}/config/offsets.ini")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/SmartDOBatching.ini")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/SmartDOBatching.ini"
        "${CMAKE_CURRENT_BINARY_DIR}/SmartDOBatching.ini"
        COPYONLY
    )
endif()

if(MSVC)
    target_compile_options(SmartDOBatching PRIVATE
        /W4
        /WX-
        /wd4100
        /wd4201
        /MP
        /sdl-
        /GS-
    )
    target_link_options(SmartDOBatching PRIVATE
        /DEBUG:FULL
        /PDB:$<TARGET_FILE_DIR:SmartDOBatching>/SmartDOBatching.pdb
    )
endif()

# Stamped SmartDOBatching.rml.json is generated directly into ${CMAKE_CURRENT_BINARY_DIR}
# by wrsr_configure_mod_version().
