#include "mcp_sensor.h"
#include "mcp_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_random.h"
#include <math.h>

static const char *TAG = "mcp_sensor";

// 传感器状态
static struct {
    bool initialized;
    bool running;
    TaskHandle_t task_handle;
    int (*update_callback)(float temperature, float humidity);
    float temperature;
    float humidity;
    SemaphoreHandle_t mutex;
} g_sensor = {0};

// 传感器参数
#define SENSOR_UPDATE_INTERVAL_MS   2000    // 2秒更新一次
#define BASE_TEMPERATURE           22.0f     // 基础温度 22°C
#define BASE_HUMIDITY             45.0f      // 基础湿度 45%
#define TEMP_VARIATION             5.0f      // 温度变化范围 ±5°C
#define HUMIDITY_VARIATION        15.0f      // 湿度变化范围 ±15%

/**
 * @brief 生成随机传感器数据
 */
static void generate_sensor_data(float *temperature, float *humidity) {
    static float temp_offset = 0.0f;
    static float humidity_offset = 0.0f;
    static uint32_t last_update_time = 0;
    
    uint32_t current_time = esp_timer_get_time() / 1000000; // 转换为秒
    
    // 模拟缓慢变化的环境条件
    if (current_time - last_update_time > 10) { // 每10秒调整一次基础偏移
        temp_offset += ((float)esp_random() / UINT32_MAX - 0.5f) * 0.5f;
        humidity_offset += ((float)esp_random() / UINT32_MAX - 0.5f) * 2.0f;
        
        // 限制偏移范围
        if (temp_offset > TEMP_VARIATION) temp_offset = TEMP_VARIATION;
        if (temp_offset < -TEMP_VARIATION) temp_offset = -TEMP_VARIATION;
        if (humidity_offset > HUMIDITY_VARIATION) humidity_offset = HUMIDITY_VARIATION;
        if (humidity_offset < -HUMIDITY_VARIATION) humidity_offset = -HUMIDITY_VARIATION;
        
        last_update_time = current_time;
    }
    
    // 添加小幅随机波动
    float temp_noise = ((float)esp_random() / UINT32_MAX - 0.5f) * 0.2f;
    float humidity_noise = ((float)esp_random() / UINT32_MAX - 0.5f) * 1.0f;
    
    // 添加时间相关的周期性变化（模拟一天的温度变化）
    float time_factor = sinf((float)current_time / 3600.0f * 2.0f * M_PI / 24.0f);
    float daily_temp_variation = time_factor * 3.0f; // ±3°C的日变化
    
    *temperature = BASE_TEMPERATURE + temp_offset + temp_noise + daily_temp_variation;
    *humidity = BASE_HUMIDITY + humidity_offset + humidity_noise;
    
    // 确保湿度在合理范围内
    if (*humidity > 95.0f) *humidity = 95.0f;
    if (*humidity < 10.0f) *humidity = 10.0f;
}

/**
 * @brief 传感器数据采集任务
 */
static void sensor_task(void *pvParameters) {
    ESP_LOGI(TAG, "Sensor task started");
    
    while (g_sensor.running) {
        float temperature, humidity;
        
        // 生成传感器数据
        generate_sensor_data(&temperature, &humidity);
        
        // 更新全局状态
        if (xSemaphoreTake(g_sensor.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            g_sensor.temperature = temperature;
            g_sensor.humidity = humidity;
            xSemaphoreGive(g_sensor.mutex);
            
            // 调用回调函数更新 MCP 服务器
            if (g_sensor.update_callback) {
                g_sensor.update_callback(temperature, humidity);
            }
            
            // ESP_LOGI(TAG, "Sensor data updated: T=%.1f°C, H=%.1f%%", 
            //         temperature, humidity);
        }
        
        vTaskDelay(pdMS_TO_TICKS(SENSOR_UPDATE_INTERVAL_MS));
    }
    
    ESP_LOGI(TAG, "Sensor task stopped");
    g_sensor.task_handle = NULL;
    vTaskDelete(NULL);
}

int mcp_sensor_init(void) {
    if (g_sensor.initialized) {
        ESP_LOGW(TAG, "Sensor already initialized");
        return 0;
    }
    
    g_sensor.mutex = xSemaphoreCreateMutex();
    if (g_sensor.mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create sensor mutex");
        return -1;
    }
    
    // 初始化传感器数据
    g_sensor.temperature = BASE_TEMPERATURE;
    g_sensor.humidity = BASE_HUMIDITY;
    g_sensor.running = false;
    g_sensor.task_handle = NULL;
    g_sensor.update_callback = NULL;
    
    g_sensor.initialized = true;
    ESP_LOGI(TAG, "Sensor module initialized");
    
    return 0;
}

int mcp_sensor_start(void) {
    if (!g_sensor.initialized) {
        ESP_LOGE(TAG, "Sensor not initialized");
        return -1;
    }
    
    if (g_sensor.running) {
        ESP_LOGW(TAG, "Sensor task already running");
        return 0;
    }
    
    g_sensor.running = true;
    
    BaseType_t ret = xTaskCreate(
        sensor_task,
        "sensor_task",
        4096,
        NULL,
        5,
        &g_sensor.task_handle
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sensor task");
        g_sensor.running = false;
        return -1;
    }
    
    ESP_LOGI(TAG, "Sensor task started");
    return 0;
}

int mcp_sensor_stop(void) {
    if (!g_sensor.running || g_sensor.task_handle == NULL) {
        ESP_LOGW(TAG, "Sensor task not running");
        return 0;
    }
    
    g_sensor.running = false;
    
    // 等待任务结束
    while (g_sensor.task_handle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    ESP_LOGI(TAG, "Sensor task stopped");
    return 0;
}

float mcp_sensor_get_temperature(void) {
    float temperature = BASE_TEMPERATURE;
    
    if (g_sensor.initialized && g_sensor.mutex) {
        if (xSemaphoreTake(g_sensor.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            temperature = g_sensor.temperature;
            xSemaphoreGive(g_sensor.mutex);
        }
    }
    
    return temperature;
}

float mcp_sensor_get_humidity(void) {
    float humidity = BASE_HUMIDITY;
    
    if (g_sensor.initialized && g_sensor.mutex) {
        if (xSemaphoreTake(g_sensor.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            humidity = g_sensor.humidity;
            xSemaphoreGive(g_sensor.mutex);
        }
    }
    
    return humidity;
}

void mcp_sensor_set_callback(int (*callback)(float temperature, float humidity)) {
    g_sensor.update_callback = callback;
    ESP_LOGI(TAG, "Sensor callback %s", callback ? "set" : "cleared");
}
