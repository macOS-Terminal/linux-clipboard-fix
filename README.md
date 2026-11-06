# cbfix

**修复 Linux 上 X11 应用与系统剪贴板不互通的问题。**

在 Wayland 桌面上，跑在 XWayland 里的应用经常出现「复制了，别处粘贴不出来；别处复制的，它粘贴不出来」。`cbfix-bridge` 用一个极小的后台守护进程把 **Wayland 剪贴板** 和 **X11 CLIPBOARD 选区** 双向、无损地连起来。

典型受害者：**HMCL**（Minecraft 启动器，JavaFX/AWT 走 X11）、**Codex CLI**、以及一切真正运行在 X11 上的程序。

- 单文件 C11 程序，无运行时框架依赖
- 双向同步，文本与图片
- 支持大负载（INCR），实测到 5 MB 逐字节一致
- systemd 用户服务，`enable --now` 即用
- MIT 许可

---

## 症状长什么样

| 场景 | 表现 |
|---|---|
| 在 HMCL 里复制服务器地址 → 浏览器粘贴 | 粘贴出上一次的旧内容，或空白 |
| 在浏览器复制 → HMCL 粘贴 | HMCL 粘贴框空着 |
| 在 Codex CLI 里复制输出 → 编辑器粘贴 | 无内容 |
| 装了 `xclip` / `xsel` 也没用 | 它们只能在 X11 内部互通，跨不过 Wayland 边界 |

如果你把 `DISPLAY` 清空、只留 Wayland 环境，某些应用的剪贴板反而正常——这通常说明该应用其实跑在 Wayland 原生后端，问题不在你。

---

## 根因

这不是应用写错了，是 **Wayland 协议的访问控制**与 **XWayland 桥接实现的缺口**共同造成的。

### 1. Wayland 要求「焦点凭证」

`wl_data_device.set_selection()` 用于写入系统剪贴板，协议要求调用方**携带近期键盘焦点的 serial**。这个设计是为了防止后台程序偷偷改写剪贴板。

一个后台桥接进程永远拿不到这个 serial，因此它的写入请求会被合成器**静默丢弃**——不报错，也不生效。

`xwayland-satellite` 正是卡在这里。实测其日志：

```
xwayland-satellite: new CLIPBOARD owner: Window { res_id: 20971527 }
xwayland-satellite: CLIPBOARD set from X11        ← 它以为自己成功了
```

但与此同时 `wl-paste` 读到的仍是旧内容。再看反向：Wayland 已有内容时，X11 侧查询 `CLIPBOARD` 属主返回 `0x0`（无人持有）——**Wayland → X11 方向它根本没有实现**。

### 2. 解药：ext-data-control-v1

Wayland 提供了 `ext-data-control-v1` 协议，专门授权给剪贴板管理器与 X11 桥接器：它的 `set_selection` **不需要 serial**。这正是为这类场景设计的合法通道。

`cbfix-bridge` 建立在它之上，并自己持有 X11 侧的 `CLIPBOARD` 选区所有权。

### 3. 版本时间线

| 组件 | 要求 |
|---|---|
| niri | ≥ 25.05 |
| sway | ≥ 1.10 |
| Hyprland | ≥ 0.40 |
| KWin | ≥ 6.3 |
| GNOME/Mutter | 不适用（自带内建桥接，不需要本工具） |

xwayland-satellite 上游的修复仍在 `master` 分支、尚未发版。在此之前本工具即可解决问题；即使将来上游发版，Wayland → X11 方向的缺口仍需本工具填补。

---

## 安装

### 从源码构建

```bash
git clone https://github.com/macOS-Terminal/linux-clipboard-fix
cd linux-clipboard-fix
make
```

构建依赖：

| 发行版 | 命令 |
|---|---|
| Arch | `sudo pacman -S base-devel wayland wayland-protocols libx11 libxfixes` |
| Debian/Ubuntu | `sudo apt install build-essential libwayland-dev wayland-protocols libx11-dev libxfixes-dev pkg-config` |
| Fedora | `sudo dnf install gcc make wayland-devel wayland-protocols-devel libX11-devel libXfixes-devel pkgconf` |

