<p align="center">
  <img src="icon.jpg" width="128" alt="Sirène Icon" />
</p>

<h1 align="center">Sirène · 跨平台本地音乐播放器（v1.2.4）</h1>

<p align="center">
  <b>Nintendo Switch（<code>.nro</code>）· Windows · Linux · macOS</b><br/>
  <b>乔布斯 iPod Click Wheel × 现代 Apple 悬浮毛玻璃 × Google Material You 融合设计</b><br/>
  <b>桌面端原生单向竖屏随身听（720×1280 等比缩放 · 实时歌词栏 · Material You 波浪进度条）</b>
</p>

纯软件渲染、无需 GPU，融合乔布斯经典 iPod 转盘骨架、现代 Apple 流体光晕卡片与 Google Material You 表现力色调与**粗波浪动态进度条**，所有切页、切歌、旋转与弹窗均采用轻量丝滑 **60 FPS 淡入动画**。Windows / Linux / macOS 桌面端专注于简洁顺手的**经典竖屏随身听**体验，支持列表内嵌实时同步歌词、`.lrc` 直接拖入窗口、WAV / MP3 / FLAC / OGG 无损音频与多形态歌词/封面解析。

## 界面预览

| 桌面竖屏列表（实时歌词栏 + 列表严格裁剪） | 竖屏全屏播放（Material You 粗波浪进度条） |
| :---: | :---: |
| ![桌面竖屏列表](previews/list_port.png) | ![竖屏播放页](previews/player_port.png) |

| Switch 横屏双栏（冰霜白 Apple × M3） | Switch 横屏全屏播放（钛空黑 · 流体光晕 + 波浪条） |
| :---: | :---: |
| ![横屏列表页](previews/list_land.png) | ![横屏全屏播放](previews/player_land.png) |

| 钛空黑主题 | 青瓷绿 Material You 主题 |
| :---: | :---: |
| ![钛空黑主题](previews/list_blue.png) | ![青瓷绿主题](previews/list_sage.png) |

## 多平台支持（v1.2.4）

| 平台 | 产物文件名 | 渲染与音频后端 | 窗口 / 形态说明 |
|---|---|---|---|
| **Windows x64** | `Sirene-v1.2.4-Windows-x64.exe` | 原生 Win32 GDI + WinMM | 单文件免安装，零外部 DLL 依赖；默认 `468×832` 锁定纵横比竖屏窗口 |
| **Linux x86_64** | `Sirene-v1.2.4-Linux-x86_64.tar.gz` | SDL2 逻辑缩放 + SDL2 Audio | 竖屏窗口（支持 HiDPI / 自由缩放），原生支持 `zenity` / `kdialog` 选目录 |
| **macOS Apple Silicon** | `Sirene-v1.2.4-macOS-arm64.zip` | SDL2 逻辑缩放 + SDL2 Audio | 独立 `Sirène.app`（已内置 SDL2/SDL3 运行库，开箱即用），原生支持文件夹选择器 |
| **Nintendo Switch** | `Sirene.nro` | Switch 帧缓冲 + SDL2 | 横屏/竖屏自动或手动切换，无手柄纯触屏闭环 + 专属按键映射 |

## 核心特性（v1.2.4）

