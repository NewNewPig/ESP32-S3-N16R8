# ESP32-S3-N16R8 Wi-Fi TCP AT 固件

这是一个基于 ESP-IDF 的 ESP32-S3-N16R8 AT 指令验证固件，使用 UART0 接收 AT 指令，支持 Wi-Fi Station、TCP 客户端和单客户端 TCP 服务端模式。

## 1. 固件信息

- 芯片：ESP32-S3
- Flash：N16R8 配置
- UART：UART0
- 默认波特率：115200
- 数据位：8
- 停止位：1
- 校验位：None
- 流控：None
- 默认 LED：GPIO2
- 固件版本：1.3.0
- 默认 AT 行结束符：回车或换行，建议发送 CRLF

## 2. 编译和烧录

在工程根目录执行：

```text
idf.py build
idf.py -p COM11 flash monitor
```

`COM11` 需要替换成实际的 ESP32-S3 下载串口。如果烧录失败，关闭串口监视器后重试；必要时按住 BOOT，按一下 RESET/EN，再释放 BOOT 后烧录。

构建成功时应看到：

```text
Successfully created ESP32-S3 image.
```

## 3. 串口连接

使用串口监视器或串口工具连接正确的 COM 口，配置为：

```text
115200, 8 data bits, 1 stop bit, no parity, no flow control
```

固件启动后会输出：

```text
ESP32-S3-N16R8 WIFI TCP AT READY
OK
```

每条 AT 指令发送后按回车。建议关闭串口回显，避免测试工具重复显示输入内容：

```text
ATE0
```

## 4. 基础 AT 指令

| 指令 | 功能 | 典型返回 |
| --- | --- | --- |
| `AT` | 检查模块是否在线 | `OK` |
| `ATI` | 查看设备信息 | 设备信息、`OK` |
| `AT+GMR` | 查看固件信息 | 设备信息、`OK` |
| `AT+VERSION` | 查看固件版本 | `1.3.0`、`OK` |
| `AT+INFO` | 查看固件描述 | 描述、`OK` |
| `AT+RST` | 重启模块 | `RESETTING` |
| `ATE0` | 关闭回显兼容命令 | `OK` |
| `ATE1` | 开启回显兼容命令 | `OK` |

基础验证：

```text
AT
ATI
AT+VERSION
AT+INFO
```

## 5. LED 指令

GPIO2 默认配置为输出：

```text
AT+LED=ON
AT+LED?
AT+LED=OFF
```

预期返回：

```text
OK
ON
OK
OK
```

如果开发板板载 LED 不在 GPIO2，需要修改 `main/main.c` 中的 `LED_PIN`。

## 6. Wi-Fi 指令

### 6.1 查询和设置工作模式

```text
AT+CWMODE?
AT+CWMODE=1
```

当前实际初始化为 Station 模式。`AT+CWMODE=1/2/3` 会返回 `OK`，但当前固件的 Wi-Fi 初始化仍以 Station 功能为主。

### 6.2 扫描热点

```text
AT+CWLAP
```

典型返回格式：

```text
SSID,RSSI,CHANNEL,AUTHMODE
OK
```

示例：

```text
HomeWifi,-45,6,3
OK
```

### 6.3 连接热点

使用真实的 Wi-Fi 名称和密码：

```text
AT+CWJAP="实际SSID","实际密码"
```

例如：

```text
AT+CWJAP="HomeWifi","Abc12345"
```

预期先返回：

```text
WIFI CONNECTING
OK
```

等待几秒后查询：

```text
AT+CWJAP?
```

连接成功：

```text
HomeWifi
OK
```

连接尚未完成：

```text
WIFI CONNECTING
OK
```

连接失败或尚未连接：

```text
NO AP
OK
```

注意：SSID 和密码区分大小写，当前固件会保留引号内参数的大小写。ESP32-S3 通常只能连接 2.4 GHz Wi-Fi，不能连接 5 GHz-only 热点。

### 6.4 断开热点

```text
AT+CWQAP
```

返回：

```text
OK
```

## 7. TCP 客户端模式

### 7.1 连接远程 TCP 服务端

先连接 Wi-Fi，然后发送：

```text
AT+CIPMUX=0
AT+CIPMODE=0
AT+CIPSTART="TCP","192.168.1.100",8080
```

将 IP 和端口替换成真实 TCP 服务端地址。成功返回：

```text
OK
```

### 7.2 查询 TCP 状态

```text
AT+CIPSTATUS
```

连接成功：

```text
STATUS:TCP_CONNECTED
OK
```

未连接：

```text
STATUS:TCP_DISCONNECTED
OK
```

### 7.3 发送 TCP 数据

发送 5 个字节：

```text
AT+CIPSEND=5
```

收到提示后：

```text
>
```

输入正好 5 个字符：

```text
hello
```

成功返回：

```text
OK
```

