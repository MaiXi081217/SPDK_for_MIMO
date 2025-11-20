# MIMO Go 通知桥接

一个简单的通知框架，允许在 MIMO C 代码中通过一行函数调用，向本地 Web 后端发送 JSON 事件通知。

## 快速开始

### 1. 在 C 代码中使用

只需包含头文件并调用一个函数：

```c
#include "spdk_go_notify.h"

// 在任意位置调用，发送事件通知
NotifyEvent("bdev_ready", "{\"name\":\"nvme0n1\",\"size\":1073741824}");

// 也可以不传 payload（传 NULL）
NotifyEvent("error_occurred", NULL);
```

**就这么简单！** 无需初始化，自动异步发送，不阻塞。

### 2. 构建和运行

#### 构建 SPDK（自动构建共享库）

```bash
cd /home/max/SPDK_for_MIMO
make -C app/spdk_tgt
```

构建过程会自动：
- 编译 Go 共享库 `libspdk_go_notify.so`
- 链接到 `mimo_tgt` 可执行文件

#### 运行 mimo_tgt

```bash
./build/bin/mimo_tgt
```

启动时会自动发送 `mimo_tgt_started` 事件。

### 3. 接收通知（测试用）

启动一个简单的 HTTP 服务器接收通知：

**Python 示例：**
```bash
python3 -c "
from http.server import HTTPServer, BaseHTTPRequestHandler
import json

class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers['Content-Length'])
        body = self.rfile.read(length)
        data = json.loads(body.decode())
        print('收到通知:')
        print(json.dumps(data, indent=2, ensure_ascii=False))
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b'{\"status\":\"ok\"}')

httpd = HTTPServer(('127.0.0.1', 9988), Handler)
print('监听 http://127.0.0.1:9988/mimo/events')
httpd.serve_forever()
"
```

**或者使用 Flask：**
```python
from flask import Flask, request
app = Flask(__name__)

@app.post("/mimo/events")
def receive():
    data = request.json
    print(f"收到通知: {data}")
    return {"status": "ok"}

if __name__ == "__main__":
    app.run(host="127.0.0.1", port=9988)
```

## JSON 格式说明

发送的 JSON 格式如下：

```json
{
  "event": "事件名称",
  "payload": {
    "自定义字段1": "值1",
    "自定义字段2": "值2"
  },
  "timestamp": 1734603290
}
```

**字段说明：**
- `event`: 事件名称（字符串，必填）
- `payload`: 自定义数据（JSON 对象，可选）
- `timestamp`: Unix 时间戳（整数，秒级，自动生成）

**示例：**
```json
{
  "event": "mimo_tgt_started",
  "payload": {
    "pid": 993227,
    "name": "mimo_tgt"
  },
  "timestamp": 1734603290
}
```

## 配置

### 默认配置

如果未找到配置文件，将使用以下默认配置：
- **接收地址**: `http://127.0.0.1:9988/mimo/events`
- **请求方法**: `POST`
- **超时时间**: `2秒`
- **重试次数**: `2次`（带指数退避）
- **重试退避**: `100毫秒`（指数增长）

### 通过 JSON 文件配置

所有配置项都可以通过 JSON 配置文件进行设置，无需修改代码。

#### 配置文件查找顺序

系统会按以下顺序查找配置文件：

1. **环境变量** `MIMO_NOTIFY_CONFIG` 指定的路径
2. `/etc/mimo/notify_config.json`（系统级配置）
3. `/usr/local/etc/mimo/notify_config.json`（本地系统配置）
4. `./notify_config.json`（当前目录）
5. `./config/notify_config.json`（config 子目录）

如果找到配置文件，将使用配置文件中的设置；如果未找到，则使用默认配置。

#### 配置文件格式

创建配置文件（例如 `/etc/mimo/notify_config.json`）：

```json
{
  "endpoint": "http://127.0.0.1:9988/mimo/events",
  "method": "POST",
  "timeout": "2s",
  "source": "mimo",
  "retry": 2,
  "retry_backoff_ms": 100
}
```

**配置字段说明：**

- `endpoint` (string): 接收通知的 HTTP 端点 URL
- `method` (string): HTTP 请求方法，默认为 "POST"
- `timeout` (string): 请求超时时间，支持 Go duration 格式（如 "2s", "500ms", "1m"）
- `source` (string): 事件源标识（可选，用于日志）
- `retry` (int): 失败重试次数，默认为 2
- `retry_backoff_ms` (int): 重试退避时间（毫秒），每次重试会指数增长