- **列表视口严格像素裁剪（修复封面越界）**：为软光栅画布加入全局裁剪矩形（`Canvas` clip），列表上下滚动时最顶端与最底端歌曲的**专辑封面、高亮边框、文字与收藏心形**严格限制在列表视窗内部，绝不再突破边框覆盖顶栏或胶囊标签。
- **Material You 粗波浪动态进度条**：全屏播放页与桌面底部播控栏全面升级为 **Google Material 3 Expressive 风格粗波浪进度条**（法向等距抗锯齿正弦波 + 平滑收敛圆头 + 4px 间距竖向胶囊游标 + 播放时实时流体波动 / 暂停自动归平）。
- **PC 三平台歌词强化与列表实时歌词栏**：
  - Windows / Linux / macOS 竖屏列表底部新增 **Apple × M3 实时歌词灵动栏**，无需切进全屏即可同步高亮当前歌词；点击卡片或 `歌词全屏`（`F` / `X`）可淡入展开多行居中歌词页。
  - 增强 `.lrc` 与内嵌解析：支持 `歌手-歌名.lrc` 无空格匹配、括弧后缀反向模糊匹配、`[mm:ss,xx]` 逗号毫秒、上级/当前 `Lyrics/lrc` 子目录扫描、非标准单字节 `\0` UTF-16 ID3 `USLT` 帧，并支持**直接将 `.lrc` 文件拖入桌面窗口即时加载**。
- **60 FPS 流畅引擎 + 融合设计**：
  - 时间驱动 Cubic Ease-Out 淡入动画 + Click Wheel 精灵缓存 + O(R) 扫描线光栅 + O(1) 字形哈希表；内置**冰霜白**、**钛空黑**、**青瓷绿**三套主题。

## 版本归档（独立 Release）

- **v1.2.4（最新 · Material You 波浪条 + 列表防溢出 + PC 歌词全面升级）**：修复滚动时顶部歌曲封面突破列表框问题，新增 Material You 粗波浪进度条、PC 端列表实时歌词栏、外部 `.lrc` 拖放与多处 ID3/模糊匹配增强，统一打包 Windows / Linux / macOS / Switch 于 `v1.2.4`。
- **v1.2.3**：新增 Windows / Linux / macOS 竖屏桌面版 + 60 FPS 流畅优化引擎。
- **v1.2.2**：`Sirène` 法语命名 + 乔布斯 Click Wheel × 现代 Apple × Material You 融合 UI + 无手柄全触控闭环 + 深度歌词/封面修复 + 专属手柄映射。
- **v1.2.1**：现代 Hi-Res 便携 MP4 双栏版。
- **v1.2.0**：复古乔布斯 iPod Classic 版。
- **v1.1.0**：Material You 初版。

## 安装与使用说明

### 桌面端（Windows / Linux / macOS）
1. 前往 [**Releases v1.2.4 页面**](../../releases/tag/v1.2.4) 下载对应系统的桌面版压缩包或单文件运行即可：
   - **Windows**：直接双击 `Sirene-v1.2.4-Windows-x64.exe`（可传入目录或音频路径作为启动参数）。
   - **Linux**：解压 `Sirene-v1.2.4-Linux-x86_64.tar.gz` 后运行 `./Sirene`（需系统已安装 `libsdl2` 运行库）。
   - **macOS**：解压 `Sirene-v1.2.4-macOS-arm64.zip` 后打开 `Sirène.app`（已内置 SDL2/SDL3 运行库；若遇系统拦截可运行 `xattr -cr Sirène.app`）。
2. **添加歌曲与歌词**：
   - 将歌曲和 `.lrc` 放入系统 **`音乐 / Music`** 文件夹（或程序旁的 `music/` 目录），启动自动扫描；
   - 点击列表顶栏 **`选目录`**（或按 `O` / `Ctrl+O`）选择任意音乐文件夹（配置自动保存在 `%APPDATA%\Sirene` / `~/.config/sirene` / `~/Library/Application Support/Sirene`）；
   - 支持直接将**音乐文件夹、音频文件或单独的 `.lrc` 歌词文件拖进播放器窗口**即可切换或加载。

### Nintendo Switch（`.nro`）
1. 从 [**Releases 页面**](../../releases) 下载 `Sirene.nro`（或兼容名 `Siren.nro`），放入 SD 卡 `sdmc:/switch/Sirene.nro`。
2. 音频与 `.lrc` 放入 `sdmc:/music/` 或 SD 卡任意非系统目录。
3. **直接启动**：在 Switch 主菜单按住 **`R` 键打开任意游戏（Title Override）** 进入 hbmenu 启动 **Sirène**。
4. **桌面前端图标**：内置标准 `256×256` 图标与 UTF-8 `Sirène` NACP，可通过机内 **Sphaira -> Install Forwarder** 或电脑端 **SAK / Nro2Nsp + 本机 `prod.keys`** 生成 `.nsp` 转发器安装。

