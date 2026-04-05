/**
 * delongi-tank: Phase 3 - WiFi + HTTP REST API
 * This is a patch file showing the Phase 3 additions
 * Merge these functions into main.c
 */

// ============================================================================
// Phase 3: WiFi Task Implementation
// ============================================================================

/**
 * @brief Task: Handle WiFi connection and REST API
 */
static void wifi_task(void *pvParameters)
{
    ESP_LOGI(TAG, "WiFi task started");
    
    // Initialize WiFi (STA + AP fallback)
    if (init_wifi() == ESP_OK) {
        ESP_LOGI(TAG, "WiFi initialized");
    }
    
    // Start HTTP Server
    httpd_handle_t server = start_webserver();
    if (server == NULL) {
        ESP_LOGE(TAG, "Failed to start web server");
    }
    
    // Monitor and keep task alive
    while (1) {
        // TODO: Monitor WiFi connection status
        // TODO: Reconnect if disconnected
        // TODO: Monitor mDNS
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ============================================================================
// Phase 3: HTTP Server Start Function
// ============================================================================

/**
 * @brief Start HTTP server on port 80 with REST API endpoints
 */
static httpd_handle_t start_webserver(void)
{
    ESP_LOGI(TAG, "Starting HTTP server on port %d", SERVER_PORT);
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = SERVER_PORT;
    config.stack_size = SERVER_STACK_SIZE;
    config.task_priority = SERVER_TASK_PRIORITY;
    config.max_open_sockets = MAX_OPEN_SOCKETS;
    
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }
    
    // Register handlers
    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &status_uri);
    
    httpd_uri_t config_get_uri = {
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = config_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &config_get_uri);
    
    httpd_uri_t config_post_uri = {
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = config_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &config_post_uri);
    
    httpd_uri_t valve_manual_uri = {
        .uri = "/api/valve/manual",
        .method = HTTP_POST,
        .handler = valve_manual_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &valve_manual_uri);
    
    httpd_uri_t emergency_stop_uri = {
        .uri = "/api/emergency_stop",
        .method = HTTP_POST,
        .handler = emergency_stop_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &emergency_stop_uri);
    
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &index_uri);
    
    ESP_LOGI(TAG, "HTTP server started successfully - all endpoints registered");
    ESP_LOGI(TAG, "Access from: http://10.1.1.1 (AP mode) or http://delongi-tank.local (mDNS)");
    return server;
}

// ============================================================================
// Phase 3: app_main - Updated for Phase 3
// ============================================================================

/**
 * @brief FreeRTOS app_main - System initialization and task creation
 */
void app_main(void)
{
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "delongi-tank %s (Build #%d)", VERSION_STRING, BUILD_NUMBER);
    ESP_LOGI(TAG, "Compiled: %s", BUILD_TIMESTAMP);
    ESP_LOGI(TAG, "===========================================");
    
    // Initialize hardware
    ESP_LOGI(TAG, "Initializing hardware...");
    
    if (init_nvs() != ESP_OK) {
        ESP_LOGE(TAG, "NVS initialization failed!");
        return;
    }
    
    if (init_i2c() != ESP_OK) {
        ESP_LOGE(TAG, "I2C initialization failed!");
        return;
    }
    
    if (init_gpio() != ESP_OK) {
        ESP_LOGE(TAG, "GPIO initialization failed!");
        return;
    }
    
    ESP_LOGI(TAG, "Hardware initialized successfully");
    
    // Start FreeRTOS tasks
    ESP_LOGI(TAG, "Creating FreeRTOS tasks...");
    
    xTaskCreate(
        sensor_task,
        "sensor_task",
        TASK_STACK_SENSOR,
        NULL,
        TASK_PRIO_SENSOR,
        &sensor_task_handle
    );
    
    xTaskCreate(
        valve_task,
        "valve_task",
        TASK_STACK_VALVE,
        NULL,
        TASK_PRIO_VALVE,
        &valve_task_handle
    );
    
    xTaskCreate(
        wifi_task,
        "wifi_task",
        TASK_STACK_WIFI,
        NULL,
        TASK_PRIO_MAIN,
        &wifi_task_handle
    );
    
    ESP_LOGI(TAG, "System startup complete - all tasks running");
    ESP_LOGI(TAG, "Configured thresholds:");
    ESP_LOGI(TAG, "  - OBEN (tank full):  %d cm", sys_state.threshold_top);
    ESP_LOGI(TAG, "  - UNTEN (tank empty): %d cm", sys_state.threshold_bottom);
    ESP_LOGI(TAG, "  - Timeout (max fill): %d ms", sys_state.timeout_max);
    
    // LED indicates system ready
    gpio_set_level(GPIO_LED_STATUS, 0);  // LED off (ready)
    
    ESP_LOGI(TAG, "Web interface: http://10.1.1.1/ (AP mode)");
}
