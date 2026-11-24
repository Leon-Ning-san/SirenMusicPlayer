# Siren · Switch 本地音乐播放器

为破解版 Nintendo Switch（Atmosphère / Hekate）制作的本地音乐播放器，输出 `.nro` 格式。纯软件渲染、无需 GPU，采用 **Google Material You (Material 3)** 视觉风格设计，完整支持**触控操作 + 手柄按键 + 陀螺仪横竖屏自动旋转**，移除多余复杂动画以保持 Switch 流畅响应，仅在列表页与播放页切换时保留约 5 帧轻量淡化过渡。

## 界面预览

| 横屏列表页（Material You 青绿） | 横屏播放页 |
| :---: | :---: |
| ![横屏列表页](previews/list_land.png) | ![横屏播放页](previews/player_land.png) |

| 竖屏列表页 | 竖屏播放页 |
| :---: | :---: |
| ![竖屏列表页](previews/list_port.png) | ![竖屏播放页](previews/player_port.png) |

| 晴蓝主题 | 苔绿主题 |
| :---: | :---: |
| ![晴蓝主题](previews/list_blue.png) | ![苔绿主题](previews/list_sage.png) |

## 核心特性

- **Google Material You (Material 3) 设计语言**：
  - 横屏采用左侧 **Navigation Rail** 导航栏 + 无边框色阶容器卡片 + 悬浮底部迷你播放栏 + Material You 圆角方矩形（Squircle）播放主按钮。
  - 竖屏采用紧凑顶栏 + **Segmented Pill Bar** 分段胶囊导航，文字与缩略图尺寸独立适配 `720×1280`。
  - 专为 Switch 优化：取消水波纹与多余连续动画，仅保留页面切换 5 帧轻量交叉淡化，拖拽列表跟手不拖影。
- **主流无损与常见音频**：支持 **MP3 / FLAC / WAV / OGG**（SDL2 音频队列输出 + `dr_libs` / `stb_vorbis` 解码），带播放进度拖拽与实时频谱条。
- **零延迟封面与标签解析**：支持 ID3v2 APIC/PIC、FLAC PICTURE、Vorbis Comment；标签解析缺失时自动扫描文件头提取内嵌封面，无封面曲目展示猫耳耳机图标占位图。
- **SD 卡自动扫描与一键重扫**：
  - 自动扫描 `sdmc:/music`、`sdmc:/Music`、`sdmc:/音乐`、`sdmc:/歌曲`、`sdmc:/CloudMusic`、`sdmc:/QQMusic`、`sdmc:/Downloads`、`sdmc:/MP3`、`sdmc:/FLAC` 及 SD 卡根目录（自动跳过 `Nintendo` / `emuMMC` / `atmosphere` / `switch` / `RetroArch` 等系统目录），修复了 `sdmc://` 双斜杠路径导致无法扫出歌曲的问题。
  - 导航栏与空列表状态均提供 **重新扫描** 按钮（空列表按 `A` 键也可触发）。
- **全能歌词支持（外置 + 内嵌 + 多编码）**：
  - 自动匹配同目录、`lyrics/` 子目录或 `sdmc:/lyrics/` 下的 `.lrc`（不区分大小写，也支持按歌曲标题/歌手-标题匹配）。
  - 支持读取 **MP3 内嵌歌词（`USLT` / `SYLT` / `TXXX`）** 与 **FLAC / OGG 内嵌歌词（`LYRICS` / `UNSYNCEDLYRICS`）**。
  - 自动识别 **UTF-8 / UTF-16LE / UTF-16BE / GBK** 编码，自动清理 `<00:12.34>` 逐字标签并支持 `[offset:ms]`；无时间轴的纯文本歌词也能按进度自动滚动显示。
- **完整触控与 Switch 软键盘**：
  - 支持触摸点按与拖动滚动、直接点击右侧 `♥` 收藏、点击 **搜索** 或按 **`X`** 呼出 Switch 原生系统软键盘（`Swkbd`）搜索歌名、艺术家或路径。
