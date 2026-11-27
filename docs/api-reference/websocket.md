# 本机 WebSocket 广播

`cabbird.websocket` v1 是与 Unity、游戏 Profile 和渲染无关的 Runtime 服务。
公开头文件是 [`services/websocket.h`](../../include/cabbird/sdk/services/websocket.h)，
manifest 必须同时声明该服务和 `websocket` capability。

Runtime 拥有监听器、连接和 socket I/O。默认监听 `127.0.0.1:14514`，
只接受本机回环连接，使用 RFC 6455 文本帧。插件负责消息格式；宿主不解释 JSON，
也不把客户端入站消息转换成插件命令。

| 入口 | 合同 |
| --- | --- |
| `publish_text(user, message)` | 复制 UTF-8 文本到有界广播队列，返回前不执行 socket I/O；单条最多 1 MiB |
| `server_info(user, *info)` | 调用者先设置 `info.struct_size`；返回当前监听端口、连接数、已发布和丢弃计数 |
| `set_port(user, port)` | 可选 v1 尾字段；先检查 `struct_size` 覆盖此字段，接受 `1..65535` 并排队更换端口 |

队列无法接收消息时返回 `CONFLICT`；服务停止或监听器不可用时返回 `UNAVAILABLE`，
`server_info` 此时报告 `port = 0`。`publish_text` 的 `OK` 只说明已入队，
不保证每个客户端均已收到；慢客户端和队列压力会反映到丢弃计数。

端口变更由 Runtime Worker 执行，先绑定新监听器，再关闭原监听器。
绑定失败时保留原端口；Worker 尚未处理的多次变更以最后一次请求为准。
调用者通过 `server_info` 观察实际生效端口，不能把 `set_port` 的 `OK` 当成绑定完成。

服务可声明为 `optional`；缺失时插件应继续提供不依赖广播的功能。
所有插件共享同一个进程级广播服务，连接生命周期不归插件所有。
