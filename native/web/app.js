// APK 签名校验移除工具 - 前端逻辑（原生接口版）
(function () {
  const $ = (id) => document.getElementById(id);
  const dropZone = $("dropZone");
  const fileInput = $("fileInput");
  const fileName = $("fileName");
  const removeBtn = $("removeBtn");
  const resetBtn = $("resetBtn");
  const progressCard = $("progressCard");
  const barFill = $("barFill");
  const statusText = $("statusText");
  const logBox = $("logBox");
  const downloadBtn = $("downloadBtn");
  const errorText = $("errorText");

  let selectedFile = null;
  let pollTimer = null;

  function setFile(file) {
    if (!file) return;
    if (!file.name.toLowerCase().endsWith(".apk")) {
      alert("仅支持 .apk 安装包");
      return;
    }
    selectedFile = file;
    fileName.textContent = "已选择：" + file.name + "（" + (file.size / 1024 / 1024).toFixed(2) + " MB）";
    removeBtn.disabled = false;
    resetBtn.disabled = false;
  }

  ["dragenter", "dragover"].forEach((ev) =>
    dropZone.addEventListener(ev, (e) => {
      e.preventDefault();
      dropZone.classList.add("drag");
    })
  );
  ["dragleave", "drop"].forEach((ev) =>
    dropZone.addEventListener(ev, (e) => {
      e.preventDefault();
      dropZone.classList.remove("drag");
    })
  );
  dropZone.addEventListener("drop", (e) => {
    if (e.dataTransfer.files.length) setFile(e.dataTransfer.files[0]);
  });
  fileInput.addEventListener("change", (e) => {
    if (e.target.files.length) setFile(e.target.files[0]);
  });

  function resetAll() {
    selectedFile = null;
    fileInput.value = "";
    fileName.textContent = "";
    removeBtn.disabled = true;
    resetBtn.disabled = true;
    progressCard.classList.add("hidden");
    downloadBtn.classList.add("hidden");
    errorText.classList.add("hidden");
    barFill.style.width = "0%";
    logBox.textContent = "";
    if (pollTimer) clearInterval(pollTimer);
  }
  resetBtn.addEventListener("click", resetAll);

  removeBtn.addEventListener("click", async () => {
    if (!selectedFile) return;
    removeBtn.disabled = true;
    progressCard.classList.remove("hidden");
    downloadBtn.classList.add("hidden");
    errorText.classList.add("hidden");
    logBox.textContent = "";
    barFill.style.width = "0%";
    statusText.textContent = "上传中…";

    let jobId;
    try {
      // 以原始二进制方式上传文件（无需 multipart）
      const res = await fetch("/api/remove", {
        method: "POST",
        headers: { "Content-Type": "application/octet-stream" },
        body: selectedFile,
      });
      const data = await res.json();
      if (data.error) throw new Error(data.error);
      jobId = data.job_id;
    } catch (err) {
      statusText.textContent = "上传失败";
      errorText.textContent = err.message;
      errorText.classList.remove("hidden");
      return;
    }
    poll(jobId);
  });

  function poll(jobId) {
    if (pollTimer) clearInterval(pollTimer);
    pollTimer = setInterval(async () => {
      try {
        const res = await fetch("/api/status/" + jobId);
        const data = await res.json();
        barFill.style.width = data.progress + "%";
        logBox.textContent = data.log.join("\n");
        logBox.scrollTop = logBox.scrollHeight;
        if (data.status === "queued") {
          statusText.textContent = "排队中…";
        } else if (data.status === "processing") {
          statusText.textContent = "处理中…（反编译 / 去校验 / 重打包 / 重签名）";
        } else if (data.status === "done") {
          statusText.textContent = "✅ 处理完成";
          clearInterval(pollTimer);
          downloadBtn.href = "/api/download/" + jobId;
          downloadBtn.classList.remove("hidden");
        } else if (data.status === "error") {
          statusText.textContent = "❌ 处理失败";
          clearInterval(pollTimer);
          errorText.textContent = data.error || "未知错误";
          errorText.classList.remove("hidden");
        }
      } catch (err) {
        // 忽略偶发网络错误，继续轮询
      }
    }, 1500);
  }
})();
