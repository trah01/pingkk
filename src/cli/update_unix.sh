#!/bin/sh
# Embedded in the CLI; only trusted, checksum-verified release packages are used.
set -eu

target=$1
current_version=$2
language=$3
staged=
work_dir=
say() {
    if [ "${language}" = en ]; then printf '%s\n' "$2"; else printf '%s\n' "$1"; fi
}
fail() { say "$1" "$2" >&2; exit 1; }
cleanup() {
    [ -z "${staged}" ] || as_target_user rm -f "${staged}" >/dev/null 2>&1 || true
    [ -z "${work_dir}" ] || rm -rf "${work_dir}"
}
trap 'update_status=$?; cleanup; exit "${update_status}"' EXIT
trap 'exit 1' HUP INT TERM
for tool in curl uname mktemp awk sed tr mkdir chmod mv install; do
    command -v "${tool}" >/dev/null 2>&1 || fail "缺少工具：${tool}。" "Required tool not found: ${tool}."
done
case "${target}" in
    /*.app/Contents/MacOS/*) fail '请下载图形界面包更新应用包中的程序。' 'Download the GUI package to update a program inside an app bundle.' ;;
    /*) ;;
    *) fail '无法确定当前程序的绝对路径。' 'Unable to determine the absolute path of the current program.' ;;
esac
[ -f "${target}" ] && [ ! -L "${target}" ] || fail '当前程序不存在或路径已变化。' 'The current program is missing or its path has changed.'

system=$(uname -s)
machine=$(uname -m)
case "${system}" in
    Darwin)
        if [ "$(sysctl -n hw.optional.arm64 2>/dev/null || true)" = 1 ]; then machine=arm64; fi
        case "${machine}" in
            arm64|aarch64) platform=macos-arm64 ;;
            x86_64|amd64) platform=macos-intel ;;
            *) fail "不支持的架构：${machine}。" "Unsupported architecture: ${machine}." ;;
        esac
        extension=zip
        command -v unzip >/dev/null 2>&1 || fail '缺少 unzip。' 'unzip is required.'
        ;;
    Linux)
        case "${machine}" in
            x86_64|amd64) platform=linux-x64 ;;
            aarch64|arm64) platform=linux-arm64 ;;
            i386|i486|i586|i686) platform=linux-x86 ;;
            *) fail "不支持的架构：${machine}。" "Unsupported architecture: ${machine}." ;;
        esac
        extension=tar.gz
        command -v tar >/dev/null 2>&1 || fail '缺少 tar。' 'tar is required.'
        if command -v ldd >/dev/null 2>&1 && ldd --version 2>&1 | awk '/musl/ { found=1 } END { exit !found }'; then
            fail '发布包需要 glibc 2.28 及以上，不支持 musl。' 'Release packages require glibc 2.28 or later and do not support musl.'
        fi
        ;;
    *) fail "不支持的系统：${system}。" "Unsupported system: ${system}." ;;
esac
if command -v sha256sum >/dev/null 2>&1; then hash_tool=sha256sum
elif command -v shasum >/dev/null 2>&1; then hash_tool=shasum
else fail '缺少 SHA256 校验工具。' 'A SHA256 checksum tool is required.'
fi

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/pingkk-update.XXXXXXXX")
asset="pingkk-${platform}-cli.${extension}"
github=https://github.com/trah01/pingkk
mirror=https://he.sb/pingkk/latest
say '正在检查更新，GitHub 连接超时为 1000ms……' 'Checking for updates; GitHub connection timeout is 1000ms...'
# The entire HEAD request, including redirects, must finish within 1000ms.
release_url=$(curl --proto '=https' --proto-redir '=https' -fsSLI \
    --connect-timeout 1 --max-time 1 -o /dev/null -w '%{url_effective}' \
    "${github}/releases/latest" 2>/dev/null) || release_url=
source=mirror
base=${mirror}
case "${release_url}" in
    "${github}/releases/tag/"*)
        tag=${release_url##*/}
        if printf '%s\n' "${tag}" | awk '/^v?[0-9]+\.[0-9]+\.[0-9]+$/ {ok=1} END {exit !ok}'; then
            source=github
            base="${github}/releases/download/${tag}"
        fi
        ;;
esac

version_is_newer() {
    awk -v old="${current_version}" -v new="$1" 'BEGIN {
        split(old,a,"."); split(new,b,".");
        for(i=1;i<=3;i++) {if(b[i]+0>a[i]+0)exit 0; if(b[i]+0<a[i]+0)exit 1} exit 1
    }'
}
no_update_needed() {
    if [ "${current_version}" = "$1" ]; then
        say "当前版本 ${current_version} 已是最新。" "Current version ${current_version} is up to date."
    else
        say "下载源版本 $1 较旧，保留当前版本 ${current_version}。" "The source version $1 is older; keeping current version ${current_version}."
    fi
}
if [ "${source}" = github ] && ! version_is_newer "${tag#v}"; then
    no_update_needed "${tag#v}"
    exit 0
