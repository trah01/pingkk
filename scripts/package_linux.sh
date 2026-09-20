#!/usr/bin/env bash
set -euo pipefail

build_dir="${1:?缺少构建目录}"
gui_output_dir="${2:?缺少图形版输出目录}"
cli_output_dir="${3:?缺少命令行版输出目录}"

# Debian 10 ships CMake 3.13, before `cmake --install` was introduced.
# Invoke the generated install script directly and keep all paths absolute.
build_dir="$(realpath -m "${build_dir}")"
gui_output_dir="$(realpath -m "${gui_output_dir}")"
cli_output_dir="$(realpath -m "${cli_output_dir}")"
cmake \
    -DCMAKE_INSTALL_PREFIX="${gui_output_dir}" \
    -DCMAKE_INSTALL_DO_STRIP=1 \
    -P "${build_dir}/cmake_install.cmake"

mkdir -p "${gui_output_dir}/lib" "${gui_output_dir}/plugins/platforms"
cp -R licenses "${gui_output_dir}/licenses"
# Debian's qmake is a qtchooser wrapper and may have no default selection.
# Query the package database so the path also follows each CPU architecture.
xcb_plugin="$(dpkg-query -L libqt5gui5 | awk '/\/platforms\/libqxcb\.so$/ { print; exit }')"
if [ -z "${xcb_plugin}" ] || [ ! -f "${xcb_plugin}" ]; then
    echo "未找到 Qt 5 xcb 平台插件" >&2
    exit 1
fi
cp "${xcb_plugin}" "${gui_output_dir}/plugins/platforms/"

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
