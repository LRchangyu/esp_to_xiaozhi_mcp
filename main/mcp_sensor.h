#ifndef _MCP_SENSOR_H_
#define _MCP_SENSOR_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化传感器模拟器
 * @return 0 on success, -1 on failure
 */
int mcp_sensor_init(void);

/**
 * @brief 启动传感器数据采集任务
 * @return 0 on success, -1 on failure
 */
int mcp_sensor_start(void);

/**
 * @brief 停止传感器数据采集任务
 * @return 0 on success, -1 on failure
 */
int mcp_sensor_stop(void);

/**
 * @brief 获取当前温度值
 * @return Temperature in Celsius
 */
float mcp_sensor_get_temperature(void);

/**
 * @brief 获取当前湿度值
 * @return Humidity in percentage
 */
float mcp_sensor_get_humidity(void);

/**
 * @brief 设置传感器更新回调函数
 * @param callback 回调函数，当传感器数据更新时调用
 */
void mcp_sensor_set_callback(int (*callback)(float temperature, float humidity));

#ifdef __cplusplus
}
#endif

#endif /* _MCP_SENSOR_H_ */
