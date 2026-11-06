---
name: Bug Report / 问题反馈
about: 报告剪贴板同步异常或环境兼容问题
labels: bug
---

## 环境信息 / Environment

- **cbfix-bridge 版本 / Version**: (e.g. `v1.1.4` / output of `cbfix-bridge --version`)
- **发行版 / Distro**: (e.g. Arch Linux / Fedora 41 / Ubuntu 24.04)
- **Wayland 合成器与版本 / Compositor & Version**: (e.g. `niri 25.05`, `sway 1.10`, `Hyprland 0.47`, `KWin 6.3`)
- **Xwayland 模式 / Xwayland Setup**: (e.g. `xwayland-satellite 0.8` / compositor built-in `Xwayland`)
- **Waydroid 环境（如涉及）/ Waydroid Setup (if applicable)**: (e.g. `Waydroid 1.4 + libgbinder` / not used)
- **涉及的应用 / Affected App(s)**: (e.g. `Waydroid Android app`, `Java Swing/AWT app`, `xclip`, `X11 GUI client`)

## 诊断输出 / Diagnostic Output

请贴出以下两条命令的终端输出（**必填**，用于定位协议与环境状态）：
Please paste the output of the following two commands (**required**):

### 1. `cbfix-bridge --status`
```text
在此粘贴 cbfix-bridge --status 输出 / Paste output here
```

### 2. `cbfix-bridge --selftest`
```text
在此粘贴 cbfix-bridge --selftest 输出 / Paste output here
```

## 复现步骤 / Steps to Reproduce

1. 从应用 A（例如 `...`）复制内容（文本 / 图片）
2. 切换到应用 B（例如 `...`）粘贴
3. 实际表现与预期表现对比：
