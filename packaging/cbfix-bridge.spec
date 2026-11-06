# cbfix-bridge RPM spec
#
# 由 scripts/build-rpm.sh 驱动：它把当前工作树打成 Source0，并把
# packaging/notice.sh 原样内联到下方 %post 的 @CBFIX_NOTICE@ 标记处。
# 直接对本文件跑 rpmbuild 也能构建，只是 %post 里不会有提示函数（会被安全跳过）。

%global debug_package %{nil}
%{!?_userunitdir: %global _userunitdir %{_prefix}/lib/systemd/user}

Name:           cbfix-bridge
Version:        1.0.0
Release:        1%{?dist}
Summary:        Bidirectional clipboard bridge between Wayland and X11 (Xwayland)
License:        MIT
URL:            https://github.com/macOS-Terminal/linux-clipboard-fix
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  pkgconfig(x11)
BuildRequires:  pkgconfig(xfixes)
BuildRequires:  /usr/bin/wayland-scanner

%description
cbfix-bridge is a tiny daemon that keeps the Wayland clipboard
(ext-data-control-v1) and the X11 CLIPBOARD selection (Xwayland) in sync,
in both directions, for text and images, including large INCR transfers.
It fixes X11 applications (HMCL, Codex CLI, ...) that cannot exchange
clipboard content with native Wayland applications.
It is shipped as a systemd *user* service.

%prep
%setup -q

%build
# 通过环境变量传 CFLAGS：Makefile 用 ?= 和 +=，命令行传参会覆盖掉 -std=gnu11。
CFLAGS="%{optflags}" make -B

%check
./build/cbfix-bridge --help | grep -q -- '--interval'
./build/cbfix-bridge --help | grep -q -- '--verbose'
# 注意：rpm 用 sh -e 跑脚本段，"! cmd" 不会触发 -e，所以写成显式 if。
if grep -a -q fixture ./build/cbfix-bridge; then echo "binary contains 'fixture'" >&2; exit 1; fi

%install
make install DESTDIR=%{buildroot} PREFIX=%{_prefix} BINDIR=%{_bindir}
install -d -m 0755 %{buildroot}%{_userunitdir}
sed -E 's|^ExecStart=[^[:space:]]*cbfix-bridge|ExecStart=%{_bindir}/cbfix-bridge|' \
    systemd/cbfix-bridge.service > %{buildroot}%{_userunitdir}/cbfix-bridge.service
chmod 0644 %{buildroot}%{_userunitdir}/cbfix-bridge.service
grep -qx 'ExecStart=%{_bindir}/cbfix-bridge\( .*\)\{0,1\}' %{buildroot}%{_userunitdir}/cbfix-bridge.service
install -D -m 0644 README.md %{buildroot}%{_datadir}/doc/%{name}/README.md
install -D -m 0644 LICENSE   %{buildroot}%{_datadir}/doc/%{name}/LICENSE

%post
@CBFIX_NOTICE@
if command -v cbfix_notice >/dev/null 2>&1; then
    if [ "${1:-1}" -ge 2 ] 2>/dev/null; then
        cbfix_notice upgrade || :
    else
        cbfix_notice install || :
    fi
fi
exit 0

%files
%attr(0755,root,root) %{_bindir}/cbfix-bridge
%attr(0644,root,root) %{_userunitdir}/cbfix-bridge.service
%dir %{_datadir}/doc/%{name}
%doc %attr(0644,root,root) %{_datadir}/doc/%{name}/README.md
%doc %attr(0644,root,root) %{_datadir}/doc/%{name}/LICENSE

%changelog
* Fri Nov 06 2026 cbfix contributors <chou114514chou@outlook.com> - 1.0.0-1
- Initial package built from real source (src/cbfix-bridge.c)
