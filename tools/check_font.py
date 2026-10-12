# tools/check_font.py —— 校验 ai-passport-printer 里"会显示"的中文是否都被字体覆盖。
# 只取字符串字面量里的中文(先剥 // 与 /* */ 注释),注释里的字不占 flash、不算缺字。
# 字体头注释第 3 行含生成时的 --symbols 全集,直接从那里取,比遍历字形数组快且准。
import io, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CJK = re.compile(r"[\u4e00-\u9fff\u3000-\u303f\uff00-\uffef]")


def read(p):
    return io.open(p, encoding="utf-8").read()


def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    return re.sub(r"//[^\n]*", "", s)


def font_symbols(path):
    """从字形数组的 `/* U+0020 " " */` 注释里读码点。
    比抠 --symbols 注释可靠:正则抠字集会把首字符空格吃掉,导致漏判。"""
    txt = read(path)
    out = set()
    for m in re.finditer(r"/\*\s*U\+([0-9A-Fa-f]{4,6})\s", txt):
        out.add(chr(int(m.group(1), 16)))
    m = re.search(r"--symbols\s+(.*?)\s*--output", txt, re.S)
    if m:
        out |= set(m.group(1))          # 双保险(个别字符可能没生成 U+ 注释)
    return out


zh14 = font_symbols(os.path.join(ROOT, "main", "font_zh14.c"))
zh20 = font_symbols(os.path.join(ROOT, "main", "font_zh20.c"))

# 源码里"会显示"的中文 = 字符串/字符字面量
chars = set()
for f in sorted(os.listdir(os.path.join(ROOT, "main"))):
    if f.endswith((".c", ".h")) and not f.startswith("font_"):
        body = strip_comments(read(os.path.join(ROOT, "main", f)))
        for m in re.finditer(r'"((?:[^"\\\n]|\\.)*)"', body):
            chars.update(CJK.findall(m.group(1)))
        for m in re.finditer(r"'([^'\\\n]|\\.)'", body):
            chars.update(CJK.findall(m.group(1)))
chars |= set("AI通行证打印机监控配网")   # ui_pixel 标题牌硬编码 zh20

all_cn = sorted(chars)
print("字体覆盖: zh14=%d 字, zh20=%d 字" % (len(zh14), len(zh20)))
print("会显示的中文去重:", len(all_cn))

miss14 = [c for c in all_cn if c not in zh14]
miss20 = [c for c in all_cn if c not in zh20]
print("[font_zh14 缺字]", "".join(miss14) or "无")
print("[font_zh20 缺字]", "".join(miss20) or "无")

# 空格单独查:它不会出现在源码中文里,但界面到处都是("剩余 4m")。
# 从字体注释抠字集的正则极易把首字符空格吃掉,所以必须显式断言。
for name, s in (("font_zh14", zh14), ("font_zh20", zh20)):
    miss_ascii = [chr(c) for c in range(0x20, 0x7F) if chr(c) not in s]
    print("[%s 缺 ASCII]" % name,
          "".join(miss_ascii) if miss_ascii else "无",
          "| 空格:", "有" if " " in s else "**缺失**")

ok = not (miss14 or miss20) and " " in zh14 and " " in zh20
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
