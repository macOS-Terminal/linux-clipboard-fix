# 提示词 B：宣传介绍视频 + 封面（v1.1.4 含 Waydroid 支持与片尾特别鸣谢）

> 把下面 `text` 代码块里的**整段内容**（从"你要为一个已完成的 Linux 开源项目..."到最后一条约束）复制给负责做视频的 AI，并同时把你的「`macOS终端`」头像图片发给它。
> 接收方不共享本会话上下文，该提示词已自包含全部所需事实。

## 适用于

- 75–90 秒技术演示短片，对标 BV1MKa66VEPS 的叙事结构
- 必须是真实录屏 + 原理图解 + 修复前后对比（涵盖 **X11/XWayland 应用 ⇄ Wayland 原生应用 ⇄ Waydroid 安卓容器应用** 的剪贴板互通）
- **含片尾特别鸣谢（Special Thanks）**：按序致谢 `macOS终端`（使用用户直接提供的头像）、`DeepSeek V4.1 Flash`、`Claude Opus 5.5`、`Gemini 4 Argon`（3 个 AI 的 Logo 由制作视频的 AI 自行从各自官网拉取 SVG 矢量图）
- **含封面**：1920×1080 与 1146×717 两版 + 可编辑源文件

## 参照视频（已核实）

| 项 | 值 |
|---|---|
| 链接 | https://www.bilibili.com/video/BV1MKa66VEPS/ |
| 标题 | 《我修复了 Linux QQ》 |
| UP 主 | 林长枫Shorin709 |
| 时长 | 83 秒 |
| 分辨率 | 1920×1080 |
| 对应项目 | https://github.com/SHORiN-KiWATA/linuxqq-wayland-fix |

## 使用前的环境提示（给派发者，不给接收方）

- 发送下方提示词给制作视频的 AI 时，请**附带发送一张「`macOS终端`」的头像图片**，供其在片尾致谢画面第一位使用。
- 本机无法逐帧分析参照视频：`video_probe` 对该链接返回 **HTTP 412 限流**，仅有 API 元数据。提示词里对参照视频风格的描述是**基于同题材推断**，非逐帧观测。若风格细节重要，请人工看一遍后再补充。
- 提示词内给出的实测数字（4/4、9/9、12/12 等）来自本机 niri + xwayland-satellite 环境，已在提示词中要求接收方若无法复现则标注为"开发者实测"。

---

