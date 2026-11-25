<p align="center">
  <img src="icon.jpg" width="128" alt="Sirène Icon" />
</p>

<h1 align="center">Sirène · 跨平台本地音频播放器（C++17 桌面学习版 v1.2.8）</h1>

<p align="center">
  <b>Windows · Linux · macOS（C++17 原生桌面播放器 × HiDPI 物理像素渲染）· Switch (Homebrew `.nro`)</b><br/>
  <b>多层流体封面动态背景 × 弹簧瀑布流逐句歌词 × 苹果风现代动效与云端扩展 Hub × 双语歌词</b><br/>
  <b>支持本地 MP3 / FLAC / WAV / OGG / AIFF / M4A / AAC / MP4 / ALAC / OPUS / WMA / CAF</b>
</p>

> [!IMPORTANT]
> **学习研究与免责声明（使用前请阅读）**
> - 本项目 **Sirène** 为纯个人编程学习与技术研究作品（用于研究跨平台 C++17 桌面开发、音频解码流水线与纯 CPU 软渲染 UI），**非商业用途**。
> - 本项目与任何硬件厂商、游戏机厂商、音乐平台或第三方公司均**无任何官方关联、授权或合作关系**，不包含任何非授权系统修改或绕过机制；任何因下载、编译、分发或运行本项目产生的一切风险与责任均**由使用者自行承担，与原作者无关**。详情请见 [DISCLAIMER.md](DISCLAIMER.md)。
> - 软件本身不内置、不提供、不传播任何受版权保护的音频资源，仅用于播放用户自身合法持有的本地音频文件。

纯软件渲染、无需 GPU：**Windows / Linux / macOS 三个 PC 版本采用 C++17 重构构建**，融合流畅现代苹果风格动效与轻量毛玻璃视觉：**播放/暂停形变按钮 × 4 柱频谱均衡动效 × 5 层旋转流体动态封面背景 × 阻尼弹簧瀑布流逐句歌词（含间奏呼吸律动点）× 无框高清封面网格与现代化云端扩展 Hub**；支持自定义壁纸、浅/深色平滑主题过渡、触控惯性滚动优化以及 Windows 任务栏悬停缩略图播控。

## 桌面端界面预览（v1.2.8）

| 深色模式 · 4 柱动效与精选横幅 | 浅色模式 · 曲目列表视图 |
| :---: | :---: |
| ![桌面深色曲库](previews/desktop_land.png) | ![桌面浅色列表](previews/desktop_day.png) |

| 分组毛玻璃设置页（外观 / 播放 / 曲库 / 诊断） | 沉浸式流体背景与弹簧流式歌词页 |
| :---: | :---: |
| ![桌面设置页](previews/desktop_set.png) | ![桌面沉浸歌词](previews/desktop_full.png) |

## 支持平台与版本（v1.2.8）

| 平台 | 产物文件名 | 渲染与音频后端 | 交互与特性亮点 |
|---|---|---|---|
| **Windows x64** | `Sirene-v1.2.8-Windows-x64.exe` | C++17 + Win32 GDI 原生物理像素（DPI V2）+ WinMM + Media Foundation | 缩放/全屏/高分屏 1:1 物理像素锐利渲染；`Segoe UI` + `Microsoft YaHei UI` 原生清晰排版；支持触控惯性滚动、桌面快捷方式自动创建、**任务栏悬停缩略图播控栏（上一首 / 播放·暂停 / 下一首）**、UTF-16 拖放、M4A/AAC/MP4/ALAC/OPUS/WMA 解码 |
| **Linux x86_64** | `Sirene-v1.2.8-Linux-x86_64.tar.gz` | C++17 + SDL2 HiDPI + SDL2 Audio + FFmpeg 管道 | 原生 HiDPI 渲染、`SDL_SetWindowIcon` 图标、支持常见有损/无损音频格式 |
| **macOS Apple Silicon** | `Sirene-v1.2.8-macOS-arm64.zip` | C++17 + SDL2 Retina + SDL2 Audio + AudioToolbox | 独立 `Sirène.app`（Retina 物理像素锐利渲染 + CoreAudio 原生解码） |
| **Switch (Homebrew `.nro`)** | `Sirene-v1.2.8-Switch.nro` | C + `libnx` + SDL2 + `dr_libs` | 纯开源自制程序环境运行（无任何官方 SDK）；支持 1080p/720p 渲染、触屏与手柄按键控制、本地无损解码与 `.lrc` 歌词 |

## 核心学习与实现特性（v1.2.8）

- **现代微交互与高帧率动画**：
  - **播放/暂停平滑形变（Play/Pause Morph）**：左右半三角四顶点平滑插值为双圆角暂停竖条，搭配进度条悬停放大与浮动时间预览提示、红心点击回弹动效以及页面切换淡入微抬过渡；
  - **4 柱动态均衡器与流式封面背景**：在正在播放的专辑封面与歌曲行上叠加 4 柱非对称阻尼律动条；歌词页背景升级为 5 层旋转/轨道采样、有机噪声域畸变、低频音能脉动与 1.2s 切歌交叉淡化，叠加高可读性深色渐变遮罩；
  - **弹簧瀑布流逐句歌词与间奏呼吸点**：引入五次多项式平滑曲线错峰波浪延迟与阻尼谐波弹簧物理定位，纯白高亮当前句并淡化非当前句至 `<= 35%` 透明度，上下边缘像素级渐隐平滑融入背景；
