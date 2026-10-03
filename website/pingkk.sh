#!/bin/sh
# macOS / Linux: curl -fsSL https://he.sb/pingkk.sh | sh
set -eu

fail() {
    printf '安装失败：%s\n' "$*" >&2
    exit 1
}

for tool in curl uname mktemp awk install; do
    command -v "$tool" >/dev/null 2>&1 || fail "缺少 $tool，请先通过系统包管理器安装。"
done

system=$(uname -s)
machine=$(uname -m)
case "$system" in
    Darwin)
        # Rosetta 下也优先使用 Apple Silicon 原生版本。
        if [ "$(sysctl -n hw.optional.arm64 2>/dev/null || true)" = 1 ]; then
            machine=arm64
        fi
        case "$machine" in
            arm64|aarch64) platform=macos-arm64 ;;
            x86_64|amd64) platform=macos-intel ;;
            *) fail "暂不支持 macOS 架构 $machine。" ;;
        esac
        extension=zip
        command -v unzip >/dev/null 2>&1 || fail '缺少 unzip。'
        ;;
    Linux)
        case "$machine" in
            x86_64|amd64) platform=linux-x64 ;;
            aarch64|arm64) platform=linux-arm64 ;;
            i386|i486|i586|i686) platform=linux-x86 ;;
            *) fail "暂不支持 Linux 架构 $machine。" ;;
        esac
        extension=tar.gz
        command -v tar >/dev/null 2>&1 || fail '缺少 tar。'
        if command -v ldd >/dev/null 2>&1 && ldd --version 2>&1 | awk '/musl/ { found=1 } END { exit !found }'; then
            fail '发布包需要 glibc 2.28 及以上，不能直接用于 Alpine / musl 系统。'
        fi
        ;;
    *) fail "暂不支持系统 $system。" ;;
esac

if command -v sha256sum >/dev/null 2>&1; then
    hash_tool=sha256sum
elif command -v shasum >/dev/null 2>&1; then
    hash_tool=shasum
else
    fail '缺少 SHA256 校验工具，请安装 sha256sum 或 shasum。'
fi

prefix=${PINGKK_PREFIX:-/usr/local}
case "$prefix" in
    /*) ;;
    *) fail 'PINGKK_PREFIX 必须是绝对路径。' ;;
esac
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/pingkk-install.XXXXXXXX")
trap 'rm -rf "$work_dir"' EXIT
trap 'exit 1' HUP INT TERM

asset="pingkk-$platform-cli.$extension"
printf '正在下载 %s…\n' "$asset"
download_url=https://he.sb/pingkk/latest
curl --proto '=https' --proto-redir '=https' -fsSL \
    -o "$work_dir/SHA256SUMS.txt" "$download_url/SHA256SUMS.txt" || fail '无法下载校验文件，请检查网站连接。'
expected=$(awk -v name="$asset" '$2 == name { print $1; exit }' "$work_dir/SHA256SUMS.txt")
[ "${#expected}" -eq 64 ] || fail "网站中缺少 $asset 的校验信息。"
curl --proto '=https' --proto-redir '=https' -fsSL \
    -o "$work_dir/$asset" "$download_url/$asset" || fail '下载失败，请检查网站连接。'
if [ "$hash_tool" = sha256sum ]; then
    actual=$(sha256sum "$work_dir/$asset" | awk '{ print $1 }')
else
    actual=$(shasum -a 256 "$work_dir/$asset" | awk '{ print $1 }')
fi
[ "$actual" = "$expected" ] || fail 'SHA256 校验不通过，请重新下载安装。'

mkdir "$work_dir/unpacked"
if [ "$extension" = zip ]; then
    unzip -q "$work_dir/$asset" -d "$work_dir/unpacked"
else
    tar -xzf "$work_dir/$asset" -C "$work_dir/unpacked"
fi
binary="$work_dir/unpacked/bin/pingkk"
[ -f "$binary" ] && [ ! -L "$binary" ] || fail '下载包中缺少 CLI 程序。'
chmod 755 "$binary"
"$binary" --help >/dev/null || fail '程序无法运行，请检查系统版本及运行库是否满足 README 中的兼容性要求。'

as_root() {
    if [ "$(id -u)" -eq 0 ] || { [ -d "$prefix" ] && [ -w "$prefix" ] && { [ ! -d "$prefix/bin" ] || [ -w "$prefix/bin" ]; } && { [ ! -d "$prefix/share" ] || [ -w "$prefix/share" ]; }; }; then
        "$@"
    else
        command -v sudo >/dev/null 2>&1 || fail "安装到 $prefix 需要管理员权限，请使用 root 或设置 PINGKK_PREFIX。"
        sudo "$@"
    fi
}

as_root mkdir -p "$prefix/bin" "$prefix/share/pingkk/licenses"
as_root install -m 644 "$work_dir/unpacked/licenses/"* "$prefix/share/pingkk/licenses/"
as_root install -m 755 "$binary" "$prefix/bin/pingkk"
printf '安装完成：%s/bin/pingkk\n' "$prefix"
case ":${PATH:-}:" in
    *":$prefix/bin:"*) printf '运行 pingkk --help 开始使用。\n' ;;
    *) printf '请将 %s/bin 加入 PATH，或直接运行 %s/bin/pingkk --help。\n' "$prefix" "$prefix" ;;
esac
