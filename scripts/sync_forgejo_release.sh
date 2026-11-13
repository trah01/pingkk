#!/usr/bin/env bash
# CI: copy release files to the Forgejo release with the same tag.
# Usage: FORGEJO_REPO_URL=... FORGEJO_TOKEN=... sync_forgejo_release.sh <tag> <commit> <files-dir>
set -euo pipefail

tag=$1
commit=$2
files_dir=$3
if [[ -z "${FORGEJO_TOKEN:-}" || -z "${FORGEJO_REPO_URL:-}" ]]; then
    echo "::warning::未配置 FORGEJO_REPO_URL 或 FORGEJO_TOKEN，跳过同步到 Forgejo。"
    exit 0
fi

# Accept common forms: surrounding spaces, http://, no scheme, trailing / or .git.
repo_url=$(printf '%s' "${FORGEJO_REPO_URL}" | tr -d '[:space:]')
repo_url=${repo_url#http://}
repo_url=${repo_url#https://}
repo_url=${repo_url%/}
repo_url=${repo_url%.git}
host=${repo_url%%/*}
repository=${repo_url#*/}
echo "::add-mask::${host}"
if [[ ! "${repository}" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]]; then
    echo "FORGEJO_REPO_URL 应为仓库网址，例如 https://git.example.com/owner/pingkk" >&2
    exit 1
fi
api="https://${host}/api/v1/repos/${repository}"

request() {
    curl --proto '=https' -sS --retry 3 --connect-timeout 20 \
        -H "Authorization: token ${FORGEJO_TOKEN}" "$@"
}
# Public CI logs must not reveal hosts, so only the status and a hint are printed.
expect() {
    local wanted=$1 status=$2
    if [[ " ${wanted} " != *" ${status} "* ]]; then
        echo "Forgejo 请求失败：HTTP ${status}" >&2
        if [[ "${status}" == 30[0-9] ]]; then
            echo "请求被重定向，可能是 Forgejo 前置网关要求登录或限制了访问来源。" >&2
        fi
        exit 1
    fi
    echo "${status}"
}

status=$(expect "200 404" "$(request -o response.json -w '%{http_code}' \
    "${api}/releases/tags/${tag}")")
if [[ "${status}" == 404 ]]; then
    # Forgejo creates the tag from the same commit, which must already be pushed there.
    jq -n --arg tag "${tag}" --arg sha "${commit}" \
        '{tag_name: $tag, target_commitish: $sha, name: ("pingkk " + $tag), draft: false, prerelease: false}' \
        > request.json
    expect "201" "$(request -X POST -H 'Content-Type: application/json' --data @request.json \
        -o response.json -w '%{http_code}' "${api}/releases")" >/dev/null
fi
cp response.json release.json
release_id=$(jq -r '.id' release.json)

for file in "${files_dir}"/*; do
    name=$(basename "${file}")
    # Replace attachments with the same name so reruns match the GitHub release.
    jq -r --arg name "${name}" '.assets[]? | select(.name == $name) | .id' release.json |
        while read -r asset_id; do
            expect "204" "$(request -X DELETE -o response.json -w '%{http_code}' \
                "${api}/releases/${release_id}/assets/${asset_id}")" >/dev/null
        done
    echo "上传：${name}"
    expect "201" "$(request -X POST --max-time 1800 -F "attachment=@${file}" \
        -o response.json -w '%{http_code}' \
        "${api}/releases/${release_id}/assets?name=${name}")" >/dev/null
done
echo "已同步 ${tag} 的 $(find "${files_dir}" -maxdepth 1 -type f | wc -l | tr -d ' ') 个文件到 Forgejo。"
