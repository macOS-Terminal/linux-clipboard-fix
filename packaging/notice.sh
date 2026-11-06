# cbfix-bridge 安装后提示 —— 唯一实现。
# 被 scripts/install.sh source，并被 build 脚本原样内联进
# Arch .install / RPM %post / deb postinst。修改提示只改这里。
#
# 用法：cbfix_notice install|upgrade
# 颜色：默认开启（pacman 等包管理器会把子进程输出转发到终端，
#       所以不能用 [ -t 1 ] 判断）；设置 NO_COLOR 可关闭。

cbfix_notice() {
    if [ -n "${NO_COLOR:-}" ]; then
        _g='' _c='' _y='' _d='' _r=''
    else
        _g='\033[1;32m' _c='\033[1;36m' _y='\033[1;33m' _d='\033[2m' _r='\033[0m'
    fi
    _line='================================================================='

    printf "${_g}%s${_r}\n" "$_line"
    if [ "${1:-install}" = "upgrade" ]; then
        printf "${_c}  cbfix-bridge 已更新！重启服务以生效：${_r}\n"
        printf "${_y}    systemctl --user restart cbfix-bridge.service${_r}\n"
    else
        printf "${_c}  cbfix-bridge 安装完成！用当前用户（不要 sudo）启用服务：${_r}\n"
        printf "${_y}    systemctl --user enable --now cbfix-bridge.service${_r}\n"
    fi
    printf "\n"
    printf "  自检：${_y}cbfix-bridge --selftest${_r}    日志：${_y}journalctl --user -u cbfix-bridge -f${_r}\n"
    printf "${_g}%s${_r}\n" "$_line"
    printf "${_d}  不生效？先执行 systemctl --user import-environment DISPLAY WAYLAND_DISPLAY\n"
    printf "  再重启服务。需要合成器支持 ext-data-control-v1（niri≥25.05 / sway≥1.10 / KWin≥6.3）。${_r}\n"
}
