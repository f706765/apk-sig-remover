# -*- coding: utf-8 -*-
"""APK 签名校验移除工具 - Web 服务。

工作流程：上传 APK → 反编译 → 去签名校验补丁 → 重打包 → 重签名 → 下载。

仅供安全研究、学习，以及对自己拥有合法权益的应用进行测试。
"""
import os
import re
import threading
import time
import uuid

from flask import (
    Flask,
    jsonify,
    render_template,
    request,
    send_file,
    abort,
)
from patcher import apktool_wrapper, signature_patch, signer

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
TOOLS_DIR = os.path.join(BASE_DIR, "tools")
WORK_DIR = os.path.join(BASE_DIR, "work")
APKTOOL_JAR = os.path.join(TOOLS_DIR, "apktool.jar")
SIGNER_JAR = os.path.join(TOOLS_DIR, "uber-apk-signer.jar")

os.makedirs(WORK_DIR, exist_ok=True)

app = Flask(__name__)

# 任务状态存储：job_id -> dict
JOBS = {}
JOBS_LOCK = threading.Lock()


def _safe_name(name: str) -> str:
    """清理文件名，避免路径穿越。"""
    name = os.path.basename(name)
    name = re.sub(r"[^\w.\-]", "_", name)
    return name or "upload.apk"


def _update(job_id: str, **kwargs):
    with JOBS_LOCK:
        job = JOBS.get(job_id)
        if not job:
            return
        for k, v in kwargs.items():
            job[k] = v


def _log(job_id: str, line: str):
    with JOBS_LOCK:
        job = JOBS.get(job_id)
        if not job:
            return
        job["log"].append(line)
        job["log"] = job["log"][-200:]  # 限制日志长度


def process_job(job_id: str, apk_path: str, original_name: str):
    """后台处理任务。"""
    job_dir = os.path.join(WORK_DIR, job_id)
    decoded_dir = os.path.join(job_dir, "decoded")
    unsigned_apk = os.path.join(job_dir, "unsigned.apk")
    signed_dir = os.path.join(job_dir, "signed")
    os.makedirs(job_dir, exist_ok=True)

    try:
        _update(job_id, progress=10, status="processing")
        decoded = apktool_wrapper.decode(
            apk_path, decoded_dir, APKTOOL_JAR, log_fn=lambda s: _log(job_id, s)
        )

        _update(job_id, progress=45, status="processing")
        patched = signature_patch.patch_smali(
            decoded, log_fn=lambda s: _log(job_id, s)
        )
        _log(job_id, f"共修补签名比对 {patched} 处。")

        _update(job_id, progress=70, status="processing")
        unsigned = apktool_wrapper.build(
            decoded, unsigned_apk, APKTOOL_JAR, log_fn=lambda s: _log(job_id, s)
        )

        _update(job_id, progress=85, status="processing")
        signed = signer.sign(
            unsigned, SIGNER_JAR, signed_dir, log_fn=lambda s: _log(job_id, s)
        )

        result_name = _safe_name(os.path.splitext(original_name)[0] + "_nosig.apk")
        result_path = os.path.join(job_dir, result_name)
        os.replace(signed, result_path)

        _update(
            job_id,
            progress=100,
            status="done",
            result=result_name,
            result_path=result_path,
            patched=patched,
        )
        _log(job_id, f"✅ 完成！已生成：{result_name}（移除 {patched} 处签名校验）")
    except Exception as exc:  # noqa: BLE001
        _update(job_id, status="error", error=str(exc))
        _log(job_id, f"❌ 处理失败：{exc}")


@app.route("/")
def index():
    return render_template("index.html")


@app.route("/api/remove", methods=["POST"])
def remove_signature():
    if "file" not in request.files:
        return jsonify({"error": "未收到文件"}), 400
    uploaded = request.files["file"]
    if not uploaded.filename:
        return jsonify({"error": "文件名为空"}), 400
    if not uploaded.filename.lower().endswith(".apk"):
        return jsonify({"error": "仅支持 .apk 安装包"}), 400

    job_id = uuid.uuid4().hex
    original_name = _safe_name(uploaded.filename)
    job_dir = os.path.join(WORK_DIR, job_id)
    os.makedirs(job_dir, exist_ok=True)
    apk_path = os.path.join(job_dir, original_name)
    uploaded.save(apk_path)

    with JOBS_LOCK:
        JOBS[job_id] = {
            "status": "queued",
            "progress": 0,
            "log": [f"已接收文件：{original_name}"],
            "result": None,
            "result_path": None,
            "error": None,
            "patched": 0,
            "created": time.time(),
        }

    t = threading.Thread(
        target=process_job, args=(job_id, apk_path, original_name), daemon=True
    )
    t.start()
    return jsonify({"job_id": job_id})


@app.route("/api/status/<job_id>")
def job_status(job_id: str):
    with JOBS_LOCK:
        job = JOBS.get(job_id)
        if not job:
            abort(404)
        return jsonify(
            {
                "status": job["status"],
                "progress": job["progress"],
                "log": job["log"],
                "result": job["result"],
                "patched": job["patched"],
                "error": job["error"],
            }
        )


@app.route("/api/download/<job_id>")
def download(job_id: str):
    with JOBS_LOCK:
        job = JOBS.get(job_id)
        if not job or not job.get("result_path"):
            abort(404)
        path = job["result_path"]
        name = job["result"]
    return send_file(path, as_attachment=True, download_name=name)


@app.route("/api/health")
def health():
    missing = []
    try:
        apktool_wrapper.resolve_apktool(APKTOOL_JAR)
    except apktool_wrapper.ApktoolError:
        missing.append("apktool (系统命令或 tools/apktool.jar)")
    if not os.path.exists(SIGNER_JAR):
        missing.append("uber-apk-signer.jar")
    return jsonify({"ok": not missing, "missing_tools": missing})


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=False)
