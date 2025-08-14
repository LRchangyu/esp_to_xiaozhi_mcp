#include "mcp_server.h"
#include "mcp_websocket.h"
#include "esp_log.h"

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "mcp_server";

// Global device status
static mcp_device_status_t g_device_status = {
    .light_enabled = false,
    .light_brightness = 50,   // Default 50% brightness
    .light_red = 255,
    .light_green = 255,
    .light_blue = 255,        // Default white light
    .fan_enabled = false,
    .fan_speed = 3,           // Default medium speed
    .fan_timer_minutes = 0,   // No timer by default
    .fan_timer_start = 0,
    .temperature = 22.5f,     // Default temperature
    .humidity = 45.0f,        // Default humidity
    .last_sensor_update = 0
};

static SemaphoreHandle_t g_status_mutex = NULL;

// WebSocket 相关状态
static struct {
    bool initialized;
    bool connected;
    mcp_transport_mode_t transport_mode;
} g_mcp_ws_state = {
    .transport_mode = MCP_TRANSPORT_HTTP,
    .initialized = false,
    .connected = false
};

// Tool definitions
static const mcp_tool_t g_tools[] = {
    {
        .name = "get_temperature",
        .description = "Get current temperature reading",
        .params = {},
        .param_count = 0
    },
    {
        .name = "get_humidity", 
        .description = "Get current humidity reading",
        .params = {},
        .param_count = 0
    },
    {
        .name = "light_power_control",
        .description = "Control light power on/off",
        .params = {
            {.name = "enabled", .type = "boolean", .description = "Enable or disable light", .required = true}
        },
        .param_count = 1
    },
    {
        .name = "light_brightness_control",
        .description = "Set light brightness level",
        .params = {
            {.name = "brightness", .type = "number", .description = "Brightness level 0-100%", .required = true}
        },
        .param_count = 1
    },
    {
        .name = "light_color_control",
        .description = "Set light RGB color",
        .params = {
            {.name = "red", .type = "number", .description = "Red component 0-255", .required = true},
            {.name = "green", .type = "number", .description = "Green component 0-255", .required = true},
            {.name = "blue", .type = "number", .description = "Blue component 0-255", .required = true}
        },
        .param_count = 3
    },
    {
        .name = "fan_power_control",
        .description = "Control fan power on/off",
        .params = {
            {.name = "enabled", .type = "boolean", .description = "Enable or disable fan", .required = true}
        },
        .param_count = 1
    },
    {
        .name = "fan_speed_control",
        .description = "Set fan speed level",
        .params = {
            {.name = "speed", .type = "number", .description = "Fan speed level 1-5", .required = true}
        },
        .param_count = 1
    },
    {
        .name = "fan_timer_control",
        .description = "Set fan timer in minutes",
        .params = {
            {.name = "minutes", .type = "number", .description = "Timer in minutes (0 to disable timer)", .required = true}
        },
        .param_count = 1
    }
};

#define TOOL_COUNT (sizeof(g_tools) / sizeof(g_tools[0]))

// Resource definitions
static const mcp_resource_t g_resources[] = {
    {
        .uri = "device://status",
        .name = "Device Status",
        .description = "Real-time device status including sensors and controls",
        .mime_type = "application/json"
    },
    {
        .uri = "device://sensors", 
        .name = "Environmental Sensors",
        .description = "Temperature and humidity sensor readings",
        .mime_type = "application/json"
    },
    {
        .uri = "device://controls",
        .name = "Device Controls", 
        .description = "Current state of all controllable devices",
        .mime_type = "application/json"
    }
};

#define RESOURCE_COUNT (sizeof(g_resources) / sizeof(g_resources[0]))

// Utility functions
static cJSON* create_error_response(int id, int code, const char* message);
static cJSON* create_success_response(int id, cJSON* result);

// WebSocket MCP 请求处理函数
static cJSON* process_mcp_request(cJSON *request);
static cJSON* process_initialize_request(cJSON *request, int id);
static cJSON* process_list_tools_request(cJSON *request, int id);
static cJSON* process_call_tool_request(cJSON *request, int id);
static cJSON* process_list_resources_request(cJSON *request, int id);
static cJSON* process_read_resource_request(cJSON *request, int id);
static void mcp_websocket_event_handler(mcp_ws_event_t *event);

