@echo off
chcp 65001 >nul
title APK 签名校验移除工具
echo ==========================================
echo   APK 签名校验移除工具 - Windows 启动
echo ==========================================
echo.

where python >nul 2>nul
if errorlevel 1 (
  echo [错误] 未检测到 Python，请先安装 Python 3.10+：https://www.python.org/downloads/
  echo        安装时勾选 "Add Python to PATH"。
  pause
  exit /b 1
)

if not exist ".venv" (
  echo 首次运行：创建虚拟环境...
  python -m venv .venv
)

call .venv\Scripts\activate.bat

echo 安装依赖...
pip install -q -r requirements.txt

echo 准备工具...
bash setup.sh 2>nul || echo   [提示] Windows 下请手动下载两个 jar 放入 tools\ 目录（见 README）。

echo.
echo 启动服务：浏览器打开 http://127.0.0.1:5000
echo 按 Ctrl+C 停止。
echo.
python app.py
pause
