# 提示词 A：从真源码重新构建 `.pkg.tar.zst` / `.rpm` / `.deb`（严禁假二进制 + 中文简洁高亮安装提示）

> 把下面 `text` 代码块里的**整段内容**复制给负责打包与发布的 AI。
> 接收方不共享本会话上下文，该提示词已自包含全部所需事实、红线警告与验收标准。

## 适用于

- 从已上传真源码与打包脚本的仓库（`https://github.com/macOS-Terminal/linux-clipboard-fix` 或本地 `/mnt/60CC4C56CC4C289C/Projects/linux-clipboard-fix`）重新构建三种 Linux 安装包：
  1. `dist/cbfix-bridge-1.0.0-1-x86_64.pkg.tar.zst` (Arch Linux)
  2. `dist/cbfix-bridge-1.0.0-1.x86_64.rpm` (Fedora / RHEL / openSUSE)
  3. `dist/cbfix-bridge_1.0.0_amd64.deb` (Debian / Ubuntu)
- **彻底杜绝 `fixture` 占位假二进制**：必须由 `src/cbfix-bridge.c` + `protocols/ext-data-control-v1.xml` 经 `wayland-scanner` 与 `gcc` 真实编译
- **中文、简洁、ANSI 高亮安装提示**（对标 `shorin-pac` 的 pacman 安装提示风格，唯一实现位于 `packaging/notice.sh`）

---

```text
## 🚨 血泪教训 / 最高优先级红线警告（必读！）

1. **严禁编写、生成或使用任何 stub / fixture / 占位假二进制！**
   - 之前一次构建中，某 AI 自己编造了一个只会打印 `cbfix-bridge 1.0.0 (fixture)`、无参数启动直接打印 `usage` 并 `exit 2` 的假 C 程序塞进安装包，导致用户安装并启用 systemd 服务后，`cbfix-bridge.service` 死循环崩溃重启 370+ 次，剪贴板完全无法工作！
2. **必须 100% 从仓库真实源码完整编译：**
   - 必须使用仓库内的真实源码 `src/cbfix-bridge.c`（约 1600 行 C11 守护进程源码）+ `protocols/ext-data-control-v1.xml`，通过 `wayland-scanner`（生成 `ext-data-control-v1-client-protocol.h/.c`）与 `gcc`（`make -B`）完整编译出 `build/cbfix-bridge`。
   - 若你克隆下来的目录里没有 `src/cbfix-bridge.c` 或行数不对，请立即报错停止，**绝对不允许自己写一个占位 C 文件顶替**！
3. **真二进制硬性特征（构建脚本与验收必须逐项核验）：**
   - 包内 `/usr/bin/cbfix-bridge` 体积约 **100KB+**（绝非十几 KB 的空壳）。
   - `ldd /usr/bin/cbfix-bridge` 必须动态链接 `libwayland-client.so.0`、`libX11.so.6`、`libXfixes.so.3`。
   - `/usr/bin/cbfix-bridge --help` 输出必须包含 `--interval MS` 与 `--verbose, -v`（以及 `--selftest`、`--status`；注意真二进制**没有** `--version` 参数）。
   - `strings /usr/bin/cbfix-bridge | grep -i fixture` **必须为空**（退出码非 0）。
   - 无参数启动 `/usr/bin/cbfix-bridge` 时会进入 Wayland/X11 剪贴板桥接守护模式，**绝不会**打印 `usage` 并以退出码 `2` 退出。

---

## 1. 任务目标与仓库来源

你要为已完成并上传了真实源码的开源项目 `cbfix-bridge` 重新构建三种 Linux 发行版安装包（`.pkg.tar.zst`、`.rpm`、`.deb`），并确保安装后提示为**中文、简洁、高亮**风格。

- **GitHub 仓库**：`https://github.com/macOS-Terminal/linux-clipboard-fix`
- **本地工作树路径**（若在同机执行）：`/mnt/60CC4C56CC4C289C/Projects/linux-clipboard-fix`
- **基础元数据**：
  - 包名：`cbfix-bridge`
  - 版本：`1.0.0`，Release：`1`
  - 许可证：`MIT`
  - 描述：`Bidirectional clipboard bridge between Wayland (ext-data-control-v1) and X11 (Xwayland)`
  - 构建依赖：`gcc`、`make`、`pkg-config`（`pkgconf`）、`wayland-scanner`、`wayland-protocols`，以及 `pkg-config` 模块 `wayland-client`、`x11`、`xfixes`
  - 运行时依赖：
    - Arch：`depends=('wayland' 'libx11' 'libxfixes')`
    - RPM：`libwayland-client.so.0()(64bit)`、`libX11.so.6()(64bit)`、`libXfixes.so.3()(64bit)`（由 `rpmbuild` 自动扫描 ELF 生成）
    - Deb：`Depends: libwayland-client0, libx11-6, libxfixes3`

