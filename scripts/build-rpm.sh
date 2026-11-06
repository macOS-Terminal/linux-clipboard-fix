#!/usr/bin/env bash
# build-rpm.sh —— 从当前仓库工作树的真源码构建 RPM 包，产物放到 dist/。
set -euo pipefail

PKGNAME="cbfix-bridge"
PKGVER="1.1.4"
PKGREL="1"
ARCH="x86_64"
RPM_PKG="${PKGNAME}-${PKGVER}-${PKGREL}.${ARCH}.rpm"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "${SCRIPT_DIR}")"
DIST="${ROOT}/dist"
PKGDIR_SRC="${ROOT}/packaging"
NOTICE_SH="${PKGDIR_SRC}/notice.sh"
SPEC_TMPL="${PKGDIR_SRC}/${PKGNAME}.spec"
MARKER='@CBFIX_NOTICE@'

usage() {
    cat <<EOF
用法：$(basename "$0") [--help]

从当前仓库工作树源码构建 RPM 包：
  产物：dist/${RPM_PKG}

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

render_spec() {
    local out="$1"
    [ -f "${NOTICE_SH}" ] || die "缺少 ${NOTICE_SH}（安装后提示的唯一实现）。"
    [ -f "${SPEC_TMPL}" ] || die "缺少 ${SPEC_TMPL}。"
    [ "$(grep -cxF -- "${MARKER}" "${SPEC_TMPL}")" = "1" ] \
        || die "${SPEC_TMPL} 中必须恰好有一行标记 ${MARKER}。"
    local line
    {
        while IFS= read -r line || [ -n "${line}" ]; do
            if [ "${line}" = "${MARKER}" ]; then
                cat "${NOTICE_SH}"
            else
                printf '%s\n' "${line}"
            fi
        done < "${SPEC_TMPL}"
    } > "${out}"
}

verify_rpm_pkg() {
    local pkg="$1" chk
    chk="$(mktemp -d)"
    info "内容自检：${pkg}"

    local list vlist
    list="$(rpm -qpl "${pkg}")"
    vlist="$(rpm -qplv "${pkg}")"
    printf '%s\n' "${list}"

    local p
    for p in \
        /usr/bin/cbfix-bridge \
        /usr/lib/systemd/user/cbfix-bridge.service \
        /usr/share/doc/cbfix-bridge/README.md \
        /usr/share/doc/cbfix-bridge/LICENSE; do
        grep -qxF "${p}" <<<"${list}" || die "RPM 包内缺少 ${p}"
    done

    check_mode() {
        local path="$1" mode="$2"
        printf '%s\n' "${vlist}" | awk -v p="${path}" -v m="${mode}" \
            '$NF==p && $1==m {f=1} END{exit !f}' \
            || die "RPM 包内 ${path} 权限不是 ${mode}"
        printf '  OK  %-46s %s\n' "${path}" "${mode}"
    }
    check_mode /usr/bin/cbfix-bridge                        -rwxr-xr-x
    check_mode /usr/lib/systemd/user/cbfix-bridge.service   -rw-r--r--
    check_mode /usr/share/doc/cbfix-bridge/README.md        -rw-r--r--
    check_mode /usr/share/doc/cbfix-bridge/LICENSE          -rw-r--r--

    (
        cd "${chk}"
        rpm2cpio "${pkg}" | cpio -idm --quiet
    )

    grep -qx 'ExecStart=/usr/bin/cbfix-bridge' "${chk}/usr/lib/systemd/user/cbfix-bridge.service" \
        || die "RPM 包内 unit 的 ExecStart 不是 /usr/bin/cbfix-bridge"
    printf '  OK  unit: %s\n' "$(grep '^ExecStart=' "${chk}/usr/lib/systemd/user/cbfix-bridge.service")"

    local help
    help="$("${chk}/usr/bin/cbfix-bridge" --help 2>&1)" || die "RPM 包内二进制 --help 执行失败"
    grep -q -- '--interval' <<<"${help}" || die "RPM 包内二进制 --help 缺少 --interval"
    grep -q -- '--verbose'  <<<"${help}" || die "RPM 包内二进制 --help 缺少 --verbose"
    if grep -qi fixture <<<"${help}" || grep -aqi 'fixture' "${chk}/usr/bin/cbfix-bridge"; then
        die "RPM 包内二进制含 fixture 字样，疑似假二进制"
    fi
    printf '  OK  --help 含 --interval/--verbose，无 fixture 字样\n'

    local lib deps
    deps="$(ldd "${chk}/usr/bin/cbfix-bridge")"
    for lib in libwayland-client.so.0 libX11.so.6 libXfixes.so.3; do
        grep -q "${lib} => /" <<<"${deps}" \
            || die "RPM 包内二进制未链接 ${lib}"
        printf '  OK  链接 %s\n' "${lib}"
    done

    local scripts
    scripts="$(rpm -qp --scripts "${pkg}")"
    grep -q 'cbfix_notice()' <<<"${scripts}" && grep -q 'cbfix_notice install' <<<"${scripts}" \
        || die "RPM %post 未内联 cbfix_notice"
    printf '  OK  %%post 已内联 cbfix_notice\n'

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

    need_cmd rpmbuild rpm rpm2cpio cpio tar gzip sha256sum ldd pkg-config wayland-scanner gcc make

    RPM_WORK="$(mktemp -d "${TMPDIR:-/tmp}/cbfix-rpm.XXXXXX")"
    local work="${RPM_WORK}"
    trap 'rm -rf "${RPM_WORK:-}"' EXIT

    mkdir -p "${work}"/{BUILD,RPMS,SOURCES,SPECS,SRPMS} "${DIST}"
    local srcname="${PKGNAME}-${PKGVER}"

    info "打包当前工作树源码 -> SOURCES/${srcname}.tar.gz"
    tar -C "${ROOT}" \
        --exclude=./.git --exclude=./build --exclude=./dist \
        --exclude="./${srcname}" --exclude='./cbfix-*' --exclude='*.tar.gz' \
        --exclude='*.pkg.tar.zst' --exclude='*.rpm' --exclude='*.deb' \
        --exclude='*.class' --exclude='*.o' \
        --sort=name --owner=0 --group=0 --numeric-owner \
        --transform="s,^\.,${srcname}," \
        -czf "${work}/SOURCES/${srcname}.tar.gz" .

    local srclist
    srclist="$(tar -tzf "${work}/SOURCES/${srcname}.tar.gz")"
    grep -qx "${srcname}/src/cbfix-bridge.c" <<<"${srclist}" \
        || die "源码包中缺少 src/cbfix-bridge.c"
    if grep -q "^${srcname}/build/" <<<"${srclist}"; then
        die "源码包意外包含 build/ 目录"
    fi

    info "生成 SPECS/${PKGNAME}.spec（内联 packaging/notice.sh）"
    render_spec "${work}/SPECS/${PKGNAME}.spec"

    info "运行 rpmbuild 构建 RPM 包"
    rpmbuild \
        --define "_topdir ${work}" \
        --define "dist %{nil}" \
        --nodeps \
        -bb "${work}/SPECS/${PKGNAME}.spec"

    local built="${work}/RPMS/${ARCH}/${RPM_PKG}"
    [ -f "${built}" ] || die "rpmbuild 未产出 ${built}"

    rm -f "${DIST}/${RPM_PKG}"
    cp "${built}" "${DIST}/${RPM_PKG}"
    cmp -s "${built}" "${DIST}/${RPM_PKG}" || die "复制 RPM 到 dist/ 失败"

    verify_rpm_pkg "${DIST}/${RPM_PKG}"

    info "产物："
    printf '  %s\n' "$(cd "${DIST}" && pwd)/${RPM_PKG}"
    printf '  sha256 %s\n' "$(sha256sum "${DIST}/${RPM_PKG}" | cut -d' ' -f1)"

    rm -rf "${work}"
    trap - EXIT
}

main "$@"
