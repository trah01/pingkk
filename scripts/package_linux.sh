#!/usr/bin/env bash
set -euo pipefail

build_dir="${1:?缺少构建目录}"
gui_output_dir="${2:?缺少图形版输出目录}"
cli_output_dir="${3:?缺少命令行版输出目录}"

cmake --install "${build_dir}" --prefix "${gui_output_dir}" --strip

mkdir -p "${gui_output_dir}/lib" "${gui_output_dir}/plugins/platforms"
cp -R licenses "${gui_output_dir}/licenses"
plugin_dir="$(qmake -query QT_INSTALL_PLUGINS)"
cp "${plugin_dir}/platforms/libqxcb.so" "${gui_output_dir}/plugins/platforms/"

copy_dependencies() {
    ldd "$1" | awk '/=> \// { print $3 }' | while IFS= read -r library; do
        case "$(basename "${library}")" in
            libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|ld-linux*)
                continue
                ;;
        esac
        cp -L -n "${library}" "${gui_output_dir}/lib/"
    done
}

copy_dependencies "${gui_output_dir}/bin/pingkk-gui"
copy_dependencies "${gui_output_dir}/plugins/platforms/libqxcb.so"

# 递归补齐 Qt 和插件的间接依赖，直到不再产生新文件。
previous_count=-1
while true; do
    current_count="$(find "${gui_output_dir}/lib" -maxdepth 1 -type f | wc -l)"
    if [ "${current_count}" -eq "${previous_count}" ]; then
        break
    fi
    previous_count="${current_count}"
    for library in "${gui_output_dir}"/lib/*.so*; do
        copy_dependencies "${library}"
    done
done

# Preserve the required dependency closure; remove only non-runtime symbols.
find "${gui_output_dir}/lib" "${gui_output_dir}/plugins" -type f \
    -exec strip --strip-unneeded '{}' +

cat > "${gui_output_dir}/run-pingkk.sh" <<'SCRIPT'
#!/usr/bin/env bash
set -euo pipefail
app_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
export LD_LIBRARY_PATH="${app_dir}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export QT_PLUGIN_PATH="${app_dir}/plugins"
exec "${app_dir}/bin/pingkk-gui" "$@"
SCRIPT
chmod +x "${gui_output_dir}/run-pingkk.sh"

cat > "${gui_output_dir}/使用说明.txt" <<'TEXT'
图形界面：运行 ./run-pingkk.sh
图形版已内置命令行程序，供图形界面测试和“安装命令行工具”功能使用。

如果目标系统缺少图形界面的底层 X11/xcb 组件，请通过系统包管理器安装 libxcb、libxkbcommon-x11 和相关运行库。
Qt 许可证和使用说明位于 licenses 目录。
TEXT

mkdir -p "${cli_output_dir}/bin"
cp "${gui_output_dir}/bin/pingkk" "${cli_output_dir}/bin/pingkk"
cp -R licenses "${cli_output_dir}/licenses"
cat > "${cli_output_dir}/使用说明.txt" <<'TEXT'
命令行：运行 ./bin/pingkk --help
TEXT
