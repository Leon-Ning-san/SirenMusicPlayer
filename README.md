# Siren · Switch 本地音乐播放器

为破解版 Nintendo Switch（Atmosphère / Hekate，通过 **hbmenu** 启动）制作的本地音乐播放器，输出 `.nro` 格式。纯软件渲染、无需 GPU，完整支持**触控操作 + 手柄按键 + 陀螺仪横竖屏自动旋转**。

## 界面预览

| 横屏列表页 | 横屏播放页 |
| :---: | :---: |
| ![横屏列表页](previews/list_land.png) | ![横屏播放页](previews/player_land.png) |

| 竖屏列表页 | 竖屏播放页 |
| :---: | :---: |
| ![竖屏列表页](previews/list_port.png) | ![竖屏播放页](previews/player_port.png) |

## 核心特性

- **主流无损与常见音频**：支持 **MP3 / FLAC / WAV / OGG**（SDL2 音频队列输出 + `dr_libs` / `stb_vorbis` 解码），带播放进度平滑拖拽与 22 段实时频谱可视化。
- **零延迟封面与标签解析**：支持 ID3v2 APIC/PIC、FLAC PICTURE、Vorbis Comment；标签解析缺失时自动扫描文件头提取内嵌 JPEG/PNG，无封面曲目自动生成渐变音符占位图。
- **全能歌词支持（外置 + 内嵌 + 多编码）**：
  - 自动匹配同目录、`lyrics/` 子目录或 `sdmc:/lyrics/` 下的 `.lrc`（不区分大小写，也支持按歌曲标题/歌手-标题匹配）。
  - 支持读取 **MP3 内嵌歌词（`USLT` / `SYLT` / `TXXX`）** 与 **FLAC / OGG 内嵌歌词（`LYRICS` / `UNSYNCEDLYRICS`）**。
  - 自动识别 **UTF-8 / UTF-16LE / UTF-16BE / GBK** 编码，自动清理 `<00:12.34>` 逐字标签并支持 `[offset:ms]`；无时间轴的纯文本歌词也能按进度自动滚动显示。
- **完整触控与 Switch 软键盘**：
  - 列表页支持**像素级惯性顺滑拖拽滚动**、直接点击卡片右侧 `♥` 收藏、顶部快捷胶囊切换（`全部` / `喜欢` / `搜索` / `主题` / `横竖屏` / `自检`）与底部迷你播放条切歌暂停。
  - 点击 **搜索** 或按 **`X`** 呼出 Switch 原生系统软键盘（`Swkbd`）搜索歌名、艺术家或路径。
- **三套内置主题 & 持久化记忆**：支持 **青绿 / 深空蓝 / 苔绿** 三套 Material 配色，自动保存主题、播放模式（列表循环 / 单曲循环 / 随机播放）、音量至 `sdmc:/Siren/settings.cfg`，收藏列表保存至 `sdmc:/Siren/favorites.cfg`。
- **横竖屏自适应**：掌机模式下竖持 Switch 约 0.35 秒自动切换为 `720×1280` 竖屏布局（触摸坐标同步旋转），也可通过 **− (Minus)** 键或右上角 **横/竖** 按钮手动切换。

## 安装与使用

1. 前往 [**Releases 页面**](../../releases) 下载 `Siren.nro`，放入 SD 卡 `sdmc:/switch/Siren.nro`。
2. 把音频（及同名 `.lrc` 歌词）放入 `sdmc:/music/`（首次启动会自动创建目录；若未放于常见目录也会自动跳过系统文件夹全卡扫描，FAT32 大小写路径自动去重）。
3. 进入 hbmenu 启动 **Siren**。

## 按键说明

| 按键 | 列表页 | 播放页 |
|---|---|---|
| `↑ / ↓` 或 左摇杆 | 上下选择歌曲 | 调节音量 `− / +` |
| `右摇杆 ↑ / ↓` | 调节音量 `− / +` | 调节音量 `− / +` |
| `A` | 播放并进入播放页 | 暂停 / 继续 |
| `B` | 退出软件（返回 hbmenu） | 返回列表（音乐继续播放） |
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
