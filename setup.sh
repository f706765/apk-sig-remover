#!/usr/bin/env bash
# 下载 / 准备运行所需的第三方工具。
#   - apktool：若系统已安装（或可通过 apt 安装）则直接使用命令；否则从 GitHub 下载 jar
#   - uber-apk-signer：从 GitHub 下载 jar（无系统包）
set -e

cd "$(dirname "$0")/tools"

UBER_VER="1.3.0"
UBER_URL="https://github.com/patrickfav/uber-apk-signer/releases/download/v${UBER_VER}/uber-apk-signer-${UBER_VER}.jar"

# ---------- apktool ----------
if command -v apktool >/dev/null 2>&1; then
  echo "✔ 检测到系统 apktool 命令：$(command -v apktool)，无需下载 jar。"
elif [ -f apktool.jar ]; then
  echo "✔ 已存在 apktool.jar，跳过。"
else
  echo "▶ 尝试通过 apt 安装 apktool（Debian/Ubuntu）…"
  if apt-get install -y apktool >/dev/null 2>&1 && command -v apktool >/dev/null 2>&1; then
    echo "✔ 已通过 apt 安装 apktool 命令。"
  else
    APKTOOL_VER="3.0.3"
    echo "▶ 从 GitHub 下载 apktool ${APKTOOL_VER} jar …"
    curl -sL -o apktool.jar "https://github.com/iBotPeaches/Apktool/releases/download/v${APKTOOL_VER}/apktool_${APKTOOL_VER}.jar"
    echo "✔ apktool.jar 下载完成。"
  fi
fi

# ---------- uber-apk-signer ----------
if [ -f uber-apk-signer.jar ]; then
  echo "✔ 已存在 uber-apk-signer.jar，跳过。"
else
  echo "▶ 下载 uber-apk-signer ${UBER_VER} …"
  curl -sL -o uber-apk-signer.jar "$UBER_URL"
  echo "✔ uber-apk-signer.jar 下载完成。"
fi

echo ""
echo "✅ 工具准备就绪。"
