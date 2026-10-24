function(openworld_add_vegetation_assets target importer recipe output_directory)
    get_filename_component(recipe "${recipe}" ABSOLUTE)
    get_filename_component(source_directory "${recipe}" DIRECTORY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${recipe}")
    file(READ "${recipe}" recipe_json)
    set(inputs "${recipe}")
    foreach(index RANGE 0 1)
        string(JSON filename GET "${recipe_json}" leaves ${index} colour)
        list(APPEND inputs "${source_directory}/${filename}")
    endforeach()
    set(outputs "${output_directory}/library.bin" "${output_directory}/library_rgba.bin" "${output_directory}/recipe.json")
    foreach(index RANGE 0 3)
        list(APPEND outputs "${output_directory}/texture${index}.dds" "${output_directory}/texture${index}_rgba.dds")
    endforeach()
    string(JSON ground ERROR_VARIABLE no_ground GET "${recipe_json}" ground)
    if(NOT no_ground)
        list(APPEND inputs "${source_directory}/${ground}")
        list(APPEND outputs "${output_directory}/ground.dds" "${output_directory}/ground_rgba.dds")
    endif()
    set(stamp "${output_directory}/.import-complete")
    add_custom_command(OUTPUT "${stamp}" ${outputs}
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${output_directory}"
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${stamp}"
        COMMAND "$<TARGET_FILE:${importer}>" "${recipe}" "${output_directory}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS ${importer} ${inputs} VERBATIM)
    add_custom_target(${target} DEPENDS "${stamp}" ${outputs})
endfunction()
