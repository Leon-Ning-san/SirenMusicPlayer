<p align="center">
  <img src="icon.jpg" width="128" alt="Sirène Icon" />
</p>

<h1 align="center">Sirène · 跨平台本地音乐播放器（v1.2.6）</h1>

<p align="center">
  <b>Windows · Linux · macOS（HiDPI 原生锐利横屏 Material You 专版）&nbsp;|&nbsp;Nintendo Switch（<code>.nro</code>）</b><br/>
  <b>全平台物理像素 1:1 原生字体/矢量渲染 × 网易云 163 key / LRCLIB 自动在线歌词 × 丝滑防爆音进度条拖拽</b><br/>
  <b>全面兼容 MP3 / FLAC / WAV / OGG / AIFF / M4A / AAC / MP4 / ALAC / OPUS / WMA / CAF</b>
</p>

纯软件渲染、无需 GPU：Switch 端继续保留 **乔布斯 iPod Click Wheel × 现代 Apple × M3** 融合形态与横竖屏无手柄全触控体验；**Windows / Linux / macOS 桌面端则专为 PC 审美独立打造为毛玻璃透明 × 随曲变色水波纹动态背景 × Material You Hero 大卡片与超大专辑封面网格 × Apple 悬浮灵动胶囊（Dynamic Island）全窗口 HiDPI 原生播放器**，支持日间/夜间双主题、全局搜索、格式筛选及双击/右键或文件拖放即播。

## 界面预览

| PC 桌面全屏质感曲库（Hero 波浪花边大卡 · 超大专辑封面 · 灵动胶囊 · 随曲水波纹） | PC 桌面沉浸双语歌词（随曲色水波纹流光 · 原文+译文大字号自动换行） |
| :---: | :---: |
| ![桌面曲库网格](previews/desktop_land.png) | ![桌面沉浸歌词](previews/desktop_full.png) |

| Switch 横屏双栏（冰霜白 Apple × M3） | Switch 横屏全屏播放（钛空黑 · 流体光晕 + 波浪条） |
| :---: | :---: |
| ![横屏列表页](previews/list_land.png) | ![横屏全屏播放](previews/player_land.png) |

| Switch 竖屏列表（列表视口严格防溢出） | 青瓷绿 Material You 主题 |
| :---: | :---: |
| ![竖屏列表页](previews/list_port.png) | ![青瓷绿主题](previews/list_sage.png) |

## 多平台支持（v1.2.6）

| 平台 | 产物文件名 | 渲染与音频后端 | 窗口 / 图标与交互亮点 |
|---|---|---|---|
| **Windows x64** | `Sirene-v1.2.6-Windows-x64.exe` | Win32 GDI 原生物理像素（DPI V2）+ WinMM + Media Foundation | 缩放/高分屏 1:1 物理像素渲染彻底告别模糊；UTF-16 命令行路径与拖放支持；内置 WinHTTP 自动匹配网易云/LRCLIB 歌词；原生支持 M4A/AAC/MP4/ALAC/OPUS/WMA |
| **Linux x86_64** | `Sirene-v1.2.6-Linux-x86_64.tar.gz` | SDL2 HiDPI + SDL2 Audio + FFmpeg 管道 | 原生 HiDPI 物理像素渲染、`SDL_SetWindowIcon` 图标、支持 M4A/AAC/MP4/OPUS/WMA/APE/WV |
| **macOS Apple Silicon** | `Sirene-v1.2.6-macOS-arm64.zip` | SDL2 Retina + SDL2 Audio + AudioToolbox | 独立 `Sirène.app`（Retina 物理像素锐利渲染 + 原生 CoreAudio 解码 M4A/AAC/ALAC/CAF/AIFF） |
| **Nintendo Switch** | `Sirene-v1.2.6-Switch.nro` | Switch 线性帧缓冲 + SDL2 | 独立维护横屏/竖屏 Click Wheel 与无手柄触控 + 自定义 A/X/Y/左方向键映射 |

## 核心特性（v1.2.6）

- **随曲变色水波纹背景 × 毛玻璃透明层级**：
  - 实时提取当前播放曲目专辑封面四象限色彩并平滑插值，在窗口底层渲染 3 源相干圆形**液体水波纹流光**与点击扩散涟漪；
  - 左侧 Material 3 导航栏、顶部搜索胶囊、筛选芯片、大封面卡片、苹果灵动胶囊均采用真实透明混合渲染，透出底层流动色彩波纹。
