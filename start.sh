#!/usr/bin/env bash
# macOS / Linux 一键启动
cd "$(dirname "$0")"

echo "=========================================="
echo "  APK 签名校验移除工具 - 启动"
echo "=========================================="

if ! command -v python3 >/dev/null 2>&1; then
  echo "[错误] 未检测到 python3，请先安装 Python 3.10+"
  exit 1
fi

# 准备工具（apktool / uber-apk-signer）
bash setup.sh

# 依赖
if [ ! -d ".venv" ]; then
  echo "首次运行：创建虚拟环境..."
  python3 -m venv .venv
fi
source .venv/bin/activate
pip install -q -r requirements.txt

echo ""
echo "启动服务：浏览器打开 http://127.0.0.1:5000"
echo "按 Ctrl+C 停止。"
echo ""
python3 app.py