static cJSON* create_error_response(int id, int code, const char* message) {
    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(response, "id", id);
    
    cJSON *error = cJSON_CreateObject();
    cJSON_AddNumberToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddItemToObject(response, "error", error);
    
    return response;
}

static cJSON* create_success_response(int id, cJSON* result) {
    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(response, "id", id);
    cJSON_AddItemToObject(response, "result", result);
    
    return response;
}

// Public API implementations
int mcp_server_init(void) {
    if (g_status_mutex == NULL) {
        g_status_mutex = xSemaphoreCreateMutex();
        if (g_status_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create status mutex");
            return -1;
        }
    }
    
    ESP_LOGI(TAG, "MCP Server initialized");
    return 0;
}



int mcp_server_get_status(mcp_device_status_t *status) {
    if (!status || !g_status_mutex) {
        return -1;
    }
    
    if (xSemaphoreTake(g_status_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return -1;
    }
    
    *status = g_device_status;
    xSemaphoreGive(g_status_mutex);
    
    return 0;
}

int mcp_server_update_sensors(float temperature, float humidity) {
    if (!g_status_mutex) {
        return -1;
    }
    
    if (xSemaphoreTake(g_status_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return -1;
    }
    
    g_device_status.temperature = temperature;
    g_device_status.humidity = humidity;
    g_device_status.last_sensor_update = esp_timer_get_time() / 1000;
    
    xSemaphoreGive(g_status_mutex);
    
    // ESP_LOGI(TAG, "Sensors updated: T=%.1f°C, H=%.1f%%", temperature, humidity);
    return 0;
}

int mcp_server_control_light_power(bool enabled) {
    if (!g_status_mutex) {
        return -1;
    }
    
    // Send MQTT command
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "command", "light_power_control");
    cJSON_AddBoolToObject(payload, "enabled", enabled);

    int ret = 0;
    cJSON_Delete(payload);
    
    if (ret == 0) {
        if (xSemaphoreTake(g_status_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            g_device_status.light_enabled = enabled;
            xSemaphoreGive(g_status_mutex);
        }
    }
    
    ESP_LOGI(TAG, "Light power control: %s (ret=%d)", enabled ? "enabled" : "disabled", ret);
    return ret;
}

int mcp_server_control_light_brightness(int brightness) {
    if (brightness < 0 || brightness > 100) {
        ESP_LOGE(TAG, "Invalid brightness: %d%% (range: 0-100)", brightness);
        return -1;
    }
    
    if (!g_status_mutex) {
        return -1;
    }
    
    // Send MQTT command
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "command", "light_brightness_control");
    cJSON_AddNumberToObject(payload, "brightness", brightness);

    int ret = 0;
    cJSON_Delete(payload);
    
    if (ret == 0) {
        if (xSemaphoreTake(g_status_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            g_device_status.light_brightness = brightness;
            xSemaphoreGive(g_status_mutex);
        }
    }
    
    ESP_LOGI(TAG, "Light brightness control: %d%% (ret=%d)", brightness, ret);
    return ret;
}

int mcp_server_control_light_color(int red, int green, int blue) {
    if (red < 0 || red > 255 || green < 0 || green > 255 || blue < 0 || blue > 255) {
        ESP_LOGE(TAG, "Invalid RGB values: (%d, %d, %d) (range: 0-255)", red, green, blue);
        return -1;
    }
    
    if (!g_status_mutex) {
        return -1;
    }
    
    // Send MQTT command
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "command", "light_color_control");
    cJSON_AddNumberToObject(payload, "red", red);
    cJSON_AddNumberToObject(payload, "green", green);
    cJSON_AddNumberToObject(payload, "blue", blue);

    int ret = 0;
    cJSON_Delete(payload);
    
    if (ret == 0) {
        if (xSemaphoreTake(g_status_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            g_device_status.light_red = red;
            g_device_status.light_green = green;
            g_device_status.light_blue = blue;
            xSemaphoreGive(g_status_mutex);
        }
    }
    
    ESP_LOGI(TAG, "Light color control: RGB(%d, %d, %d) (ret=%d)", red, green, blue, ret);
    return ret;
}

int mcp_server_control_fan_power(bool enabled) {
    if (!g_status_mutex) {
        return -1;
    }
    
    // Send MQTT command
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "command", "fan_power_control");
    cJSON_AddBoolToObject(payload, "enabled", enabled);

    int ret = 0;
    cJSON_Delete(payload);
    
    if (ret == 0) {
        if (xSemaphoreTake(g_status_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            g_device_status.fan_enabled = enabled;
            xSemaphoreGive(g_status_mutex);
        }
    }
    
    ESP_LOGI(TAG, "Fan power control: %s (ret=%d)", enabled ? "enabled" : "disabled", ret);
    return ret;
}

int mcp_server_control_fan_speed(int speed) {
    if (speed < 1 || speed > 5) {
        ESP_LOGE(TAG, "Invalid fan speed: %d (range: 1-5)", speed);
        return -1;
    }
    
    if (!g_status_mutex) {
        return -1;
    }
    
    // Send MQTT command
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "command", "fan_speed_control");
    cJSON_AddNumberToObject(payload, "speed", speed);

    int ret = 0;
    cJSON_Delete(payload);
    
    if (ret == 0) {
        if (xSemaphoreTake(g_status_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            g_device_status.fan_speed = speed;
            xSemaphoreGive(g_status_mutex);
        }
    }
    
    ESP_LOGI(TAG, "Fan speed control: %d (ret=%d)", speed, ret);
    return ret;
}

int mcp_server_control_fan_timer(int minutes) {
    if (minutes < 0) {
        ESP_LOGE(TAG, "Invalid fan timer: %d minutes (must be >= 0)", minutes);
        return -1;
    }
    
    if (!g_status_mutex) {
        return -1;
    }
    
    // Send MQTT command
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "command", "fan_timer_control");
    cJSON_AddNumberToObject(payload, "minutes", minutes);

    int ret = 0;
    cJSON_Delete(payload);
    
    if (ret == 0) {
        if (xSemaphoreTake(g_status_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            g_device_status.fan_timer_minutes = minutes;
            if (minutes > 0) {
                g_device_status.fan_timer_start = esp_timer_get_time() / 1000; // ms
            } else {
                g_device_status.fan_timer_start = 0;
            }
            xSemaphoreGive(g_status_mutex);
        }
    }
    
    ESP_LOGI(TAG, "Fan timer control: %d minutes (ret=%d)", minutes, ret);
    return ret;
}

// WebSocket 事件处理函数
// 角色说明：
// - WebSocket层面：ESP32是WebSocket Client，连接到远程WebSocket Server (api.xiaozhi.me)
// - MCP层面：ESP32是MCP Server，提供工具和资源给MCP Client
static void mcp_websocket_event_handler(mcp_ws_event_t *event) {
    switch (event->event_type) {
        case MCP_WS_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket Client connected to WebSocket Server");
            ESP_LOGI(TAG, "ESP32 MCP Server is ready to serve MCP Client requests");
            g_mcp_ws_state.connected = true;
            
            // 根据MCP协议，作为MCP Server的ESP32应等待MCP Client发送initialize请求
            // 不应该主动发送initialize请求
            ESP_LOGI(TAG, "Waiting for MCP Client to send initialize request...");
            break;
            
        case MCP_WS_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "WebSocket Client disconnected from WebSocket Server");
            g_mcp_ws_state.connected = false;
            break;
            
        case MCP_WS_EVENT_MESSAGE_RECEIVED:
            ESP_LOGI(TAG, "WebSocket received message: %.*s", (int)event->data_len, event->data);
            
            // 解析并处理来自MCP Client的消息
            if (event->data && event->data_len > 0) {
                ESP_LOGI(TAG, "Parsing MCP Client message:%s", event->data);
                cJSON *request = cJSON_ParseWithLength(event->data, event->data_len);
                if (request) {
                    // 检查消息类型
                    cJSON *method_item = cJSON_GetObjectItem(request, "method");
                    
                    if (method_item && cJSON_IsString(method_item)) {
                        const char *method = method_item->valuestring;
                        ESP_LOGI(TAG, "Received MCP method from client: %s", method);
                    }
                    
                    // 处理 MCP 请求并发送响应（只对请求发送响应，通知不需要响应）
                    cJSON *id_item = cJSON_GetObjectItem(request, "id");
                    if (id_item) {
                        // 这是一个请求，需要响应
                        cJSON *response = process_mcp_request(request);
                        if (response) {
                            char *response_str = cJSON_PrintUnformatted(response);
                            if (response_str) {
                                ESP_LOGI(TAG, "Sending MCP response to client: %s", response_str);
                                mcp_websocket_send_text(response_str);
                                cJSON_free(response_str);
                            }
                            cJSON_Delete(response);
                        }
                    } else {
                        // 这是一个通知，不需要响应
                        ESP_LOGI(TAG, "Received MCP notification from client, no response needed");
                    }
                    
                    cJSON_Delete(request);
                } else {
                    ESP_LOGE(TAG, "Failed to parse WebSocket message as JSON");
                }
            }
            break;
            
        case MCP_WS_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error occurred");
            g_mcp_ws_state.connected = false;
            break;
            
        default:
            break;
    }
}

// 处理 MCP 请求的通用函数 (从 HTTP 处理器中提取)
static cJSON* process_mcp_request(cJSON *request) {
    cJSON *method_item = cJSON_GetObjectItem(request, "method");
    cJSON *id_item = cJSON_GetObjectItem(request, "id");
    
    if (!method_item || !cJSON_IsString(method_item)) {
        return create_error_response(0, -32600, "Invalid Request");
    }
    
    const char *method = method_item->valuestring;
    int id = id_item ? id_item->valueint : 0;
    
    ESP_LOGI(TAG, "Processing MCP method: %s", method);
    
    if (strcmp(method, "initialize") == 0) {
        return process_initialize_request(request, id);;
    } else if (strcmp(method, "ping") == 0) {
        // 处理MCP ping请求 - 简单返回空结果
        ESP_LOGI(TAG, "Processing MCP ping request from client");
        cJSON *empty_result = cJSON_CreateObject();
        return create_success_response(id, empty_result);
    } else if (strcmp(method, "prompts/list") == 0) {
        // 处理prompts列表请求
        ESP_LOGI(TAG, "Processing prompts/list request from client");
        cJSON *result = cJSON_CreateObject();
        cJSON *prompts_array = cJSON_CreateArray();
        // ESP32目前不提供prompts，返回空数组
        cJSON_AddItemToObject(result, "prompts", prompts_array);
        return create_success_response(id, result);
    } else if (strcmp(method, "prompts/get") == 0) {
        // 处理获取特定prompt请求
        ESP_LOGI(TAG, "Processing prompts/get request from client");
        return create_error_response(id, -32601, "Prompts not supported");
    } else if (strcmp(method, "logging/setLevel") == 0) {
        // 处理日志级别设置请求
        ESP_LOGI(TAG, "Processing logging/setLevel request from client");
        cJSON *empty_result = cJSON_CreateObject();
        return create_success_response(id, empty_result);
    } else if (strcmp(method, "completion/complete") == 0) {
        // 处理补全请求
        ESP_LOGI(TAG, "Processing completion/complete request from client");
        return create_error_response(id, -32601, "Completion not supported");
    } else if (strcmp(method, "resources/subscribe") == 0) {
        // 处理资源订阅请求
        ESP_LOGI(TAG, "Processing resources/subscribe request from client");
        cJSON *empty_result = cJSON_CreateObject();
        return create_success_response(id, empty_result);
    } else if (strcmp(method, "resources/unsubscribe") == 0) {
        // 处理资源取消订阅请求
        ESP_LOGI(TAG, "Processing resources/unsubscribe request from client");
        cJSON *empty_result = cJSON_CreateObject();
        return create_success_response(id, empty_result);
    } else if (strcmp(method, "tools/list") == 0) {
        return process_list_tools_request(request, id);
    } else if (strcmp(method, "tools/call") == 0) {
        return process_call_tool_request(request, id);
    } else if (strcmp(method, "resources/list") == 0) {
        return process_list_resources_request(request, id);
    } else if (strcmp(method, "resources/read") == 0) {
        return process_read_resource_request(request, id);
    } else {
        return create_error_response(id, -32601, "Method not found");
    }
}

// 提取的 MCP 请求处理函数
static cJSON* process_initialize_request(cJSON *request, int id) {
    ESP_LOGI(TAG, "Processing initialize request from MCP Client");
    
    cJSON *result = cJSON_CreateObject();
    cJSON *protocol_version = cJSON_CreateString("2024-11-05");
    cJSON *capabilities = cJSON_CreateObject();
    
    // ESP32作为MCP Server的能力声明
    cJSON *tools = cJSON_CreateObject();
    cJSON *resources = cJSON_CreateObject();
    cJSON *prompts = cJSON_CreateObject();
    cJSON *experimental = cJSON_CreateObject();
    
    cJSON_AddBoolToObject(tools, "listChanged", false);
    cJSON_AddBoolToObject(resources, "subscribe", false);
    cJSON_AddBoolToObject(resources, "listChanged", false);
    cJSON_AddBoolToObject(prompts, "listChanged", false);
    
    cJSON_AddItemToObject(capabilities, "tools", tools);
    cJSON_AddItemToObject(capabilities, "resources", resources);
    cJSON_AddItemToObject(capabilities, "prompts", prompts);
    cJSON_AddItemToObject(capabilities, "experimental", experimental);
    
    cJSON_AddItemToObject(result, "protocolVersion", protocol_version);
    cJSON_AddItemToObject(result, "capabilities", capabilities);
    
    // MCP Server信息 (ESP32作为MCP Server)
    cJSON *server_info = cJSON_CreateObject();
    cJSON_AddStringToObject(server_info, "name", "light_and_desk");
    cJSON_AddStringToObject(server_info, "version", "1.0.0");
    cJSON_AddItemToObject(result, "serverInfo", server_info);

    return create_success_response(id, result);
}

static cJSON* process_list_tools_request(cJSON *request, int id) {
    cJSON *result = cJSON_CreateObject();
    cJSON *tools_array = cJSON_CreateArray();

    for (int i = 0; i < TOOL_COUNT; i++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", g_tools[i].name);
        cJSON_AddStringToObject(tool, "description", g_tools[i].description);
        
        cJSON *input_schema = cJSON_CreateObject();
        cJSON_AddStringToObject(input_schema, "type", "object");
        
        cJSON *properties = cJSON_CreateObject();
        cJSON *required = cJSON_CreateArray();
        
        for (int j = 0; j < g_tools[i].param_count; j++) {
            cJSON *param = cJSON_CreateObject();
            cJSON_AddStringToObject(param, "type", g_tools[i].params[j].type);
            cJSON_AddStringToObject(param, "description", g_tools[i].params[j].description);
            cJSON_AddItemToObject(properties, g_tools[i].params[j].name, param);
            
            if (g_tools[i].params[j].required) {
                cJSON_AddItemToArray(required, cJSON_CreateString(g_tools[i].params[j].name));
            }
        }
        
        cJSON_AddItemToObject(input_schema, "properties", properties);
        if (cJSON_GetArraySize(required) > 0) {
            cJSON_AddItemToObject(input_schema, "required", required);
        } else {
            cJSON_Delete(required);
        }
        
        cJSON_AddItemToObject(tool, "inputSchema", input_schema);
        cJSON_AddItemToArray(tools_array, tool);
    }

    cJSON_AddItemToObject(result, "tools", tools_array);
    return create_success_response(id, result);
}

static cJSON* process_call_tool_request(cJSON *request, int id) {
    cJSON *params = cJSON_GetObjectItem(request, "params");
    if (!params) {
        return create_error_response(id, -32602, "Invalid params");
    }
    
    cJSON *name_item = cJSON_GetObjectItem(params, "name");
    cJSON *arguments = cJSON_GetObjectItem(params, "arguments");
    
    if (!name_item || !cJSON_IsString(name_item)) {
        return create_error_response(id, -32602, "Tool name required");
    }
    
    const char *tool_name = name_item->valuestring;
    cJSON *result = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    
    ESP_LOGI(TAG, "Calling tool via WebSocket: %s", tool_name);
    
    // 处理所有工具调用逻辑
    if (strcmp(tool_name, "light_power_control") == 0) {
        cJSON *enabled_item = cJSON_GetObjectItem(arguments, "enabled");
        if (enabled_item && cJSON_IsBool(enabled_item)) {
            bool enabled = cJSON_IsTrue(enabled_item);
            int ret = mcp_server_control_light_power(enabled);
            
            cJSON *response_content = cJSON_CreateObject();
            cJSON_AddStringToObject(response_content, "type", "text");
            if (ret == 0) {
                char text[128];
                snprintf(text, sizeof(text), "Light %s successfully", enabled ? "enabled" : "disabled");
                cJSON_AddStringToObject(response_content, "text", text);
            } else {
                cJSON_AddStringToObject(response_content, "text", "Failed to control light power");
            }
            cJSON_AddItemToArray(content, response_content);
        }
    } else if (strcmp(tool_name, "light_brightness_control") == 0) {
        cJSON *brightness_item = cJSON_GetObjectItem(arguments, "brightness");
        if (brightness_item && cJSON_IsNumber(brightness_item)) {
            int brightness = brightness_item->valueint;
            int ret = mcp_server_control_light_brightness(brightness);
            
            cJSON *response_content = cJSON_CreateObject();
            cJSON_AddStringToObject(response_content, "type", "text");
            if (ret == 0) {
                char text[128];
                snprintf(text, sizeof(text), "Light brightness set to %d%%", brightness);
                cJSON_AddStringToObject(response_content, "text", text);
            } else {
                cJSON_AddStringToObject(response_content, "text", "Failed to set light brightness");
            }
            cJSON_AddItemToArray(content, response_content);
        }
    } else if (strcmp(tool_name, "light_color_control") == 0) {
        cJSON *red_item = cJSON_GetObjectItem(arguments, "red");
        cJSON *green_item = cJSON_GetObjectItem(arguments, "green");
        cJSON *blue_item = cJSON_GetObjectItem(arguments, "blue");
        
        if (red_item && green_item && blue_item && 
            cJSON_IsNumber(red_item) && cJSON_IsNumber(green_item) && cJSON_IsNumber(blue_item)) {
            int red = red_item->valueint;
            int green = green_item->valueint;
            int blue = blue_item->valueint;
            int ret = mcp_server_control_light_color(red, green, blue);
            
            cJSON *response_content = cJSON_CreateObject();
            cJSON_AddStringToObject(response_content, "type", "text");
            if (ret == 0) {
                char text[128];
                snprintf(text, sizeof(text), "Light color set to RGB(%d, %d, %d)", red, green, blue);
                cJSON_AddStringToObject(response_content, "text", text);
            } else {
                cJSON_AddStringToObject(response_content, "text", "Failed to set light color");
            }
            cJSON_AddItemToArray(content, response_content);
        }
    } else if (strcmp(tool_name, "fan_power_control") == 0) {
        cJSON *enabled_item = cJSON_GetObjectItem(arguments, "enabled");
        if (enabled_item && cJSON_IsBool(enabled_item)) {
            bool enabled = cJSON_IsTrue(enabled_item);
            int ret = mcp_server_control_fan_power(enabled);
            
            cJSON *response_content = cJSON_CreateObject();
            cJSON_AddStringToObject(response_content, "type", "text");
            if (ret == 0) {
                char text[128];
                snprintf(text, sizeof(text), "Fan %s successfully", enabled ? "enabled" : "disabled");
                cJSON_AddStringToObject(response_content, "text", text);
            } else {
                cJSON_AddStringToObject(response_content, "text", "Failed to control fan power");
            }
            cJSON_AddItemToArray(content, response_content);
        }
    } else if (strcmp(tool_name, "fan_speed_control") == 0) {
        cJSON *speed_item = cJSON_GetObjectItem(arguments, "speed");
        if (speed_item && cJSON_IsNumber(speed_item)) {
            int speed = speed_item->valueint;
            int ret = mcp_server_control_fan_speed(speed);
            
            cJSON *response_content = cJSON_CreateObject();
            cJSON_AddStringToObject(response_content, "type", "text");
            if (ret == 0) {
                char text[128];
                snprintf(text, sizeof(text), "Fan speed set to level %d", speed);
                cJSON_AddStringToObject(response_content, "text", text);
            } else {
                cJSON_AddStringToObject(response_content, "text", "Failed to set fan speed");
            }
            cJSON_AddItemToArray(content, response_content);
        }
    } else if (strcmp(tool_name, "fan_timer_control") == 0) {
        cJSON *minutes_item = cJSON_GetObjectItem(arguments, "minutes");
        if (minutes_item && cJSON_IsNumber(minutes_item)) {
            int minutes = minutes_item->valueint;
            int ret = mcp_server_control_fan_timer(minutes);
            
            cJSON *response_content = cJSON_CreateObject();
            cJSON_AddStringToObject(response_content, "type", "text");
            if (ret == 0) {
                char text[128];
                if (minutes > 0) {
                    snprintf(text, sizeof(text), "Fan timer set to %d minutes", minutes);
                } else {
                    snprintf(text, sizeof(text), "Fan timer disabled");
                }
                cJSON_AddStringToObject(response_content, "text", text);
            } else {
                cJSON_AddStringToObject(response_content, "text", "Failed to set fan timer");
            }
            cJSON_AddItemToArray(content, response_content);
        }
    } else if (strcmp(tool_name, "get_temperature") == 0) {
        mcp_device_status_t status;
        if (mcp_server_get_status(&status) == 0) {
            cJSON *response_content = cJSON_CreateObject();
            cJSON_AddStringToObject(response_content, "type", "text");
            char text[128];
            snprintf(text, sizeof(text), "Current temperature: %.1f°C", status.temperature);
            cJSON_AddStringToObject(response_content, "text", text);
            cJSON_AddItemToArray(content, response_content);
        }
    } else if (strcmp(tool_name, "get_humidity") == 0) {
        mcp_device_status_t status;
        if (mcp_server_get_status(&status) == 0) {
            cJSON *response_content = cJSON_CreateObject();
            cJSON_AddStringToObject(response_content, "type", "text");
            char text[128];
            snprintf(text, sizeof(text), "Current humidity: %.1f%%", status.humidity);
            cJSON_AddStringToObject(response_content, "text", text);
            cJSON_AddItemToArray(content, response_content);
        }
    } else {
        return create_error_response(id, -32601, "Tool not found");
    }
    
    cJSON_AddItemToObject(result, "content", content);
    return create_success_response(id, result);
}

static cJSON* process_list_resources_request(cJSON *request, int id) {
    cJSON *result = cJSON_CreateObject();
    cJSON *resources_array = cJSON_CreateArray();

    for (int i = 0; i < RESOURCE_COUNT; i++) {
        cJSON *resource = cJSON_CreateObject();
        cJSON_AddStringToObject(resource, "uri", g_resources[i].uri);
        cJSON_AddStringToObject(resource, "name", g_resources[i].name);
        cJSON_AddStringToObject(resource, "description", g_resources[i].description);
        cJSON_AddStringToObject(resource, "mimeType", g_resources[i].mime_type);
        cJSON_AddItemToArray(resources_array, resource);
    }

    cJSON_AddItemToObject(result, "resources", resources_array);
    return create_success_response(id, result);
}

static cJSON* process_read_resource_request(cJSON *request, int id) {
    cJSON *params = cJSON_GetObjectItem(request, "params");
    if (!params) {
        return create_error_response(id, -32602, "Invalid params");
    }
    
    cJSON *uri_item = cJSON_GetObjectItem(params, "uri");
    if (!uri_item || !cJSON_IsString(uri_item)) {
        return create_error_response(id, -32602, "URI required");
    }
    
    const char *uri = uri_item->valuestring;
    mcp_device_status_t status;
    mcp_server_get_status(&status);
    
    cJSON *result = cJSON_CreateObject();
    cJSON *contents = cJSON_CreateArray();
    cJSON *content = cJSON_CreateObject();
    
    if (strcmp(uri, "device://status") == 0) {
        cJSON_AddStringToObject(content, "uri", uri);
        cJSON_AddStringToObject(content, "mimeType", "application/json");
        
        cJSON *status_json = cJSON_CreateObject();
        cJSON_AddBoolToObject(status_json, "light_enabled", status.light_enabled);
        cJSON_AddNumberToObject(status_json, "light_brightness", status.light_brightness);
        cJSON_AddNumberToObject(status_json, "light_red", status.light_red);
        cJSON_AddNumberToObject(status_json, "light_green", status.light_green);
        cJSON_AddNumberToObject(status_json, "light_blue", status.light_blue);
        cJSON_AddBoolToObject(status_json, "fan_enabled", status.fan_enabled);
        cJSON_AddNumberToObject(status_json, "fan_speed", status.fan_speed);
        cJSON_AddNumberToObject(status_json, "fan_timer_minutes", status.fan_timer_minutes);
        cJSON_AddNumberToObject(status_json, "fan_timer_start", status.fan_timer_start);
        cJSON_AddNumberToObject(status_json, "temperature", status.temperature);
        cJSON_AddNumberToObject(status_json, "humidity", status.humidity);
        cJSON_AddNumberToObject(status_json, "last_sensor_update", status.last_sensor_update);
        
        char *status_str = cJSON_PrintUnformatted(status_json);
        cJSON_AddStringToObject(content, "text", status_str);
        cJSON_free(status_str);
        cJSON_Delete(status_json);
    } else {
        cJSON_Delete(result);
        cJSON_Delete(contents);
        cJSON_Delete(content);
        return create_error_response(id, -32602, "Resource not found");
    }
    
    cJSON_AddItemToArray(contents, content);
    cJSON_AddItemToObject(result, "contents", contents);
    
    return create_success_response(id, result);
}

int mcp_server_start_websocket(const char *endpoint) {
    ESP_LOGI(TAG, "Starting WebSocket connection to endpoint: %s", endpoint ? endpoint : "NULL");
    
    mcp_ws_config_t ws_config = {
        .ping_interval_ms = 20000,    
        .auto_reconnect = true,
        .reconnect_delay_ms = 5000
    };
    
    // 使用endpoint，如果为NULL则使用默认值
    if (endpoint) {
        strlcpy(ws_config.endpoint, endpoint, sizeof(ws_config.endpoint));
    } else {
        // 使用默认endpoint（包含默认token）
        const char *default_endpoint = "wss://api.xiaozhi.me/mcp/?token=eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCJ9.eyJ1c2VySWQiOjE4ODQ3NCwiYWdlbnRJZCI6MTA0ODExLCJlbmRwb2ludElkIjoiYWdlbnRfMTA0ODExIiwicHVycG9zZSI6Im1jcC1lbmRwb2ludCIsImlhdCI6MTc1NTA1MzY0Nn0.7qgkbaHlrqqZzzyBC236LCk6kHL_uItr4Tasr4WEXv1M51BiIoV7d5hgjQfYc_YMNzmmLqYUBC2w4mY75qD_Mw";
        strlcpy(ws_config.endpoint, default_endpoint, sizeof(ws_config.endpoint));
    }
    
    ws_config.event_callback = mcp_websocket_event_handler;
    
    esp_err_t ret = mcp_websocket_init(&ws_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client: %s", esp_err_to_name(ret));
        return -1;
    }
    
    ret = mcp_websocket_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client: %s", esp_err_to_name(ret));
        return -1;
    }
    
    g_mcp_ws_state.initialized = true;
    ESP_LOGI(TAG, "WebSocket client started successfully");

    return 0;
}

