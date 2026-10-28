# 使用说明

## 1. 编译

要求：Linux/WSL2、GCC（支持 C11）、GNU Make、pthread。

```bash
make
```

生成：

```text
bin/chat_server
bin/chat_client
```

清理：

```bash
make clean
```

## 2. 启动服务器

```bash
./bin/chat_server [port]
```

- 不写端口时默认 `8888`。
- 服务器绑定 `0.0.0.0`，同一局域网内的机器可以连接。
- `Ctrl+C` 停止；服务器会关闭所有客户端连接。
- 最多 64 个并发在线客户端。

示例：

```bash
./bin/chat_server 8888
```

输出：

```text
Chat server listening on 0.0.0.0:8888
Protocol: CHAT/1, max clients: 64. Press Ctrl+C to stop.
[2026-10-28 20:19:40] 127.0.0.1:38358 joined as "Alice" (online=1)
```

## 3. 启动客户端

```bash
./bin/chat_client [-s|--show-seq] <host> <port> <nickname>
```

| 参数 | 说明 |
|---|---|
| `-s`, `--show-seq` | 在每条服务器消息前显示 `#序号`，用于观察消息连续性 |
| `host` | 服务器 IP 或主机名；本机用 `127.0.0.1` |
| `port` | 服务器端口 |
| `nickname` | 1–16 个 Unicode 字符，不能包含空格、`|`、控制字符；中文可用 |

示例：

```bash
./bin/chat_client 127.0.0.1 8888 Alice
./bin/chat_client 127.0.0.1 8888 Bob
./bin/chat_client -s 127.0.0.1 8888 小明
```

局域网内其他机器把 `127.0.0.1` 换成服务器 IP，例如：

```bash
./bin/chat_client 192.168.1.20 8888 Alice
```

## 4. 客户端命令

| 命令 | 作用 |
|---|---|
| `/help` | 显示帮助 |
| `/list` | 请求在线用户列表 |
| `/ping` | 发送应用层 PING，服务器回 PONG |
| `/quit`、`/exit` | 发送 QUIT 并退出 |
| 其他文本 | 作为聊天消息发送；支持中英文、Emoji 和 `|` |
| `Ctrl+D` | 输入 EOF，等效退出 |

单条消息最多 3000 个 UTF-8 字节。聊天消息由服务器加时间戳后广播给所有人（包括发送者）。

## 5. 使用示例

```text
=== 多用户聊天室 / Multi-user chat ===
server: 127.0.0.1:8888   nickname: Alice
[2026-10-28 20:19:40] #1 * 欢迎 Alice！输入 /help 查看命令。Welcome! Online users: 1

[2026-10-28 20:19:41] #3 * Bob 加入了聊天室（joined the chat）
[2026-10-28 20:19:44] #4 <Alice> 大家好，我是 Alice。Hello, this is Alice.
[2026-10-28 20:19:46] #5 <Bob> Hi Alice! 中英混合 mixed message 😀
[2026-10-28 20:19:48] #6 * 服务器响应 PONG (server is alive)
[2026-10-28 20:19:50] #7 * 在线用户（2）：Bob, Alice
```

## 6. 运行测试

```bash
make test
```

单独运行：

```bash
./build/protocol_test
python3 tests/integration_test.py
```

集成测试会使用 `19001` 端口，自动启动服务器和 3 个客户端，检查中英文广播、重复昵称拒绝、序号连续性等。日志在 `build/integration/`。

## 7. 抓包与丢包观察

需要 root（`CAP_NET_RAW`）：

```bash
sudo bash scripts/capture_test.sh
```

生成：

- `build/chat-test.pcap`
- `build/chat-test.tshark.txt`（TCP 会话、重传/丢包统计、包数）

模拟 15% 回环丢包：

```bash
sudo env CHAT_LOSS_PERCENT=15 \
  CHAT_PCAP="$PWD/build/chat-loss15.pcap" \
  bash scripts/capture_test.sh
```

脚本退出时会自动删除 `lo` 上的 `netem` 规则。

手动 Wireshark 过滤：

```text
tcp.port == 19002 && (tcp.analysis.lost_segment || tcp.analysis.retransmission || tcp.flags.reset == 1)
```

## 8. 常见问题

**`bind: Address already in use`**

端口被占用。换端口，或查找进程：

```bash
ss -ltnp | grep 8888
```

**`connection refused`**

服务器未启动、端口不匹配，或被防火墙拦截。

**客户端显示英文但中文乱码**

终端不是 UTF-8。Linux/WSL 可设置：

```bash
export LANG=C.UTF-8
```

**局域网无法连接**

检查服务器 IP、WSL2 端口转发和 Windows 防火墙入站规则。跨主机实验时，最简单的方式是把程序运行在 Ubuntu 虚拟机/物理机；WSL2 需要额外端口代理。

**Windows 原生编译失败**

本项目使用 POSIX Socket 和 pthread，请在 Linux/WSL 下编译运行；不要使用 TDM-GCC/MSVC 直接编译 `src/*.c`。
