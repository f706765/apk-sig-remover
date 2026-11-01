# -*- coding: utf-8 -*-
"""把 Web 资源（index.html / app.js / style.css）转成 C 字符串头文件 assets.h。"""
import os

WEB = os.path.join(os.path.dirname(__file__), "web")


def to_c_string(text: str) -> str:
    # 以 UTF-8 字节为单位转义，确保多字节字符（中文、emoji）被正确嵌入为字节序列
    data = text.encode("utf-8")
    out = []
    for b in data:
        if b == 34:            # "
            out.append('\\"')
        elif b == 92:          # backslash
            out.append("\\\\")
        elif b == 10:          # \n
            out.append("\\n")
        elif b == 13:          # \r
            out.append("\\r")
        elif b == 9:           # \t
            out.append("\\t")
        elif 32 <= b < 127:
            out.append(chr(b))
        else:
            out.append("\\x%02x" % b)
    return "".join(out)


def main():
    files = {
        "INDEX_HTML": "index.html",
        "APP_JS": "app.js",
        "STYLE_CSS": "style.css",
    }
    parts = ["#ifndef ASSETS_H", "#define ASSETS_H", ""]
    for name, fname in files.items():
        with open(os.path.join(WEB, fname), "r", encoding="utf-8") as fp:
            content = fp.read()
        parts.append(f'static const char* {name} =')
        parts.append(f'    "{to_c_string(content)}";')
        parts.append("")
    parts.append("#endif")
    with open(os.path.join(os.path.dirname(__file__), "assets.h"), "w", encoding="utf-8") as fp:
        fp.write("\n".join(parts) + "\n")
    print("assets.h 生成完成")


if __name__ == "__main__":
    main()
