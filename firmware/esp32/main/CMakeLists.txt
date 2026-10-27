idf_component_register(SRCS "main.cpp" "board.cpp" "storage.cpp" "manifest.cpp"
    INCLUDE_DIRS "." "../../shared"
    REQUIRES driver esp_lcd esp_timer nvs_flash json esp_psram)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