```text
你要为一个已完成的 Linux 开源项目制作一支宣传介绍视频（含封面），风格参照 B 站（Bilibili）上同类项目的技术演示短片。

## 参照基准（重要）

参照视频：https://www.bilibili.com/video/BV1MKa66VEPS/
标题《我修复了 Linux QQ》，UP 主「林长枫Shorin709」，时长 83 秒，1920×1080，项目 https://github.com/SHORiN-KiWATA/linuxqq-wayland-fix

它的成功要素是要模仿的重点（不是模仿它的界面，而是模仿它的叙事结构与节奏）：
- 开头痛点直击：先展示「坏」的状态，让观众立刻对号入座
- 中间极简原理：用一个直观示意图讲清为什么会坏、修法是什么，不堆术语
- 全程真实录屏：终端命令、真实报错、真实效果，不摆拍
- 结果对比鲜明：修复前后并排，一眼看出差别
- 结尾给仓库地址 + 一句可复制的安装命令 + 高质感片尾特别鸣谢（Special Thanks）画面
- 总时长控制在 90 秒内，信息密度高，不啰嗦

## 项目事实（已核实，直接采用）

- 项目名：cbfix / cbfix-bridge
- 当前版本：v1.1.4（GitHub Release v1.1.4）
- 目标仓库（视频与封面都要出现）：https://github.com/macOS-Terminal/linux-clipboard-fix
- 一句话定位：修复 Linux Wayland 桌面上 X11（XWayland）应用、Waydroid（Android 容器应用）与 Wayland 原生应用之间剪贴板不互通的问题
- 受影响的具体场景（观众最有共鸣的钩子，请保持以下通用分类表述，不要点名特定游戏或专有软件名）：
    1. 跑在 XWayland 上的各类 X11 应用（Java AWT/Swing/OpenJFX 桌面程序、使用 xclip/xsel 的 CLI 工具、传统 X11 GUI 软件）
    2. 跑在 Waydroid 容器里的 Android 应用（安卓容器内应用与宿主机 Wayland 原生应用、X11 应用之间跨环境复制粘贴失灵）
    3. Wayland 原生应用（浏览器、原生终端、编辑器等）与上述 X11 / Waydroid 环境之间交换文本或图片
- 典型受害环境：niri + xwayland-satellite、sway、Hyprland 等 Wayland 桌面及 Waydroid 容器环境，装了 xclip/xsel 也无法跨边界打通
- 病症：在 X11 应用或 Waydroid 安卓应用里 Ctrl+C 复制，切到系统别处粘贴不出来；在 Wayland 原生应用里复制，X11 应用或 Waydroid 安卓应用粘贴为空；X11 应用与 Waydroid 安卓应用之间更是无法直接互粘
- 根因（要用大白话讲清）：
    Wayland 规定，往系统剪贴板写内容，必须携带「你刚刚碰过键盘/拥有焦点」的凭证（focus serial）。
    后台的桥接程序或跨环境同步通道拿不到有效焦点凭证，或者选区所有权与 MIME 协商在合成器、XWayland 与 Waydroid 之间断链：
    xwayland-satellite 的写入被合成器静默丢弃（日志明明打印了「CLIPBOARD set from X11」，系统剪贴板却纹丝不动），而反向（Wayland→X11）它压根没实现；同时 Waydroid 安卓容器与 X11 / Wayland 宿主机之间也常因后台权限壁垒与剪贴板通道隔离而失灵。
- 解法：Wayland 有一个专门给剪贴板管理器/桥接器用的协议 ext-data-control-v1，它不需要焦点凭证。
    cbfix-bridge（v1.1.4）基于 ext-data-control-v1 + X11 ICCCM（含 INCR 大负载分片）+ Waydroid 剪贴板同步支持，在后台实现 Wayland 原生应用 ⇄ X11（XWayland）应用 ⇄ Waydroid 安卓容器应用 的双向/三方无损互通，并妥善管理 X11 CLIPBOARD 所有权。
- 单文件 C11 程序，无运行时框架依赖，提供 v1.1.4 的 Arch / Deb / RPM 安装包与 systemd 用户服务，MIT 许可。

## 实测数据（可上字幕，但要明确标注为实测）

- 自检 4/4 通过
- 功能回归 9/9（双向、Unicode/emoji、多行、空负载、20 次快速交替、回环抑制）
- 大负载 100 KB–5 MB 逐字节比对 12/12
- 4 个读者并发读 5 MB，4/4 字节一致
- 读进程被 kill -9 后守护进程存活并恢复
- Wayland 原生应用 ⇄ X11（XWayland）应用 ⇄ Waydroid 安卓容器应用 跨端剪贴板互通

## 视频规格

- 时长：75–90 秒（对标参照视频的 83 秒）
- 分辨率 1920×1080，16:9，30 fps
- 语言：中文旁白/字幕（技术名词保留英文）
- 必须包含真实录屏；若无法录屏，用真实终端截图序列代替，并在成片说明中注明哪些镜头是示意图

## 必须包含的内容（六个段落缺一不可）

1. 0–10s  痛点开场：在 X11（XWayland）应用或 Waydroid 安卓容器应用里复制，切到 Wayland 原生应用（或反向/跨端）粘贴为空。屏幕直接展示跨环境复制粘贴失败的真实痛点
2. 10–25s 病症复盘：展示 Wayland 原生应用、X11（XWayland）应用、Waydroid 安卓容器应用之间剪贴板各自为政、互不连通的真实输出（如 wl-paste 与 X11 CLIPBOARD / Waydroid 侧结果不一致）
3. 25–50s 原理图解：Wayland 的 focus serial 焦点凭证限制 → 后台桥为何被静默丢弃、X11 与 Waydroid 为何沦为剪贴板孤岛 → cbfix-bridge v1.1.4 如何通过 ext-data-control-v1 免凭证协议将 Wayland 原生 ⇄ X11/XWayland ⇄ Waydroid 三方彻底打通
4. 50–70s 修复演示：v1.1.4 安装命令、systemctl --user enable --now cbfix-bridge.service 启用、修复前后对比（演示 Wayland 原生应用、X11 应用、Waydroid 安卓容器应用之间丝滑双向/三方互粘）
5. 70–82s 收尾总结：仓库地址 https://github.com/macOS-Terminal/linux-clipboard-fix 与 Release v1.1.4 醒目展示 + 一行安装命令 + 项目卖点（单文件 C11 / Wayland+X11+Waydroid 互通 / 双向无损 / 零框架依赖）
6. 82–90s 片尾特别鸣谢（Special Thanks / Credits，强制要求）：
   在视频片尾（最后 5–8 秒）必须设计一页质感高级的「特别鸣谢 / Special Thanks」独立画面，按以下顺序感谢 4 位贡献者（名字必须一字不差）：
   (1) `macOS终端`
       - 角色标签：项目作者 / Project Creator
       - 头像来源：**由用户直接提供给你（制作视频的 AI）**。请直接使用用户在对话中提供给你的头像图片，排在第一位（或主视觉C位），裁切为精致的圆形或圆角矩形高清头像展示，搭配名称 `macOS终端`。
   (2) `DeepSeek V4.1 Flash`
       - 角色标签：AI Engineering Contributor
       - Logo 来源：**请你自行前往 DeepSeek 官方网站拉取官方 SVG 矢量图 Logo**（保存在 `assets/logos/deepseek.svg`，严禁手绘劣质替代图形或使用模糊锯齿位图），搭配标准名称 `DeepSeek V4.1 Flash`。
   (3) `Claude Opus 5.5`
       - 角色标签：AI Engineering Contributor
       - Logo 来源：**请你自行前往 Anthropic / Claude 官方网站拉取官方 SVG 矢量图 Logo**（保存在 `assets/logos/claude.svg`，严禁手绘劣质替代图形或使用模糊锯齿位图），搭配标准名称 `Claude Opus 5.5`。
   (4) `Gemini 4 Argon`
       - 角色标签：AI Engineering Contributor
       - Logo 来源：**请你自行前往 Google / Gemini 官方网站拉取官方 SVG 矢量图 Logo**（保存在 `assets/logos/gemini.svg`，严禁手绘劣质替代图形或使用模糊锯齿位图），搭配标准名称 `Gemini 4 Argon`。
   - **片尾视觉排版要求**：4 位贡献者（1 位人类开发者 + 3 个大模型）在片尾以高颜值深色卡片阵列或横排对称布局展示（每个单元含：高清头像/官方矢量 Logo + 一字不差的名称 + 角色标签），画面干净大气、对比清晰。

## 封面（必须做，用户明确要求）

- 尺寸 1920×1080（另出一版 1146×717 适配 B 站投稿页）
- 必须出现：项目名 cbfix（可标注 v1.1.4）、一句痛点式标题（如「X11 与 Waydroid 的剪贴板，终于通了」）、仓库地址 macOS-Terminal/linux-clipboard-fix
- 视觉建议：左右分屏或三方连通构图——左「坏」（X11 / Waydroid / Wayland 剪贴板断联，用警示色），右「好」（三方内容实时互通，用正向色），中间一个连通的桥形/双向箭头符号；整体高对比、大字、缩略图尺寸下仍可读
- 字体不要用系统默认衬线；中文字体选思源黑体/Noto Sans CJK 一类，确保无「豆腐块」
- 同时交付封面源文件（如 SVG 或 PPTX/分层 PNG），便于后续修改

## 交付物

1. 成片：MP4（H.264 + AAC），1080p
2. 封面：两张 PNG（1920×1080、1146×717）+ 可编辑源文件
3. 视频脚本表：一个表格，列出 时间码 / 画面 / 旁白字幕 / 素材来源（**必须包含第 6 段「82–90s 片尾特别鸣谢」的完整时间码、4 位贡献者名单及头像/官网 SVG Logo 素材来源说明**）
4. 一份可直接用于 B 站投稿的简介文案，包含：一句钩子、项目要解决的问题（含 X11/XWayland 与 Waydroid 剪贴板互通）、原理一句话、v1.1.4 仓库链接 https://github.com/macOS-Terminal/linux-clipboard-fix 、片尾特别鸣谢名单（`macOS终端`、`DeepSeek V4.1 Flash`、`Claude Opus 5.5`、`Gemini 4 Argon`）、标签建议
5. 所有素材（截图、录屏、示意图源文件、用户提供的 `macOS终端` 头像、从官网拉取并存放在 `assets/logos/` 下的 3 个 AI 官方 SVG 矢量图）放在 `assets/` 目录并列出清单

## 验收标准

1. 成片时长落在 75–90 秒，`ffprobe` 输出真实时长与分辨率作为证据
2. 六个内容段落齐全（含片尾特别鸣谢段），给出每段的时间码
3. 仓库地址在片中至少出现 2 次且清晰可读（贴出对应时间点的静帧截图）
4. 封面两张尺寸正确（`identify` 或 `ffprobe` 输出为证），中文无乱码/豆腐块
5. 原理与演示段必须把「focus serial 限制」、「ext-data-control-v1 绕过」以及「X11（XWayland）应用 + Waydroid 安卓容器应用 + Wayland 原生应用的剪贴板互通」讲明白，不能只喊口号
6. **片尾致谢画面与矢量 Logo 硬性验收**：提供**片尾致谢静帧截图**，并贴出 `ls -lh assets/logos/` 与 SVG 文件头部校验输出，证明：① 片尾画面按顺序清晰展示 `macOS终端`（使用用户提供的头像）、`DeepSeek V4.1 Flash`、`Claude Opus 5.5`、`Gemini 4 Argon`（名字一字不差）；② 3 个 AI 的官方 SVG 矢量图文件已从官网拉取并放入 `assets/logos/`。

## 输出格式（回复控制在 35 行以内）

- 交付物清单（路径 + 时长/尺寸，含 `assets/logos/` 下的 SVG 矢量图文件）
- 六段内容的时间码表（含片尾特别鸣谢段）
- 6 条验收证据（每条 1–2 行真实输出 + 片尾致谢静帧截图说明）
- 未验证事项（例如未能取得某段真实录屏，明确说明用示意图代替）

## 约束

- 不要开子代理。
- 不要虚构实测数据；本提示词给出的数字可以引用，但若你无法复现，就在片中标注为「开发者实测」而非你亲测。
- 严格保持通用软件分类表述（X11/XWayland 应用、Waydroid 安卓容器应用、Wayland 原生应用），不要在视频或文案中点名特定第三方游戏启动器、游戏或特定专有 CLI 工具名称。
- 片尾致谢中，`macOS终端` 必须使用用户提供给你的头像；`DeepSeek V4.1 Flash`、`Claude Opus 5.5`、`Gemini 4 Argon` 的 Logo 必须自行去对应官网拉取真实 SVG 矢量图放入 `assets/logos/`，严禁用简陋几何图形敷衍。
- 不要使用未经授权的第三方素材（音乐、字体、图标）；音乐若无版权来源，用无音轨或使用明确可商用的曲库并在简介注明。
- 不要伪造项目不在其位的功能（例如不要宣称支持文本与 PNG/BMP 图片剪贴板之外的能力）。
```
