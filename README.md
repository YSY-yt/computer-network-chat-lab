# 计算机网络实验：多用户聊天程序（TCP / C）

使用 C 语言和基本 Socket 函数实现的命令行多用户聊天室。程序基于 TCP 流式套接字，自带应用层聊天协议 `CHAT/1`，支持中文与英文、服务器统一时间戳、多线程并发聊天，并带有应用层序号检测和丢包观察脚本。

## 功能

- 使用 `socket`、`bind`、`listen`、`accept`、`connect`、`send`、`recv` 等基本 Socket 函数，未使用任何 Socket 封装类/框架。
- 自定义协议：`TYPE|SEQ|TIME|FROM|BODY\n`，支持正文中出现 `|`。
- 所有聊天消息由服务器统一加时间戳（`YYYY-MM-DD HH:MM:SS`），避免客户端时钟不一致。
- UTF-8 中文、英文、Emoji；严格 UTF-8 校验；昵称支持中文。
- 服务器每连接一个 `pthread`；客户端主线程 + 接收线程。
- 登录校验、昵称唯一性、最多 64 人、超时/断线清理、`SIGINT` 优雅退出。
- 每条“服务器→客户端”消息带独立递增序号；客户端检测序号缺口。
- 支持 `/help`、`/list`、`/ping`、`/quit`。
- 提供 `tcpdump` + `tc netem` 丢包观察实验脚本。

## 快速开始

环境：Linux 或 WSL（使用 POSIX Socket 和 pthread；本实验在 Ubuntu 24.04 / WSL2 上验证），GCC、GNU Make、Python 3（仅测试用）。

```bash
make
```

启动服务器：

```bash
./bin/chat_server 8888
```

打开多个终端启动客户端：

```bash
./bin/chat_client 127.0.0.1 8888 Alice
./bin/chat_client 127.0.0.1 8888 Bob
./bin/chat_client 127.0.0.1 8888 小明
```

直接输入中文或英文并回车即可发送。客户端命令：

```text
/help          显示帮助
/list          查看在线用户
/ping          探测服务器
/quit, /exit   退出聊天室
```

显示服务器→客户端序号（用于观察是否丢消息）：

```bash
./bin/chat_client -s 127.0.0.1 8888 Alice
```

输出示例：

```text
[2026-10-28 20:19:44] #4 <Alice> 大家好，我是 Alice。Hello, this is Alice.
[2026-10-28 20:19:46] #5 <Bob> Hi Alice! 中英混合 mixed message 😀
[2026-10-28 20:19:47] #6 <小明> 三人都在：Hello 大家好 123，管道符 | 也可以发送。
```

## 测试

```bash
make test
```

包含：

1. `tests/protocol_test.c`：帧编解码、非法序号、超长正文、中文/Emoji、非法 UTF-8、昵称规则。
2. `tests/integration_test.py`：启动真实服务器和 3 个真实客户端（Alice、Bob、小明），验证中英文广播、`/ping`、`/list`、`/quit`、重复昵称拒绝，并检查每个客户端收到的服务器帧序号 `1..N` 连续。测试日志写入 `build/integration/`。

丢包观察（需要 root 权限进行抓包）：

```bash
sudo bash scripts/capture_test.sh

# 在回环接口上模拟 15% 丢包，再跑一遍
sudo env CHAT_LOSS_PERCENT=15 \
  CHAT_PCAP="$PWD/build/chat-loss15.pcap" \
  bash scripts/capture_test.sh
```

脚本会在 `build/` 下生成 pcap 和 `tshark` 统计。正常网络下重传/丢失标记为 0；模拟 15% 丢包时可观察到 TCP Retransmission / Previous segment not captured，但应用层序号仍连续。更多数据见 [`docs/report.md`](docs/report.md)。

## 目录

```text
include/chat_protocol.h     协议定义
include/net_util.h          Socket 辅助接口
src/chat_protocol.c         帧编解码、时间、UTF-8 校验
src/net_util.c              TCP 连接、完整发送、按行接收
src/server.c                多线程服务器
src/client.c                命令行客户端
tests/                      单元测试 + 端到端测试
scripts/capture_test.sh     抓包与丢包模拟
docs/protocol.md            完整协议语法、语义、时序
docs/design.md              模块划分、线程模型、流程图
docs/report.md              实验报告
docs/report.docx            实验报告 Word 版
docs/images/                流程图与时序图
docs/samples/               实际运行日志
docs/pcap/                  抓包文件
```

## 文档

- [协议规范](docs/protocol.md)
- [程序设计与模块划分](docs/design.md)
- [实验报告 Markdown](docs/report.md) / [Word 版](docs/report.docx) / [PDF 版](docs/report.pdf)
- [使用方法与命令说明](docs/usage.md)

## 说明与限制

- 明文传输、无身份认证，仅用于课程实验。
- 每连接一个线程、广播持锁，适合 ≤64 人的小规模聊天；生产环境需要 epoll/线程池和发送队列。
- 服务器显示本地时区时间，精度到秒。
- 不支持历史消息、私聊、文件传输。

## GitHub

源码与报告仓库：<https://github.com/YSY-yt/computer-network-chat-lab>
