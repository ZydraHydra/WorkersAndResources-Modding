# ============================================================================
# EmbedOffsets.cmake — Embed a mod's config/offsets.ini into its binary
# ============================================================================
function(wrsr_embed_offsets target_name ini_path)
    if(NOT EXISTS "${ini_path}")
        message(FATAL_ERROR "wrsr_embed_offsets: offsets file not found: ${ini_path}")
    endif()

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ini_path}")

    file(READ "${ini_path}" _offsets_raw)

    set(_delim "WRSROFFSETS")
    string(FIND "${_offsets_raw}" ")${_delim}\"" _has_collision)
    if(NOT _has_collision EQUAL -1)
        message(FATAL_ERROR "wrsr_embed_offsets: ${ini_path} contains forbidden delimiter ')${_delim}\"'")
    endif()

    set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(_out_hdr "${_gen_dir}/embedded_offsets.hpp")
    file(MAKE_DIRECTORY "${_gen_dir}")

    set(_hdr_text
"#pragma once
// Auto-generated from ${ini_path} — do not edit manually.
namespace wrsr::generated {
inline constexpr const char* kEmbeddedOffsetsIni = R\"${_delim}(
${_offsets_raw}
)${_delim}\";
} // namespace wrsr::generated
")
    file(WRITE "${_out_hdr}.tmp" "${_hdr_text}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_out_hdr}.tmp" "${_out_hdr}"
        RESULT_VARIABLE _copy_rc
    )
    file(REMOVE "${_out_hdr}.tmp")
    if(NOT _copy_rc EQUAL 0)
        message(FATAL_ERROR "wrsr_embed_offsets: failed to generate ${_out_hdr}")
    endif()

    target_include_directories(${target_name} PRIVATE "${_gen_dir}")
endfunction()
