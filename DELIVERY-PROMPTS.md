# 交付提示词包（索引）

两份提示词已拆分为独立文件，各自可单独分发、单独复制：

| 任务 | 文件 | 内容 |
|---|---|---|
| A | [prompts/PROMPT-A-packaging-and-release.md](prompts/PROMPT-A-packaging-and-release.md) | 从仓库真源码（`src/cbfix-bridge.c`）通过 `scripts/build-packages.sh all` 重新构建 `.pkg.tar.zst` / `.rpm` / `.deb`（**严禁 fixture 占位假二进制**），内联 `packaging/notice.sh` **中文简洁高亮**安装后提示并完成 6 项真实验收 |
| B | [prompts/PROMPT-B-promo-video.md](prompts/PROMPT-B-promo-video.md) | 75–90 秒宣传视频 + **封面**（两版尺寸 + 可编辑源文件），对标 BV1MKa66VEPS 叙事结构 |

## 两份提示词的共同特征

- **自包含**：接收方不共享任何会话上下文，所需事实全部内联
- **带验收标准**：每条完成声明都要求贴真实命令输出，禁止口头声称
- **带禁止项**：不得开子代理、不得放宽断言、不得伪造数据、不得静默跳过阻塞

## 项目速览

| 项 | 值 |
|---|---|
| 项目名 | cbfix / cbfix-bridge |
| 作用 | 修复 Linux 上 X11 应用（HMCL、Codex CLI 等）与系统剪贴板不互通 |
| 语言 | C11，单二进制，链接 `libwayland-client.so.0`、`libX11.so.6`、`libXfixes.so.3` |
| 构建 | `make`（pkg-config: wayland-client / x11 / xfixes，需 wayland-scanner） |
| 版本 | 1.0.0 |
| 许可 | MIT |
| 目标仓库 | https://github.com/macOS-Terminal/linux-clipboard-fix |
| 本地路径 | `/mnt/60CC4C56CC4C289C/Projects/linux-clipboard-fix` |

## 实测结果（来自本机 niri + xwayland-satellite 环境）

- `--selftest` 4/4
- 功能回归 9/9
- 大负载 INCR（100 KB–5 MB）逐字节比对 12/12
- 4 并发读者读 5 MB 4/4 字节一致
- 读者被 `kill -9` 后守护进程存活并恢复

## 执行顺序建议

两份可并行，互不依赖。任务 A 会改动仓库并推送，任务 B 只读取项目信息——B 所需的仓库地址已在提示词内给定，无需等 A 完成。
