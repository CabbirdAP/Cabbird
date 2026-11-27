# 本机网页服务

`cabbird.web` v1（capability：`web`）提供 Runtime 拥有的本机 HTTP 端点。
公开头文件为 [`services/web.h`](../../include/cabbird/sdk/services/web.h)，不依赖 Unity 或游戏 Profile。

| 入口 | 合同 |
| --- | --- |
| `register_application(user, assets_relative_directory, callback, callback_user, *application)` | 在 Lifecycle／Worker 注册本插件 generation 的网页应用，返回 scope 所有的代次句柄 |
| `unregister_application(user, application)` | 关闭入口并排空已进入的回调；仅限 Lifecycle／Worker |
| `application_url(user, application, destination, *inout_size)` | 两次调用的 NUL 结尾字符串协议，大小包含终止符 |

每个 generation 最多注册一个应用。资源目录相对于当前 DLL generation 的包快照，
例如 `assets/web`；宿主拒绝越界路径和 reparse，要求存在 `index.html`，
最多预载 64 个文件、总计 8 MiB。预载发生在 Worker，加载期间请求可能返回 503；
静态资源随后由已加载的不可变快照提供。

## URL 与认证

监听器只绑定 `127.0.0.1`，端口由系统分配。应用 URL 形如：

```text
http://127.0.0.1:<port>/plugins/<plugin-id>/#token=<session-token>
```

令牌只属于本次注册，不写入配置或日志。网页从 fragment 读取令牌，
在所有 `/api/` 请求中发送 `X-Cabbird-Token`：

```javascript
const token = new URLSearchParams(location.hash.slice(1)).get("token");
const response = await fetch("api/state", {
  headers: { "X-Cabbird-Token": token }
});
```

宿主同时校验 Host 和请求中存在的 Origin。静态资源不需要令牌，API 不开放 CORS。
服务图中 `cabbird.internal.web-server` 的 Ready 表示可选初始化任务已结束，
不保证监听成功；只有版本化 provider 成功发布后，公开服务才能被查询到。
监听失败不会阻止其他 Runtime 服务启动。

## 回调、响应与卸载

`CabbirdWebCallbackV1` 在 Runtime Worker 上、持有 PluginScope generation lease 时调用：

```c
CabbirdStatusV1 CABBIRD_CALL handler(
    void* user, const CabbirdWebRequestV1* request,
    CabbirdWebResponseV1* response);
```

`request` 包含 GET／POST 方法、应用内路径（例如 `/api/state`）和最多 64 KiB 的请求体。
借用视图仅在当前调用有效。回调不得直接访问 Game／Render 对象；GET 读取已经发布的快照，
游戏操作进入插件的请求队列，在 `on_update` 执行。

宿主提供最多 1 MiB 的可写响应缓冲。插件复制内容后填写 `body_size`、
`http_status`（200..599）和 `content_type`（JSON 或 TEXT），
不能替换 `body.data`、扩大 `body.size` 或保留请求／响应引用。
每次请求只调用回调一次；HTTP 200 本身不证明某个游戏操作已生效。

在应用自己的 HTTP 回调内注销会返回 `CONFLICT`，应投递停止意图。
停用和热重载由 Scope ledger 自动撤销遗漏的应用，旧 generation 不再接收新请求。
等待超时返回 HTTP 504；已开始的回调可能产生业务效果，客户端不能仅凭 504 自动重发。
