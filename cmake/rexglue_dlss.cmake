if(NOT REXGLUE_ENABLE_DLSS)
    return()
endif()

if(NOT REXGLUE_USE_VULKAN OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux"
        OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    message(WARNING "REXGLUE_ENABLE_DLSS needs the Vulkan backend on Linux x86_64. Disabling DLSS.")
    set(REXGLUE_ENABLE_DLSS OFF CACHE BOOL "" FORCE)
    return()
endif()

set(REXGLUE_DLSS_TAG "v310.9.16" CACHE STRING "NVIDIA DLSS SDK tag to fetch")
set(_rexglue_dlss_dir "${CMAKE_BINARY_DIR}/_deps/dlss-${REXGLUE_DLSS_TAG}")
set(_rexglue_dlss_lib_dir "${_rexglue_dlss_dir}/lib/Linux_x86_64")

if(NOT EXISTS "${_rexglue_dlss_dir}/include/nvsdk_ngx_vk.h"
        OR NOT EXISTS "${_rexglue_dlss_lib_dir}/libnvsdk_ngx.a")
    find_package(Git REQUIRED)
    file(REMOVE_RECURSE "${_rexglue_dlss_dir}")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" clone --quiet --depth 1 --branch "${REXGLUE_DLSS_TAG}"
                --filter=blob:none --sparse https://github.com/NVIDIA/DLSS.git "${_rexglue_dlss_dir}"
        RESULT_VARIABLE _rexglue_dlss_result)
    if(_rexglue_dlss_result EQUAL 0)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${_rexglue_dlss_dir}" sparse-checkout set --no-cone
                    /include/ /lib/Linux_x86_64/libnvsdk_ngx.a
                    /lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.* /LICENSE.txt
            RESULT_VARIABLE _rexglue_dlss_result)
    endif()
    if(NOT _rexglue_dlss_result EQUAL 0)
        message(WARNING "Could not fetch the NVIDIA DLSS SDK ${REXGLUE_DLSS_TAG}. Disabling DLSS.")
        set(REXGLUE_ENABLE_DLSS OFF CACHE BOOL "" FORCE)
        return()
    endif()
endif()

file(GLOB _rexglue_dlss_snippet "${_rexglue_dlss_lib_dir}/rel/libnvidia-ngx-dlss.so.*")
if(NOT _rexglue_dlss_snippet)
    message(WARNING "The NVIDIA DLSS SDK has no Linux DLSS library. Disabling DLSS.")
    set(REXGLUE_ENABLE_DLSS OFF CACHE BOOL "" FORCE)
    return()
endif()

set(REXGLUE_DLSS_INCLUDE_DIR "${_rexglue_dlss_dir}/include" CACHE INTERNAL "")
set(REXGLUE_DLSS_LIBRARY "${_rexglue_dlss_lib_dir}/libnvsdk_ngx.a" CACHE INTERNAL "")
set(REXGLUE_DLSS_SNIPPET "${_rexglue_dlss_snippet}" CACHE INTERNAL "")

install(FILES "${REXGLUE_DLSS_SNIPPET}" DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES "${_rexglue_dlss_dir}/LICENSE.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/rexglue/licenses" RENAME DLSS-LICENSE.txt)
message(STATUS "DLSS: NVIDIA DLSS SDK ${REXGLUE_DLSS_TAG}")
