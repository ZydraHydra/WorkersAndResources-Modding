# ============================================================================
# ModVersioning.cmake — Automatic per-mod versioning & game-version targeting
# ============================================================================
# Reads mods/<Mod>/VERSION ("MAJOR.MINOR" or "MAJOR.MINOR.PATCH"), derives
# patch count from git commits touching mods/<Mod>/ or common/ (unless a
# matching <Mod>-*v<SemVer> tag or WRSR_MOD_VERSION_OVERRIDE is provided),
# embeds Windows VS_VERSION_INFO into the DLL, exposes WRSR_MOD_VERSION /
# WRSR_GAME_VERSION defines, and stamps <Mod>.rml.json in the build output.
# ============================================================================

function(wrsr_configure_mod_version target_name mod_dir)
    set(base_ver "1.0")
    if(EXISTS "${mod_dir}/VERSION")
        file(READ "${mod_dir}/VERSION" _raw_ver)
        string(STRIP "${_raw_ver}" base_ver)
    endif()

    set(ver_major 1)
    set(ver_minor 0)
    set(ver_patch 0)
    set(has_explicit_patch FALSE)

    if(base_ver MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
        set(ver_major "${CMAKE_MATCH_1}")
        set(ver_minor "${CMAKE_MATCH_2}")
        set(ver_patch "${CMAKE_MATCH_3}")
        set(has_explicit_patch TRUE)
    elseif(base_ver MATCHES "^([0-9]+)\\.([0-9]+)$")
        set(ver_major "${CMAKE_MATCH_1}")
        set(ver_minor "${CMAKE_MATCH_2}")
        set(ver_patch 0)
    endif()

    set(resolved_version "${ver_major}.${ver_minor}.${ver_patch}")

    # Allow CI/scripts to inject an exact SemVer (e.g., from release tag or get_mod_version.ps1)
    if(DEFINED WRSR_MOD_VERSION_OVERRIDE AND NOT "${WRSR_MOD_VERSION_OVERRIDE}" STREQUAL "")
        set(resolved_version "${WRSR_MOD_VERSION_OVERRIDE}")
        if(resolved_version MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
            set(ver_major "${CMAKE_MATCH_1}")
            set(ver_minor "${CMAKE_MATCH_2}")
            set(ver_patch "${CMAKE_MATCH_3}")
        endif()
    elseif(NOT has_explicit_patch)
        find_package(Git QUIET)
        if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
            file(RELATIVE_PATH rel_mod_dir "${CMAKE_SOURCE_DIR}" "${mod_dir}")
            # Check if HEAD is directly on a release tag for this mod
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" tag --points-at HEAD "${target_name}-*v*"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                OUTPUT_VARIABLE _exact_tags
                ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            if(_exact_tags MATCHES "-v([0-9]+)\\.([0-9]+)\\.([0-9]+)")
                set(ver_major "${CMAKE_MATCH_1}")
                set(ver_minor "${CMAKE_MATCH_2}")
                set(ver_patch "${CMAKE_MATCH_3}")
            else()
                execute_process(
                    COMMAND "${GIT_EXECUTABLE}" rev-list --count HEAD -- "${rel_mod_dir}" "common"
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                    RESULT_VARIABLE _git_rc
                    OUTPUT_VARIABLE _git_count
                    ERROR_QUIET
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                )
                if(_git_rc EQUAL 0 AND _git_count MATCHES "^[0-9]+$")
                    set(ver_patch "${_git_count}")
                endif()
            endif()
            set(resolved_version "${ver_major}.${ver_minor}.${ver_patch}")
        endif()
    endif()

    set(game_ver "latest")
    if(DEFINED WRSR_GAME_VERSION AND NOT "${WRSR_GAME_VERSION}" STREQUAL "")
        set(game_ver "${WRSR_GAME_VERSION}")
    endif()

    set(WRSR_TARGET_NAME  "${target_name}")
    set(WRSR_MOD_VERSION  "${resolved_version}")
    set(WRSR_GAME_VERSION "${game_ver}")
    set(WRSR_VER_MAJOR    "${ver_major}")
    set(WRSR_VER_MINOR    "${ver_minor}")
    set(WRSR_VER_PATCH    "${ver_patch}")

    set(_rc_out "${CMAKE_CURRENT_BINARY_DIR}/${target_name}_version.rc")
    configure_file(
        "${CMAKE_SOURCE_DIR}/common/resources/mod_version.rc.in"
        "${_rc_out}"
        @ONLY
    )
    target_sources(${target_name} PRIVATE "${_rc_out}")

    target_compile_definitions(${target_name} PRIVATE
        WRSR_MOD_NAME="${target_name}"
        WRSR_MOD_VERSION="${resolved_version}"
        WRSR_GAME_VERSION="${game_ver}"
    )

    # Stamp version & supportedGameVersions into the output .rml.json manifest
    set(_manifest_in "${mod_dir}/${target_name}.rml.json")
    if(EXISTS "${_manifest_in}")
        file(READ "${_manifest_in}" _manifest_json)
        string(JSON _manifest_json SET "${_manifest_json}" "version" "\"${resolved_version}\"")

        # Resolve supportedGameVersions from game_versions.json if available
        set(_gv_file "${mod_dir}/game_versions.json")
        if(EXISTS "${_gv_file}")
            file(READ "${_gv_file}" _gv_json)
            string(JSON _gv_len LENGTH "${_gv_json}" "gameVersions")
            math(EXPR _gv_last "${_gv_len} - 1")
            foreach(_idx RANGE 0 ${_gv_last})
                string(JSON _entry_id GET "${_gv_json}" "gameVersions" ${_idx} "id")
                if(_entry_id STREQUAL "${game_ver}")
                    string(JSON _supported GET "${_gv_json}" "gameVersions" ${_idx} "supportedGameVersions")
                    string(JSON _manifest_json SET "${_manifest_json}" "supportedGameVersions" "${_supported}")
                    break()
                endif()
            endforeach()
        endif()

        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${target_name}.rml.json" "${_manifest_json}\n")
    endif()

    message(STATUS "Configured ${target_name} v${resolved_version} (WRSR target: ${game_ver})")
endfunction()
