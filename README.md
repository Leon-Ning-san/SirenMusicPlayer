# Siren — 沉浸式 Switch 音乐播放器

[中文](#中文) | [English](#english)

<a id="中文"></a>

## 中文

为破解版 Switch（Atmosphère / Hekate，通过 hbmenu 启动）制作的**极简本地音乐播放器**，输出 `.nro` 格式。纯软件渲染，无需 GPU，触摸与手柄双操作。

### 功能

- **极简界面**：单列歌曲列表（48px 小封面 + 歌名 + 艺术家），底部播放条；播放页大封面 + 同步歌词
- **格式支持**：MP3 / FLAC / WAV / OGG（SDL2 音频输出，dr_libs + stb_vorbis 解码）
- **封面**：ID3 APIC/PIC、FLAC PICTURE、Vorbis comment；标签缺失时扫描文件头 2MB 搜索 JPEG/PNG 签名兜底，无封面歌曲用内嵌海报占位
- **歌词**：同名 `.lrc` 自动加载，支持 UTF-8 / UTF-16LE 与多时间戳；无歌词时显示实时频谱
- **自动扫描**：启动自动创建 `sdmc:/music/`，优先扫描常见音乐目录，找不到时全卡扫描（跳过系统目录），FAT32 大小写路径自动去重
- **平滑动画**：页面切换、列表滚动、封面交叉淡化、进度条跟随、触点涟漪反馈
- **双操作**：触摸（点歌 / 拖动 / 进度条跳转 / 分区控制）+ 手柄全按键

### 按键

| 按键 | 列表页 | 播放页 |
|---|---|---|
| ↑ / ↓ | 选择歌曲 | 音量 − / + |
| A | 播放并进入播放页 | 暂停 / 继续 |
| B | — | 返回列表 |
| X | 进入播放页 | — |
| L / R 或 ← / → | 上一首 / 下一首 | 上一首 / 下一首 |
| ← / → | — | 快退 / 快进 10 秒 |
| Y | 音频自检面板 | 音频自检面板 |
| Plus | 退出 | 退出 |

### 安装

1. 从 [Releases](../../releases) 下载 `Siren.nro`，复制到 SD 卡 `switch/` 目录
2. 进入 hbmenu，启动 **Siren**
3. 把音乐放入 `sdmc:/music/`（程序启动会自动创建该目录）

### 从源码构建

需要 [devkitPro](https://devkitpro.org/) 工具链：

```bash
# 安装依赖（devkitPro pacman）
sudo dkp-pacman -S switch-dev switch-sdl2

# 编译
make

# 产物：PureMusic.nro（构建后可用 fix_nacp.py 修正应用标题为 UTF-16LE）
```

目录结构：

```
src/        源码（渲染 / 音频 / 元数据 / 解码 / UI）
deps/       第三方单头文件库 + 内嵌中文字体 + 海报资源
Makefile    devkitPro NRO 构建脚本
icon.jpg    hbmenu 图标
```

### 许可

- 本项目代码：MIT
- 内嵌字体 [Noto Sans SC](https://fonts.google.com/noto)：SIL Open Font License
- 解码库 dr_libs、stb：公共领域 / MIT，详见各自文件头

---

<a id="english"></a>

## English

A minimal local music player for hacked Nintendo Switch (Atmosphère / Hekate, launched via hbmenu), packaged as `.nro`. Pure software rendering — no GPU required. Full touch and gamepad support.

**Features**: single-column track list with cover thumbnails, full-screen player with large cover and synced lyrics, MP3/FLAC/WAV/OGG playback via SDL2, ID3/FLAC/Vorbis cover art extraction with binary-signature fallback, auto `.lrc` lyrics loading, automatic music directory creation and SD card scanning with FAT32 case-insensitive dedup, smooth animations.

**Build**: requires [devkitPro](https://devkitpro.org/) with `switch-dev` and `switch-sdl2` packages, then `make`.

**License**: MIT for the project code; Noto Sans SC (SIL OFL) for the embedded font; dr_libs / stb are public domain / MIT.
