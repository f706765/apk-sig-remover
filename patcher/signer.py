"""使用 uber-apk-signer 对 APK 重新签名。"""
import glob
import os
import subprocess


class SignerError(Exception):
    """签名失败。"""


def sign(apk_path: str, signer_jar: str, out_dir: str, log_fn=print):
    """对未签名 APK 重新签名（v1+v2+v3），返回签名后的文件路径。"""
    os.makedirs(out_dir, exist_ok=True)
    cmd = ["java", "-jar", signer_jar, "--apks", apk_path, "--out", out_dir]
    log_fn("▶ 重新签名 APK（uber-apk-signer）…")
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise SignerError(proc.stderr[-2000:] or proc.stdout[-2000:])

    signed = sorted(
        glob.glob(os.path.join(out_dir, "*.apk")),
        key=os.path.getmtime,
        reverse=True,
    )
    if not signed:
        raise SignerError("未找到签名后的 APK 文件")
    return signed[0]
