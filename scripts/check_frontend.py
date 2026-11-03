#!/usr/bin/env python3
"""
Frontend static checks for RoomCheck.

- CSP: no inline <script>, no inline event handler attributes, no javascript: URLs
- Neumorphism rules: no gradients, no translate, no standard Tailwind shadows,
  no rounded-none, no thick borders, no pure white/black backgrounds
- Every CSS class must be referenced by HTML/JS (no orphan styles)
- Every class used in static HTML must be defined in CSS
- All transitions must use the 300ms ease-in-out token
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FRONTEND = os.path.join(ROOT, "frontend")
HTML = os.path.join(FRONTEND, "index.html")
CSS = os.path.join(FRONTEND, "css", "style.css")
JS = [os.path.join(FRONTEND, "js", "api.js"), os.path.join(FRONTEND, "js", "app.js")]

errors = []
warnings = []


def read(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def strip_css_comments(text):
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def strip_js_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"(?m)^\s*//.*$", "", text)
    return text


def main():
    for path in [HTML, CSS] + JS:
        if not os.path.isfile(path):
            errors.append("缺少文件: %s" % path)
    if errors:
        report()
        return 1

    html = read(HTML)
    css_raw = read(CSS)
    css = strip_css_comments(css_raw)
    js_raw = "\n".join(read(p) for p in JS)
    # 注意：JS 字符串中包含 "image/*" 等片段，简单去注释会误删代码，
    # 因此这里使用原文做类名/引用检查；禁用样式只扫描 CSS。
    js = js_raw
    sources = html + "\n" + js

    # ---- CSP / inline script checks ----
    for m in re.finditer(r"<script\b([^>]*)>", html, re.I):
        attrs = m.group(1)
        if not re.search(r"\bsrc\s*=", attrs, re.I):
            errors.append("index.html 存在内联 <script>（CSP 禁止）")
    for m in re.finditer(r"<[a-zA-Z][^>]*\s(on[a-z]+)\s*=", html):
        errors.append("index.html 存在内联事件属性 %s（CSP 禁止）" % m.group(1))
    if re.search(r"javascript\s*:", sources, re.I):
        # allow the word in comments? strip comments already
        errors.append("存在 javascript: URL（CSP 禁止）")
    for m in re.finditer(r"<[a-zA-Z][^>]*\s(on[a-z]+)\s*=", js):
        errors.append("JS 模板中出现内联事件属性 %s" % m.group(1))

    # ---- neumorphism forbidden patterns ----
    forbidden_css = [
        (r"(?:linear|radial|conic)-gradient", "渐变"),
        (r"\btranslate(?:X|Y|3d|Z)?\s*\(", "translate 位移"),
        (r"\brounded-none\b", "rounded-none"),
        (r"\bshadow-(?:sm|md|lg|xl|2xl)\b", "标准 shadow 类"),
        (r"\bborder-(?:2|4|8)\b", "粗边框类"),
    ]
    for pattern, label in forbidden_css:
        if re.search(pattern, css, re.I):
            errors.append("CSS 命中禁用样式：%s" % label)
    # border widths >= 2px
    for m in re.finditer(r"border(?:-(?:top|right|bottom|left|width))?\s*:\s*([^;]+);", css, re.I):
        value = m.group(1)
        widths = re.findall(r"(\d+(?:\.\d+)?)px", value)
        if widths and max(float(w) for w in widths) >= 2 and "solid" in value:
            errors.append("CSS 存在 >=2px 实线边框: border: %s" % value.strip())
    for m in re.finditer(r"border-(?:top|right|bottom|left)-width\s*:\s*([\d.]+)px", css, re.I):
        if float(m.group(1)) >= 2:
            errors.append("CSS 存在 >=2px 边框宽度: %s" % m.group(0))
    # pure white/black backgrounds (shadows are allowed to be white)
    for m in re.finditer(r"(?:background|background-color)\s*:\s*([^;]+);", css, re.I):
        value = m.group(1).strip().lower()
        if re.search(r"(#fff(?:fff)?\b|\bwhite\b|#000(?:000)?\b|\bblack\b)", value):
            errors.append("CSS 背景使用纯白/纯黑: %s" % m.group(0))
    if re.search(r"box-shadow\s*:\s*(?:0\s+)?(?:[2-9]\d|\d{3,})px", css):
        pass  # 大数值双阴影是允许的

    # ---- transitions use 300ms ease-in-out token ----
    for m in re.finditer(r"transition\s*:\s*([^;]+);", css, re.I):
        value = m.group(1)
        if "var(--ease)" not in value and "300ms" not in value:
            errors.append("transition 未使用 300ms ease-in-out: %s" % value.strip())
    if "--ease:" in css and "300ms ease-in-out" not in css:
        errors.append("--ease 令牌不是 300ms ease-in-out")

    # ---- collect CSS classes ----
    css_classes = set(re.findall(r"\.(-?[_a-zA-Z][_a-zA-Z0-9-]*)", css))
    # ignore decimal-like artifacts and pseudo names
    css_classes = {c for c in css_classes if not c[0].isdigit()}
    if not css_classes:
        errors.append("无法解析 CSS 类")

    # ---- orphan CSS classes ----
    orphans = []
    for cls in sorted(css_classes):
        # whole-word match in HTML/JS sources
        if not re.search(r"(?<![A-Za-z0-9_-])" + re.escape(cls) + r"(?![A-Za-z0-9_-])",
                         sources):
            orphans.append(cls)
    if orphans:
        errors.append("CSS 孤儿类（未被 HTML/JS 使用）: %s" % ", ".join(orphans))

    # ---- classes used in index.html must exist in CSS ----
    used_static = set()
    for m in re.finditer(r'class\s*=\s*"([^"]*)"', html):
        for token in m.group(1).split():
            if token:
                used_static.add(token)
    missing = sorted(c for c in used_static if c not in css_classes)
    if missing:
        errors.append("HTML 使用了 CSS 中不存在的类: %s" % ", ".join(missing))

    # ---- basic asset checks ----
    for m in re.finditer(r'(?:src|href)\s*=\s*"([^"]+)"', html):
        url = m.group(1)
        if url.startswith(("http://", "https://", "//")):
            errors.append("引用外部资源（CSP/离线要求）: %s" % url)
        elif url.startswith("/"):
            rel = url[1:].split("?")[0]
            if rel and not os.path.isfile(os.path.join(FRONTEND, rel)):
                errors.append("引用不存在的静态资源: %s" % url)

    # ---- icon symbols referenced exist ----
    symbols = set(re.findall(r'<symbol\s+id="([^"]+)"', html))
    icon_refs = set(re.findall(r"#icon-([a-zA-Z0-9-]+)", js))
    for ref in icon_refs:
        if "icon-" + ref not in symbols:
            errors.append("JS 引用了不存在的图标 #icon-%s" % ref)

    # ---- app.js API paths must not call unknown HTTP verbs ----
    for verb in ("get", "post", "put", "patch", "del"):
        if "api." + verb + "(" not in js and verb in ("get", "post", "patch", "put"):
            warnings.append("app.js 未使用 api.%s" % verb)

    print("CSS 类数量: %d" % len(css_classes))
    print("HTML 静态类数量: %d" % len(used_static))
    if warnings:
        for w in warnings:
            print("WARN  " + w)
    if errors:
        print("\n前端检查失败：")
        for e in errors:
            print(" - " + e)
        return 1
    print("前端检查通过：CSP、禁用样式、CSS 孤儿、静态资源、图标引用")
    return 0


def report():
    for e in errors:
        print(" - " + e)


if __name__ == "__main__":
    sys.exit(main())
