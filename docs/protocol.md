# CHAT/1 聊天协议规范

本文档说明聊天程序使用的应用层协议：消息语法、字段语义、状态与时序、错误处理和编码约束。服务端和客户端共享同一份协议实现（`include/chat_protocol.h`、`src/chat_protocol.c`）。

## 1. 设计目标

1. 基于 TCP 流式套接字（`SOCK_STREAM`），不依赖任何 Socket 封装类或 RPC 框架。
2. 在字节流之上提供明确的消息边界，支持一条消息被拆成多个 TCP 段、一个 TCP 段包含多条消息的情况。
3. 每条聊天消息提供服务器统一时间戳，避免不同客户端时钟不一致。
4. 支持 UTF-8 中文、英文、Emoji 及 `|` 等普通字符。
5. 为每条“服务器→客户端”消息提供独立递增序号，便于观察应用层是否有消息缺失。
6. 协议足够简单，能以 C 语言基本 `socket` / `send` / `recv` 函数实现。

## 2. 传输层与分帧

- 传输层：TCP，默认端口 `8888`。
- 字符编码：UTF-8，不带 BOM。
- 分帧方式：一行一帧，以 LF（`\n`，0x0A）结束；接收端兼容 CRLF，读取时去掉行尾 `\r`。
- 单帧上限：4096 字节（含行尾 LF）。超过上限的连接被视为协议错误。
- 字段分隔符：`|`。解析时**只把前 4 个 `|` 当作分隔符**，第 5 个字段（`BODY`）的内容可以继续包含 `|`。
- 字段内部不得出现 CR、LF；昵称还不得出现 `|` 和空白字符。消息正文不得包含控制字符。
- 读取端必须处理“半包/粘包”：`reader_readline()` 把 `recv()` 收到的字节追加到缓冲区，直到看见 LF 才返回一行；一次 `recv()` 中多出的字节保留给下一帧。

### 2.1 帧语法（ABNF）

```abnf
frame   = type "|" seq "|" time "|" from "|" body LF
type    = 1*15(%x41-5A)              ; 大写字母，如 CHAT、WELCOME
seq     = %x31-39 *DIGIT             ; 十进制正整数，不使用 0
time    = 4DIGIT "-" 2DIGIT "-" 2DIGIT SP
          2DIGIT ":" 2DIGIT ":" 2DIGIT
from    = nickname / "server"
nickname = 1*48(UTF8-non-space)      ; 同时限制为 1–16 个 Unicode 字符
body    = *3000(UTF8-char)           ; UTF-8 字节数 ≤ 3000，可含 "|"
```

说明：

| 字段 | 含义 | 约束 |
|---|---|---|
| `TYPE` | 消息类型 | 1–15 个大写英文字母 |
| `SEQ` | 序列号 | 正整数，见 2.2 |
| `TIME` | 时间戳 | `YYYY-MM-DD HH:MM:SS`，服务器本地时区 |
| `FROM` | 发送者 | 登录后由服务器填写，普通消息不能伪造 |
| `BODY` | 正文 | 最多 3000 个 UTF-8 字节；聊天正文不能为空 |

昵称限制为 **1–16 个 Unicode 字符且不超过 48 个 UTF-8 字节**。中文昵称按字符计数，不按字节计数。

示例（`\n` 表示实际换行）：

```text
CHAT|4|2026-10-28 20:19:44|Alice|Hello 你好，Bob\n
```

### 2.2 序列号语义

协议区分两个方向：

1. **服务器 → 客户端（权威序号）**
   - 每个连接维护独立的 `out_seq`，从 1 开始。
   - 服务器发往该连接的每一帧（`WELCOME`、`JOIN`、`LEAVE`、`CHAT`、`PONG`、`INFO`、`ERROR`）都使用下一个序号。
   - 客户端保存 `expected_seq`；收到帧后若 `SEQ != expected_seq`，打印“序号不连续”警告，然后把期望值更新为 `SEQ + 1`。
   - 由于每个接收端独立编号，同一次广播在不同客户端看到的序号可能不同，这是正常的。

