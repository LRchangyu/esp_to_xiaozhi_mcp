# 前言基础

小智 AI
- https://github.com/78/xiaozhi-esp32

MCP 参考说明例程
- https://github.com/78/mcp-calculator

MCP 接入 https://xiaozhi.me/ 教程
- https://ccnphfhqs21z.feishu.cn/wiki/HiPEwZ37XiitnwktX13cEM5KnSb

> MCP（Model Context Protocol）是一种新兴的开放协议，旨在为 AI 模型（尤其是大语言模型 LLM）提供统一的上下文接入方式。你可以把它理解为 AI 应用的“USB-C 接口”：让模型可以标准化地连接各种工具、数据源和服务。

# 说明

如果你不想去看和修改小智 AI 的源码，又想玩一下语音加大模型控制自己的硬件设备，那可以试一下接下来介绍的这一种方式。

使用自己的 esp32 硬件实现 MCP ，接入到小智 AI 。

就可以实现：
`你好，小智！开灯`
`你好，小智！关灯`
`你好，小智！开风扇`

等语音指令，控制自己的硬件。

需要买一个标准的小智 AI 硬件，作为语音控制的中心即可。

大概这样子：

<img width="1004" height="555" alt="image" src="https://github.com/user-attachments/assets/306051af-6cae-4a6f-80a2-ef50013666cb" />


# 使用说明

## 前置工作

按照小智 AI 的使用说明：
- 准备好硬件，并烧录好固件、配网等前置工作。
- https://xiaozhi.me/ 注册账号并添加好设备。

> 没有小智 AI 硬件的，可以尝试电脑端模拟小智 AI：https://github.com/huangjunsen0406/py-xiaozhi
本人就是使用电脑端 py-xiaozhi 测试。

## 编译代码

- github：
- SDK：ESP-IDF 5.5
- VS CODE
- 硬件：esp32c2/esp32c3 都可以，ESP8266 可以自己移植。

1、下好代码，建好工程

2、获取小智 AI 的 MCP 接入点：
https://xiaozhi.me/ -> 控制台 -> 智能体 -> 找你的设备 -> 配置角色 -> MCP 接入点 -> 复制

<img width="734" height="548" alt="image" src="https://github.com/user-attachments/assets/ff47fe97-c8d7-430f-8535-8725e6e2a53d" />


<img width="934" height="224" alt="image" src="https://github.com/user-attachments/assets/5b00b8e4-aae4-40d1-a631-75b8c982d016" />


<img width="816" height="399" alt="image" src="https://github.com/user-attachments/assets/fb75483e-86e0-4612-ac54-08a165677065" />


把内容替换 main/station_example_main.c 中的 MCP_ENDPOINT 内容

3、menu_config 配置：
Example Configuration 中的 WiFi SSID、WiFi Password

Component configc 的 ESP-TLS：
<img width="822" height="168" alt="image" src="https://github.com/user-attachments/assets/01efe72e-9058-4f3a-82c4-d833bb5c665c" />

4、构建项目、烧录、运行，可以查看到设备端的 log：
<img width="1531" height="731" alt="image" src="https://github.com/user-attachments/assets/ca47cf98-bcf2-43b9-ba66-11b34e6163e1" />


5、再次查看 https://xiaozhi.me/ -> 控制台 -> 智能体 -> 找你的设备 -> 配置角色 -> MCP 接入点，
可以看到：
<img width="901" height="300" alt="image" src="https://github.com/user-attachments/assets/7a78c1b1-efc6-4110-9f33-2b5eea240e9d" />


表示成功。

开启的小智 AI，发出指令"你好，小智，开灯"

设备端可以看到 log：
<img width="1529" height="236" alt="image" src="https://github.com/user-attachments/assets/497e090e-ca18-4382-85fd-335194103426" />


这就完成了：小智 AI 控制我们自己的硬件。

## 代码说明

可以参考 mcp_server.c 中的内容，自行添加功能。