fi

download() {
    if [ "${source}" = github ]; then
        curl --proto '=https' --proto-redir '=https' -fsSL \
            --connect-timeout 1 --speed-limit 1 --speed-time 1 --max-time 1800 -o "$2" "$1"
    else
        curl --proto '=https' --proto-redir '=https' -fsSL \
            --connect-timeout 10 --max-time 1800 -o "$2" "$1"
    fi
}
fetch_package() {
    download "${base}/SHA256SUMS.txt" "${work_dir}/SHA256SUMS.txt" &&
    download "${base}/${asset}" "${work_dir}/${asset}"
}
if [ "${source}" = github ]; then
    say '正在从 GitHub Releases 下载……' 'Downloading from GitHub Releases...'
    if ! fetch_package; then
        source=mirror
        base=${mirror}
    fi
fi
if [ "${source}" = mirror ]; then
    say 'GitHub 不可用，改从 https://he.sb/pingkk/latest/ 更新。' 'GitHub is unavailable; using https://he.sb/pingkk/latest/.'
    fetch_package || fail '更新下载失败，当前程序未改动。' 'Download failed. The current program was not changed.'
fi
expected=$(awk -v name="${asset}" '$2 == name || $2 == "*" name {print $1; exit}' "${work_dir}/SHA256SUMS.txt")
printf '%s\n' "${expected}" | awk 'length($0)==64 && $0 !~ /[^0-9a-fA-F]/ {ok=1} END {exit !ok}' ||
    fail '下载源缺少有效的 SHA256 校验信息。' 'The download source has no valid SHA256 checksum for this package.'
if [ "${hash_tool}" = sha256sum ]; then actual=$(sha256sum "${work_dir}/${asset}" | awk '{print $1}')
else actual=$(shasum -a 256 "${work_dir}/${asset}" | awk '{print $1}')
fi
expected=$(printf '%s' "${expected}" | tr 'A-F' 'a-f')
[ "${actual}" = "${expected}" ] || fail 'SHA256 校验失败，当前程序未改动。' 'SHA256 verification failed. The current program was not changed.'

mkdir "${work_dir}/unpacked"
if [ "${extension}" = zip ]; then unzip -q "${work_dir}/${asset}" -d "${work_dir}/unpacked"
else tar -xzf "${work_dir}/${asset}" -C "${work_dir}/unpacked"
fi
binary="${work_dir}/unpacked/bin/pingkk"
[ -d "${work_dir}/unpacked/bin" ] && [ ! -L "${work_dir}/unpacked/bin" ] &&
    [ -f "${binary}" ] && [ ! -L "${binary}" ] || fail '发布包中缺少命令行程序。' 'The release package does not contain the CLI executable.'
chmod 755 "${binary}"
"${binary}" --help > "${work_dir}/help.txt" || fail '新版本无法在当前系统运行，当前程序未改动。' 'The new version cannot run on this system. The current program was not changed.'
# Older releases do not yet provide --version; their help banner includes it.
new_version=$(sed -n '1s/.*v\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p' "${work_dir}/help.txt")
[ -n "${new_version}" ] || fail '无法确认新程序的版本。' 'Unable to determine the new program version.'
if [ "${source}" = github ] && [ "${tag#v}" != "${new_version}" ]; then
    fail '程序版本与 GitHub Release 不一致，当前程序未改动。' 'The executable version does not match the GitHub release. The current program was not changed.'
fi
if ! version_is_newer "${new_version}"; then
    no_update_needed "${new_version}"
    exit 0
fi

target_dir=${target%/*}
as_target_user() {
    if [ -w "${target_dir}" ]; then "$@"
    else
        command -v sudo >/dev/null 2>&1 || fail '更新此目录需要管理员权限。' 'Administrator privileges are required to update this directory.'
        sudo "$@"
    fi
}
say "正在更新 ${current_version} → ${new_version}……" "Updating ${current_version} to ${new_version}..."
# Stage on the same filesystem; rename also works while the old Unix binary runs.
staged=$(as_target_user mktemp "${target_dir}/.pingkk-update.XXXXXXXX")
as_target_user install -m 755 "${binary}" "${staged}" || fail '无法准备更新文件，当前程序未改动。' 'Unable to prepare the replacement. The current program was not changed.'
as_target_user mv -f "${staged}" "${target}" || fail '无法替换程序，当前程序未改动。' 'Unable to replace the program. The current program was not changed.'
staged=
say "更新完成：${new_version}" "Updated to ${new_version}."
