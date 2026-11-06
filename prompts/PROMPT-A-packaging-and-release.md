# 提示词 A：构建 .pkg.tar.zst / .rpm / .deb 并发布到 GitHub

> 把下面 `text` 代码块里的**整段内容**（从"你要为一个已完成..."到最后一条约束）复制给负责打包与发布的 AI。
> 接收方不共享本会话上下文，该提示词已自包含全部所需事实。

## 适用于

- 产出发行包：Arch `cbfix-bridge-1.0.0-1-x86_64.pkg.tar.zst`、RPM `cbfix-bridge-1.0.0-1.x86_64.rpm`、Deb `cbfix-bridge_1.0.0_amd64.deb`
- 服务以 **systemd 用户服务**方式运行
- 安装后输出**高亮**提示，引导用户 `systemctl --user enable --now`
- 更新仓库内 `scripts/install.sh` 与别名的路径一致性
- 推送到 https://github.com/macOS-Terminal/linux-clipboard-fix

## 使用前的环境提示（给派发者，不给接收方）

- 本机 `dpkg`/`dpkg-deb` **未安装**，`sudo` **需要密码**（无免密）。所以 .deb 分支很可能需要先 `sudo pacman -S --noconfirm dpkg`，若无权限安装则该项会被如实标为未验证——这是预期结果，不是失败。
- 当前目录**不是 git 仓库**；`gh` **未安装**；`~/.ssh/id_ed25519` 与 `~/.ssh/id_ed25519_dsh` 对 github.com 均返回 `Permission denied (publickey)`。推送很可能停在"本地已提交待推送"，提示词已把它定义为一个合法的终态。

---