2. **客户端 → 服务器（本地序号）**
   - 客户端发送 `LOGIN`、`CHAT`、`PING`、`LIST`、`QUIT` 时使用本地递增序号，从 1 开始。
   - 服务器只把它用于调试/追踪，**不信任客户端提供的 `SEQ`、`TIME`、`FROM`**。聊天消息的发送者由连接身份决定，时间由服务器重新生成。

### 2.3 时间语义

- 服务器在**广播前**调用 `chat_now()` 生成时间，格式为 `YYYY-MM-DD HH:MM:SS`，精度为秒。
- 一次广播事件（例如一条 `CHAT`）对所有接收者使用同一个时间戳。
- 客户端 `LOGIN`/`PING` 中的时间为客户端本地时间，仅用于记录；显示聊天消息时不使用客户端时间。
- 服务器本地时区由运行环境决定（实验环境为 `Asia/Shanghai`，WSL 继承 Windows 时区）。

## 3. 消息类型与语义

| 类型 | 方向 | 发送者 | BODY | 语义 |
|---|---|---|---|---|
| `LOGIN` | C→S | 客户端 | `CHAT/1` | 连接后的第一帧；声明昵称和协议版本。`FROM` 字段是请求的昵称。 |
| `WELCOME` | S→C | server | 欢迎语、在线人数 | 登录成功；该连接的第一帧，`SEQ=1`。 |
| `CHAT` | C→S | 客户端 | 聊天正文 | 请求广播一条聊天消息；服务器重新生成 `SEQ/TIME/FROM` 后广播。 |
| `CHAT` | S→all | server | 聊天正文 | 广播给包括发送者在内的所有在线客户端；发送者也通过回显确认服务器已接收。 |
| `JOIN` | S→all | server | `昵称 加入…` | 有新用户登录，广播给所有在线用户（含新用户）。 |
| `LEAVE` | S→all | server | `昵称 离开…` | 用户主动退出、断线或被服务器关闭时广播。 |
| `PING` | C→S | 客户端 | 空或任意短文本 | 应用层探活；服务器立即回 `PONG`。 |
| `PONG` | S→C | server | `pong` | 对 `PING` 的响应。 |
| `LIST` | C→S | 客户端 | 空 | 请求在线用户列表。 |
| `INFO` | S→C | server | `在线用户（N）：…` | 返回当前在线昵称列表。 |
| `QUIT` | C→S | 客户端 | 空 | 主动退出；客户端发送后立即关闭连接。 |
| `ERROR` | S→C | server | `CODE: 说明` | 协议/业务错误；错误码见表 3。 |

### 3.1 错误码

| 错误码 | 触发条件 | 连接处理 |
|---|---|---|
| `LOGIN_TIMEOUT` | 连接后 10 秒内未收到 `LOGIN` | 发送 `ERROR` 后关闭 |
| `BAD_LOGIN` | 首帧不是 `LOGIN` 或版本不是 `CHAT/1` | 关闭 |
| `BAD_NICKNAME` | 昵称非法（空、超长、空白、`|`、非法 UTF-8） | 关闭 |
| `NICK_TAKEN` | 昵称已被在线用户占用（ASCII 不区分大小写） | 关闭 |
| `SERVER_FULL` | 在线连接数达到 64 | 关闭 |
| `FRAME_TOO_LONG` | 单帧超过 4096 字节 | 关闭 |
| `BAD_FRAME` | 字段缺失、序号非法等 | 已登录时回 `ERROR` 并继续 |
| `BAD_TEXT` | `CHAT` 正文为空、含控制字符、非法 UTF-8 或超过 3000 字节 | 回 `ERROR` 并继续 |
| `UNKNOWN_TYPE` | 未定义的消息类型 | 回 `ERROR` 并继续 |
| `SERVER_STOPPING` | 服务器正在关闭 | 关闭 |

`ERROR` 的 `BODY` 采用“错误码: 可读说明”的形式，方便客户端直接显示，也方便实验调试。

## 4. 交互时序

### 4.1 正常登录与聊天

