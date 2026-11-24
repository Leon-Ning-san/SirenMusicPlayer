<p align="center">
  <img src="icon.jpg" width="128" alt="Sirène Icon" />
</p>

<h1 align="center">Sirène · Switch 本地音乐播放器（v1.2.2）</h1>

<p align="center">
  为破解版 Nintendo Switch（Atmosphère / Hekate）打造的本地无损音乐播放器（<code>.nro</code>）<br/>
  <b>乔布斯 iPod Click Wheel × 现代 Apple 悬浮毛玻璃 × Google Material You 融合设计</b><br/>
  <b>无手柄 100% 全触屏直控（含触屏退出 / 音量滑块）+ 专属手柄快捷映射</b>
</p>

纯软件渲染、无需 GPU，融合乔布斯经典 iPod 转盘骨架、现代 Apple 流体光晕卡片与 Google Material You 色调容器体系，所有切页、切歌、旋转与弹窗均采用轻量丝滑**淡入动画**。拆下 Joy-Con 当纯触屏随身听也能完整操控一切功能。

## 界面预览

| 横屏双栏（冰霜白 Apple × M3） | 横屏全屏播放（钛空黑 · 封面流体光晕 + 双语歌词） |
| :---: | :---: |
| ![横屏双栏](previews/list_land.png) | ![横屏全屏播放](previews/player_land.png) |

| 竖屏单向 iPod 形态 | 竖屏全屏播放（冰霜白 · 歌词与转盘） |
| :---: | :---: |
| ![竖屏列表页](previews/list_port.png) | ![竖屏播放页](previews/player_port.png) |

| 钛空黑主题 | 青瓷绿 Material You 主题 |
| :---: | :---: |
| ![钛空黑主题](previews/list_blue.png) | ![青瓷绿主题](previews/list_sage.png) |

## 核心特性（v1.2.2）

- **法语品牌升级 `Sirène`（塞壬）**：全面统一应用名称、NACP 元数据与界面视觉为法语 **`Sirène`**。
- **乔布斯 iPod × 现代 Apple × Material You 融合 UI**：
  - 摒弃复古厚黑框与生硬分割线，采用无边框圆角悬浮视窗、圆角超椭圆封面、Apple Music 专辑流体环境光晕与 Material You 动态色调胶囊。
  - **经典 Click Wheel 触控转盘**：支持点按 `MENU / |◀◀ / ▶▶| / ▶❚❚ / 中心键`，以及**顺/逆时针环形滑动选曲或调音量**。
  - **三套现代融合主题**：**冰霜白 (Apple Frost × M3)**、**钛空黑 (Titanium Dark)**、**青瓷绿 (Material You Sage)**，全场景配备轻量**淡入动画**。
- **无手柄 100% 纯触屏闭环**：
  - 常驻触控 **`退出`** 胶囊（带防误触确认弹窗）、触控 **`音量`** 胶囊（支持滑块拖拽、`−/+` 微调、一键静音）、`横/竖屏`、`搜索`、`扫描`、`主题`、`自检` 全部可单指触屏操作。
- **深度升级全能歌词与封面引擎**：
  - 修复左上角图标与非正方形封面步长/裁切问题；新增 FLAC 大端序 `PICTURE`、OGG Base64 封面、ID3v2.4 去非同步化封面及同目录 `cover.jpg / folder.jpg` 支持。
  - 支持外挂 `.lrc`（含 `歌名 - 歌手.lrc`、去曲号匹配、模糊匹配、网易云/QQ JSON 转义换行、双语同时间轴合并）与 MP3（`USLT / SYLT / TXXX / COMM / APEv2`）、FLAC/OGG（`LYRICS / LYRICS-XXX` 多语言后缀）、WAV 内嵌及二进制特征兜底扫描。

## 版本归档（独立 Release）

- **v1.2.2（最新）**：`Sirène` 法语命名 + 乔布斯 Click Wheel × 现代 Apple × Material You 融合 UI + 无手柄全触控闭环 + 深度歌词/封面修复 + 专属手柄映射。
- **v1.2.1**：现代 Hi-Res 便携 MP4 双栏版。
- **v1.2.0**：复古乔布斯 iPod Classic 版。
- **v1.1.0**：Material You 初版。

## 安装与启动说明

1. 前往 [**Releases 页面**](../../releases) 下载 `Sirene.nro`（或兼容名 `Siren.nro`），放入 SD 卡 `sdmc:/switch/Sirene.nro`。
2. 音频与 `.lrc` 歌词放入 `sdmc:/music/` 或 SD 卡任意非系统目录。
3. **直接启动**：在 Switch 主菜单按住 **`R` 键打开任意游戏（Title Override）** 进入 hbmenu 启动 **Sirène**（DBI / Tinfoil 仅安装 `.nsp`，不能直接安装 `.nro`）。
4. **桌面前端图标**：已内置标准 `256×256` 基准 `4:2:0 JFIF` 图标与 UTF-8 `Sirène` NACP，可通过机内 **Sphaira -> Install Forwarder** 或电脑端 **SAK / Nro2Nsp + 本机 `prod.keys`** 生成 `.nsp` 转发器安装到主菜单。

## 手柄专属按键映射

| 按键 | 列表页 | 全屏播放页 |
|---|---|---|
| **`A`** | **就地播放 / 暂停选中歌曲（不跳进全屏）** | 暂停 / 继续 |
| **`X`** | **淡入进入全屏歌词页** | **淡入返回曲库列表** |
| **`Y`** | **一键切换横屏 / 单向竖屏** | **一键切换横屏 / 单向竖屏** |
| **`左手柄方向键 ← / →`（非摇杆）或 `L / R`** | **切换上一首 / 下一首** | **切换上一首 / 下一首** |
| `↑ / ↓` 或 `左摇杆 ↑ / ↓` | 上下移动选曲光标 | 调节音量（`↑ / ↓`） |
| `左摇杆 ← / →` | —（不误触切歌） | 快退 / 快进 10 秒 |
| `右摇杆 ↑ / ↓` | 调节音量 `− / +` | 调节音量 `− / +` |
| `ZL` | 收藏 / 取消收藏选中歌曲 | 收藏 / 取消收藏当前歌曲 |
| `ZR` | 切换主题（冰霜白 / 钛空黑 / 青瓷绿） | 切换主题 |
| `− (Minus)` | 打开 Switch 软键盘搜索 | 打开软键盘搜索 |
| `B` | 呼出退出确认框（再按 `A` 退出） | 返回列表（音乐继续播放） |
| `+ (Plus)` | 直接退出程序 | 直接退出程序 |

## 从源码构建

需要 [devkitPro](https://devkitpro.org/) 工具链（`switch-dev`、`switch-sdl2`）：

```bash
make clean && make
```

## 许可

- 本项目源码：MIT
- 内嵌字体 [Noto Sans SC](https://fonts.google.com/noto)：SIL Open Font License
- 解码与图像单头文件库（`dr_libs`、`stb`）：Public Domain / MIT
