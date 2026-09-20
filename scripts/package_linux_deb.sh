#!/usr/bin/env bash
set -euo pipefail

gui_output_dir="${1:?缺少图形版目录}"
architecture="${2:?缺少 Debian 架构}"
version="${3:?缺少版本号}"
output_file="${4:?缺少输出文件}"

staging_dir="$(mktemp -d)"
trap 'rm -rf "${staging_dir}"' EXIT

app_dir="${staging_dir}/usr/lib/pingkk"
mkdir -p \
    "${app_dir}" \
    "${staging_dir}/usr/bin" \
    "${staging_dir}/usr/share/applications" \
    "${staging_dir}/usr/share/icons/hicolor/512x512/apps" \
    "${staging_dir}/usr/share/doc/pingkk" \
    "${staging_dir}/DEBIAN"

cp -R "${gui_output_dir}/bin" "${app_dir}/bin"
cp -R "${gui_output_dir}/lib" "${app_dir}/lib"
cp -R "${gui_output_dir}/plugins" "${app_dir}/plugins"
cp "${gui_output_dir}/bin/pingkk" "${staging_dir}/usr/bin/pingkk"
cp packaging/linux/pingkk.desktop "${staging_dir}/usr/share/applications/pingkk.desktop"
cp assets/pingkk-icon.png \
    "${staging_dir}/usr/share/icons/hicolor/512x512/apps/pingkk.png"
cp licenses/* "${staging_dir}/usr/share/doc/pingkk/"

cat > "${staging_dir}/usr/bin/pingkk-gui" <<'SCRIPT'
#!/usr/bin/env bash
set -euo pipefail
app_dir="/usr/lib/pingkk"
export LD_LIBRARY_PATH="${app_dir}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export QT_PLUGIN_PATH="${app_dir}/plugins"
exec "${app_dir}/bin/pingkk-gui" "$@"
SCRIPT
chmod 755 "${staging_dir}/usr/bin/pingkk" "${staging_dir}/usr/bin/pingkk-gui"

installed_size="$(du -sk "${staging_dir}/usr" | awk '{print $1}')"
cat > "${staging_dir}/DEBIAN/control" <<CONTROL
Package: pingkk
Version: ${version}
Section: net
Priority: optional
Architecture: ${architecture}
Installed-Size: ${installed_size}
Maintainer: trah01
Homepage: https://github.com/trah01/pingkk
Description: ping看看端口连通性测试工具
 提供 TCP、UDP、Ping 和路由追踪的图形界面与命令行工具。
CONTROL

dpkg-deb --build "${staging_dir}" "${output_file}"