---

## 2. 仓库现有结构与构建流程

仓库已包含完整真源码、systemd 用户单元与打包脚本：

- `src/cbfix-bridge.c`：真实 C11 双向剪贴板桥接守护进程源码（支持文本、图片、大体积 INCR 增量传输）
- `protocols/ext-data-control-v1.xml`：Wayland `ext-data-control-v1` 协议定义
- `Makefile`：`make -B` 调 `wayland-scanner` + `cc` 产出 `build/cbfix-bridge`；`make install DESTDIR=... PREFIX=/usr` 安装到 `/usr/bin/cbfix-bridge`
- `systemd/cbfix-bridge.service`：systemd 用户级服务单元（`ExecStart=/usr/bin/cbfix-bridge`）
- `packaging/`：
  - `packaging/notice.sh`：中文简洁高亮安装后提示的**唯一实现**（定义 `cbfix_notice install|upgrade`）
  - `packaging/PKGBUILD` 与 `packaging/cbfix-bridge.install`（含 `#@CBFIX_NOTICE_SH@` 内联标记）
  - `packaging/cbfix-bridge.spec`（含 `@CBFIX_NOTICE@` 内联标记与 `%check` 防假二进制校验）
  - `packaging/debian/`（`control`、`postinst`、`prerm` 等）
- `scripts/build-packages.sh`：一键打包入口，支持 `arch | rpm | deb | all` 以及 `arch --notice-only`（调用 `scripts/build-rpm.sh` 与 `scripts/build-deb.sh`）

### 构建步骤
1. 克隆或拉取最新 `https://github.com/macOS-Terminal/linux-clipboard-fix`（或直接使用本地 `/mnt/60CC4C56CC4C289C/Projects/linux-clipboard-fix`），先确认 `src/cbfix-bridge.c` 为约 1600 行真实源码。
2. 直接使用或完善 `bash scripts/build-packages.sh all`，从干净状态（`make -B`，绝不复用不明旧产物）构建出以下三个安装包：
   1. `dist/cbfix-bridge-1.0.0-1-x86_64.pkg.tar.zst` (Arch Linux)
   2. `dist/cbfix-bridge-1.0.0-1.x86_64.rpm` (Fedora / RHEL / openSUSE)
   3. `dist/cbfix-bridge_1.0.0_amd64.deb` (Debian / Ubuntu)
3. **三个包内布局必须完全统一**：
   - `/usr/bin/cbfix-bridge`（权限 `0755`，真编译二进制）
   - `/usr/lib/systemd/user/cbfix-bridge.service`（权限 `0644`，`ExecStart=/usr/bin/cbfix-bridge`）
   - `/usr/share/doc/cbfix-bridge/README.md`（权限 `0644`）
   - `/usr/share/doc/cbfix-bridge/LICENSE`（权限 `0644`）
4. 若 `scripts/install.sh` 尚未统一到 `/usr/bin/cbfix-bridge` 或尚未复用 `packaging/notice.sh`，请同步确保其安装路径为 `/usr/bin/cbfix-bridge` 并在末尾调用 `cbfix_notice install`。

---

## 3. 安装后高亮提示规范（中文、简洁、高亮）

严禁再使用长篇英文 ASCII 方框！仓库已在 `packaging/notice.sh` 中提供了唯一标准实现（函数 `cbfix_notice install|upgrade`），构建时由脚本自动内联进 Arch `.INSTALL`（`post_install`/`post_upgrade`）、RPM `%post` 与 Deb `postinst`。