int mcp_server_stop_websocket(void) {
    if (!g_mcp_ws_state.initialized) {
        return 0;
    }
    
    esp_err_t ret = mcp_websocket_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop WebSocket client: %s", esp_err_to_name(ret));
        return -1;
    }
    
    g_mcp_ws_state.initialized = false;
    g_mcp_ws_state.connected = false;
    
    ESP_LOGI(TAG, "WebSocket client stopped");
    return 0;
}

bool mcp_server_websocket_is_connected(void) {
    return g_mcp_ws_state.connected && mcp_websocket_is_connected();
}

int mcp_server_set_transport_mode(mcp_transport_mode_t mode) {
    g_mcp_ws_state.transport_mode = mode;
    ESP_LOGI(TAG, "Transport mode set to: %d", mode);
    return 0;
}

mcp_transport_mode_t mcp_server_get_transport_mode(void) {
    return g_mcp_ws_state.transport_mode;
}

int mcp_server_websocket_send_response(const char *response) {
    if (!response) {
        return -1;
    }
    
    if (!mcp_server_websocket_is_connected()) {
        ESP_LOGW(TAG, "WebSocket not connected, cannot send response");
        return -1;
    }
    
    esp_err_t ret = mcp_websocket_send_text(response);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send WebSocket response: %s", esp_err_to_name(ret));
        return -1;
    }
    
    return 0;
}

void mcp_server_get_websocket_stats(uint32_t *sent_messages, uint32_t *received_messages, uint32_t *reconnect_count) {
    mcp_websocket_get_stats(sent_messages, received_messages, reconnect_count);
}
