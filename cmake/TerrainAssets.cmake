# Build-time import only; the renderer consumes the resulting DDS files directly.
function(openworld_add_terrain_assets target importer recipe output_directory)
    get_filename_component(recipe "${recipe}" ABSOLUTE)
    get_filename_component(source_directory "${recipe}" DIRECTORY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${recipe}")
    file(READ "${recipe}" recipe_json)
    string(JSON material_count LENGTH "${recipe_json}" materials)
    if(NOT material_count EQUAL 4)
        message(FATAL_ERROR "Terrain palette must contain four materials: ${recipe}")
    endif()
    set(inputs "${recipe}")
    set(has_macro FALSE)
    foreach(index RANGE 0 3)
        foreach(semantic colour height macroColour)
            string(JSON filename ERROR_VARIABLE error GET "${recipe_json}" materials ${index} ${semantic})
            if(error)
                if(NOT semantic STREQUAL "macroColour")
                    message(FATAL_ERROR "Missing ${semantic} for terrain material ${index}: ${recipe}")
                endif()
            else()
                list(APPEND inputs "${source_directory}/${filename}")
                if(semantic STREQUAL "macroColour")
                    set(has_macro TRUE)
                endif()
            endif()
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES inputs)
    set(outputs "${output_directory}/palette.json")
    set(semantics colour normal scalar)
    if(has_macro)
        list(APPEND semantics macro)
    endif()
    foreach(semantic IN LISTS semantics)
        list(APPEND outputs "${output_directory}/${semantic}.dds" "${output_directory}/${semantic}_rgba.dds")
    endforeach()
    set(stamp "${output_directory}/.import-complete")
    add_custom_command(
        OUTPUT "${stamp}" ${outputs}
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${output_directory}"
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${stamp}"
        COMMAND "$<TARGET_FILE:${importer}>" "${recipe}" "${output_directory}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS ${importer} ${inputs}
        COMMENT "Importing terrain textures into ${output_directory}"
        VERBATIM)
    add_custom_target(${target} DEPENDS "${stamp}" ${outputs})
endfunction()
