# tools/gen_symbols_printer.py —— 从 ai-passport-printer 源码里抠出所有会显示的字符串中的汉字,
# 并上现有 font_zh20.c 的字集,生成新的 symbols 文件给 lv_font_conv 用。
# 只取字符串字面量里的中文(先剥掉 // 与 /* */ 注释),避免注释里的字白白占 flash。
import io, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAIN = os.path.join(ROOT, "main")


def read(p):
    return io.open(p, encoding="utf-8").read()


def strip_comments(s):
    # 去掉块注释与行注释(不处理字符串里的 // ,本工程没有这种写法)
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    s = re.sub(r"//[^\n]*", "", s)
    return s


CJK = re.compile(r"[\u4e00-\u9fff\u3000-\u303f\uff00-\uffef]")

chars = set()
for f in sorted(os.listdir(MAIN)):
    if not f.endswith((".c", ".h")) or f.startswith("font_"):
        continue
    body = strip_comments(read(os.path.join(MAIN, f)))
    # 字符串字面量
    for m in re.finditer(r'"((?:[^"\\\n]|\\.)*)"', body):
        chars.update(CJK.findall(m.group(1)))
    # 单字符字面量(如 '℃')
    for m in re.finditer(r"'([^'\\\n]|\\.)'", body):
        chars.update(CJK.findall(m.group(1)))

# 现有 font_zh20.c 的字集(从生成注释里取)
cur = read(os.path.join(MAIN, "font_zh20.c"))
m = re.search(r"--symbols\s+(.*?)\s*--output", cur, re.S)
old = set(m.group(1)) if m else set()

# 界面固定还要保留的:菜单标题牌(ui_pixel_screen_create 用 zh20)
extra = set("AI通行证打印机监控配网")

# ⚠ 血泪:上面从字体注释里抠字集用的正则 `--symbols\s+(.*?)\s*--output`,
# 会把 "--symbols" 后面的「分隔空格 + 字集首字符空格」一起吃掉 —— 空格(U+0020)丢失,
# 界面上 "剩余 4m" 的空格就渲染成方块。所以 ASCII 可打印字符一律显式补全,不靠正则。
ascii_all = {chr(c) for c in range(0x20, 0x7F)}

allc = sorted(chars | old | extra | ascii_all)
out = "".join(allc)
io.open(os.path.join(ROOT, "tools", "symbols_zh20_printer.txt"), "w", encoding="utf-8").write(out)

print("源码字符串中文:", len(chars))
print("原 zh20 字集:", len(old))
print("合并后总字数:", len(allc))
print("新增:", "".join(sorted((chars | extra) - old)))