```text
客户端 A                        服务器                        客户端 B
   |                              |                              |
   |------ TCP connect ---------->|                              |
   |-- LOGIN|1|T_A|Alice|CHAT/1 ->|                              |
   |<-- WELCOME|1|T_S|server|...--|                              |
   |<-- JOIN|2|T_S|server|...-----|                              |
   |                              |<--------- TCP connect -------|
   |                              |<-- LOGIN|1|T_B|Bob|CHAT/1 ---|
   |                              |--- WELCOME|1|T_S|server|...>|
   |<-- JOIN|3|T_S|server|Bob ---|---- JOIN|2|T_S|server|Bob --->|
   |-- CHAT|2|T_A|Alice|你好 ---->|                              |
   |<-- CHAT|4|T_S|Alice|你好 ----|---- CHAT|3|T_S|Alice|你好 --->|
   |                              |<-- PING|2|T_B|Bob|-----------|
   |                              |--- PONG|4|T_S|server|pong -->|
   |                              |<-- QUIT|3|T_B|Bob|-----------|
   |<-- LEAVE|5|T_S|server|... ---|                              |
```

对应的图形化时序图见 [`images/protocol_sequence.png`](images/protocol_sequence.png)。

关键点：

1. 登录和 `JOIN` 广播在同一把 `pthread_mutex_t` 保护下完成，保证新客户端先收到 `WELCOME`，再收到自己的 `JOIN`，不会在欢迎语之前看到其他聊天消息。
2. 服务器处理一条 `CHAT` 时，先校验正文，再在锁内取时间、为每个接收者分配序号并发送；因此所有在线客户端看到的广播顺序一致。
3. 发送者也会收到自己的 `CHAT` 回显；回显是服务器已接收该消息的应用层证据。
4. B 的 `PING` 不影响 A 的序号；A 和 B 的 `SEQ` 序列分别独立连续。

### 4.2 登录状态机

**客户端：**

```text
DISCONNECTED
    │ socket/connect
    ▼
CONNECTING ──发送 LOGIN──► WAIT_WELCOME
                              │ WELCOME
                              ▼
                            CHAT ◄──── 输入消息 / PING / LIST
                              │ /quit、EOF、服务器断开
                              ▼
                           CLOSING → CLOSED
```

**服务器（每个连接一个线程）：**

```text
ACCEPTED
   │ 读取首帧（≤10 s）
   ▼
WAIT_LOGIN ──校验失败──► REJECTING → CLOSED
   │ 校验成功，加入在线表
   ▼
ONLINE ◄──── CHAT / PING / LIST
   │ QUIT、EOF、发送失败、服务器停止
   ▼
LEAVING（广播 LEAVE、从链表删除）→ CLOSED
```

### 4.3 超时与关闭时序

- 连接建立后，服务器为 socket 设置 `SO_RCVTIMEO = 10 s`，只用于登录握手；登录成功后清除超时，允许长时间空闲。
- 客户端发送 `LOGIN` 后等待 `WELCOME`，同样设置 10 秒接收超时。
- 服务器对所有客户端 socket 设置 `SO_SNDTIMEO = 5 s`，防止某个不读取数据的客户端长期阻塞广播。
- 服务器启用 `SO_KEEPALIVE`。
- 主动退出：客户端发送 `QUIT`，然后 `shutdown(SHUT_RDWR)` 并退出；服务器读到 `QUIT` 或 EOF 后广播 `LEAVE`，删除客户端并关闭 fd。
- 异常断开：`recv()` 返回 0 或错误，服务器执行同样的 `LEAVE` 清理。
- `SIGINT`/`SIGTERM`：服务器设置停止标志，最多 500 ms 内退出 accept 循环，`shutdown` 所有在线连接，等待连接线程退出（最多 1 秒）。

## 5. 完整交互示例

假设 A 的昵称为 `Alice`，B 的昵称为 `Bob`，服务器时间为 `2026-10-28 20:19:40`：

