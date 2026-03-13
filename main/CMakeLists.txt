idf_component_register(
    SRCS
        "main.cpp"
        "console_commands.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        clock_manager
        stepper_motor
        networking
        driver
        freertos
        log
        nvs_flash
)


# target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-unused-function")