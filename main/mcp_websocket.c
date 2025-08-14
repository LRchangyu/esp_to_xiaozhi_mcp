/**
 * @file mcp_websocket.c
 * @brief MCP WebSocket客户端实现 - 单任务状态机架构
 */

#include "mcp_websocket.h"
#include "esp_log.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "mcp_websocket";

// WebSocket客户端状态机
typedef struct {
    // 基本配置
    mcp_ws_config_t config;
    mcp_ws_event_callback_t event_callback;
    
    // 状态机
    mcp_ws_state_t state;
    uint32_t state_start_time;
    
    // 传输层
    esp_transport_list_handle_t transport_list;
    esp_transport_handle_t transport;
    
    // URL解析结果
    char *host;                     // 动态分配
    int port;
    char *path;                     // 动态分配
    bool use_ssl;
    
    // 任务和队列
    TaskHandle_t main_task;
    QueueHandle_t send_queue;
    
    // 定时器
    esp_timer_handle_t ping_timer;

    uint64_t last_ping_time;
    
    // 统计信息
    uint32_t sent_messages;
    uint32_t received_messages;
    uint32_t reconnect_count;
    
    // 控制标志
    bool initialized;
    bool should_stop;
    bool auto_reconnect_enabled;
    
} mcp_websocket_client_t;

static mcp_websocket_client_t g_ws_client = {0};

static void websocket_main_task(void *pvParameters);
static void ping_timer_callback(void *arg);

static esp_err_t parse_url(const char *url);
static esp_err_t create_transport_list(void);
static void cleanup_transport(void);
static void set_state(mcp_ws_state_t new_state);
static void trigger_event(mcp_ws_event_type_t event_type, const char *data, size_t data_len, esp_err_t error_code);
static esp_err_t enqueue_send_message(mcp_ws_msg_type_t type, const char *data, size_t data_len);
static void free_send_message(mcp_ws_send_msg_t *msg);

static void set_state(mcp_ws_state_t new_state) {
    if (g_ws_client.state != new_state) {
        ESP_LOGD(TAG, "State change: %d -> %d", g_ws_client.state, new_state);
        g_ws_client.state = new_state;
        g_ws_client.state_start_time = esp_timer_get_time() / 1000; // ms
    }
}

static void trigger_event(mcp_ws_event_type_t event_type, const char *data, size_t data_len, esp_err_t error_code) {
    if (g_ws_client.event_callback) {
        mcp_ws_event_t event = {
            .event_type = event_type,
            .data = (char*)data,
            .data_len = data_len,
            .error_code = error_code
        };
        g_ws_client.event_callback(&event);
    }
}