- **三套 Material You 主题 & 持久化记忆**：支持 **青绿 / 晴蓝 / 苔绿** 三套配色，自动保存主题、播放模式（列表循环 / 单曲循环 / 随机播放）、音量至 `sdmc:/Siren/settings.cfg`，收藏列表保存至 `sdmc:/Siren/favorites.cfg`。
- **横竖屏自适应**：掌机模式下竖持 Switch 约 0.35 秒自动切换为 `720×1280` 竖屏布局（触摸坐标同步旋转），也可通过 **− (Minus)** 键或 **横/竖** 按钮手动切换。

## 安装与前端（桌面图标）说明

### 1. 基础使用（通过 hbmenu 启动）
1. 前往 [**Releases 页面**](../../releases) 下载 `Siren.nro`，放入 SD 卡 `sdmc:/switch/Siren.nro`（或 `sdmc:/switch/Siren/Siren.nro`）。
2. 把音频（及同名 `.lrc` 歌词）放入 `sdmc:/music/` 或 SD 卡任意非系统目录。
3. 在 Switch 主菜单按住 **`R` 键打开任意游戏（Title Override 模式）** 或从相册进入 hbmenu，启动 **Siren**（建议按住 `R` 启动游戏进入 hbmenu，内存更充裕、音频解码更稳定）。

### 2. 如何安装到 Switch 主菜单（前端桌面图标）
`.nro` 是自制程序可执行文件，**无法直接使用 DBI / Tinfoil 当作 `.nsp` 游戏安装**；如需在 Switch 桌面直接显示图标启动，需打包 **Forwarder（转发器 `.nsp`）**（`Siren.nro` 已内置符合主机要求的 `256×256` 基准 `4:2:0 JFIF` 猫耳图标与标准 UTF-8 NACP 信息）：
- **方式 A（机内直接生成，推荐）**：使用自制工具 [**Sphaira**](https://github.com/ITotalJustice/sphaira) 找到 `Siren.nro` -> 选择 `Install Forwarder`（需在 `sdmc:/switch/prod.keys` 放置你本机导出的密钥）。
- **方式 B（电脑端打包）**：在电脑使用 **Nro2Nsp** 或 **SAK (Switch Army Knife)**，导入 `Siren.nro` 与本机 `prod.keys`，指定路径为 `sdmc:/switch/Siren.nro` 生成 Forwarder `.nsp`，再用 DBI 安装该 `.nsp` 即可（SD 卡里仍需保留 `Siren.nro`）。

## 按键说明

| 按键 | 列表页 | 播放页 |
|---|---|---|
| `↑ / ↓` 或 左摇杆 | 上下选择歌曲 | 调节音量 `− / +` |
| `右摇杆 ↑ / ↓` | 调节音量 `− / +` | 调节音量 `− / +` |
| `A` | 播放并进入播放页（空列表时重新扫描） | 暂停 / 继续 |
| `B` | **退出软件**（返回 hbmenu / 桌面） | 返回列表（音乐继续播放） |
| `X` | 打开 Switch 软键盘搜索 | 收藏 / 取消收藏 |
| `L / R` 或 `← / →` | 上一首 / 下一首 | 上一首 / 下一首（`← / →` 也可快退/快进 10 秒） |
| `ZL` | 收藏 / 取消收藏选中歌曲 | 切换播放模式（列表循环 / 单曲循环 / 随机播放） |
| `ZR` | 切换主题配色 | 下一首 |
| `Y` | 音频与系统自检面板 | 音频与系统自检面板 |
| `− (Minus)` | 切换横屏 / 竖屏 | 切换横屏 / 竖屏 |
| `+ (Plus)` | 退出程序 | 退出程序 |

## 从源码构建

需要 [devkitPro](https://devkitpro.org/) 工具链（`switch-dev`、`switch-sdl2`）：

```bash
make clean && make
```

## 许可

- 本项目源码：MIT
- 内嵌字体 [Noto Sans SC](https://fonts.google.com/noto)：SIL Open Font License
- 解码与图像单头文件库（`dr_libs`、`stb`）：Public Domain / MIT
