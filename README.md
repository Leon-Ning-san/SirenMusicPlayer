<p align="center">
  <img src="icon.jpg" width="128" alt="Sirène Icon" />
</p>

<h1 align="center">Sirène · 跨平台本地音频播放器（C++17 桌面学习版 v1.2.6）</h1>

<p align="center">
  <b>Windows · Linux · macOS（C++17 原生桌面播放器 × HiDPI 物理像素渲染）</b><br/>
  <b>软渲染流体水波纹动态背景 × Hinting 锐利排版 × 网易云 / LRCLIB 在线双语歌词学习实现</b><br/>
  <b>支持本地 MP3 / FLAC / WAV / OGG / AIFF / M4A / AAC / MP4 / ALAC / OPUS / WMA / CAF</b>
</p>

> [!IMPORTANT]
> **学习研究与免责声明（使用前请阅读）**
> - 本项目 **Sirène** 为纯个人编程学习与技术研究作品（用于研究跨平台 C++17 桌面开发、音频解码流水线与纯 CPU 软渲染 UI），**非商业用途**。
> - 本项目与任何硬件厂商、游戏机厂商或第三方公司均**无任何官方关联、授权或合作关系**，任何因下载、编译、分发或运行本项目产生的一切风险与责任均**由使用者自行承担，与原作者无关**。详情请见 [DISCLAIMER.md](DISCLAIMER.md)。
> - 软件本身不内置、不提供、不传播任何受版权保护的音频资源，仅用于播放用户自身合法持有的本地音频文件。

纯软件渲染、无需 GPU：**Windows / Linux / macOS 三个 PC 版本采用 C++17 重构构建**，专为桌面端审美独立打造为 **毛玻璃透明层级 × 随曲变色相干水波纹背景 × Material You Hero 大卡片与超大专辑封面网格 × Apple 悬浮灵动胶囊** 的全窗口 HiDPI 原生桌面音乐播放器；支持日间/夜间平滑护眼主题过渡、全局搜索、格式筛选、鼠标拖拽进度/文件以及 Windows 任务栏悬停播控。

## 桌面端界面预览

| PC 桌面全屏质感曲库（Hero 波浪花边大卡 · 超大专辑封面 · 灵动胶囊 · 随曲水波纹） | PC 桌面沉浸双语歌词（随曲色水波纹流光 · 原文+译文大字号自动换行） |
| :---: | :---: |
| ![桌面曲库网格](previews/desktop_land.png) | ![桌面沉浸歌词](previews/desktop_full.png) |

## 多平台桌面版本（C++17 · v1.2.6）

| 平台 | 产物文件名 | 渲染与音频后端（C++17） | 交互与桌面特性亮点 |
|---|---|---|---|
| **Windows x64** | `Sirene-v1.2.6-Windows-x64.exe` | C++17 + Win32 GDI 原生物理像素（DPI V2）+ WinMM + Media Foundation | 缩放/全屏/高分屏 1:1 物理像素锐利渲染；支持系统缩放 `WM_DPICHANGED`、桌面快捷方式自动创建、**任务栏鼠标悬停缩略图播控栏（上一首/播放·暂停/下一首 + 实时进度条）**、UTF-16 命令行与拖放、M4A/AAC/MP4/ALAC/OPUS/WMA 解码 |
| **Linux x86_64** | `Sirene-v1.2.6-Linux-x86_64.tar.gz` | C++17 + SDL2 HiDPI + SDL2 Audio + FFmpeg 管道 | 原生 HiDPI 渲染、`SDL_SetWindowIcon` 图标、支持常见有损/无损音频格式 |
| **macOS Apple Silicon** | `Sirene-v1.2.6-macOS-arm64.zip` | C++17 + SDL2 Retina + SDL2 Audio + AudioToolbox | 独立 `Sirène.app`（Retina 物理像素锐利渲染 + CoreAudio 原生解码） |