```text
A → S  LOGIN|1|2026-10-28 20:19:38|Alice|CHAT/1
S → A  WELCOME|1|2026-10-28 20:19:40|server|欢迎 Alice！... Online users: 1
S → A  JOIN|2|2026-10-28 20:19:40|server|Alice 加入了聊天室（joined the chat）

B → S  LOGIN|1|2026-10-28 20:19:41|Bob|CHAT/1
S → B  WELCOME|1|2026-10-28 20:19:41|server|欢迎 Bob！... Online users: 2
S → A  JOIN|3|2026-10-28 20:19:41|server|Bob 加入了聊天室（joined the chat）
S → B  JOIN|2|2026-10-28 20:19:41|server|Bob 加入了聊天室（joined the chat）

A → S  CHAT|2|2026-10-28 20:19:44|Alice|Hello 你好，Bob
S → A  CHAT|4|2026-10-28 20:19:44|Alice|Hello 你好，Bob
S → B  CHAT|3|2026-10-28 20:19:44|Alice|Hello 你好，Bob
```

注意：

- A 的 `CHAT` 请求中的 `SEQ=2` 是 A 的本地编号；服务器广播时改为各接收连接的下一个 `SEQ`（A 为 4，B 为 3）。
- 服务器广播的 `FROM` 使用连接登录时的昵称，忽略客户端在 `CHAT` 帧中自行填写的 `FROM`。
- 服务器广播的 `TIME` 由服务器统一生成；客户端时间字段只在 `LOGIN`/`PING` 中有记录意义。
- 正文中的 `|` 不会被误分割：解析器只切前 4 个分隔符。

## 6. 中文与 UTF-8 处理

1. 协议层只按字节寻找 `|` 和 `\n`，这两个字节不会出现在任何 UTF-8 多字节字符的内部，因此不会破坏中文。
2. `chat_valid_utf8()` 严格检查：
   - 过长编码（overlong）；
   - 非法续字节；
   - UTF-16 代理区（U+D800–U+DFFF）；
   - 超出 U+10FFFF；
   - C0/C1 控制字符和 DEL。
3. 昵称按 Unicode 码点计数（1–16），正文按 UTF-8 字节计数（≤3000）。
4. 客户端和服务器均不进行编码转换；终端必须使用 UTF-8。实验环境设置 `LANG=C.UTF-8`，Windows Terminal / WSL 默认可显示中文。
5. 单条消息过长时客户端本地拒绝；服务器也独立校验，避免恶意客户端绕过。

## 7. 可靠性与“丢包观察”机制

- TCP 提供可靠、按序的字节流；应用层正常情况下不会出现消息丢失。
- 服务器为每个“服务器→客户端”连接分配连续序号；客户端检查 `SEQ` 是否连续。
  - 若连续，说明该连接的应用层帧没有缺失。
  - 若出现缺口，客户端打印“序号不连续，疑似丢失或乱序”，便于观察到协议实现或异常中断问题。
- 网络层丢包由 TCP 通过重传、SACK/快速重传恢复。实验中使用 `tc netem` 在回环接口上模拟 15% 丢包，抓包中可观察到 TCP Retransmission / Previous segment not captured，但应用层序号仍连续，说明聊天消息未被应用层丢失。详见实验报告第 7 节。
- 该协议**没有**应用层 ACK/重传；这是有意设计：TCP 已提供可靠传输，再实现一套应用层重传会增加复杂度且意义有限。应用层序号用于观测和调试，而不是替代 TCP。

## 8. 协议边界与限制

- 明文传输，无 TLS、无身份认证；昵称只用于显示，不构成安全身份。仅用于课程实验。
- 最多 64 个并发在线连接；每连接一个线程。
- 广播时持有全局互斥锁；某个慢客户端最多阻塞 5 秒（发送超时），适合小规模课堂实验，不适合高并发生产环境。
- 时间精度为秒；不处理跨时区显示（统一显示服务器本地时间）。
- 不支持图片、文件、历史消息、私聊和持久化。
- 协议版本为 `CHAT/1`；版本不匹配会被服务器拒绝。若扩展字段，应升级版本号并保持向后兼容策略。
