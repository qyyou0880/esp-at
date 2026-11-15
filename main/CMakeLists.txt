
file(GLOB_RECURSE srcs *.c)

set(includes "interface/include")

idf_component_register(SRCS ${srcs}
                    INCLUDE_DIRS ${includes})

if(CONFIG_IDF_TARGET_ESP32C6 AND CONFIG_BT_NIMBLE_ENABLED)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
                               LINK_GPIO_MAX_CONNECTIONS=${CONFIG_BT_NIMBLE_MAX_CONNECTIONS})
    target_link_options(${COMPONENT_LIB} INTERFACE
                        "-Wl,--wrap=nimble_port_init"
                        "-Wl,--wrap=nimble_port_deinit")
endif()