### 一键安装（构建 + 安装 + 启用服务）

```bash
./scripts/install.sh
```

脚本会构建二进制、安装到 `/usr/local/bin`、安装 systemd 用户单元并执行 `enable --now`，完成后验证服务确为 `active`。

### 启用 systemd 服务

若你自行安装了二进制，手动启用：

```bash
systemctl --user enable --now cbfix-bridge.service
systemctl --user status cbfix-bridge.service
journalctl --user -u cbfix-bridge -f      # 实时日志
```

> **注意**：这是 systemd **用户级**服务（`--user`），不是 root 系统服务。它必须运行在你的图形会话里才能访问剪贴板。

### 不用 systemd 的替代方案

niri 用户可在 `~/.config/niri/config.kdl` 中加入：

```kdl
spawn-at-startup "cbfix-bridge"
```

### 自检

```bash
cbfix-bridge --selftest
```

输出四项 `[ OK ]` 表示环境完备：

```
cbfix-bridge selftest
  DISPLAY         : :0
  WAYLAND_DISPLAY : wayland-1
  [ OK ] X11 connected (XFixes yes)
  [ OK ] Wayland connected, ext-data-control-v1 bound
  [ OK ] Wayland publish -> read back
  [ OK ] X11 second client reads our selection
selftest passed
```

### 卸载

```bash
./scripts/uninstall.sh
```

---

## 工作原理

```
┌──────────────────────┐   ext-data-control-v1    ┌──────────────────┐
│   Wayland 剪贴板      │ ◄──────────────────────► │   cbfix-bridge   │
│   (合成器持有)         │    免 serial 读 / 写      │  (后台守护进程)   │
└──────────────────────┘                          └────────┬─────────┘
                                                           │ 持有 X11 CLIPBOARD
                                                           │ 完整 TARGETS / INCR
                                                   ┌───────┴────────┐
                                                   │ HMCL / Codex / │
                                                   │ 所有 X11 客户端 │
                                                   └────────────────┘
```

| 方向 | 触发 | 实现 |
|---|---|---|
| X11 → Wayland | XFixes `SetSelectionOwnerNotify` | 读取 X11 `CLIPBOARD`（优先 `image/png`，其次 `UTF8_STRING`/`TEXT`），经 `ext_data_control_device_v1.set_selection()` 发布 |
| Wayland → X11 | `ext_data_control_device_v1.selection` 事件 | 读取合成器 offer，声明 X11 `CLIPBOARD` 所有权，按 ICCCM 应答 `TARGETS`/`UTF8_STRING`/`TIMESTAMP`，大负载走 INCR |

几个关键设计：

- **回声抑制**：用 FNV-1a 内容哈希标记「自己刚推过去的负载」。该内容在另一端再出现时不再转发，杜绝无限循环。
- **粘滞所有权**：X11 侧无主时自动夺回 `CLIPBOARD`，保证内容常驻可粘贴。
- **传输钉住（pinning）**：每个进行中的 INCR 传输持有自己的一份负载快照。否则 Wayland 侧中途变更会让正在读取的应用被截断。
- **事件饥饿防护**：Xlib 的同步往返会把已到达的事件吞进内部队列，使 `poll()` 驱动的循环永久睡死。主循环因此**先 drain 再 poll**，以 `XPending()` 而非 `poll()` 作为「是否有 X 工作」的权威判据。
- **自愈**：INCR 传输若丢失 `PropertyDelete` 通知，会探测请求方属性的真实状态，区分「对方慢」与「通知丢了」，后者从断点续发，而非丢弃整个传输。

---

## 测试

```bash
make && ./build/cbfix-bridge --selftest   # 环境自检
bash tests/run-tests.sh                   # 功能回归 9 项
bash tests/run-bulk-tests.sh              # 大负载 / 并发 / 中断恢复
```

### 实测结果

环境：Arch Linux，niri 26.04 + xwayland-satellite 0.8.3，Xwayland 24.1。

