#!/usr/bin/env bash
# build-packages.sh —— 从当前仓库工作树的真源码构建发行包，产物放到 dist/。
#
# 用法见 usage()。arch 分支由本脚本实现；rpm/deb 分支委托给
# scripts/build-rpm.sh / scripts/build-deb.sh（由其它脚本负责）。
set -euo pipefail

PKGNAME="cbfix-bridge"
PKGVER="1.1.4"
PKGREL="1"
ARCH="x86_64"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "${SCRIPT_DIR}")"
DIST="${ROOT}/dist"
PKGDIR_SRC="${ROOT}/packaging"
NOTICE_SH="${PKGDIR_SRC}/notice.sh"
INSTALL_TMPL="${PKGDIR_SRC}/${PKGNAME}.install"
MARKER='#@CBFIX_NOTICE_SH@'
ARCH_PKG="${PKGNAME}-${PKGVER}-${PKGREL}-${ARCH}.pkg.tar.zst"

usage() {
    cat <<EOF
用法：$(basename "$0") <子命令> [选项]

子命令：
  arch               构建 Arch 包 dist/${ARCH_PKG}
  rpm                调用 scripts/build-rpm.sh 构建 RPM
  deb                调用 scripts/build-deb.sh 构建 deb
  all                依次执行 arch、rpm、deb
  help, -h, --help   显示本帮助

选项（仅 arch）：
  --notice-only      不构建，只生成 .install 并执行其中的 post_install，
                     用于预览安装后提示（设置 NO_COLOR=1 可关闭颜色）
EOF
}

die() { printf '错误：%s\n' "$*" >&2; exit 1; }
info() { printf '==> %s\n' "$*"; }

need_cmd() {
    local c
    for c in "$@"; do
        command -v "$c" >/dev/null 2>&1 || die "缺少命令 '$c'，请先安装后重试。"
    done
}

# 把 packaging/notice.sh 原样内联进 .install 模板（替换标记行），写到 $1。
render_install() {
    local out="$1"
    [ -f "${NOTICE_SH}" ] || die "缺少 ${NOTICE_SH}（安装后提示的唯一实现），无法生成 .install。"
    [ -f "${INSTALL_TMPL}" ] || die "缺少 ${INSTALL_TMPL}。"
    [ "$(grep -cxF -- "${MARKER}" "${INSTALL_TMPL}")" = "1" ] \
        || die "${INSTALL_TMPL} 中必须恰好有一行标记 ${MARKER}。"
    local line
    {
        while IFS= read -r line || [ -n "${line}" ]; do
            if [ "${line}" = "${MARKER}" ]; then
                cat "${NOTICE_SH}"
            else
                printf '%s\n' "${line}"
            fi
        done < "${INSTALL_TMPL}"
    } > "${out}"
    bash -n "${out}" || die "生成的 ${out} 语法错误。"
}

notice_only() {
    NOTICE_TMP="$(mktemp -d)"
    trap 'rm -rf "${NOTICE_TMP:-}"' EXIT
    render_install "${NOTICE_TMP}/${PKGNAME}.install"
    # 与 pacman 一致：在子 shell 中 source scriptlet 后调用 post_install。
    bash -c '. "$1"; post_install' _ "${NOTICE_TMP}/${PKGNAME}.install"
}

# 包内容自检：文件清单、权限、unit 路径、二进制是否为真桥。
verify_arch_pkg() {
    local pkg="$1" chk
    chk="$(mktemp -d)"
    info "内容自检：${pkg}"
    local list
    list="$(bsdtar -tvf "${pkg}")"
    grep -v ' \.[A-Z]' <<<"${list}" || true
    check_mode() {
        local path="$1" mode="$2"
        printf '%s\n' "${list}" | awk -v p="${path}" -v m="${mode}" \
            '$NF==p && $1==m {f=1} END{exit !f}' \
            || die "包内 ${path} 缺失或权限不是 ${mode}"
        printf '  OK  %-46s %s\n' "${path}" "${mode}"
    }
    check_mode usr/bin/cbfix-bridge                        -rwxr-xr-x
    check_mode usr/lib/systemd/user/cbfix-bridge.service   -rw-r--r--
    check_mode usr/share/doc/cbfix-bridge/README.md        -rw-r--r--
    check_mode usr/share/doc/cbfix-bridge/LICENSE          -rw-r--r--

    bsdtar -xf "${pkg}" -C "${chk}"
    grep -qx 'ExecStart=/usr/bin/cbfix-bridge' "${chk}/usr/lib/systemd/user/cbfix-bridge.service" \
        || die "包内 unit 的 ExecStart 不是 /usr/bin/cbfix-bridge"
    printf '  OK  unit: %s\n' "$(grep '^ExecStart=' "${chk}/usr/lib/systemd/user/cbfix-bridge.service")"

    local help
    help="$("${chk}/usr/bin/cbfix-bridge" --help 2>&1)" || die "包内二进制 --help 执行失败"
    grep -q -- '--interval' <<<"${help}" || die "包内二进制 --help 缺少 --interval（不是真桥）"
    grep -q -- '--verbose'  <<<"${help}" || die "包内二进制 --help 缺少 --verbose（不是真桥）"
    if grep -qi fixture <<<"${help}" || grep -aqi 'fixture' "${chk}/usr/bin/cbfix-bridge"; then
        die "包内二进制含 fixture 字样，疑似占位假二进制"
    fi
    printf '  OK  --help 含 --interval/--verbose，无 fixture 字样\n'

    local lib deps
    deps="$(ldd "${chk}/usr/bin/cbfix-bridge")"
    for lib in libwayland-client.so.0 libX11.so.6 libXfixes.so.3; do
        grep -q "${lib} => /" <<<"${deps}" \
            || die "包内二进制未链接 ${lib}"
        printf '  OK  链接 %s\n' "${lib}"
    done

    grep -q 'cbfix_notice()' "${chk}/.INSTALL" && grep -q '^post_install()' "${chk}/.INSTALL" \
        || die "包内 .INSTALL 未内联 cbfix_notice 或缺少 post_install"
    printf '  OK  .INSTALL 已内联 cbfix_notice，含 post_install/post_upgrade\n'
    rm -rf "${chk}"
}

