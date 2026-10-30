#!/bin/bash
# 此脚本由应用写入独立更新目录，在旧进程退出后替换完整发行包。
set -uo pipefail
work="$(cd "$(dirname "$0")" && pwd)"
token="$(basename "$work")"
exec >> "$work/apply.log" 2>&1

install_tree() {
    local incoming="$target.pingkk-new-$token"
    local backup="$target.pingkk-old-$token"
    [ ! -e "$incoming" ] && [ ! -L "$incoming" ] &&
        [ ! -e "$backup" ] && [ ! -L "$backup" ] || return 1
    if [ "$kind" = macos ]; then
        /usr/bin/ditto "$extracted/pingkk.app" "$incoming" || return 1
    else
        cp -a "$extracted" "$incoming" || return 1
    fi
    if ! mv "$target" "$backup"; then
        rm -rf "$incoming"
        return 1
    fi
    if ! mv "$incoming" "$target"; then
        mv "$backup" "$target"
        return 1
    fi
    local cli="$target/bin/pingkk"
    [ "$kind" != macos ] || cli="$target/Contents/MacOS/pingkk-cli"
    if [ "$("$cli" --version)" != "$expected" ]; then
        if mv "$target" "$incoming" && mv "$backup" "$target"; then
            rm -rf "$incoming"
        fi
        return 1
    fi
    rm -rf "$backup" || true
}

# macOS 受保护的应用目录由系统认证窗口授权，不以 root 身份重启应用。
if [ "${1:-}" = --install-only ]; then
    kind=$2 target=$3 extracted=$4 expected=$5
    install_tree
    exit $?
fi

parent_pid=$1 kind=$2 target=$3 package=$4 extracted=$5 expected=$6 language=$7
for ((attempt=0; attempt<120; attempt++)); do
    kill -0 "$parent_pid" 2>/dev/null || break
    sleep 0.25
done
if kill -0 "$parent_pid" 2>/dev/null; then
    printf 'error\nThe application did not exit; no files were changed.\n%s\n' "$language" > "$work/result.txt"
    exit 1
fi

installed=1
case "$kind" in
    macos)
        if [ -w "$(dirname "$target")" ]; then
            install_tree || installed=0
        else
            /usr/bin/osascript - "$0" --install-only "$kind" "$target" "$extracted" "$expected" <<'APPLESCRIPT' || installed=0
on run argv
    set commandText to "/bin/bash"
    repeat with argumentText in argv
        set commandText to commandText & " " & quoted form of (argumentText as text)
    end repeat
    do shell script commandText with administrator privileges
end run
APPLESCRIPT
        fi
        ;;
    linux-directory) install_tree || installed=0 ;;
    linux-deb) pkexec /usr/bin/apt-get install -y "$package" || installed=0 ;;
    *) installed=0 ;;
esac

if [ "$installed" = 1 ]; then
    printf 'ok\n%s\n%s\n' "$expected" "$language" > "$work/result.txt"
else
    if [ "$language" = en ]; then
        printf 'error\nUpdate failed or permission was denied. Please check the update log.\nen\n' > "$work/result.txt"
    else
        printf 'error\n更新失败或未获得安装权限，请查看更新日志。\nzh\n' > "$work/result.txt"
    fi
fi

if [ "$kind" = macos ]; then
    /usr/bin/open -n "$target" --args --update-result "$work/result.txt"
    launched=$?
elif [ "$kind" = linux-deb ]; then
    /usr/bin/pingkk-gui --update-result "$work/result.txt" </dev/null >> "$work/apply.log" 2>&1 &
    launched=$?
else
    "$target/run-pingkk.sh" --update-result "$work/result.txt" </dev/null >> "$work/apply.log" 2>&1 &
    launched=$?
fi
if [ "$installed" = 1 ] && [ "$launched" = 0 ]; then
    # 保留小体积的结果和日志，移除本次下载、解压的数据。
    rm -f "$package"
    rm -rf "$extracted"
fi
exit "$launched"
