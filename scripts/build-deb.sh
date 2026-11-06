#!/usr/bin/env bash
# build-deb.sh —— 无 dpkg-deb 环境下，用纯 bash + tar + xz + ar 从当前工作树真源码构建标准 Debian 包。
set -euo pipefail

PKGNAME="cbfix-bridge"
PKGVER="1.1.4"
DEB_ARCH="amd64"
DEB_PKG="${PKGNAME}_${PKGVER}_${DEB_ARCH}.deb"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "${SCRIPT_DIR}")"
DIST="${ROOT}/dist"
PKGDIR_SRC="${ROOT}/packaging"
DEBIAN_SRC="${PKGDIR_SRC}/debian"
NOTICE_SH="${PKGDIR_SRC}/notice.sh"
CONTROL_TMPL="${DEBIAN_SRC}/control"
POSTINST_TMPL="${DEBIAN_SRC}/postinst"
MARKER='#@CBFIX_NOTICE_SH@'

usage() {
    cat <<EOF
用法：$(basename "$0") [--help]

从当前仓库工作树源码构建 Debian 2.0 安装包（无需 dpkg-deb）：
  产物：dist/${DEB_PKG}

选项：
  -h, --help   显示本帮助并退出
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

render_postinst() {
    local out="$1"
    [ -f "${NOTICE_SH}" ] || die "缺少 ${NOTICE_SH}（安装后提示的唯一实现）。"
    [ -f "${POSTINST_TMPL}" ] || die "缺少 ${POSTINST_TMPL}。"
    [ "$(grep -cxF -- "${MARKER}" "${POSTINST_TMPL}")" = "1" ] \
        || die "${POSTINST_TMPL} 中必须恰好有一行标记 ${MARKER}。"
    local line
    {
        while IFS= read -r line || [ -n "${line}" ]; do
            if [ "${line}" = "${MARKER}" ]; then
                cat "${NOTICE_SH}"
            else
                printf '%s\n' "${line}"
            fi
        done < "${POSTINST_TMPL}"
    } > "${out}"
    chmod 0755 "${out}"
    sh -n "${out}" || die "生成的 ${out} 语法错误。"
}

verify_deb_pkg() {
    local pkg="$1" chk
    chk="$(mktemp -d)"
    info "内容自检：${pkg}"

    local members
    members="$(ar t "${pkg}")"
    printf '%s\n' "${members}"
    local expected_members=$'debian-binary\ncontrol.tar.xz\ndata.tar.xz'
    [ "${members}" = "${expected_members}" ] \
        || die "deb 归档成员顺序不符，实际为：${members}"
    printf '  OK  ar 成员顺序：debian-binary, control.tar.xz, data.tar.xz\n'

    (
        cd "${chk}"
        ar x "${pkg}"
    )

    [ "$(cat "${chk}/debian-binary")" = "2.0" ] || die "debian-binary 内容不是 2.0"

    local ctrl_list data_list
    ctrl_list="$(tar -tvf "${chk}/control.tar.xz")"
    data_list="$(tar -tvf "${chk}/data.tar.xz")"
    printf '%s\n' "${data_list}"

    check_tar_mode() {
        local list_text="$1" path="$2" mode="$3"
        printf '%s\n' "${list_text}" | awk -v p="${path}" -v m="${mode}" \
            '$NF==p && $1==m {f=1} END{exit !f}' \
            || die "归档内 ${path} 缺失或权限不是 ${mode}"
        printf '  OK  %-46s %s\n' "${path}" "${mode}"
    }

    check_tar_mode "${ctrl_list}" ./control                                  -rw-r--r--
    check_tar_mode "${ctrl_list}" ./postinst                                 -rwxr-xr-x
    check_tar_mode "${data_list}" ./usr/bin/cbfix-bridge                     -rwxr-xr-x
    check_tar_mode "${data_list}" ./usr/lib/systemd/user/cbfix-bridge.service -rw-r--r--
    check_tar_mode "${data_list}" ./usr/share/doc/cbfix-bridge/README.md     -rw-r--r--
    check_tar_mode "${data_list}" ./usr/share/doc/cbfix-bridge/LICENSE       -rw-r--r--

    mkdir -p "${chk}/ctrl" "${chk}/data"
    tar -xJf "${chk}/control.tar.xz" -C "${chk}/ctrl"
    tar -xJf "${chk}/data.tar.xz"    -C "${chk}/data"

    grep -qx 'Package: cbfix-bridge' "${chk}/ctrl/control" || die "control 缺少 Package: cbfix-bridge"
    grep -qx "Version: ${PKGVER}" "${chk}/ctrl/control" || die "control 缺少 Version: ${PKGVER}"
    grep -qx 'Architecture: amd64' "${chk}/ctrl/control" || die "control 缺少 Architecture: amd64"
    grep -qx 'Depends: libwayland-client0, libx11-6, libxfixes3' "${chk}/ctrl/control" \
        || die "control 缺少 Depends: libwayland-client0, libx11-6, libxfixes3"

    grep -q 'cbfix_notice()' "${chk}/ctrl/postinst" \
        && grep -q 'cbfix_notice install' "${chk}/ctrl/postinst" \
        && grep -q 'cbfix_notice upgrade' "${chk}/ctrl/postinst" \
        || die "postinst 未内联 cbfix_notice"
    printf '  OK  postinst 已内联 cbfix_notice（含 install/upgrade 分支）\n'

    grep -qx 'ExecStart=/usr/bin/cbfix-bridge' "${chk}/data/usr/lib/systemd/user/cbfix-bridge.service" \
        || die "deb 包内 unit 的 ExecStart 不是 /usr/bin/cbfix-bridge"
    printf '  OK  unit: %s\n' "$(grep '^ExecStart=' "${chk}/data/usr/lib/systemd/user/cbfix-bridge.service")"

    local help
    help="$("${chk}/data/usr/bin/cbfix-bridge" --help 2>&1)" || die "deb 包内二进制 --help 执行失败"
    grep -q -- '--interval' <<<"${help}" || die "deb 包内二进制 --help 缺少 --interval"
    grep -q -- '--verbose'  <<<"${help}" || die "deb 包内二进制 --help 缺少 --verbose"
    if grep -qi fixture <<<"${help}" || grep -aqi 'fixture' "${chk}/data/usr/bin/cbfix-bridge"; then
        die "deb 包内二进制含 fixture 字样，疑似假二进制"
    fi
    printf '  OK  --help 含 --interval/--verbose，无 fixture 字样\n'

    local lib deps
    deps="$(ldd "${chk}/data/usr/bin/cbfix-bridge")"
    for lib in libwayland-client.so.0 libX11.so.6 libXfixes.so.3; do
        grep -q "${lib} => /" <<<"${deps}" \
            || die "deb 包内二进制未链接 ${lib}"
        printf '  OK  链接 %s\n' "${lib}"
    done

    rm -rf "${chk}"
}

main() {
    case "${1:-}" in
        "")
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf '错误：未知选项 %s\n\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac

    need_cmd ar tar xz md5sum sha256sum ldd pkg-config wayland-scanner gcc make

    DEB_WORK="$(mktemp -d "${TMPDIR:-/tmp}/cbfix-deb.XXXXXX")"
    local work="${DEB_WORK}"
    trap 'rm -rf "${DEB_WORK:-}"' EXIT

    mkdir -p "${work}/src" "${work}/data" "${work}/control" "${work}/pkg" "${DIST}"

    info "复制工作树源码到临时目录并执行 make -B"
    tar -C "${ROOT}" \
        --exclude=./.git --exclude=./build --exclude=./dist \
        --exclude='./cbfix-*' --exclude='*.tar.gz' \
        --exclude='*.pkg.tar.zst' --exclude='*.rpm' --exclude='*.deb' \
        --exclude='*.class' --exclude='*.o' \
        -cf - . | tar -C "${work}/src" -xf -

    [ -f "${work}/src/src/cbfix-bridge.c" ] || die "临时源码目录缺少 src/cbfix-bridge.c"
    [ ! -d "${work}/src/build" ] || die "临时源码目录意外包含 build/"

    make -C "${work}/src" -B
    make -C "${work}/src" install DESTDIR="${work}/data" PREFIX=/usr

    install -d -m 0755 "${work}/data/usr/lib/systemd/user"
    sed -E 's#^ExecStart=[^[:space:]]*cbfix-bridge#ExecStart=/usr/bin/cbfix-bridge#' \
        "${work}/src/systemd/cbfix-bridge.service" \
        > "${work}/data/usr/lib/systemd/user/cbfix-bridge.service"
    chmod 0644 "${work}/data/usr/lib/systemd/user/cbfix-bridge.service"
    grep -qx 'ExecStart=/usr/bin/cbfix-bridge' "${work}/data/usr/lib/systemd/user/cbfix-bridge.service" \
        || die "unit 的 ExecStart 未能设置为 /usr/bin/cbfix-bridge"

    install -Dm0644 "${work}/src/README.md" "${work}/data/usr/share/doc/${PKGNAME}/README.md"
    install -Dm0644 "${work}/src/LICENSE"   "${work}/data/usr/share/doc/${PKGNAME}/LICENSE"
    find "${work}/data" -type d -exec chmod 0755 {} +

    info "生成 control 与 postinst（内联 packaging/notice.sh）"
    [ -f "${CONTROL_TMPL}" ] || die "缺少 ${CONTROL_TMPL}"
    install -m 0644 "${CONTROL_TMPL}" "${work}/control/control"
    render_postinst "${work}/control/postinst"
    (
        cd "${work}/data"
        find usr -type f -print0 | LC_ALL=C sort -z | xargs -0 md5sum > "${work}/control/md5sums"
    )
    chmod 0644 "${work}/control/md5sums"
    chmod 0755 "${work}/control"

    info "组装 Debian 2.0 归档 -> ${DEB_PKG}"
    printf '2.0\n' > "${work}/pkg/debian-binary"
    tar -C "${work}/control" \
        --sort=name --owner=0 --group=0 --numeric-owner \
        -cJf "${work}/pkg/control.tar.xz" .
    tar -C "${work}/data" \
        --sort=name --owner=0 --group=0 --numeric-owner \
        -cJf "${work}/pkg/data.tar.xz" .

    (
        cd "${work}/pkg"
        ar rcD "${DEB_PKG}" debian-binary control.tar.xz data.tar.xz
    )

    local built="${work}/pkg/${DEB_PKG}"
    [ -f "${built}" ] || die "未生成 ${built}"

    rm -f "${DIST}/${DEB_PKG}"
    cp "${built}" "${DIST}/${DEB_PKG}"
    cmp -s "${built}" "${DIST}/${DEB_PKG}" || die "复制 deb 到 dist/ 失败"

    verify_deb_pkg "${DIST}/${DEB_PKG}"

    info "产物："
    printf '  %s\n' "$(cd "${DIST}" && pwd)/${DEB_PKG}"
    printf '  sha256 %s\n' "$(sha256sum "${DIST}/${DEB_PKG}" | cut -d' ' -f1)"

    rm -rf "${work}"
    trap - EXIT
}

main "$@"
