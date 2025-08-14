#ifndef _MCP_WEBSOCKET_H_
#define _MCP_WEBSOCKET_H_

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MCP_WS_MAX_URL_LEN          512
#define MCP_WS_MAX_PATH_LEN         512
#define MCP_WS_MAX_HOST_LEN         256

#define MCP_WS_MAX_MESSAGE_LEN      2048
#define MCP_WS_RECONNECT_DELAY_MS   5000
#define MCP_WS_PING_INTERVAL_MS     20000

// 发送队列配置
#define MCP_WS_SEND_QUEUE_SIZE      10
#define MCP_WS_SEND_TIMEOUT_MS      1000

/**
 * @brief WebSocket连接状态（状态机）
 */
typedef enum {
    MCP_WS_STATE_IDLE = 0,          ///< 空闲状态
    MCP_WS_STATE_INITIALIZING,      ///< 初始化中
    MCP_WS_STATE_CONNECTING,        ///< 连接中
    MCP_WS_STATE_CONNECTED,         ///< 已连接
    MCP_WS_STATE_DISCONNECTING,     ///< 断开连接中
    MCP_WS_STATE_DISCONNECTED,      ///< 已断开
    MCP_WS_STATE_RECONNECTING,      ///< 重连中
    MCP_WS_STATE_ERROR              ///< 错误状态
} mcp_ws_state_t;

/**
 * @brief WebSocket事件类型
 */
typedef enum {
    MCP_WS_EVENT_CONNECTED = 0,
    MCP_WS_EVENT_DISCONNECTED,
    MCP_WS_EVENT_MESSAGE_RECEIVED,
    MCP_WS_EVENT_MESSAGE_SENT,      ///< 消息发送成功
    MCP_WS_EVENT_ERROR
} mcp_ws_event_type_t;

/**
 * @brief 发送消息类型
 */
typedef enum {
    MCP_WS_MSG_TYPE_TEXT = 0,       ///< 文本消息
    MCP_WS_MSG_TYPE_PING,           ///< Ping消息
    MCP_WS_MSG_TYPE_PONG,           ///< Pong消息
    MCP_WS_MSG_TYPE_CLOSE           ///< 关闭消息
} mcp_ws_msg_type_t;

/**
 * @brief 发送队列消息结构
 */
typedef struct {
    mcp_ws_msg_type_t type;         ///< 消息类型
    char *data;                     ///< 消息数据（动态分配）
    size_t data_len;                ///< 数据长度
} mcp_ws_send_msg_t;

/**
 * @brief WebSocket事件数据
 */
typedef struct {
    mcp_ws_event_type_t event_type;
    char *data;
    size_t data_len;
    esp_err_t error_code;
} mcp_ws_event_t;

/**
 * @brief WebSocket事件回调函数类型
 */
typedef void (*mcp_ws_event_callback_t)(mcp_ws_event_t *event);

/**
 * @brief WebSocket配置结构
 */
typedef struct {
    char endpoint[MCP_WS_MAX_URL_LEN];  // 完整的endpoint URL，包含token参数
    mcp_ws_event_callback_t event_callback;
    bool auto_reconnect;
    uint32_t reconnect_delay_ms;
    uint32_t ping_interval_ms;
} mcp_ws_config_t;

/**
 * @brief 初始化WebSocket客户端
 * @param config WebSocket配置
 * @return ESP_OK on success, error code on failure
 */
esp_err_t mcp_websocket_init(const mcp_ws_config_t *config);

/**
 * @brief 启动WebSocket连接
 * @return ESP_OK on success, error code on failure
 */
esp_err_t mcp_websocket_start(void);

/**
 * @brief 停止WebSocket连接
 * @return ESP_OK on success, error code on failure
 */
esp_err_t mcp_websocket_stop(void);

/**
 * @brief 发送WebSocket消息
 * @param message 要发送的消息
 * @param len 消息长度
 * @return ESP_OK on success, error code on failure
 */
esp_err_t mcp_websocket_send(const char *message, size_t len);

/**
 * @brief 发送文本消息
 * @param message 要发送的文本消息
 * @return ESP_OK on success, error code on failure
 */
esp_err_t mcp_websocket_send_text(const char *message);

/**
 * @brief 获取WebSocket连接状态
 * @return 当前连接状态
 */
mcp_ws_state_t mcp_websocket_get_state(void);

/**
 * @brief 检查WebSocket是否已连接
 * @return true if connected, false otherwise
 */
bool mcp_websocket_is_connected(void);

/**
 * @brief 设置WebSocket事件回调
 * @param callback 事件回调函数
 */
void mcp_websocket_set_callback(mcp_ws_event_callback_t callback);

/**
 * @brief 获取连接统计信息
 * @param sent_messages 发送的消息数量
 * @param received_messages 接收的消息数量
 * @param reconnect_count 重连次数
 */
void mcp_websocket_get_stats(uint32_t *sent_messages, uint32_t *received_messages, uint32_t *reconnect_count);

/**
 * @brief 销毁WebSocket客户端并释放资源
 * @return ESP_OK on success, error code on failure
 */
esp_err_t mcp_websocket_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* _MCP_WEBSOCKET_H_ */