#### 使用示例

**方法 1：通过环境变量指定配置文件**

```bash
export MIMO_NOTIFY_CONFIG=/path/to/notify_config.json
./build/bin/mimo_tgt
```

**方法 2：使用系统级配置文件**

```bash
# 创建配置文件
sudo mkdir -p /etc/mimo
sudo cp go/notifybridge/config.example.json /etc/mimo/notify_config.json

# 编辑配置（可选）
sudo nano /etc/mimo/notify_config.json

# 运行应用（自动读取配置）
./build/bin/mimo_tgt
```

**方法 3：使用本地配置文件**

```bash
# 在当前目录创建配置文件
cp go/notifybridge/config.example.json ./notify_config.json

# 编辑配置
nano notify_config.json

# 运行应用
./build/bin/mimo_tgt
```

#### 配置示例

**自定义端口和路径：**

```json
{
  "endpoint": "http://192.168.1.100:8080/api/notifications",
  "method": "POST",
  "timeout": "5s",
  "retry": 3,
  "retry_backoff_ms": 200
}
```

**增加超时时间和重试次数：**

```json
{
  "endpoint": "http://127.0.0.1:9988/mimo/events",
  "timeout": "10s",
  "retry": 5,
  "retry_backoff_ms": 500
}
```

## 在 MIMO 代码中添加通知

### 示例 1：在 bdev 创建时通知

```c
#include "spdk_go_notify.h"

static void
bdev_create_callback(struct spdk_bdev *bdev)
{
    char payload[256];
    snprintf(payload, sizeof(payload), 
             "{\"name\":\"%s\",\"size\":%lu}", 
             spdk_bdev_get_name(bdev),
             spdk_bdev_get_num_blocks(bdev) * spdk_bdev_get_block_size(bdev));
    
    NotifyEvent("bdev_created", payload);
}
```

### 示例 2：在错误发生时通知

```c
#include "spdk_go_notify.h"

static void
handle_error(const char *error_msg)
{
    char payload[512];
    snprintf(payload, sizeof(payload), "{\"error\":\"%s\"}", error_msg);
    NotifyEvent("error_occurred", payload);
}
```

### 示例 3：简单事件（无 payload）

```c
#include "spdk_go_notify.h"

// 发送简单事件，不需要额外数据
NotifyEvent("system_ready", NULL);
```

## 特性

-  **零配置**：首次调用自动初始化，无需手动设置
-  **JSON 配置**：支持通过配置文件灵活配置所有参数
-  **自动查找**：自动在多个位置查找配置文件
-  **环境变量**：支持通过环境变量指定配置文件路径
-  **异步发送**：不阻塞调用线程
-  **自动重试**：网络失败时自动重试（带指数退避）
-  **线程安全**：可在多线程环境安全使用
-  **简单易用**：只需一行代码

## 目录结构

```
SPDK_for_MIMO/
├── go/notifybridge/
│   ├── bridge.go              # Go 实现
│   ├── go.mod
│   ├── config.example.json    # 配置文件示例
│   └── README.md
├── include/
│   └── spdk_go_notify.h       # C 头文件
└── app/spdk_tgt/
    ├── mimo_tgt.c             # 已集成示例
    └── Makefile               # 已配置构建
```

## 故障排查

### 通知未收到

1. **检查 Web 服务器是否运行**
   ```bash
   curl http://127.0.0.1:9988/mimo/events
   ```

2. **检查日志**
   - 查看 MIMO 日志中是否有 "Failed to send startup notification"
   - 检查 Web 服务器日志

3. **检查配置文件**
   - 确认配置文件路径正确
   - 验证 JSON 格式是否正确
   ```bash
   # 验证 JSON 格式
   python3 -m json.tool /etc/mimo/notify_config.json
   ```

4. **检查防火墙**
   - 确保 9988 端口未被阻止

### 编译错误

1. **确保 Go 已安装**
   ```bash
   go version
   ```

2. **确保共享库已构建**
   ```bash
   ls -la build/lib/libspdk_go_notify.so
   ```

3. **重新构建**
   ```bash
   make clean -C app/spdk_tgt
   make -C app/spdk_tgt
   ```

## 技术细节

- 使用 Go 的 `c-shared` 构建模式生成共享库
- 通过 CGO 导出 C 接口
- HTTP 客户端使用 Go 标准库 `net/http`
- 异步发送使用 goroutine，不阻塞 C 调用线程
