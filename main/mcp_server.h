#ifndef _MCP_SERVER_H_
#define _MCP_SERVER_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// MCP Server configuration
#define MCP_SERVER_PORT 3001
#define MCP_SERVER_MAX_CONNECTIONS 5
#define MCP_SERVER_BUFFER_SIZE 4096


// MCP 传输模式
typedef enum {
    MCP_TRANSPORT_HTTP = 0,
    MCP_TRANSPORT_WEBSOCKET,
    MCP_TRANSPORT_BOTH
} mcp_transport_mode_t;

// MCP Message types
typedef enum {
    MCP_MSG_TYPE_REQUEST = 0,
    MCP_MSG_TYPE_RESPONSE,
    MCP_MSG_TYPE_NOTIFICATION
} mcp_msg_type_t;

// MCP Methods
typedef enum {
    MCP_METHOD_INITIALIZE = 0,
    MCP_METHOD_LIST_TOOLS,
    MCP_METHOD_CALL_TOOL,
    MCP_METHOD_LIST_RESOURCES,
    MCP_METHOD_READ_RESOURCE,
    MCP_METHOD_SUBSCRIBE,
    MCP_METHOD_UNSUBSCRIBE
} mcp_method_t;

// Device status structure
typedef struct {
    // Light control
    bool light_enabled;
    int light_brightness;     // 0-100%
    int light_red;           // RGB values 0-255
    int light_green;
    int light_blue;
    
    // Fan control
    bool fan_enabled;
    int fan_speed;           // 1-5 speed levels
    int fan_timer_minutes;   // Timer in minutes, 0 = no timer
    uint32_t fan_timer_start; // Timestamp when timer started
    
    // Environmental sensors
    float temperature;        // Temperature in Celsius
    float humidity;          // Humidity in %
    uint32_t last_sensor_update; // Timestamp of last sensor reading
} mcp_device_status_t;

// Tool parameter structure
typedef struct {
    char name[64];
    char type[32];           // "string", "number", "boolean"
    char description[256];
    bool required;
} mcp_tool_param_t;

// Tool definition structure
typedef struct {
    char name[64];
    char description[256];
    mcp_tool_param_t params[8];  // Max 8 parameters per tool
    int param_count;
} mcp_tool_t;

// Resource structure
typedef struct {
    char uri[128];
    char name[64];
    char description[256];
    char mime_type[64];
} mcp_resource_t;

/**
 * @brief Initialize MCP server
 * @return 0 on success, -1 on error
 */
int mcp_server_init(void);

/**
 * @brief Get current device status
 * @param status Pointer to status structure to fill
 * @return 0 on success, -1 on error
 */
int mcp_server_get_status(mcp_device_status_t *status);

/**
 * @brief Update sensor readings
 * @param temperature Temperature in Celsius
 * @param humidity Humidity in %
 * @return 0 on success, -1 on error
 */
int mcp_server_update_sensors(float temperature, float humidity);

/**
 * @brief Control light power
 * @param enabled True to enable, false to disable
 * @return 0 on success, -1 on error
 */
int mcp_server_control_light_power(bool enabled);

/**
 * @brief Control light brightness
 * @param brightness Brightness level 0-100%
 * @return 0 on success, -1 on error
 */
int mcp_server_control_light_brightness(int brightness);

/**
 * @brief Control light color
 * @param red Red component 0-255
 * @param green Green component 0-255  
 * @param blue Blue component 0-255
 * @return 0 on success, -1 on error
 */
int mcp_server_control_light_color(int red, int green, int blue);

/**
 * @brief Control fan power
 * @param enabled True to enable, false to disable
 * @return 0 on success, -1 on error
 */
int mcp_server_control_fan_power(bool enabled);

/**
 * @brief Control fan speed
 * @param speed Fan speed level 1-5
 * @return 0 on success, -1 on error
 */
int mcp_server_control_fan_speed(int speed);

/**
 * @brief Set fan timer
 * @param minutes Timer in minutes (0 to disable timer)
 * @return 0 on success, -1 on error
 */
int mcp_server_control_fan_timer(int minutes);

// WebSocket 相关 API

/**
 * @brief 初始化并启动 WebSocket 客户端
 * @param endpoint WebSocket 服务器 endpoint
 * @param token 预留参数，向后兼容 (可以为 NULL)
 * @return 0 on success, -1 on error
 */
int mcp_server_start_websocket(const char *endpoint);

/**
 * @brief 停止 WebSocket 客户端
 * @return 0 on success, -1 on error
 */
int mcp_server_stop_websocket(void);

/**
 * @brief 检查 WebSocket 连接状态
 * @return true if connected, false otherwise
 */
bool mcp_server_websocket_is_connected(void);

/**
 * @brief 设置 MCP 传输模式
 * @param mode 传输模式 (HTTP, WebSocket, 或两者)
 * @return 0 on success, -1 on error
 */
int mcp_server_set_transport_mode(mcp_transport_mode_t mode);

/**
 * @brief 获取当前传输模式
 * @return 当前传输模式
 */
mcp_transport_mode_t mcp_server_get_transport_mode(void);

/**
 * @brief 通过 WebSocket 发送 MCP 响应
 * @param response JSON 响应字符串
 * @return 0 on success, -1 on error
 */
int mcp_server_websocket_send_response(const char *response);

/**
 * @brief 获取 WebSocket 连接统计信息
 * @param sent_messages 发送的消息数量 (可为 NULL)
 * @param received_messages 接收的消息数量 (可为 NULL)
 * @param reconnect_count 重连次数 (可为 NULL)
 */
void mcp_server_get_websocket_stats(uint32_t *sent_messages, uint32_t *received_messages, uint32_t *reconnect_count);

#ifdef __cplusplus
}
#endif

#endif // _MCP_SERVER_H_