- **Material You Hero 大卡 × 超大封面网格 × 苹果灵动胶囊**：
  - **Material You 精选 Hero 卡**：跨越两列展示当前曲目封面与大字号进度百分比、加粗直线水纹流光进度条、正弦波浪分割线与旋转 10 叶星芒播放/暂停按钮；
  - **锐利清晰字体引擎与柔和护眼主题过渡**：Windows 原生对接 Hinting 网格对齐字体渲染（支持真实粗体），结合自适应 HiDPI 缩放与平滑渐隐双语歌词；切换日间/夜间主题时执行 `0.85s` 连续色阶水波纹渐进过渡，护眼防闪屏；
  - **超大专辑封面 Bento 网格**：200px 级高清晰圆角封面卡片（可随时切换紧凑列表视图），悬浮微抬升、左上角格式胶囊与右下角动态频谱；
  - **Apple 悬浮灵动胶囊 + M3 `+ 导入` FAB**：底部居中自适应悬浮胶囊（鼠标悬停弹性舒展、实时 5 柱音律频谱、播放控制、音量与集成进度条），独立于右下角 Material 3 导入按钮。
- **沉浸式大字号双语歌词与桌面鼠标/键盘全适配**：
  - 歌词界面自动整合网易云 `lrc` + `tlyric` 双语逐行对齐与大字号自动换行（34–48px 原文 + 译文），支持鼠标点击任意歌词行跳转进度、拖入音乐/文件夹/`.lrc`、快捷键（`/`或`S` 搜索、`V` 切换网格/列表、`C` 日夜主题、`M` 播放模式、`F11` 全屏）。

## 版本归档（独立 Release）

- **v1.2.6（最新 · 现代透明水波纹 × Apple 灵动胶囊 × M3 超大封面网格 + 全格式防爆音与双语歌词）**：PC 桌面端全新升级随曲变色相干水波纹背景、半透明毛玻璃控件、Material You 精选波浪星芒 Hero 卡、200px 超大专辑封面网格、Apple 悬浮灵动胶囊与全窗口 HiDPI 原生缩放；同步修复 Windows UTF-16 CLI 路径、进度条拖拽爆音与全主流格式兼容（M4A/AAC/MP4/OPUS/WMA/AIFF/FLAC/OGG/MP3/WAV）及网易云 163 key/LRCLIB 自动双语歌词。
- **v1.2.5**：PC 横屏 1:1 高清分栏重构、移除冗余 iPod 转盘、全面鼠标悬停与拖拽适配。
- **v1.2.4**：列表严格像素裁剪防溢出 + Material You 粗波浪进度条 + `.lrc` 拖放与解析增强。
- **v1.2.3**：首批 Windows / Linux / macOS 桌面端与 60 FPS 光栅引擎重构。
- **v1.2.2**：`Sirène` 法语命名 + Switch 无手柄全触控闭环 + 歌词/封面深度修复。
- **v1.2.1**：现代 Hi-Res 便携 MP4 双栏版。
- **v1.2.0**：复古乔布斯 iPod Classic 版。
- **v1.1.0**：Material You 初版。

## 安装与使用说明

### 桌面端（Windows / Linux / macOS）
1. 前往 [**Releases v1.2.6 页面**](../../releases/tag/v1.2.6) 下载对应系统安装包：
   - **Windows**：直接双击 `Sirene-v1.2.6-Windows-x64.exe`（支持命令行参数或拖入文件/文件夹，首次运行将自动在桌面生成 `Sirène` 快捷方式图标）。
   - **Linux**：解压 `Sirene-v1.2.6-Linux-x86_64.tar.gz` 后运行 `./Sirene`（需系统安装 `libsdl2` 与可选 `ffmpeg` 扩展格式支持）。
   - **macOS**：解压 `Sirene-v1.2.6-macOS-arm64.zip` 打开 `Sirène.app`（如遇未知开发者拦截可执行 `xattr -cr Sirène.app`）。
2. **添加歌曲与鼠标操控**：
   - 自动扫描系统 `Music` 文件夹，也可点击右下角 **`+ 导入`**（`Ctrl/Cmd+O`）或将**文件夹 / 音频 / `.lrc` 文件直接拖入窗口**；
   - 点击歌曲封面、Hero 卡或灵动胶囊 **`词`** 进入大字号双语水波纹歌词页；点击任意一句歌词立即跳转对应时间进度。

### Nintendo Switch（`.nro`）
1. 下载 `Sirene-v1.2.6-Switch.nro` 重命名或直接放入 `sdmc:/switch/Sirene.nro`，歌曲与 `.lrc` 放入 `sdmc:/music/`。
2. 在 Switch 主界面按住 **`R` 键打开任意游戏**进入 hbmenu 启动 **Sirène**。

## 从源码构建

### 桌面端（Windows / Linux / macOS）
使用 [`Makefile.desktop`](Makefile.desktop)：
```bash
# Windows (MinGW-w64 + windres)
mingw32-make -f Makefile.desktop

# Linux (libsdl2-dev + pkg-config)
make -f Makefile.desktop

# macOS (Homebrew sdl2 + pkg-config)
make -f Makefile.desktop macos-app
```

### Nintendo Switch（`.nro`）
```bash
make clean && make
```

## 许可

- 本项目源码：MIT
- 内嵌字体 [Noto Sans SC](https://fonts.google.com/noto)：SIL Open Font License
- 解码与图像单头文件库（`dr_libs`、`stb`）：Public Domain / MIT
