#!/usr/bin/env bash
# Linux server: synchronize installer scripts and all latest release assets.
set -Eeuo pipefail
umask 022

site_root=${PINGKK_SITE_ROOT:-/opt/1panel/www/sites/he.sb/index}
repository=trah01/pingkk

log() { printf '[%s] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*"; }
fail() { log "同步失败：$*" >&2; exit 1; }
for tool in curl python3 sha256sum flock mktemp install; do
    command -v "$tool" >/dev/null 2>&1 || fail "缺少 $tool，请先安装。"
done
[[ "$site_root" = /* ]] || fail '网站目录必须是绝对路径。'
[[ -d "$site_root" && -w "$site_root" ]] || fail "网站目录不存在或不可写：$site_root"

# Prevent overlapping scheduled runs. The lock is outside the public website.
exec 9>/tmp/pingkk-site-sync.lock
if ! flock -n 9; then
    log '已有同步任务运行，本次跳过。'
    exit 0
fi
work_dir=$(mktemp -d "$site_root/.pingkk-sync.XXXXXXXX")
trap 'rm -rf "$work_dir"' EXIT
trap 'exit 1' HUP INT TERM
trap 'log "同步失败（第 $LINENO 行），请检查 GitHub 网络连接、发布附件及目录权限；未重试或切换镜像。" >&2' ERR

download() {
    curl --proto '=https' --proto-redir '=https' -fsSL \
        --connect-timeout 20 --max-time 1800 -o "$2" "$1"
}

log '检查最新正式 Release 和 main 分支安装脚本。'
download "https://api.github.com/repos/$repository/releases/latest" "$work_dir/release.json"
download "https://api.github.com/repos/$repository/commits/main" "$work_dir/commit.json"
release_tag=$(python3 - "$work_dir/release.json" "$work_dir/assets.txt" <<'PY'
import json, re, sys
with open(sys.argv[1], encoding='utf-8') as stream:
    release = json.load(stream)
tag = release['tag_name']
if not re.fullmatch(r'v[0-9][A-Za-z0-9._+-]*', tag):
    raise SystemExit('Release 标签必须是以 v 和数字开头的安全目录名：' + repr(tag))
names = [asset['name'] for asset in release['assets']]
if 'SHA256SUMS.txt' not in names:
    raise SystemExit('Release 缺少 SHA256SUMS.txt。')
if len(names) != len(set(names)):
    raise SystemExit('Release 包含重复的附件名称。')
for name in names:
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._+-]*', name):
        raise SystemExit('Release 包含不支持的附件名称：' + repr(name))
packages = [name for name in names if name != 'SHA256SUMS.txt']
if not packages:
    raise SystemExit('Release 中没有可下载的发布文件。')
with open(sys.argv[2], 'w', encoding='utf-8') as stream:
    stream.write('\n'.join(packages) + '\n')
print(tag)
PY
)
version_dir="$site_root/pingkk/$release_tag"
latest_dir="$site_root/pingkk/latest"
[[ ! -L "$version_dir" ]] || fail '版本目录不能是符号链接。'
[[ ! -e "$latest_dir" || -L "$latest_dir" ]] || fail 'latest 已是普通目录，请先手动移走后再同步。'
assets=()
while IFS= read -r asset; do
    assets+=("$asset")
done < "$work_dir/assets.txt"
log "本次同步 ${#assets[@]} 个发布文件及 SHA256SUMS.txt。"
commit=$(python3 - "$work_dir/commit.json" <<'PY'
import json, re, sys
with open(sys.argv[1], encoding='utf-8') as stream:
    commit = json.load(stream)['sha']
if not re.fullmatch(r'[0-9a-f]{40}', commit):
    raise SystemExit('GitHub 返回了无效的提交编号。')
print(commit)
PY
)

for script in pingkk.sh pingkk.ps1; do
    download "https://raw.githubusercontent.com/$repository/$commit/website/$script" "$work_dir/$script"
    [[ -s "$work_dir/$script" ]] || fail "安装脚本为空：$script"
done
sh -n "$work_dir/pingkk.sh"

release_url="https://github.com/$repository/releases/download/$release_tag"
download "$release_url/SHA256SUMS.txt" "$work_dir/release-checksums.txt"
mkdir "$work_dir/packages"
: > "$work_dir/packages/SHA256SUMS.txt"
for asset in "${assets[@]}"; do
    expected=$(awk -v name="$asset" '$2 == name { print $1; exit }' "$work_dir/release-checksums.txt")
    [[ "$expected" =~ ^[0-9a-fA-F]{64}$ ]] || fail "缺少有效的 SHA256：$asset"
    if [[ -f "$version_dir/$asset" ]] && \
        [[ "$(sha256sum "$version_dir/$asset" | awk '{ print $1 }')" = "$expected" ]]; then
        log "文件未变化，复用：$asset"
        cp "$version_dir/$asset" "$work_dir/packages/$asset"
    else
        log "下载：$asset"
        download "$release_url/$asset" "$work_dir/packages/$asset"
    fi
    printf '%s  %s\n' "$expected" "$asset" >> "$work_dir/packages/SHA256SUMS.txt"
done
(cd "$work_dir/packages" && sha256sum -c SHA256SUMS.txt)
python3 - "$release_tag" "$work_dir/packages/version.json" <<'PY'
import json, sys
with open(sys.argv[2], 'w', encoding='utf-8') as stream:
    json.dump({'tag_name': sys.argv[1]}, stream)
    stream.write('\n')
PY

# Publish a complete version before switching the relative latest symlink.
# Relative symlinks also work with 1Panel's different host/container root paths.
install -d -m 755 "$site_root/pingkk"
chmod 644 "$work_dir/pingkk.sh" "$work_dir/pingkk.ps1" "$work_dir/packages/"*
if [[ ! -e "$version_dir" ]]; then
    mv "$work_dir/packages" "$version_dir"
else
    [[ -d "$version_dir" ]] || fail "版本路径不是目录：$version_dir"
    for asset in "${assets[@]}"; do
        mv -f "$work_dir/packages/$asset" "$version_dir/$asset"
    done
    mv -f "$work_dir/packages/SHA256SUMS.txt" "$version_dir/SHA256SUMS.txt"
    mv -f "$work_dir/packages/version.json" "$version_dir/version.json"
fi
ln -s "$release_tag" "$work_dir/latest"
python3 - "$work_dir/latest" "$latest_dir" <<'PY'
import os, sys
os.replace(sys.argv[1], sys.argv[2])
PY
mv -f "$work_dir/pingkk.sh" "$site_root/pingkk.sh"
mv -f "$work_dir/pingkk.ps1" "$site_root/pingkk.ps1"
log "同步完成：pingkk/$release_tag/，latest -> $release_tag，脚本提交 $commit。"
