# cbfix-bridge 打包与安装指南

本目录存放 `cbfix-bridge` 的多发行版打包元数据与安装后提示唯一实现（`notice.sh`）。所有包均从当前工作树真源码（`src/cbfix-bridge.c`）干净编译（`make -B`），输出到 `dist/`。

## 1. 目录结构与文件说明

- `packaging/notice.sh`：安装/升级后彩色高亮提示的唯一实现（`cbfix_notice install|upgrade`），被 `scripts/install.sh` 直接 `source`，并在构建时原样内联进三类安装包的钩子脚本。
- `packaging/PKGBUILD`、`packaging/cbfix-bridge.install`：Arch Linux 打包模板（由 `scripts/build-packages.sh arch` 驱动）。
- `packaging/cbfix-bridge.spec`：RPM 打包 spec（由 `scripts/build-rpm.sh` 驱动）。
- `packaging/debian/control`、`packaging/debian/postinst`：Debian/Ubuntu 打包控制文件与 `postinst` 模板（由 `scripts/build-deb.sh` 在无 `dpkg-deb` 环境下使用 `tar + xz + ar` 组装标准 Debian 2.0 归档）。

统一安装路径（三种包一致）：
- `/usr/bin/cbfix-bridge` (`0755`)
- `/usr/lib/systemd/user/cbfix-bridge.service` (`0644`)
- `/usr/share/doc/cbfix-bridge/README.md` (`0644`)
- `/usr/share/doc/cbfix-bridge/LICENSE` (`0644`)

## 2. 构建安装包

在仓库根目录执行：

```bash
# 一键构建全部三种包（Arch + RPM + DEB）
bash scripts/build-packages.sh all

# 单独构建某一类包
bash scripts/build-packages.sh arch
bash scripts/build-packages.sh rpm
bash scripts/build-packages.sh deb

# 仅预览安装后高亮提示（不构建包）
bash scripts/build-packages.sh arch --notice-only
```

构建产物位于 `dist/`：
- Arch Linux：`dist/cbfix-bridge-1.1.4-1-x86_64.pkg.tar.zst`
- Fedora / RHEL / openSUSE：`dist/cbfix-bridge-1.1.4-1.x86_64.rpm`
- Debian / Ubuntu：`dist/cbfix-bridge_1.1.4_amd64.deb`

## 3. 验证安装包

`build-packages.sh` 在每个包生成后都会自动执行内容自检（校验 4 个必需文件路径与权限、`ExecStart=/usr/bin/cbfix-bridge`、真二进制 `--help` 含 `--interval`/`--verbose` 且无 `fixture`、`ldd` 链接 `libwayland-client.so.0`/`libX11.so.6`/`libXfixes.so.3`、以及安装后钩子内联 `cbfix_notice`）。也可手动检查：

```bash
# Arch 包
bsdtar -tvf dist/cbfix-bridge-1.1.4-1-x86_64.pkg.tar.zst

# RPM 包
rpm -qplv dist/cbfix-bridge-1.1.4-1.x86_64.rpm
rpm -qp --scripts dist/cbfix-bridge-1.1.4-1.x86_64.rpm

# DEB 包（无需 dpkg-deb）
ar t dist/cbfix-bridge_1.1.4_amd64.deb
```

## 4. 安装与启用服务

```bash
# Arch Linux
sudo pacman -U dist/cbfix-bridge-1.1.4-1-x86_64.pkg.tar.zst

# Fedora / RHEL
sudo dnf install ./dist/cbfix-bridge-1.1.4-1.x86_64.rpm

# Debian / Ubuntu
sudo apt install ./dist/cbfix-bridge_1.1.4_amd64.deb
```

安装完成后，以**当前桌面普通用户**（不要加 `sudo`）启用并启动 systemd 用户服务：

```bash
systemctl --user enable --now cbfix-bridge.service
cbfix-bridge --selftest
```