`AT+CIPSEND=N` 后，后续 N 个非换行字符会作为 TCP 数据发送。N 必须和实际发送的字节数一致，建议先使用 ASCII 数据。

### 7.4 接收 TCP 数据

当远端发送数据时，串口输出类似：

```text
+IPD,5:world
```

其中 `5` 是收到的数据长度。

### 7.5 关闭 TCP 连接

```text
AT+CIPCLOSE
```

返回：

```text
OK
```

## 8. TCP 服务端模式

当前服务端为单客户端模式。

### 8.1 启动服务端

ESP32 连接 Wi-Fi 后执行：

```text
AT+CIPMUX=0
AT+CIPMODE=0
AT+CIPSERVER=1,8080
```

成功启动监听后返回：

```text
OK
```

服务端监听 ESP32 当前 IP 的 TCP 8080 端口。

### 8.2 获取 ESP32 IP 地址

当前版本没有实现 `AT+CIFSR`。可以从路由器管理页面查看 ESP32 的 DHCP 地址，或者使用网络扫描工具查找设备。

### 8.3 使用 PowerShell 连接 ESP32

将 IP 改成 ESP32 的实际 IP：

```powershell
$client = New-Object System.Net.Sockets.TcpClient
$client.Connect("192.168.1.120", 8080)
$stream = $client.GetStream()
```

ESP32 串口会输出：

```text
CONNECT
```

### 8.4 PowerShell 向 ESP32 发送数据

```powershell
$data = [Text.Encoding]::ASCII.GetBytes("hello")
$stream.Write($data, 0, $data.Length)
```

ESP32 串口应收到：

```text
+IPD,5:hello
```

### 8.5 ESP32 向 PowerShell 发送数据

ESP32 串口发送：

```text
AT+CIPSEND=5
```

看到：

```text
>
```

然后发送：

```text
world
```

PowerShell 读取：

```powershell
$buffer = New-Object byte[] 128
$count = $stream.Read($buffer, 0, $buffer.Length)
[Text.Encoding]::ASCII.GetString($buffer, 0, $count)
```

预期输出：

```text
world
```

ESP32 串口随后返回：

```text
OK
```

### 8.6 关闭服务端连接

ESP32 串口执行：

```text
AT+CIPCLOSE
```

PowerShell 端执行：

```powershell
$client.Close()
```

## 9. 推荐的完整验证流程

按以下顺序执行：

```text
AT
ATE0
AT+CWMODE?
AT+CWLAP
AT+CWJAP="实际SSID","实际密码"
AT+CWJAP?
AT+CIPSTATUS
AT+CIPMUX=0
AT+CIPMODE=0
AT+CIPSERVER=1,8080
```

然后使用电脑 TCP 客户端连接 ESP32 的 IP 和 8080 端口，验证双向收发：

1. 电脑发送 `hello`，串口应出现 `+IPD,5:hello`。
2. 串口发送 `AT+CIPSEND=5`。
3. 收到 `>` 后输入 `world`。
4. 电脑应收到 `world`。
5. 串口发送 `AT+CIPSTATUS`，确认状态仍为 `STATUS:TCP_CONNECTED`。
6. 串口发送 `AT+CIPCLOSE`，关闭连接。

## 10. 常见问题

### 返回 `NO AP`

- 检查 SSID 和密码是否完全正确。
- 检查大小写，SSID 和密码不能写成占位符。
- 确认热点是 2.4 GHz。
- 先执行 `AT+CWLAP`，确认能扫描到目标热点。
- 重新烧录当前最新固件。

### `AT+CIPSEND` 后没有反应

- 确认 TCP 状态为 `STATUS:TCP_CONNECTED`。
- 确认 `AT+CIPSEND=N` 中 N 大于 0 且小于 512。
- 收到 `>` 后再输入数据。
- 后续数据长度必须正好为 N 个字节。
- 不要把下一条 AT 指令直接当成发送数据。

### 能收到数据，但 AT 指令无响应

- 确认已经烧录包含非阻塞 TCP socket 修复的最新固件。
- 重新执行 `AT+CIPCLOSE` 后再测试 `AT`。
- 检查串口监视器是否连接在正确 COM 口。
- 确认没有同时打开多个占用同一串口的监视器。

### 烧录失败

- 关闭串口监视器。
- 确认下载端口，例如 `COM11`。
- 按住 BOOT，按一下 RESET/EN，释放 RESET，再释放 BOOT。
- 执行：

```text
idf.py -p COM11 flash
```

## 11. 当前版本限制

- TCP 服务端只支持一个客户端连接。
- 当前 `AT+CIPSTART` 实际按 TCP socket 工作，UDP 仅保留命令格式兼容，不应作为 UDP 功能使用。
- 当前未实现 `AT+CIFSR`，ESP32 IP 需要从路由器或其他网络工具获取。
- 当前不是完整的 Espressif 官方 AT 固件，命令集和返回格式是本项目的简化实现。
