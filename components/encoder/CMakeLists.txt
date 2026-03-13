idf_component_register(
    SRCS "encoder.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c freertos
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)