"""apktool 反编译 / 重打包封装。"""
import os
import shutil
import subprocess


class ApktoolError(Exception):
    """apktool 执行失败。"""


def resolve_apktool(apktool_jar: str):
    """优先使用系统 PATH 中的 `apktool` 命令，否则回退到 jar。"""
    from shutil import which

    cli = which("apktool")
    if cli:
        return [cli]
    if apktool_jar and os.path.exists(apktool_jar):
        return ["java", "-jar", apktool_jar]
    raise ApktoolError("未找到 apktool：请执行 setup.sh 或 apt install apktool")


def decode(apk_path: str, out_dir: str, apktool_jar: str, log_fn=print):
    """把 APK 反编译为 smali 源码。"""
    if os.path.exists(out_dir):
        shutil.rmtree(out_dir)
    base = resolve_apktool(apktool_jar)
    cmd = base + ["d", "-f", "-r", "-o", out_dir, apk_path]
    log_fn("▶ 反编译 APK（apktool decode）…")
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise ApktoolError(proc.stderr[-2000:] or proc.stdout[-2000:])
    return out_dir


def build(decoded_dir: str, out_apk: str, apktool_jar: str, log_fn=print):
    """把反编译目录重新打包为未签名 APK。"""
    base = resolve_apktool(apktool_jar)
    cmd = base + ["b", "-f", "-o", out_apk, decoded_dir]
    log_fn("▶ 重新打包 APK（apktool build）…")
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise ApktoolError(proc.stderr[-2000:] or proc.stdout[-2000:])
    return out_apk
