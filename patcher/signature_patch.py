"""smali 层签名校验补丁。

核心策略（默认开启）
--------------------
将 ``android.content.pm.Signature.equals(Object)`` 的调用结果强制为 ``true``。
Dalvik 约定 ``invoke-virtual`` 之后紧跟 ``move-result``，因此把这两行：

    invoke-virtual {rx, ry}, Landroid/content/pm/Signature;->equals(Ljava/lang/Object;)Z
    move-result rz

替换为：

    const/4 rz, 0x1   # 强制签名比对结果为“一致”

从而使任何基于 ``Signature.equals`` 的运行时签名自校验都判定为通过。

说明
----
这是社区中常见的通用绕过手段，对“直接比对 Signature 对象”的应用效果最好。
若应用改用 ``MessageDigest`` 比对哈希、或比对 ``PackageInfo.signatures[0]``
的字节/哈希字符串，则需要更针对性的手工补丁——本工具会在日志中如实说明命中情况。
"""
import os
import re

# 匹配：invoke-virtual ... Signature.equals(...) 紧接着 move-result <reg>
_EQUALS_PATTERN = re.compile(
    r"^(?P<indent>\s*)invoke-virtual\s+\{(?P<regs>[^}]*)\},\s*"
    r"Landroid/content/pm/Signature;->equals\(Ljava/lang/Object;\)Z\s*\n"
    r"(?P=indent)move-result\s+(?P<dst>\w+)\s*$",
    re.MULTILINE,
)


def patch_smali(decoded_dir: str, log_fn=print):
    """遍历反编译目录，修补所有 .smali 中的签名比对，返回修补处总数。"""
    total = 0
    for root, _dirs, files in os.walk(decoded_dir):
        for name in files:
            if not name.endswith(".smali"):
                continue
            path = os.path.join(root, name)
            with open(path, "r", encoding="utf-8", errors="ignore") as fp:
                content = fp.read()

            def _repl(m: re.Match) -> str:
                dst = m.group("dst")
                indent = m.group("indent")
                return (
                    f"{indent}# [sig-remover] 强制 Signature.equals 返回 true\n"
                    f"{indent}const/4 {dst}, 0x1"
                )

            new_content, n = _EQUALS_PATTERN.subn(_repl, content)
            if n:
                with open(path, "w", encoding="utf-8") as fp:
                    fp.write(new_content)
                total += n
                rel = os.path.relpath(path, decoded_dir)
                log_fn(f"  ✓ 修补 {rel}：{n} 处签名比对")
    return total