| 套件 | 结果 |
|---|---|
| `--selftest` | **4/4** |
| 功能回归：Java/AWT 双向、Unicode/CJK/emoji、多行、空负载、20 次快速交替、回环抑制、存活 | **9/9** |
| 大负载 INCR 100 KB / 300 KB / 1 MB / 5 MB × 3 轮，逐字节比对 | **12/12** |
| 4 个读者并发读 5 MB | **4/4 字节一致** |
| 读者被 `kill -9` 后恢复服务 | 通过 |
| 30 次快速复制风暴（fcitx5 干扰下） | 两侧终态一致，零警告 |

Java 参考客户端（JVM 自带的 ICCCM INCR 实现，即 HMCL 的实际代码路径）在 300 KB / 1 MB / 5 MB 全部读到完整长度。

---

## 命令行

```
cbfix-bridge [选项]

  --interval MS   回退轮询 X11 属主的间隔（默认 120，最小 20）
  --verbose, -v   详细日志
  --selftest      检查环境后退出
  --status        打印两侧剪贴板当前状态后退出
  --help, -h      帮助
```

退出码：`0` 正常 / `1` 环境不可用或自检失败 / `2` 参数错误。

---

## 排障

| 症状 | 原因 | 处理 |
|---|---|---|
| 启动即退：`compositor does not offer ext-data-control-v1` | 合成器过旧或不支持 | 按上文版本表升级合成器 |
| 启动即退：`cannot connect to the Wayland compositor` | systemd 用户环境缺少 `WAYLAND_DISPLAY` | `systemctl --user import-environment WAYLAND_DISPLAY DISPLAY` 后重启服务 |
| 启动即退：`cannot open the X display` | 无 XWayland 或 `$DISPLAY` 为空 | 确认 Xwayland 在运行且 `echo $DISPLAY` 有值 |
| 自检 X11 项失败 | X 连接可用但选区异常 | 检查是否有其他程序长期霸占 `CLIPBOARD` |
| 粘贴出旧内容 | 粘贴端自身缓存（如某些 Java 版本缓存 Transferable） | 重启该应用；查桥日志确认内容已正确送达 |
| 大文件复制中断 | 若复现请附 `-v` 日志开 issue | `journalctl --user -u cbfix-bridge -b \| grep -iE "stalled\|lost"` |

`cbfix-bridge --status` 可快速查看两侧当前状态：

```
X11 CLIPBOARD owner : 0x400001
Wayland mime types  : text/plain;charset=utf-8, text/plain, UTF8_STRING, TEXT, STRING
Wayland content     : 42 bytes (text)
```

---

## 已知边界

- 同步文本与 `image/png` / `image/bmp`。专有 MIME（如 LibreOffice 内部格式）降级为纯文本。
- 只桥接 `CLIPBOARD`，**不桥接 PRIMARY**（X11 的中键粘贴选区）。
- 依赖合成器提供 `ext-data-control-v1`；GNOME/Mutter 不需要也不适用。
- 需要在图形会话内运行；纯 TTY 或 headless 环境下无意义。

---

## 项目结构

```
src/cbfix-bridge.c                    桥实现（C11）
protocols/ext-data-control-v1.xml     供应商化的协议定义
systemd/cbfix-bridge.service          systemd 用户单元
scripts/install.sh                    构建 + 安装 + 启用（幂等，--uninstall 反转）
scripts/uninstall.sh                  卸载
tests/run-tests.sh                    功能回归（9 项）
tests/run-bulk-tests.sh               大负载 / 并发 / 恢复
tests/x11-read.c                      测试辅助：ICCCM 正确的 X11 读取器
Makefile                              make / make check / make install / make test
```

`make` 常用目标：

```bash
make            # 构建
make deps       # 检查构建依赖
make check      # 环境自检
make install    # 安装（支持 DESTDIR / PREFIX）
make test       # 跑全部测试
```

---

## 致谢与参考

- [`ext-data-control-v1`](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/tree/main/staging/ext-data-control) —— wayland-protocols
- [xwayland-satellite](https://github.com/Supreeeme/xwayland-satellite) —— niri 生态下的 XWayland 管理器
- ICCCM 2.0 中关于 `TARGETS` / `INCR` 选区的规定

## 许可

[MIT](LICENSE)