build_arch() {
    need_cmd makepkg fakeroot bsdtar tar gzip sha256sum ldd
    local srcname
    # 全局变量：EXIT trap 在函数返回后仍需引用它。
    ARCH_WORK="$(mktemp -d "${TMPDIR:-/tmp}/cbfix-arch.XXXXXX")"
    local work="${ARCH_WORK}"
    trap 'rm -rf "${ARCH_WORK:-}"' EXIT
    srcname="${PKGNAME}-${PKGVER}"

    info "打包当前工作树源码 -> ${srcname}.tar.gz"
    # 排除构建产物、发行物与 VCS；后续在 build() 中 make -B 干净重编。
    tar -C "${ROOT}" \
        --exclude=./.git --exclude=./build --exclude=./dist \
        --exclude="./${srcname}" --exclude='*.tar.gz' \
        --exclude='*.pkg.tar.zst' --exclude='*.rpm' --exclude='*.deb' \
        --exclude='*.class' --exclude='*.o' \
        --sort=name --owner=0 --group=0 --numeric-owner \
        --transform="s,^\.,${srcname}," \
        -czf "${work}/${srcname}.tar.gz" .
    # 先读入变量再匹配，避免 grep -q 提前退出让 tar 收到 SIGPIPE（pipefail 下会误报）。
    local srclist
    srclist="$(tar -tzf "${work}/${srcname}.tar.gz")"
    grep -qx "${srcname}/src/cbfix-bridge.c" <<<"${srclist}" \
        || die "源码包中缺少 src/cbfix-bridge.c"
    if grep -q "^${srcname}/build/" <<<"${srclist}"; then
        die "源码包意外包含 build/ 目录"
    fi

    info "生成 ${PKGNAME}.install（内联 packaging/notice.sh）"
    render_install "${work}/${PKGNAME}.install"

    local sum
    sum="$(sha256sum "${work}/${srcname}.tar.gz" | cut -d' ' -f1)"
    sed "s/^sha256sums=('SKIP')$/sha256sums=('${sum}')/" \
        "${PKGDIR_SRC}/PKGBUILD" > "${work}/PKGBUILD"
    grep -q "^sha256sums=('${sum}')$" "${work}/PKGBUILD" || die "无法写入源码 sha256 到 PKGBUILD"

    mkdir -p "${work}/out" "${DIST}"
    info "运行 makepkg（不安装依赖、不需要 root）"
    (
        cd "${work}"
        PKGDEST="${work}/out" SRCDEST="${work}" BUILDDIR="${work}/b" \
        SRCPKGDEST="${work}/out" LOGDEST="${work}" \
            makepkg -f --cleanbuild --noconfirm --nosign
    )

    [ -f "${work}/out/${ARCH_PKG}" ] || die "makepkg 未产出 ${ARCH_PKG}"
    verify_arch_pkg "${work}/out/${ARCH_PKG}"
    rm -f "${DIST}/${ARCH_PKG}"
    # 用 cp 而非 install -m：dist/ 可能位于不支持 chmod 的文件系统（如 NTFS/fuseblk）。
    cp "${work}/out/${ARCH_PKG}" "${DIST}/${ARCH_PKG}"
    cmp -s "${work}/out/${ARCH_PKG}" "${DIST}/${ARCH_PKG}" || die "复制产物到 dist/ 失败"

    info "产物："
    printf '  %s\n' "$(cd "${DIST}" && pwd)/${ARCH_PKG}"
    printf '  sha256 %s\n' "$(sha256sum "${DIST}/${ARCH_PKG}" | cut -d' ' -f1)"
    rm -rf "${work}"
    trap - EXIT
}

delegate() {
    local kind="$1" script="${ROOT}/scripts/build-$1.sh"
    if [ ! -f "${script}" ]; then
        die "${kind} 打包尚未实现：缺少 ${script}（由 rpm/deb 打包脚本负责提供）。"
    fi
    bash "${script}"
}

main() {
    local cmd="${1:-}"
    [ $# -gt 0 ] && shift
    case "${cmd}" in
        arch)
            case "${1:-}" in
                "") build_arch ;;
                --notice-only) notice_only ;;
                *) printf '错误：未知选项 %s\n\n' "$1" >&2; usage >&2; exit 2 ;;
            esac
            ;;
        rpm|deb)
            [ $# -eq 0 ] || { printf '错误：%s 不接受参数\n\n' "${cmd}" >&2; usage >&2; exit 2; }
            delegate "${cmd}"
            ;;
        all)
            [ $# -eq 0 ] || { printf '错误：all 不接受参数\n\n' >&2; usage >&2; exit 2; }
            bash "${BASH_SOURCE[0]}" arch
            bash "${BASH_SOURCE[0]}" rpm
            bash "${BASH_SOURCE[0]}" deb
            info "全部完成，dist/ 内容："
            ls -l "${DIST}"
            ;;
        help|-h|--help)
            usage
            ;;
        "")
            printf '错误：缺少子命令\n\n' >&2; usage >&2; exit 2
            ;;
        *)
            printf '错误：未知子命令 %s\n\n' "${cmd}" >&2; usage >&2; exit 2
            ;;
    esac
}

main "$@"
