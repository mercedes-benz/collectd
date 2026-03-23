cmake_minimum_required(VERSION 3.0)
project(collectd)

if (QNX_BUILD_USING_CMAKE)
    if ("${PRODUCT_LINE}" STREQUAL "i2")
        set(LAYER_PLATFORM_SERVICES "qnx-services")
    endif()
    set(COLLECTD_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})
    set(COLLECTD_BIN_DIR "${CMAKE_BINARY_DIR}/${LAYER_PLATFORM_SERVICES}/collectd")

    add_custom_command(
        OUTPUT "collectd_out"
        COMMAND ${COLLECTD_SRC_DIR}/build-for-qnx.sh ${CMAKE_INSTALL_PREFIX} ${QNX_AP_BASE_DIR} ${CMAKE_BINARY_DIR} ${LAYER_PLATFORM_SERVICES}
        COMMENT "Compiling collectd"
        DEPENDS dlt
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
    )

    add_custom_target(collectd ALL DEPENDS "collectd_out" VERBATIM)

    install(FILES ${COLLECTD_SRC_DIR}/configs/qnx/collectd.conf DESTINATION etc/collectd/ RENAME collectd_qnx.conf)
    install(FILES ${COLLECTD_SRC_DIR}/qnx-types.db DESTINATION share/collectd/)
else()
    message(FATAL_ERROR "CMake build is currently supported for QNX only")
endif()
