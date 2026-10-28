# 程序设计与模块划分

## 1. 总体结构

程序由两个可执行文件和一组共享模块组成：

```text
                    TCP (SOCK_STREAM, UTF-8, CHAT/1)
  +-------------+  ==================================  +----------------------+
  | chat_client |                                      | chat_server          |
  |-------------|                                      |----------------------|
  | 主线程      |  stdin -> CHAT/PING/LIST/QUIT         | 主线程               |
  | 接收线程    |  <----- WELCOME/CHAT/JOIN/LEAVE       |  poll + accept        |
  +-------------+                                      |  每连接一个 pthread   |
                                                       |  全局 clients 链表    |
                                                       |  mutex 保护广播       |
                                                       +----------------------+
```

客户端主线程读取键盘输入并发送；接收线程持续 `recv` 并把服务器消息格式化输出。服务器主线程只负责 `accept`，每个客户端由一个独立线程处理，所有在线客户端共享一个带互斥锁的链表。

## 2. 文件与模块

```text
network-lab/
├── include/
│   ├── chat_protocol.h      # 协议常量、消息结构、编解码/UTF-8 接口
│   └── net_util.h           # 原始 socket 辅助接口、行读取器
├── src/
│   ├── chat_protocol.c      # 帧构造/解析、时间戳、UTF-8 校验
│   ├── net_util.c           # bind/listen/connect/send_all/readline
│   ├── server.c             # 多线程服务器、客户端管理、广播
│   └── client.c             # 命令行客户端、接收线程
├── tests/
│   ├── protocol_test.c      # 协议与 UTF-8 单元测试
│   └── integration_test.py  # 真实三客户端端到端测试
├── scripts/capture_test.sh  # tcpdump + tc netem 丢包观察脚本
├── docs/                    # 协议、设计、报告、图表、测试样本
├── Makefile
└── README.md
```

| 模块 | 主要功能 | 关键接口 |
|---|---|---|
| 协议模块 `chat_protocol` | 组帧、拆帧、时间戳、昵称/正文/UTF-8 校验 | `chat_make_frame`、`chat_parse_frame`、`chat_now`、`chat_valid_text` |
| 网络工具 `net_util` | TCP 建连/监听、完整发送、按行接收、超时与 keepalive | `tcp_listen`、`tcp_connect`、`send_all`、`reader_readline` |
| 服务器 `server` | 接受连接、登录校验、在线表、广播、退出清理 | `client_thread`、`broadcast_locked`、`send_frame_locked` |
| 客户端 `client` | 登录、键盘输入、接收线程、命令处理 | `receiver_thread`、`display_frame`、`send_client_frame` |
| 测试 | 协议边界、中文/英文、多人广播、序号连续性、丢包观察 | `make test`、`scripts/capture_test.sh` |

所有网络操作直接调用基本 Socket 函数：`socket`、`bind`、`listen`、`accept`、`connect`、`send`、`recv`、`shutdown`、`close`、`setsockopt`、`getaddrinfo`。`send_all` 和 `reader_readline` 只是对短读/短写与分帧的必要处理，不是 Socket 类或网络框架。

## 3. 关键数据结构

```c
typedef struct client {
    int              fd;          /* TCP 连接 */
    int              linked;      /* 是否在链表中 */
    int              active;      /* 0 表示发送失败/正在退出 */
    int              logged_in;   /* 已完成 LOGIN/WELCOME */
    uint64_t         out_seq;     /* 该连接的服务器→客户端序号 */
    char             nickname[49];
    char             peer[64];
    struct client   *next;
} client_t;

typedef struct {
    int             listen_fd;
    pthread_mutex_t lock;         /* 保护链表、count 和所有发送 */
    client_t       *head;
    size_t          count;
} server_t;
```

客户端使用 `line_reader_t` 保存接收缓冲区和已用长度；协议消息 `chat_message_t` 保存 `type/seq/time/from/body` 五个字段。

## 4. 线程模型与同步

### 4.1 服务器

- **主线程**：`poll(listen_fd, 500ms)` + `accept`。收到新连接后 `calloc` 一个 `client_t`，以 detached 方式创建连接线程。
- **连接线程**：每个客户端一个，先做 10 秒登录握手，然后循环读帧。消息处理、广播和清理都在线程内完成。
- **互斥锁**：`g_server.lock` 保护：
  - 在线链表和 `count`；
  - 昵称唯一性检查；
  - 所有 `send`（保证一条广播不会被另一个线程的发送穿插）；
  - `out_seq` 分配。