## 操控与快捷键说明

### 桌面端键盘与鼠标
- **鼠标操作**：点击或拖拽界面按钮 / 列表 / 进度条 / 音量滑块；在 **Click Wheel 转盘**上顺/逆时针环形拖动可快速选曲或调音量；鼠标滚轮上下滚动列表或调音量；点击底部实时歌词播控栏即可打开全屏歌词。
- **快捷键**：
  - `↑ / ↓`：移动列表光标 / 全屏调节音量
  - `← / →`：切换上一首 / 下一首
  - `Space / Enter / A`：列表页**就地播放 / 暂停**选中歌曲；全屏页暂停 / 继续
  - `F / X`：淡入进入 / 退出全屏歌词页
  - `Ctrl+F`（macOS `Cmd+F`）/ `S` / `/`：打开内置搜索框（支持中英文输入过滤）
  - `Ctrl+O`（macOS `Cmd+O`）/ `O`：打开系统文件夹选择器更换曲库目录
  - `T`：切换主题（冰霜白 / 钛空黑 / 青瓷绿） · `H`：收藏切换 · `V`：音量面板 · `D`：歌词与系统诊断 · `M / Esc`：返回/退出确认

### Nintendo Switch 手柄专属映射
| 按键 | 列表页 | 全屏播放页 |
|---|---|---|
| **`A`** | **就地播放 / 暂停选中歌曲（不跳进全屏）** | 暂停 / 继续 |
| **`X`** | **淡入进入全屏歌词页** | **淡入返回曲库列表** |
| **`Y`** | **一键切换横屏 / 单向竖屏** | **一键切换横屏 / 单向竖屏** |
| **`左手柄方向键 ← / →`（非摇杆）或 `L / R`** | **切换上一首 / 下一首** | **切换上一首 / 下一首** |
| `↑ / ↓` 或 `左摇杆 ↑ / ↓` | 上下移动选曲光标 | 调节音量（`↑ / ↓`） |
| `左摇杆 ← / →` | —（不误触切歌） | 快退 / 快进 10 秒 |
| `右摇杆 ↑ / ↓` | 调节音量 `− / +` | 调节音量 `− / +` |
| `ZL / ZR` | `ZL` 收藏 · `ZR` 切换主题 | `ZL` 收藏 · `ZR` 切换主题 |
| `− (Minus)` / `B` / `+` | `−` 软键盘搜索 · `B` 退出确认 · `+` 退出 | `−` 搜索 · `B` 返回列表 · `+` 退出 |

## 从源码构建

### 桌面端（Windows / Linux / macOS）
使用仓库内的 [`Makefile.desktop`](Makefile.desktop)：
- **Windows（MinGW-w64 GCC，零额外依赖）**：
  ```bash
  mingw32-make -f Makefile.desktop
  ```
- **Linux（需要 `libsdl2-dev`、`pkg-config`）**：
  ```bash
  make -f Makefile.desktop
  ```
- **macOS（需要 Homebrew `sdl2`、`pkg-config`）**：
  ```bash
  make -f Makefile.desktop macos-app
  ```

### Nintendo Switch（`.nro`）
需要 [devkitPro](https://devkitpro.org/) 工具链（`switch-dev`、`switch-sdl2`）：
```bash
make clean && make
```

## 许可

- 本项目源码：MIT
- 内嵌字体 [Noto Sans SC](https://fonts.google.com/noto)：SIL Open Font License
- 解码与图像单头文件库（`dr_libs`、`stb`）：Public Domain / MIT
