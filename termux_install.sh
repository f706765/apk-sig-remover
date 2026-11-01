#!/data/data/com.termux/files/usr/bin/bash
# ======================================================================
#  APK 签名校验移除工具 —— Termux（手机）一键安装脚本
#  用法（在 Termux 里执行任意一行）：
#     bash <(curl -fsSL https://raw.githubusercontent.com/f706765/apk-sig-remover/master/termux_install.sh)
#  或：
#     pkg install -y curl
#     curl -fsSL -o install.sh https://raw.githubusercontent.com/f706765/apk-sig-remover/master/termux_install.sh
#     bash install.sh
# ======================================================================
set -e

echo "=================================================="
echo "  APK 签名校验移除工具 · Termux 一键安装"
echo "=================================================="

# 1) 安装运行 + 编译依赖
echo "[1/4] 安装依赖（apktool / openjdk-17 / apksigner / git / clang）…"
pkg update -y
pkg install -y apktool openjdk-17 apksigner git curl clang make
echo "      依赖安装完成。"

# 2) 拉取源码
echo "[2/4] 拉取源码…"
cd "$HOME"
if [ -d "apk-sig-remover" ]; then
    echo "      已存在 apk-sig-remover，执行 git pull 更新…"
    cd apk-sig-remover && git pull --ff-only || true
else
    git clone https://github.com/f706765/apk-sig-remover.git
    cd apk-sig-remover
fi
echo "      源码就绪：$(pwd)"

# 3) 编译原生程序（链接 Termux 自带的 bionic libc，自动匹配手机架构）
echo "[3/4] 编译原生程序（sigremover）…"
clang native/launcher.c -o "$HOME/sigremover" -pthread -O2
chmod +x "$HOME/sigremover"
echo "      编译完成：$HOME/sigremover"

# 4) 启动
echo "[4/4] 启动服务…"
echo "--------------------------------------------------"
echo "  启动后，在手机浏览器打开： http://127.0.0.1:5000"
echo "  拖入 .apk → 点「去除签名校验」→ 下载"
echo "  按 Ctrl+C 可停止服务。"
echo "--------------------------------------------------"
cd "$HOME"
exec "$HOME/sigremover"