- **广播语义**：一个线程在锁内对该事件的所有接收者逐个发送。这样所有客户端观察到的全局消息顺序一致。
- **慢客户端**：socket 设置 5 秒发送超时；超时或 `send` 失败时把 `active` 置 0 并 `shutdown`，避免永久阻塞。其线程随后会读到 EOF/错误并清理。
- **关闭**：`SIGINT/SIGTERM` 只设置 `volatile sig_atomic_t g_stop`；主循环最多 500 ms 退出，加锁 shutdown 所有连接，关闭监听 fd，等待连接线程退出（最多 1 秒）。

### 4.2 客户端

- **主线程**：解析参数、建立连接、完成登录，然后启动接收线程；之后 `poll(stdin, 500ms)` 读取用户输入，发送 `CHAT`/`PING`/`LIST`/`QUIT`。
- **接收线程**：阻塞在 `readline()` 上，解析服务器帧、检查序号、格式化输出；服务器关闭或发送失败时设置 `g_running=0` 并 `shutdown`。
- **发送锁**：当前只有主线程发送；`g_send_lock` 把发送路径串行化，为后续扩展（例如接收线程主动回 PONG）预留线程安全保证。
- **输出锁**：`g_print_lock` 保护 `stdout`，避免接收线程输出和错误提示互相覆盖。
- **共享 reader**：登录阶段主线程读取 `WELCOME`，之后把同一个 `line_reader_t` 交给接收线程，避免把已经预读的 `JOIN` 帧丢掉。

## 5. 主要流程

### 5.1 服务器主线程

![服务器主线程流程图](images/server_main_flow.png)

### 5.2 服务器连接线程

![服务器连接线程流程图](images/server_worker_flow.png)

### 5.3 客户端主线程

![客户端主线程流程图](images/client_main_flow.png)

### 5.4 客户端接收线程

![客户端接收线程流程图](images/client_receiver_flow.png)

### 5.5 协议时序

![协议交互时序图](images/protocol_sequence.png)

## 6. 关键实现点

1. **处理 TCP 短读/短写**：`send_all()` 循环发送直到全部字节发出；`reader_readline()` 维护缓冲区，解决拆包和粘包。
2. **帧边界与正文中的 `|`**：解析器从左到右找前 4 个 `|`，第 5 段整体作为 `BODY`，因此 `a|b|c` 这样的聊天内容不会被截断。
3. **中文支持**：协议层按 UTF-8 字节处理，`chat_valid_utf8()` 拒绝非法编码；显示层不做转码。单元测试覆盖中文、Emoji、超长编码和代理区。
4. **统一时间**：`chat_now()` 使用 `localtime_r` + `strftime`；服务器在广播前生成时间，客户端只显示服务器时间。
5. **每连接序号**：`out_seq` 在 `send_frame_locked()` 中递增且受锁保护；客户端 `expected_seq` 检查连续性，用来观察应用层消息缺失。
6. **昵称防伪造**：服务器以登录时保存的 `client->nickname` 作为广播 `FROM`，忽略 `CHAT` 帧中的 `from` 字段。
7. **命令与聊天分离**：客户端以 `/` 开头的行解释为命令；普通行校验 UTF-8 和长度后发送 `CHAT`。
8. **可观测性**：服务器打印上线/下线与人数；客户端 `-s` 显示每个服务器帧的序号；实验脚本用 tcpdump/tshark 输出重传统计。

## 7. 构建与运行

```bash
make            # 生成 bin/chat_server 和 bin/chat_client
make test       # 协议单元测试 + 三客户端集成测试
```

```bash
# 终端 1
./bin/chat_server 8888

# 终端 2、3、4
./bin/chat_client 127.0.0.1 8888 Alice
./bin/chat_client 127.0.0.1 8888 Bob
./bin/chat_client 127.0.0.1 8888 小明
```

客户端命令：`/help`、`/list`、`/ping`、`/quit`。`-s` 用于显示服务器→客户端序号，便于实验观察。

## 8. 设计取舍

- **为什么用行分帧而不是二进制长度前缀**：聊天文本无换行，命令简单，便于抓包和肉眼检查；通过限制单帧长度和严格校验保证安全。正文中的 `|` 通过“只切前四个分隔符”保留。
- **为什么不用应用层 ACK**：TCP 已经保证可靠有序；应用层序号用于观察而不是重传，避免重复实现传输层功能。
- **为什么每连接一个线程**：课程实验规模小（≤64 人），逻辑直观，便于展示 `pthread_create`/`pthread_mutex_t` 的使用；高并发场景应改用 epoll/线程池。
- **为什么广播持锁发送**：保证全局顺序和实现简单；生产环境应为每个客户端设置发送队列和独立写线程。
