# APK 签名校验移除工具（SigRemover）

一个轻量的 Web 工具：上传 Android `.apk` 安装包 → 一键「去除签名校验」→ 处理完成后直接下载。

> ⚠️ **合法使用声明（必读）**
> 本项目**仅供安全研究、学习，以及对自己拥有合法权益的应用进行测试**。
> 请勿将其用于：
> - 破解 / 绕过付费校验、会员校验；
> - 去除他人应用的版权保护或数字版权管理（DRM）；
> - 篡改、重打包他人应用并分发；
> - 任何其他违反你所在国家 / 地区法律法规的用途。
>
> 使用本工具产生的任何后果由使用者自行承担，作者不承担任何法律责任。

---

## 它能做什么

很多 Android 应用会在运行时校验自身的 APK 签名（防止被篡改 / 重打包）。
本工具对 APK 做如下处理：

1. **反编译**：用 [apktool](https://github.com/iBotPeaches/Apktool) 把 APK 解码为 smali 源码；
2. **去校验补丁**：在 smali 层，将 `android.content.pm.Signature.equals(...)` 的调用结果强制为 `true`，
   使基于该 API 的运行时签名自校验判定为「一致」；
3. **重打包**：用 apktool 重新构建未签名 APK；
4. **重签名**：用 [uber-apk-signer](https://github.com/patrickfav/uber-apk-signer) 以 v1+v2+v3 方案重新签名。

最终产出一个可直接安装、且运行时签名自校验被置为通过的 APK。

---

## 工作原理（简述）

核心补丁在 `patcher/signature_patch.py` 中。它把形如：

```smali
invoke-virtual {p1, p2}, Landroid/content/pm/Signature;->equals(Ljava/lang/Object;)Z
move-result v0
```

替换为：

```smali
const/4 v0, 0x1   # 强制 Signature.equals 返回 true
```

由于 Dalvik 约定 `invoke` 之后紧跟 `move-result`，直接把结果寄存器置 `1`（true）
即可让任何基于 `Signature.equals` 的比对通过。

---

## 环境要求

- Python 3.10+
- Java 17+（已验证 Java 20 可用）
- 网络（首次运行需下载两个 jar 工具）

---

## 快速开始

```bash
# 1. 克隆仓库
git clone https://github.com/f706765/apk-sig-remover.git
cd apk-sig-remover

# 2. 下载第三方工具（apktool / uber-apk-signer）
bash setup.sh

# 3. 安装依赖
pip install -r requirements.txt

# 4. 启动
python app.py
```

浏览器打开 <http://localhost:5000> 即可使用。

---

## 使用方法

1. 打开网页，把 `.apk` 安装包拖入上传区（或点「选择安装包」）；
2. 点击 **去除签名校验**；
3. 等待进度完成（反编译 → 去校验 → 重打包 → 重签名）；
4. 点击 **下载处理后的安装包**，文件名形如 `原名_nosig.apk`。

---

## 技术栈

| 组件 | 作用 |
| --- | --- |
| Flask | Web 服务 / 上传下载 / 任务状态 |
| apktool | APK 反编译与重打包 |
| uber-apk-signer | APK 重新签名（v1+v2+v3） |
| Python 正则 | smali 层签名校验补丁 |

---

## 目录结构

```
apk-sig-remover/
├── app.py                 # Flask 后端（上传/状态/下载）
├── patcher/
│   ├── apktool_wrapper.py # apktool 封装
│   ├── signature_patch.py # smali 签名校验补丁
│   └── signer.py          # 重签名封装
├── templates/index.html   # 中文界面
├── static/                # 样式与前端脚本
├── tools/                 # 第三方 jar（setup.sh 下载）
├── work/                  # 运行期产物（已 gitignore）
├── requirements.txt
├── setup.sh
└── README.md
```

---

## 已知限制

- 本工具基于 `Signature.equals` 的**通用**补丁，对「直接比对 Signature 对象」的应用效果最好；
- 若应用改用 `MessageDigest` 比对哈希、或比对 `PackageInfo.signatures[0]` 的字节 / 哈希字符串，
  则需要更针对性的手工补丁，工具会在日志中如实说明命中情况；
- 部分应用还有额外的完整性校验（如腾讯乐固、360 加固、腾讯 Bugly 等），需先脱壳，超出本工具范围；
- 处理后的 APK 仅供学习研究，安装前请确认你拥有相应权利。

---

## 开源协议

本项目以 [MIT 协议](LICENSE) 开源。工具所依赖的 apktool、uber-apk-signer 遵循各自的开源许可。