### 渲染效果（必须完全一致）
```text
=================================================================
  cbfix-bridge 安装完成！用当前用户（不要 sudo）启用服务：
    systemctl --user enable --now cbfix-bridge.service

  自检：cbfix-bridge --selftest    日志：journalctl --user -u cbfix-bridge -f
=================================================================
  不生效？先执行 systemctl --user import-environment DISPLAY WAYLAND_DISPLAY
  再重启服务。需要合成器支持 ext-data-control-v1（niri≥25.05 / sway≥1.10 / KWin≥6.3）。
```
（升级时首行文案为：`cbfix-bridge 已更新！重启服务以生效：` + `systemctl --user restart cbfix-bridge.service`）

### 颜色与实现铁律
- **分隔线 `===...===`**：粗体绿色 `\033[1;32m`
- **标题行**：粗体青色 `\033[1;36m`
- **命令高亮**：粗体黄色 `\033[1;33m`
- **底部 2 行排障提示**：暗色（dim）`\033[2m`
- **重置**：`\033[0m`
- **颜色开关规范**：默认开启颜色，仅当设置了环境变量 `NO_COLOR`（`[ -n "${NO_COLOR:-}" ]`）时关闭颜色。**绝对不要用 `[ -t 1 ]` 拦截颜色**——因为 `pacman` 等包管理器执行 `.INSTALL` scriptlet 时会将子进程 stdout 通过管道转发给终端，加 `[ -t 1 ]` 会导致实际安装时颜色全部丢失！
- **安全要求**：安装后脚本只打印提示，不在 root 包管理器上下文中强行执行 `systemctl --user`，确保在 chroot / 容器 / CI 环境下退出码永远为 `0`。

---

## 4. 严格的 6 条验收证据（必须在最终回复中贴出真实命令输出，禁止口头声称）

1. **产物列表与校验和**：
   贴出 `ls -lh dist/` 与 `sha256sum dist/*` 的真实输出，确认三个包（`.pkg.tar.zst`、`.rpm`、`.deb`）齐全且体积正常。
2. **三个包的文件清单与权限**：
   贴出三个包的文件列表真实输出（如 `bsdtar -tvf`、`rpm -qplv`、`dpkg-deb -c` 或 `ar`/`tar`），证明 `/usr/bin/cbfix-bridge` (0755)、`/usr/lib/systemd/user/cbfix-bridge.service` (0644)、`/usr/share/doc/cbfix-bridge/{README.md,LICENSE}` (0644) 全部就位。
3. **真二进制铁证（拒绝假二进制）**：
   分别从三个包中解出 `/usr/bin/cbfix-bridge`，贴出：
   - `/usr/bin/cbfix-bridge --help` 真实输出（必须含 `--interval MS` 与 `--verbose, -v`）
   - `ldd /usr/bin/cbfix-bridge` 真实输出（必须链接 `libwayland-client.so.0`、`libX11.so.6`、`libXfixes.so.3`）
   - 执行 `! strings usr/bin/cbfix-bridge | grep -qi fixture` 证明三个包内二进制均**绝无 `fixture` 字样**。
4. **安装后提示脚本真实带色输出**：
   贴出 `env -u NO_COLOR bash scripts/build-packages.sh arch --notice-only | cat -v` 的真实输出，证明含 `\033[1;32m`、`\033[1;36m`、`\033[1;33m`、`\033[2m` 转义序列及完整中文简洁提示。
5. **一键构建脚本全流程自检输出**：
   贴出 `bash scripts/build-packages.sh all` 从干净状态完整跑通的输出摘要（含内置包内容自检 `OK` 记录）。
6. **GitHub 远端同步验证**：
   若对仓库打包脚本或配置有改动，提交并推送回 `https://github.com/macOS-Terminal/linux-clipboard-fix`，贴出 `git ls-remote origin refs/heads/main` 与 `git status --short` 真实输出（注意：不要把 `build/`、`dist/` 或二进制包提交进 git 分支，产物可通过 GitHub Release 上传或保留在 `dist/`）。
```
