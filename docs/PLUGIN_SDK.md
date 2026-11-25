# Sirène 桌面端外部扩展插件开发指南（Lua 5.4 SDK）

Sirène PC 端（Windows / Linux / macOS）内置了纯沙盒化的 **Lua 5.4** 外部扩展引擎与原生 QR 二维码/音频缓冲解码支持。仓库本体不包含任何第三方音乐平台的私有域名、账号协议或加密算法，所有在线数据源均可由用户通过外部 `.lua` 脚本自由接入。

## 1. 安装扩展

将编写好或获取的 `.lua` 扩展脚本（或包含 `main.lua` 的文件夹）放入以下任意位置，或在运行 Sirène 时**直接将 `.lua` 文件拖入播放器窗口**：

- **Windows**：`%APPDATA%\Sirene\plugins\`
- **macOS / Linux**：`~/.config/sirene/plugins/`（或左侧导航「扩展插件 → 管理扩展」中显示的路径）

## 2. 插件基础结构

每个 `.lua` 文件需返回或定义全局 `plugin` 表：

```lua
plugin = {
    id = "my_http_source",
    name = "自定义 HTTP 音乐源",
    version = "1.0.0",
    author = "YourName",
    description = "连接自建标准 HTTP 音乐服务网关",
    supports_login = true, -- 可选：开启后界面显示「扫码登录 / 退出」按钮
    config_fields = {      -- 可选：在管理页直接配置参数并持久化到本地 kv
        { key = "base_url", label = "API 服务地址", placeholder = "http://127.0.0.1:3000" }
    }
}

function plugin.search(query, page)
    local base = sirene.kv_get("base_url", "http://127.0.0.1:3000")
    local res = sirene.http({
        method = "GET",
        url = base .. "/search?keywords=" .. sirene.url_encode(query) .. "&page=" .. tostring(page or 1),
        headers = { Cookie = sirene.kv_get("cookie", "") }
    })
    if not res.ok then return {} end
    local data = sirene.json_decode(res.body)
    local out = {}
    if data and data.tracks then
        for _, item in ipairs(data.tracks) do
            table.insert(out, {
                id = tostring(item.id),
                title = item.title or "未知曲目",
                artist = item.artist or "",
                album = item.album or "",
                duration = tonumber(item.duration) or 0,
                cover_url = item.cover_url or "",
                fmt = item.fmt or "MP3"
            })
        end
    end
    return out
end

function plugin.get_playlists()
    -- 返回 { { id="...", name="...", category="recommend"|"mine" }, ... }
    return {}
end

function plugin.get_playlist_tracks(playlist_id)
    -- 根据 playlist_id 返回曲目列表
    return {}
end

function plugin.get_favorites()
    -- 返回收藏曲目列表
    return {}
end

function plugin.get_track_url(track_id, quality)
    -- 可直接返回 URL 字符串，或返回 { url="...", headers={...}, fmt="FLAC" }
    local base = sirene.kv_get("base_url", "http://127.0.0.1:3000")
    return base .. "/stream?id=" .. sirene.url_encode(track_id) .. "&q=" .. (quality or "high")
end

function plugin.get_lyrics(track_id)
    -- 可选：返回 LRC 字符串或 { lrc="...", tlyric="..." }；若不实现将自动走内建双语歌词匹配
    return nil
end

function plugin.login_qr_start()
    -- 可选：返回 { key="...", url="https://...", message="请扫码确认" }
    return nil
end

function plugin.login_qr_poll(key)
    -- 可选：轮询状态返回 { status="waiting"|"scanned"|"ok"|"expired", nickname="..." }
    -- 成功时可调用 sirene.kv_set("cookie", ...) 持久化凭证（仅存于用户本地）
    return { status = "waiting" }
end

function plugin.logout()
    sirene.kv_set("cookie", nil)
end

return plugin
```

## 3. 宿主 API（`sirene.*`）

出于安全考虑，插件运行在无文件系统写权限、无系统命令执行权限的沙盒中，仅可使用以下标准接口：

| API | 说明 |
|---|---|
| `sirene.http(req)` | 发起 HTTP/HTTPS 请求，`req = { method, url, headers, body }`，返回 `{ ok, status, body, headers, set_cookie, error }` |
| `sirene.json_decode(str)` / `sirene.json_encode(val)` | UTF-8 JSON 解析与序列化 |
| `sirene.kv_get(key, default)` / `sirene.kv_set(key, val)` | 读写当前插件专属的本地持久化键值配置（仅存本地 `plugin_data/`） |
| `sirene.md5(str)` / `sirene.sha256(str)` | 计算标准摘要（小写十六进制） |
| `sirene.base64_encode(str)` / `sirene.base64_decode(str)` | Base64 编解码 |
| `sirene.url_encode(str)` | URL 百分比编码 |
| `sirene.log(msg)` | 记录一条状态信息显示在插件卡片上 |