```text
你要为一个已完成并已验证的 C 语言开源项目构建三种 Linux 安装包，然后发布到 GitHub。源代码在本地路径：
/mnt/60CC4C56CC4C289C/Projects/linux-clipboard-fix

## 项目事实（已核实，直接采用，不要臆造替代方案）

- 包名：cbfix-bridge    版本：1.0.0    许可证：MIT
- 一句话描述：Bridge the Wayland and X11 clipboard so X11 apps (HMCL, Codex CLI, ...) share the system clipboard.
- 它是单文件 C11 守护进程，源码 src/cbfix-bridge.c，用 Makefile 构建：`make` 产出 build/cbfix-bridge
- 构建依赖（需要 pkg-config 模块）：wayland-client、x11、xfixes；另需 wayland-scanner 及仓库内自带的 protocols/ext-data-control-v1.xml
- 运行时依赖：libwayland-client.so.0、libX11.so.6、libXfixes.so.3（libm/libc 无需列入）
- 安装布局：二进制装到 /usr/bin/cbfix-bridge；systemd 用户单元装到 /usr/lib/systemd/user/cbfix-bridge.service；文档装到 /usr/share/doc/cbfix-bridge/{README.md,LICENSE}
- 该二进制**没有 --version 参数**（只有 --help / --selftest / --status / --interval N / --verbose）。不要写依赖 --version 的打包逻辑或探活逻辑，否则一定失败。版本号只体现在包元数据里。
- Makefile 支持 `make install DESTDIR=<root> PREFIX=/usr`，已实测可用，产物路径 <root>/usr/bin/cbfix-bridge
- systemd 单元（仓库内 systemd/cbfix-bridge.service，装包后改名为用户级单元）：
    [Unit]  Description / PartOf=graphical-session.target / After=graphical-session.target
    [Service] ExecStart=/usr/bin/cbfix-bridge / Restart=on-failure / RestartSec=2
    [Install] WantedBy=graphical-session.target
  注意：装包后 ExecStart 路径必须是 /usr/bin/cbfix-bridge（仓库原文件里是 /usr/local/bin，打包时必须改写）
- 必须附带一个共享的安装后提示脚本，内容见下面第 4 节。

## 交付物

在仓库根目录下建立 packaging/ 目录，产出：

1. packaging/PKGBUILD                     —— Arch Linux，产出 cbfix-bridge-1.0.0-1-x86_64.pkg.tar.zst
2. packaging/cbfix-bridge.spec             —— RPM，产出 cbfix-bridge-1.0.0-1.x86_64.rpm
3. packaging/debian/                       —— 目录树（control、postinst、rules 或等价结构），产出 cbfix-bridge_1.0.0_amd64.deb
4. packaging/post-install-notice.txt       —— 三个包共用的安装后提示文本（唯一真实来源，其他脚本引用它或内联同一份内容）
5. scripts/build-packages.sh               —— 一键构建全部三种包的脚本，产出到 dist/ 目录
6. packaging/README.md                     —— 如何构建、如何验证、如何安装
7. scripts/notice.sh                       —— 高亮安装提示的唯一实现，被 install.sh 与三个包的 post 脚本共同复用
8. 更新后的 scripts/install.sh 与 systemd/cbfix-bridge.service（见第 4.5 节）

## 关键技术要求

- **三种包都必须真实构建成功并逐字节验证内容**，不允许只写脚本不跑通。
- 每个包必须包含：/usr/bin/cbfix-bridge（0755）、/usr/lib/systemd/user/cbfix-bridge.service（0644）、/usr/share/doc/cbfix-bridge/README.md、/usr/share/doc/cbfix-bridge/LICENSE。
- 包元数据里的依赖必须与前述运行时依赖一致（Arch: depends=('wayland' 'libx11' 'libxfixes')；RPM: Requires: wayland-libs-client libX11 libXfixes；deb: Depends: libwayland-client0, libx11-6, libxfixes3）。请按各发行版实际的包名核对后再写入，不要照抄，若名字有出入以发行版实际为准并在 packaging/README.md 中说明。
- 三种包都要执行那个安装后提示脚本：Arch 用 .install / post_install，RPM 用 %post，deb 用 postinst。这些脚本必须幂等（重复安装不报错），并且在没有 systemd 用户会话的环境（如 chroot、CI）下不得失败退出——用正确判断包裹 `systemctl --user` 调用，失败时降级为文字提示。
- 构建机是 Arch Linux，工具链已确认可用：makepkg 7.1.0、rpmbuild 6.0.1、bsdtar、zstd、fakeroot、ar、xz。dpkg/debootstrap 可能需要安装（`sudo pacman -S --noconfirm dpkg`）。
- `scripts/build-packages.sh` 必须：可重复执行、失败即停（set -euo pipefail）、在每个产物生成后做一次「内容自检」（列出包内文件清单并确认 4 个必需路径齐全、权限正确），最后打印 dist/ 下产物的绝对路径与 sha256。
- 不要修改 src/、tests/、Makefile 的功能逻辑。若为了打包必须新增 Makefile 目标，只允许追加，不得改动既有目标行为。

## 第 4 节：安装后提示脚本（必须高亮）

这是用户明确要求的重点。三种包安装完成后都要向用户输出下述提示，必须视觉突出（ANSI 颜色 + 边框），并且在终端不支持颜色时自动降级为纯文本：

提示内容要点（措辞可润色，但信息不可缺失）：
- 说明服务是「systemd 用户级服务」，不随 root 系统服务运行
- 给出启用命令：`systemctl --user enable --now cbfix-bridge.service`
- 给出检查命令：`systemctl --user status cbfix-bridge.service`
- 给出日志命令：`journalctl --user -u cbfix-bridge -f`
- 给出自检命令：`cbfix-bridge --selftest`
- 提醒：它需要 Wayland 合成器提供 ext-data-control-v1（niri ≥25.05 / sway ≥1.10 / Hyprland ≥0.40 / KWin ≥6.3），且需要 Xwayland 与可用的 $DISPLAY
- 提醒：若 systemd 用户环境缺少 DISPLAY/WAYLAND_DISPLAY，需 `systemctl --user import-environment WAYLAND_DISPLAY DISPLAY` 后重启服务
- 提醒：不想用 systemd 可在 ~/.config/niri/config.kdl 加 `spawn-at-startup "cbfix-bridge"`

高亮实现建议（自行择优）：用 ASCII/Unicode 框线包住提示，标题用粗体+青色，命令用反色或加粗，警告用黄色。保持宽度不超过 78 列以适配窄终端。务必检查 `[ -t 1 ]` 与 `NO_COLOR` 环境变量。

## 第 4.5 节：顺带更新仓库内的 scripts/install.sh（用户明确要求）

现状（已核实）：scripts/install.sh 已经会安装 systemd 用户单元并 `systemctl --user enable --now`，但存在两个问题：

1. 它把二进制装到 /usr/local/bin/cbfix-bridge，而三种发行版包约定装到 /usr/bin/cbfix-bridge，两套路径不一致会让同时用包和用脚本的用户产生困惑，排障时也容易看错版本。请统一：脚本改为装到 /usr/bin（通过 make install DESTDIR=/ PREFIX=/usr 或等价方式），并同步修改 systemd/cbfix-bridge.service 的 ExecStart 为 /usr/bin/cbfix-bridge。改完后要确认 `make install DESTDIR=<tmp> PREFIX=/usr` 与单元文件路径一致。
2. 结尾的启用提示只是 3 行普通 echo，没有高亮，与包安装后的提示风格不统一。请让 install.sh 复用 packaging/post-install-notice.txt 的同一段高亮提示（建议把提示渲染抽成 scripts/notice.sh，install.sh 与三个包的 post 脚本共同 source 它，避免三处内容漂移）。

验收证据要求：改完后真实执行一遍 `bash scripts/install.sh --help`，以及在模拟环境（或 DESTDIR 安装）下确认提示渲染文本；贴出实际输出。不要真的在构建机上 `enable --now` 你无法控制的用户服务——若会真的启动服务，先说明并按用户环境决定，必要时只做静态检查并如实标注为「未实机启用」。

## 第 5 节：发布到 GitHub

- 目标仓库：https://github.com/macOS-Terminal/linux-clipboard-fix
- 本地目录当前**不是** git 仓库，需要 `git init`。分支用 main。
- 先写 .gitignore，至少忽略：build/、dist/、*.tar.gz、cbfix-*/（解包目录）、tests/java/*.class、*.pkg.tar.zst、*.rpm、*.deb
- 提交前确认没有把编译产物或二进制提交进仓库。用 `git status --short` 与 `git ls-files | wc -l` 自查，并在回复里贴出这两个命令的真实输出。
- 提交信息用英文，第一行简短祈使句；如需多提交，按「源码+构建」「打包」「文档」分组。
- 推送方式：优先尝试 SSH（已配置的 key 位于 ~/.ssh/，但实测 id_ed25519 与 id_ed25519_dsh 对 github.com 均返回 Permission denied (publickey)）；若 SSH 不通，检查是否有 GITHUB_TOKEN/GH_TOKEN 环境变量或已登录的 gh CLI。**如果都没有可用凭据，不要伪造成功**：完成本地提交，把仓库保持在「只差 push」的状态，然后明确报告「本地已提交，推送需要凭据」并给出用户可执行的推送命令。
- 推送成功后，用 `git ls-remote` 或 API 读取真实结果来证明，不要只凭 push 命令退出码下结论。

## 验收标准（逐条给出证据，禁止只口头声称）

1. dist/ 下三个包都存在，`ls -l dist/` 真实输出
2. 每个包的文件清单真实输出（pacman -Qlp / rpm -qpl / dpkg-deb -c），且 4 个必需路径齐全、权限正确
3. 从包中提取出的 cbfix-bridge 可执行：`--help` 输出正常，且 `--selftest` 在具备 Wayland 会话的机器上给出预期结果（若当前环境无图形会话，如实说明该项未验证）
4. 安装后提示脚本真实输出一次（可在模拟环境手动执行该脚本片段），贴出实际渲染文本
5. `scripts/build-packages.sh` 从干净状态完整跑通一次，贴出末尾摘要
6. GitHub 仓库状态：`git ls-remote` 或等价证据

## 输出格式（回复控制在 40 行以内）

- 每个交付物的路径 + 一行状态
- 6 条验收证据，每条最多 2 行原始输出
- 未验证事项单独一节，明确标注「未验证」及原因
- 不要粘贴整份源码或整份 PKGBUILD

## 约束

- 可以开子代理。
- 不要为了让测试通过而放宽断言或伪造产物。
- 遇到无法解决的阻塞（如缺 dpkg 且无 sudo 权限安装），如实报告并给出替代方案，不要静默跳过。
```
