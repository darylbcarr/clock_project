idf_component_register(
    SRCS "ota_manager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_https_ota
        esp_http_client
        app_update
        mbedtls
        display
        log
        freertos
        json
)
