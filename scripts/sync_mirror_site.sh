#!/usr/bin/env bash
# Synchronize the domestic mirror exclusively from he.sb.
set -Eeuo pipefail
umask 022

site_root=${PINGKK_SITE_ROOT:-/opt/1panel/www/sites/trah.cn/index}
upstream=https://he.sb
mirror=https://sh.trah.cn

log() { printf '[%s] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*"; }
fail() { log "同步失败：$*" >&2; exit 1; }
for tool in curl python3 sha256sum flock mktemp install; do
    command -v "$tool" >/dev/null 2>&1 || fail "缺少 $tool。"
done
[[ "$site_root" = /* && -d "$site_root" && -w "$site_root" ]] || fail "网站目录不可写：$site_root"
exec 9>/run/lock/pingkk-mirror-sync.lock
if ! flock -n 9; then
    log '已有镜像同步任务运行，本次跳过。'
    exit 0
fi
work_dir=$(mktemp -d "$site_root/.pingkk-mirror-sync.XXXXXXXX")
trap 'rm -rf "$work_dir"' EXIT
trap 'exit 1' HUP INT TERM
trap 'log "同步失败（第 $LINENO 行），保留当前已发布版本。" >&2' ERR

download() {
    curl -4 --proto '=https' -fsS \
        --connect-timeout 15 --max-time 1800 -o "$2" "$1"
}

log '从 he.sb 获取最新版本信息。'
download "$upstream/pingkk/latest/version.json" "$work_dir/latest-version.json"
release_tag=$(python3 - "$work_dir/latest-version.json" <<'PY'
import json, re, sys
with open(sys.argv[1], encoding='utf-8') as stream:
    tag = json.load(stream).get('tag_name', '')
if not isinstance(tag, str) or not re.fullmatch(r'v?[0-9]+\.[0-9]+\.[0-9]+', tag):
    raise SystemExit('上游版本信息格式不正确。')
print(tag if tag.startswith('v') else 'v' + tag)
PY
)
base="$upstream/pingkk/$release_tag"
version_dir="$site_root/pingkk/$release_tag"
latest_dir="$site_root/pingkk/latest"
[[ ! -L "$version_dir" ]] || fail '版本目录不能是符号链接。'
[[ ! -e "$latest_dir" || -L "$latest_dir" ]] || fail 'latest 已是普通目录，请先手动移走后再同步。'
mkdir "$work_dir/packages"
download "$base/version.json" "$work_dir/packages/version.json"
download "$base/SHA256SUMS.txt" "$work_dir/packages/SHA256SUMS.txt"
python3 - "$release_tag" "$work_dir/packages" "$work_dir/assets.txt" <<'PY'
import json, re, sys
from pathlib import Path
directory = Path(sys.argv[2])
tag = json.loads((directory / 'version.json').read_text()).get('tag_name', '')
if tag not in (sys.argv[1], sys.argv[1][1:]):
    raise SystemExit('上游指定版本与版本信息不一致。')
names = []
for line in (directory / 'SHA256SUMS.txt').read_text().splitlines():
    match = re.fullmatch(r'([a-fA-F0-9]{64}) [ *]([A-Za-z0-9][A-Za-z0-9._+-]*)', line)
    if not match:
        raise SystemExit('上游校验文件格式或文件名不正确。')
    name = match[2]
    if not name.endswith(('.zip', '.tar.gz', '.tar.xz', '.exe', '.deb')):
        raise SystemExit('上游校验文件包含不支持的发布文件。')
    names.append(name)
if not names or len(names) != len(set(names)):
    raise SystemExit('上游发布文件为空或包含重复文件。')
Path(sys.argv[3]).write_text('\n'.join(names) + '\n')
PY

assets=()
while IFS= read -r asset; do
    assets+=("$asset")
    expected=$(awk -v name="$asset" '$2 == name || $2 == "*" name { print $1; exit }' "$work_dir/packages/SHA256SUMS.txt")
    if [[ -f "$version_dir/$asset" && ! -L "$version_dir/$asset" ]] && \
        [[ "$(sha256sum "$version_dir/$asset" | awk '{ print $1 }')" = "${expected,,}" ]]; then
        log "复用：$asset"
        cp "$version_dir/$asset" "$work_dir/packages/$asset"
    else
        log "从 he.sb 下载：$asset"
        download "$base/$asset" "$work_dir/packages/$asset"
    fi
done < "$work_dir/assets.txt"
(cd "$work_dir/packages" && sha256sum -c SHA256SUMS.txt)

for script in pingkk.sh pingkk.ps1; do
    download "$upstream/$script" "$work_dir/$script"
done
python3 - "$work_dir" "$upstream" "$mirror" <<'PY'
import sys
from pathlib import Path
for name in ('pingkk.sh', 'pingkk.ps1'):
    path = Path(sys.argv[1]) / name
    text = path.read_text(encoding='utf-8-sig')
    if sys.argv[2] not in text or 'github.com' in text.lower():
        raise SystemExit('上游安装脚本来源不符合镜像规则。')
    path.write_text(text.replace(sys.argv[2], sys.argv[3]), encoding='utf-8')
PY
sh -n "$work_dir/pingkk.sh"
chmod 644 "$work_dir/packages/"* "$work_dir/pingkk.sh" "$work_dir/pingkk.ps1"

install -d -m 755 "$site_root/pingkk"
if [[ ! -e "$version_dir" ]]; then
    mv "$work_dir/packages" "$version_dir"
else
    [[ -d "$version_dir" ]] || fail '版本路径不是目录。'
    for asset in "${assets[@]}" version.json SHA256SUMS.txt; do
        mv -f "$work_dir/packages/$asset" "$version_dir/$asset"
    done
fi
ln -s "$release_tag" "$work_dir/latest"
mv -f "$work_dir/pingkk.sh" "$site_root/pingkk.sh"
mv -f "$work_dir/pingkk.ps1" "$site_root/pingkk.ps1"
python3 - "$work_dir/latest" "$latest_dir" <<'PY'
import os, sys
os.replace(sys.argv[1], sys.argv[2])
PY
log "同步完成：${#assets[@]} 个发布文件，latest -> $release_tag，安装脚本下载地址为 $mirror。"