- **轻量化自适应布局 × 半透明毛玻璃视觉**：
  - 宽窗口（`>= 1000px`）展开约 `210px` 轻量侧边栏（无边框搜索框、4 项精简导航配合 `bg-white/8` 选中底色与左侧 `3px` 蓝色指示竖条、「我的目录」标题右侧快捷刷新与 `+` 图标、底部三合一纯图标栏及悬停 Tooltip），窄窗口自动切换为 `76px` 紧凑图标栏；
  - 底部悬浮播放栏与顶部精选横幅完整联动本地与在线扩展曲目（支持进度拖拽跳转、上一首/下一首队列切换、封面同步与任务栏媒体键）；
- **现代云端扩展 Hub × 沙盒 Lua 5.4 SDK**：
  - 桌面端内建沙盒化 **Lua 5.4 扩展插件引擎**（提供 `http`/`sha1`/`aes_cbc_encrypt`/`timestamp`/JSON/KV/QR 等基础原语），主仓库零内置任何第三方平台私有域名或协议；
  - 云端扩展页集成独立搜索栏、标准/极高/无损音质分级切换、高清封面歌单卡片、Shimmer 骨架屏加载态以及扫码/Cookie 凭证双模式弹窗（支持 `Ctrl+V` 一键完整粘贴），详见 [插件开发指南 (PLUGIN_SDK.md)](docs/PLUGIN_SDK.md)；
  - 支持点击任意歌词行跳转、外部 `.lrc` 拖放与快捷键（`S` 搜索、`V` 视图切换、`C` 浅深主题、`B` 背景壁纸、`D` 设置、`M` 播放模式、`F11` 全屏）。

## 安装与使用（桌面端）

1. 前往 [**Releases v1.2.8 页面**](../../releases/tag/v1.2.8) 下载对应平台包（本工具仅做本地测试与技术学习使用）：
   - **Windows**：双击 `Sirene-v1.2.8-Windows-x64.exe` 即可运行（首次运行自动在桌面生成带图标快捷方式；支持拖入文件夹、音频、`.lrc` 歌词或背景图片）。
   - **Linux**：解压 `Sirene-v1.2.8-Linux-x86_64.tar.gz` 后运行 `./Sirene`（需系统已安装 `libsdl2`，可选 `ffmpeg`）。
   - **macOS**：解压 `Sirene-v1.2.8-macOS-arm64.zip` 打开 `Sirène.app`（如系统拦截可执行 `xattr -cr Sirène.app`）。
2. 自动扫描用户 `Music` 文件夹，也可点击左侧边栏 **`我的目录 +`** 或在 **设置 → 曲库** 中添加/移除任意合法持有的本地音频目录。

## 补充：Switch 版 `.nro` 独立文件使用与免责说明（开源自制环境）

> [!IMPORTANT]
> **Switch 版（Homebrew `.nro`）专项学习研究与免责声明**
> - 本分支产物 `Sirene-v1.2.8-Switch.nro` 仅采用 `libnx`、`SDL2`、`dr_libs` 等**纯开源自制程序工具链**编写与构建，**完全不包含任何官方商业 SDK、专利固件代码或系统破解/绕过机制**，仅供个人嵌入式与跨平台音视频渲染技术学习交流。
> - 本项目与任何游戏机制造商及第三方公司**没有任何官方关联、合作或授权关系**；软件本身亦**不内置、不提供任何受版权保护的音频内容**，仅用于读取与播放用户本人合法持有的本地音频文件。
> - 下载、放置、运行 `.nro` 文件及所使用自制环境产生的一切技术、设备或法律合规责任**均由使用者自行承担，与原作者及仓库维护者无关**（详见 [DISCLAIMER.md](DISCLAIMER.md)）。

1. **放置 `.nro` 文件**：前往 [**Releases v1.2.8 页面**](../../releases/tag/v1.2.8) 下载对应版本的独立 `.nro` 文件（`Sirene-v1.2.8-Switch.nro`），放入存储卡 `sdmc:/switch/Sirene.nro`（或 `sdmc:/switch/Sirene/Sirene.nro`）。
2. **准备本地音乐与歌词**：将个人合法持有的本地音频文件（支持 MP3 / FLAC / WAV / OGG / AIFF / M4A 等）及同名 `.lrc` 歌词放入 `sdmc:/music/` 目录（程序亦会自动扫描存储卡非系统目录下的音频文件）。
3. **启动与使用**：在开源自制程序菜单（Homebrew Menu）中找到并运行 `Sirène`，即可通过触控或物理按键浏览曲库、搜索歌曲、调节进度并播放本地音乐与外挂/内嵌歌词。

| `.nro` 曲库浏览界面 | `.nro` 播放与歌词界面 |
| :---: | :---: |
| ![nro 曲库界面](previews/list_land.png) | ![nro 播放界面](previews/player_land.png) |

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
