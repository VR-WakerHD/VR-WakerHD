# Local Vulkan recipes must not survive changes to the code that interprets them.
# Hash public runtime sources, never generated game code or local cache data.
function(wwhd_pipeline_runtime_identity source_root output)
  # Source files only: any other file there (Finder's .DS_Store, an editor's swap file) re-ran cmake and
  # recompiled the Vulkan backend when it came or went, though no code changed.
  set(patterns)
  # Only the code that can change a pipeline: the shader translator, the GX2 layer and the renderers. A change
  # elsewhere (gameplay, VR, mods, the overlay) kept nothing from the saved warm-up: every pipeline was compiled again
  # during play after each build, a stall of its own on the render thread (docs/VR_PERFORMANCE.md, "Pipeline warm-up
  # across builds"). The handoff checks each warmed pipeline's SPIR-V, descriptors and whole fixed state against the
  # draw's (pipeline_worker.cpp take_warm_pipeline), so a recipe the code no longer makes is never used
  foreach(dir runtime/src/gfx runtime/src/gx2 runtime/include runtime/third_party/cemu)
    foreach(extension c cpp h hpp inc mm)
      list(APPEND patterns "${source_root}/${dir}/*.${extension}")
    endforeach()
  endforeach()
  file(GLOB_RECURSE inputs CONFIGURE_DEPENDS ${patterns})
  list(APPEND inputs "${source_root}/CMakeLists.txt" "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
  list(SORT inputs)
  set(material "pipeline-runtime-1\n")
  foreach(input IN LISTS inputs)
    if(NOT IS_DIRECTORY "${input}")
      # Content edits, additions and removals all trigger automatic reconfiguration.
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
      file(RELATIVE_PATH relative "${source_root}" "${input}")
      file(SHA256 "${input}" digest)
      string(APPEND material "${relative}\n${digest}\n")
    endif()
  endforeach()
  string(SHA256 identity "${material}")
  set(${output} "${identity}" PARENT_SCOPE)
endfunction()
