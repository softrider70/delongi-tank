idf_component_register(
    SRCS "safe_ota_manager.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash 
             esp_partition 
             esp_ota 
             esp_task_wdt 
             freertos
)