static esp_err_t enqueue_send_message(mcp_ws_msg_type_t type, const char *data, size_t data_len) {
    if (!g_ws_client.send_queue) {
        ESP_LOGE(TAG, "Send queue not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 创建发送消息
    mcp_ws_send_msg_t *msg = malloc(sizeof(mcp_ws_send_msg_t));
    if (!msg) {
        ESP_LOGE(TAG, "Failed to allocate memory for send message");
        return ESP_ERR_NO_MEM;
    }
    
    msg->type = type;
    msg->data_len = data_len;
    
    if (data && data_len > 0) {
        msg->data = malloc(data_len + 1);
        if (!msg->data) {
            free(msg);
            ESP_LOGE(TAG, "Failed to allocate memory for message data");
            return ESP_ERR_NO_MEM;
        }
        memcpy(msg->data, data, data_len);
        msg->data[data_len] = '\0';
    } else {
        msg->data = NULL;
    }
    
    // 入队
    if (xQueueSend(g_ws_client.send_queue, &msg, pdMS_TO_TICKS(MCP_WS_SEND_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Send queue full, dropping message");
        free_send_message(msg);
        return ESP_ERR_TIMEOUT;
    }
    
    ESP_LOGD(TAG, "Message enqueued, type: %d, size: %d", type, (int)data_len);
    return ESP_OK;
}

static void free_send_message(mcp_ws_send_msg_t *msg) {
    if (msg) {
        if (msg->data) {
            free(msg->data);
        }
        free(msg);
    }
}

static esp_err_t parse_url(const char *endpoint) {
    char *url_copy = malloc(MCP_WS_MAX_URL_LEN);
    if (!url_copy) {
        ESP_LOGE(TAG, "Failed to allocate memory for URL parsing");
        return ESP_ERR_NO_MEM;
    }
    
    strlcpy(url_copy, endpoint, MCP_WS_MAX_URL_LEN);
    
    char *work_url = url_copy;
    
    // 解析协议
    if (strncmp(work_url, "wss://", 6) == 0) {
        g_ws_client.use_ssl = true;
        g_ws_client.port = 443;
        work_url += 6;
    } else if (strncmp(work_url, "ws://", 5) == 0) {
        g_ws_client.use_ssl = false;
        g_ws_client.port = 80;
        work_url += 5;
    } else {
        ESP_LOGE(TAG, "Invalid WebSocket URL scheme");
        free(url_copy);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 释放之前的内存
    if (g_ws_client.host) {
        free(g_ws_client.host);
        g_ws_client.host = NULL;
    }
    if (g_ws_client.path) {
        free(g_ws_client.path);
        g_ws_client.path = NULL;
    }
    
    // 分配新的内存
    g_ws_client.host = malloc(MCP_WS_MAX_HOST_LEN);
    g_ws_client.path = malloc(MCP_WS_MAX_PATH_LEN);

    if (!g_ws_client.host || !g_ws_client.path) {
        ESP_LOGE(TAG, "Failed to allocate memory for host/path");
        if (g_ws_client.host) free(g_ws_client.host);
        if (g_ws_client.path) free(g_ws_client.path);
        g_ws_client.host = NULL;
        g_ws_client.path = NULL;
        free(url_copy);
        return ESP_ERR_NO_MEM;
    }
    memset(g_ws_client.host, 0, MCP_WS_MAX_HOST_LEN);
    memset(g_ws_client.path, 0, MCP_WS_MAX_PATH_LEN);

    // 解析主机和路径 - endpoint已经包含完整路径和token参数
    const char *path_start = strchr(work_url, '/');
    if (path_start) {
        ESP_LOGI(TAG, "Path found: %s", path_start);
        // 直接使用完整路径（已包含token参数）
        strlcpy(g_ws_client.path, path_start, MCP_WS_MAX_PATH_LEN);
        
        int host_len = path_start - work_url;
        if (host_len > 0 && host_len < MCP_WS_MAX_HOST_LEN) {
            strlcpy(g_ws_client.host, work_url, host_len + 1);  // +1 for null terminator
            g_ws_client.host[host_len] = '\0';  // Ensure termination at host end
        } else {
            ESP_LOGE(TAG, "Invalid host name length");
            free(g_ws_client.host);
            free(g_ws_client.path);
            g_ws_client.host = NULL;
            g_ws_client.path = NULL;
            free(url_copy);
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        ESP_LOGI(TAG, "No path found, using default");
        strlcpy(g_ws_client.host, work_url, MCP_WS_MAX_HOST_LEN);
        strcpy(g_ws_client.path, "/mcp/");  // 默认路径
    }
    
    // 检查端口
    char *port_start = strchr(g_ws_client.host, ':');
    if (port_start) {
        *port_start = '\0';
        int parsed_port = atoi(port_start + 1);
        if (parsed_port > 0 && parsed_port <= 65535) {
            g_ws_client.port = parsed_port;
        } else {
            ESP_LOGE(TAG, "Invalid port number");
            free(g_ws_client.host);
            free(g_ws_client.path);
            g_ws_client.host = NULL;
            g_ws_client.path = NULL;
            free(url_copy);
            return ESP_ERR_INVALID_ARG;
        }
    }

    ESP_LOGI(TAG, "Parsed endpoint: %s://%s:%d", 
             g_ws_client.use_ssl ? "wss" : "ws",
             g_ws_client.host, g_ws_client.port);
    
    ESP_LOGI(TAG, "path : %s", 
              g_ws_client.path);

    free(url_copy);
    return ESP_OK;
}

static esp_err_t create_transport_list(void) {
    // 清理旧的传输列表
    cleanup_transport();
    
    // 创建传输列表
    g_ws_client.transport_list = esp_transport_list_init();
    if (!g_ws_client.transport_list) {
        ESP_LOGE(TAG, "Failed to create transport list");
        return ESP_ERR_NO_MEM;
    }
    
    if (g_ws_client.use_ssl) {
        // 创建 SSL 传输
        esp_transport_handle_t ssl = esp_transport_ssl_init();
        if (!ssl) {
            ESP_LOGE(TAG, "Failed to create SSL transport");
            return ESP_ERR_NO_MEM;
        }
        
        // 暂时跳过SSL证书验证，专注于WebSocket握手问题
        // esp_transport_ssl_skip_cert_verify(ssl, true);
        esp_transport_set_default_port(ssl, 443);
        esp_transport_list_add(g_ws_client.transport_list, ssl, "ssl");
        
        // 创建 WebSocket over SSL 传输
        g_ws_client.transport = esp_transport_ws_init(ssl);
        if (!g_ws_client.transport) {
            ESP_LOGE(TAG, "Failed to create WSS transport");
            return ESP_ERR_NO_MEM;
        }
        
        esp_transport_set_default_port(g_ws_client.transport, 443);
        esp_transport_ws_set_path(g_ws_client.transport, g_ws_client.path);
        
        // 使用最简单的User-Agent，避免服务器拒绝
        esp_transport_ws_set_user_agent(g_ws_client.transport, "websocket-client");
        
        // 明确设置子协议为NULL，避免协议协商问题
        esp_transport_ws_set_subprotocol(g_ws_client.transport, NULL);
        
        // 不设置任何自定义头部，让ESP-IDF使用标准WebSocket头部
        
        esp_transport_list_add(g_ws_client.transport_list, g_ws_client.transport, "wss");
        
    } else {
        // 创建 TCP 传输
        esp_transport_handle_t tcp = esp_transport_tcp_init();
        if (!tcp) {
            ESP_LOGE(TAG, "Failed to create TCP transport");
            return ESP_ERR_NO_MEM;
        }
        
        esp_transport_set_default_port(tcp, 80);
        esp_transport_list_add(g_ws_client.transport_list, tcp, "tcp");
        
        // 创建 WebSocket over TCP 传输
        g_ws_client.transport = esp_transport_ws_init(tcp);
        if (!g_ws_client.transport) {
            ESP_LOGE(TAG, "Failed to create WS transport");
            return ESP_ERR_NO_MEM;
        }
        
        esp_transport_set_default_port(g_ws_client.transport, 80);
        esp_transport_ws_set_path(g_ws_client.transport, g_ws_client.path);
        esp_transport_ws_set_user_agent(g_ws_client.transport, "ESP32 Websocket Client");
        
        // 重要：不设置任何额外的HTTP头，只使用URL中的token参数
        // 这与Python实现保持完全一致
        // 设置为空字符串确保不会添加额外头部
        esp_transport_ws_set_headers(g_ws_client.transport, "");
        
        esp_transport_list_add(g_ws_client.transport_list, g_ws_client.transport, "ws");
    }
    
    return ESP_OK;
}

static void cleanup_transport(void) {
    mcp_ws_send_msg_t *send_msg = NULL;

    if (g_ws_client.send_queue) {
        while(xQueueReceive(g_ws_client.send_queue, &send_msg, 0) == pdTRUE) {
            free_send_message(send_msg);
        }
    }

    if (g_ws_client.transport) {
        esp_transport_close(g_ws_client.transport);
    }
    
    if (g_ws_client.transport_list) {
        esp_transport_list_destroy(g_ws_client.transport_list);
        g_ws_client.transport_list = NULL;
        g_ws_client.transport = NULL;
    }
}

static void ping_timer_callback(void *arg) {
    ESP_LOGD(TAG, "Ping timer triggered - sending WebSocket PING frame");
    // 发送WebSocket层面的PING帧 (4字节随机数据)
    uint8_t ping_data[4] = {0x12, 0x34, 0x56, 0x78};
    enqueue_send_message(MCP_WS_MSG_TYPE_PING, (char*)ping_data, 4);
    esp_timer_start_once(g_ws_client.ping_timer, g_ws_client.config.ping_interval_ms * 1000);
}

// 主状态机任务
static void websocket_main_task(void *pvParameters) {
    ESP_LOGI(TAG, "WebSocket main task started");
    
    char *recv_buffer = malloc(MCP_WS_MAX_MESSAGE_LEN);
    if (!recv_buffer) {
        ESP_LOGE(TAG, "Failed to allocate receive buffer");
        set_state(MCP_WS_STATE_ERROR);
        vTaskDelete(NULL);
        return;
    }
    
    mcp_ws_send_msg_t *send_msg = NULL;
    
    while (!g_ws_client.should_stop) {
        
        // 状态机处理
        switch (g_ws_client.state) {
            
            case MCP_WS_STATE_IDLE:
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
                
            case MCP_WS_STATE_INITIALIZING:
                ESP_LOGI(TAG, "Initializing transport...");
                if (create_transport_list() == ESP_OK) {
                    set_state(MCP_WS_STATE_CONNECTING);
                } else {
                    set_state(MCP_WS_STATE_ERROR);
                }
                break;
                
            case MCP_WS_STATE_CONNECTING:
                ESP_LOGI(TAG, "Connecting to %s://%s:%d%s", 
                         g_ws_client.use_ssl ? "wss" : "ws",
                         g_ws_client.host, g_ws_client.port, g_ws_client.path);
                
                // 显示完整的连接URL用于调试
                ESP_LOGI(TAG, "Full connection URL would be: %s://%s:%d%s", 
                         g_ws_client.use_ssl ? "wss" : "ws",
                         g_ws_client.host, g_ws_client.port, g_ws_client.path);
                
                // 启用更详细的WebSocket调试日志
                esp_log_level_set("transport_ws", ESP_LOG_DEBUG);
                esp_log_level_set("transport", ESP_LOG_DEBUG);
                esp_log_level_set("transport_base", ESP_LOG_DEBUG);
                esp_log_level_set("HTTP_CLIENT", ESP_LOG_DEBUG);
                esp_log_level_set("esp-tls", ESP_LOG_DEBUG);
                
                // 添加连接前的调试信息
                ESP_LOGI(TAG, "About to connect with WebSocket path: %s", g_ws_client.path);
                ESP_LOGI(TAG, "Host: %s, Port: %d, SSL: %s", 
                        g_ws_client.host, g_ws_client.port, g_ws_client.use_ssl ? "yes" : "no");
                
                int result = esp_transport_connect(g_ws_client.transport, g_ws_client.host, 
                                                 g_ws_client.port, 15000);  // 增加超时时间
                ESP_LOGI(TAG, "Transport connect result: %d", result);
                if (result < 0) {
                    ESP_LOGE(TAG, "Failed to connect: %d (errno: %d)", result, errno);
                    // 恢复正常日志级别
                    esp_log_level_set("transport_ws", ESP_LOG_INFO);
                    esp_log_level_set("transport", ESP_LOG_INFO);
                    set_state(MCP_WS_STATE_DISCONNECTED);
                } else {
                    // 检查握手状态
                    int status = esp_transport_ws_get_upgrade_request_status(g_ws_client.transport);
                    ESP_LOGI(TAG, "WebSocket handshake status: %d", status);
                    
                    // 恢复正常日志级别
                    esp_log_level_set("transport_ws", ESP_LOG_INFO);
                    esp_log_level_set("transport", ESP_LOG_INFO);
                    
                    if (status == 101) {
                        ESP_LOGI(TAG, "WebSocket connected successfully");
                        set_state(MCP_WS_STATE_CONNECTED);
                        g_ws_client.reconnect_count = 0;
                        trigger_event(MCP_WS_EVENT_CONNECTED, NULL, 0, ESP_OK);
                        
                        
                        // 启动ping定时器
                        if (g_ws_client.ping_timer) {
                            esp_timer_start_once(g_ws_client.ping_timer, 
                                                   5000 * 1000);
                        }
                        
                    } else {
                        ESP_LOGE(TAG, "WebSocket handshake failed: status %d", status);
                        if (status == 400) {
                            ESP_LOGE(TAG, "Bad Request - check URL and token parameters");
                        } else if (status == 401) {
                            ESP_LOGE(TAG, "Unauthorized - invalid token");
                        } else if (status == 1002) {
                            ESP_LOGE(TAG, "Protocol error 1002 - WebSocket protocol violation");
                        } else if (status == -1) {
                            ESP_LOGE(TAG, "HTTP parsing failed - check server response format");
                        }
                        set_state(MCP_WS_STATE_DISCONNECTED);
                    }
                }
                break;
                
            case MCP_WS_STATE_CONNECTED:
                // 处理发送队列
                while (xQueueReceive(g_ws_client.send_queue, &send_msg, 0) == pdTRUE) {
                    if (g_ws_client.transport && send_msg) {
                        int opcode;
                        switch (send_msg->type) {
                            case MCP_WS_MSG_TYPE_TEXT:
                                opcode = WS_TRANSPORT_OPCODES_TEXT;
                                break;
                            case MCP_WS_MSG_TYPE_PING:
                                opcode = WS_TRANSPORT_OPCODES_PING;
                                break;
                            case MCP_WS_MSG_TYPE_PONG:
                                opcode = WS_TRANSPORT_OPCODES_PONG;
                                break;
                            case MCP_WS_MSG_TYPE_CLOSE:
                                opcode = WS_TRANSPORT_OPCODES_CLOSE;
                                break;
                            default:
                                opcode = WS_TRANSPORT_OPCODES_TEXT;
                                break;
                        }
                        
                        ESP_LOGD(TAG, "Sending WebSocket message, type: %d, size: %d", 
                                send_msg->type, (int)send_msg->data_len);
                        
                        int sent = esp_transport_ws_send_raw(g_ws_client.transport, opcode|WS_TRANSPORT_OPCODES_FIN,
                                                           send_msg->data, send_msg->data_len, 1000);
                        if (sent < 0) {
                            ESP_LOGW(TAG, "Failed to send message: %d", sent);
                            set_state(MCP_WS_STATE_DISCONNECTED);
                        } else {
                            g_ws_client.sent_messages++;
                            if (send_msg->type == MCP_WS_MSG_TYPE_TEXT) {
                                trigger_event(MCP_WS_EVENT_MESSAGE_SENT, send_msg->data, send_msg->data_len, ESP_OK);
                            }
                        }
                        
                        free_send_message(send_msg);
                        send_msg = NULL;
                    }
                }
                
                // 处理接收数据
                if (g_ws_client.transport) {
                    int len = esp_transport_read(g_ws_client.transport, recv_buffer, 
                                               MCP_WS_MAX_MESSAGE_LEN - 1, 500);
                    if (len > 0) {
                        recv_buffer[len] = '\0';
                        
                        ws_transport_opcodes_t opcode = esp_transport_ws_get_read_opcode(g_ws_client.transport);
                        
                        switch (opcode) {
                            case WS_TRANSPORT_OPCODES_TEXT:
                                ESP_LOGI(TAG, "Received text: %.*s", len, recv_buffer);
                                g_ws_client.received_messages++;
                                trigger_event(MCP_WS_EVENT_MESSAGE_RECEIVED, recv_buffer, len, ESP_OK);
                                break;
                                
                            case WS_TRANSPORT_OPCODES_BINARY:
                                ESP_LOGI(TAG, "Received binary: %d bytes", len);
                                g_ws_client.received_messages++;
                                trigger_event(MCP_WS_EVENT_MESSAGE_RECEIVED, recv_buffer, len, ESP_OK);
                                break;
                                
                            case WS_TRANSPORT_OPCODES_CLOSE:
                                ESP_LOGI(TAG, "Received close frame");
                                if (len >= 2) {
                                    uint16_t close_code = (recv_buffer[0] << 8) | recv_buffer[1];
                                    ESP_LOGW(TAG, "WebSocket close code: %d", close_code);
                                }
                                set_state(MCP_WS_STATE_DISCONNECTED);
                                break;
                                
                            case WS_TRANSPORT_OPCODES_PING:
                                ESP_LOGD(TAG, "Received ping, sending pong");
                                enqueue_send_message(MCP_WS_MSG_TYPE_PONG, recv_buffer, len);
                                break;
                                
                            case WS_TRANSPORT_OPCODES_PONG:
                                ESP_LOGD(TAG, "Received pong");
                                break;
                                
                            default:
                                ESP_LOGW(TAG, "Unknown opcode: 0x%02X", opcode);
                                break;
                        }
                    } else if (len < 0 && len != ESP_ERR_TIMEOUT) {
                        ESP_LOGW(TAG, "Read error: %d", len);
                        set_state(MCP_WS_STATE_DISCONNECTED);
                    }
                }
                
                vTaskDelay(pdMS_TO_TICKS(10));
                break;
                
            case MCP_WS_STATE_DISCONNECTED:
                // 停止ping定时器
                if (g_ws_client.ping_timer) {
                    esp_timer_stop(g_ws_client.ping_timer);
                }

                
                cleanup_transport();
                trigger_event(MCP_WS_EVENT_DISCONNECTED, NULL, 0, ESP_OK);
                
                // 检查是否需要重连
                ESP_LOGI(TAG, "WebSocket disconnected, auto_reconnect_enabled: %d", g_ws_client.auto_reconnect_enabled);
                if (g_ws_client.auto_reconnect_enabled && !g_ws_client.should_stop) {
                    g_ws_client.reconnect_count++;
                    
                    set_state(MCP_WS_STATE_RECONNECTING);
                } else {
                    set_state(MCP_WS_STATE_IDLE);
                }
                break;
                
            case MCP_WS_STATE_RECONNECTING:
                uint32_t delay_ms = g_ws_client.config.reconnect_delay_ms;
                if (g_ws_client.reconnect_count > 3) {
                    delay_ms *= (1 << (g_ws_client.reconnect_count - 3));
                    if (delay_ms > 60000) delay_ms = 60000; // 最大60秒
                }
                ESP_LOGI(TAG, "Reconnecting in %d ms...", delay_ms);
                vTaskDelay(pdMS_TO_TICKS(delay_ms));
                set_state(MCP_WS_STATE_INITIALIZING);
                break;
                
            case MCP_WS_STATE_ERROR:
                ESP_LOGE(TAG, "WebSocket in error state");
                cleanup_transport();
                trigger_event(MCP_WS_EVENT_ERROR, NULL, 0, ESP_FAIL);
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
                
            default:
                ESP_LOGW(TAG, "Unknown state: %d", g_ws_client.state);
                set_state(MCP_WS_STATE_ERROR);
                break;
        }
    }
    
    // 清理发送队列
    while (xQueueReceive(g_ws_client.send_queue, &send_msg, 0) == pdTRUE) {
        free_send_message(send_msg);
    }
    
    free(recv_buffer);
    ESP_LOGI(TAG, "WebSocket main task ended");
    g_ws_client.main_task = NULL;
    vTaskDelete(NULL);
}

// 公共API实现
esp_err_t mcp_websocket_init(const mcp_ws_config_t *config) {
    if (!config || strlen(config->endpoint) == 0) {
        ESP_LOGE(TAG, "Invalid WebSocket configuration");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (g_ws_client.initialized) {
        ESP_LOGW(TAG, "WebSocket client already initialized");
        return ESP_OK;
    }
    
    // 复制配置
    memcpy(&g_ws_client.config, config, sizeof(mcp_ws_config_t));
    g_ws_client.event_callback = config->event_callback;
    g_ws_client.auto_reconnect_enabled = config->auto_reconnect;
    
    // 设置默认值
    if (g_ws_client.config.reconnect_delay_ms == 0) {
        g_ws_client.config.reconnect_delay_ms = MCP_WS_RECONNECT_DELAY_MS;
    }
    if (g_ws_client.config.ping_interval_ms == 0) {
        g_ws_client.config.ping_interval_ms = MCP_WS_PING_INTERVAL_MS;
    }
    
    // 解析 endpoint
    esp_err_t ret = parse_url(g_ws_client.config.endpoint);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse WebSocket endpoint");
        return ret;
    }
    
    // 创建发送队列
    g_ws_client.send_queue = xQueueCreate(MCP_WS_SEND_QUEUE_SIZE, sizeof(mcp_ws_send_msg_t*));
    if (!g_ws_client.send_queue) {
        ESP_LOGE(TAG, "Failed to create send queue");
        return ESP_ERR_NO_MEM;
    }
    
    // 创建ping定时器
    esp_timer_create_args_t ping_timer_args = {
        .callback = ping_timer_callback,
        .name = "ws_ping"
    };
    ret = esp_timer_create(&ping_timer_args, &g_ws_client.ping_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ping timer");
        vQueueDelete(g_ws_client.send_queue);
        return ret;
    }
    
    set_state(MCP_WS_STATE_IDLE);
    g_ws_client.initialized = true;
    
    ESP_LOGI(TAG, "WebSocket client initialized for %s://%s:%d%s", 
             g_ws_client.use_ssl ? "wss" : "ws",
             g_ws_client.host, g_ws_client.port, g_ws_client.path);
    
    return ESP_OK;
}

esp_err_t mcp_websocket_start(void) {
    if (!g_ws_client.initialized) {
        ESP_LOGE(TAG, "WebSocket client not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (g_ws_client.main_task) {
        ESP_LOGW(TAG, "WebSocket task already running");
        return ESP_OK;
    }
    
    g_ws_client.should_stop = false;
    g_ws_client.auto_reconnect_enabled = g_ws_client.config.auto_reconnect;
    
    // 启动主任务
    xTaskCreate(websocket_main_task, "ws_main", 4096, NULL, 5, &g_ws_client.main_task);
    if (!g_ws_client.main_task) {
        ESP_LOGE(TAG, "Failed to create WebSocket main task");
        return ESP_ERR_NO_MEM;
    }
    
    // 开始连接
    set_state(MCP_WS_STATE_INITIALIZING);
    
    ESP_LOGI(TAG, "WebSocket client started");
    return ESP_OK;
}

esp_err_t mcp_websocket_stop(void) {
    if (!g_ws_client.initialized) {
        return ESP_OK;
    }
    
    g_ws_client.should_stop = true;
    g_ws_client.auto_reconnect_enabled = false;
    
    // 停止定时器
    if (g_ws_client.ping_timer) {
        esp_timer_stop(g_ws_client.ping_timer);
    }
    
    // 发送关闭消息
    if (g_ws_client.state == MCP_WS_STATE_CONNECTED) {
        enqueue_send_message(MCP_WS_MSG_TYPE_CLOSE, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(100)); // 等待发送
    }
    
    set_state(MCP_WS_STATE_DISCONNECTED);
    
    // 等待任务结束
    int timeout_count = 0;
    while (g_ws_client.main_task && timeout_count < 50) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout_count++;
    }
    
    if (g_ws_client.main_task) {
        ESP_LOGW(TAG, "Force deleting main task");
        vTaskDelete(g_ws_client.main_task);
        g_ws_client.main_task = NULL;
    }
    
    cleanup_transport();
    
    ESP_LOGI(TAG, "WebSocket client stopped");
    return ESP_OK;
}

esp_err_t mcp_websocket_send_text(const char *message) {
    if (!message) {
        return ESP_ERR_INVALID_ARG;
    }
    return enqueue_send_message(MCP_WS_MSG_TYPE_TEXT, message, strlen(message));
}

esp_err_t mcp_websocket_send(const char *data, size_t len) {
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return enqueue_send_message(MCP_WS_MSG_TYPE_TEXT, data, len);
}

mcp_ws_state_t mcp_websocket_get_state(void) {
    return g_ws_client.state;
}

bool mcp_websocket_is_connected(void) {
    return g_ws_client.state == MCP_WS_STATE_CONNECTED;
}

void mcp_websocket_set_callback(mcp_ws_event_callback_t callback) {
    g_ws_client.event_callback = callback;
}

void mcp_websocket_get_stats(uint32_t *sent_messages, uint32_t *received_messages, uint32_t *reconnect_count) {
    if (sent_messages) *sent_messages = g_ws_client.sent_messages;
    if (received_messages) *received_messages = g_ws_client.received_messages;
    if (reconnect_count) *reconnect_count = g_ws_client.reconnect_count;
}

esp_err_t mcp_websocket_deinit(void) {
    if (!g_ws_client.initialized) {
        return ESP_OK;
    }
    
    // 停止客户端
    mcp_websocket_stop();
    
    // 删除定时器
    if (g_ws_client.ping_timer) {
        esp_timer_delete(g_ws_client.ping_timer);
        g_ws_client.ping_timer = NULL;
    }
    
    // 删除队列
    if (g_ws_client.send_queue) {
        vQueueDelete(g_ws_client.send_queue);
        g_ws_client.send_queue = NULL;
    }
    
    // 释放动态分配的内存
    if (g_ws_client.host) {
        free(g_ws_client.host);
        g_ws_client.host = NULL;
    }
    if (g_ws_client.path) {
        free(g_ws_client.path);
        g_ws_client.path = NULL;
    }
    
    // 清零状态
    memset(&g_ws_client, 0, sizeof(g_ws_client));
    
    ESP_LOGI(TAG, "WebSocket client deinitialized");
    return ESP_OK;
}