> *注：仓库底层亦包含早期的开源掌机自制 `.nro` 兼容代码归档，仅供学习参考，已不再作为主线演示。*

## 核心学习与实现特性（v1.2.6）

- **随曲变色相干水波纹背景 × 半透明毛玻璃层级**：
  - 实时提取当前播放曲目专辑封面四象限色彩并平滑插值，在底层渲染 3 源相干圆形水波纹与点击扩散涟漪；
  - 左侧 Material 3 导航栏、顶部搜索胶囊、筛选芯片、大封面卡片、悬浮灵动胶囊均采用真实 Alpha 混合呈现。
- **Material You Hero 大卡 × 超大封面网格 × 悬浮灵动胶囊**：
  - **Material You 精选 Hero 卡**：跨越两列展示当前曲目封面、大字号进度百分比、加粗直线水纹流光进度条、正弦波浪分割线与旋转星芒播放/暂停按钮；
  - **锐利 Hinting 字体与柔和护眼过渡**：Windows 原生对接 Hinting 字体渲染与 LRU 字体缓存池，日间/夜间主题切换采用 `0.85s` 连续色阶水波纹渐进过渡；
  - **Apple 悬浮灵动胶囊 + M3 `+ 导入` FAB**：底部居中自适应悬浮胶囊（实时 5 柱频谱、播控、音量与一体进度条）。
- **任务栏悬停控制与沉浸式双语歌词**：
  - Windows 支持任务栏鼠标悬停缩略图工具栏与键盘媒体键直接切歌/播放/暂停；
  - 歌词界面自动匹配并对齐显示原文与译文（34–48px 大字号自动换行），支持点击任意歌词行跳转、外部 `.lrc` 拖放与快捷键（`/`或`S` 搜索、`V` 视图切换、`C` 日夜主题、`M` 播放模式、`F11` 全屏）。

## 安装与使用（桌面端）

1. 前往 [**Releases v1.2.6 页面**](../../releases/tag/v1.2.6) 下载对应平台包（本工具仅做本地测试与技术学习使用）：
   - **Windows**：双击 `Sirene-v1.2.6-Windows-x64.exe` 即可运行（首次运行自动在桌面生成带图标快捷方式；支持拖入文件夹/音频/`.lrc`）。
   - **Linux**：解压 `Sirene-v1.2.6-Linux-x86_64.tar.gz` 后运行 `./Sirene`（需系统已安装 `libsdl2`，可选 `ffmpeg`）。
   - **macOS**：解压 `Sirene-v1.2.6-macOS-arm64.zip` 打开 `Sirène.app`（如系统拦截可执行 `xattr -cr Sirène.app`）。
2. 自动扫描用户 `Music` 文件夹，也可点击右下角 **`+ 导入`**（`Ctrl/Cmd+O`）选择任意合法持有的本地音频目录。

*(归档说明：历史 `.nro` 归档文件请自行在合规开源自制环境 `sdmc:/switch/` 下加载本地 `sdmc:/music/` 音频，风险自负。)*

## 从源码构建（C++17 桌面端）

使用 [`Makefile.desktop`](Makefile.desktop)：
```bash
# Windows (MinGW-w64 g++ C++17 + windres，静态链接零额外运行库 DLL)
mingw32-make -f Makefile.desktop

# Linux (g++ C++17 + libsdl2-dev + pkg-config)
make -f Makefile.desktop

# macOS (clang++/g++ C++17 + Homebrew sdl2 + pkg-config)
make -f Makefile.desktop macos-app
```

## 许可与开源组件

- 本项目源码（学习研究用途）：[MIT License](LICENSE)（配合 [DISCLAIMER.md](DISCLAIMER.md)）
- 内嵌字体 [Noto Sans SC](https://fonts.google.com/noto)：SIL Open Font License
- 解码与图像单头文件库（`dr_libs`、`stb`）：Public Domain / MIT
