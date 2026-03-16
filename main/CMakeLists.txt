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
        display
        encoder
        menu
        led
        matter
        webserver
        config_store
        driver
        freertos
        log
        nvs_flash
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
