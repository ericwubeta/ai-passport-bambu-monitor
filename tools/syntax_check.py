# tools/syntax_check.py —— 从 compile_commands.json 提取命令,对改动的源文件做 -fsyntax-only 快速检查。
# 用法: python syntax_check.py app_printer.c app_boot.c app_wifi.c
import json, subprocess, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CC = json.load(open(os.path.join(ROOT, "build", "compile_commands.json"), encoding="utf-8"))

# 不用 shlex(路径含中文/反斜杠会出问题),自写带引号 tokenizer。
# ninja 在 Windows 上用 \" 转义字面引号,普通 " 包裹含空格路径。
def tok(s):
    out, cur, inq = [], "", False
    i = 0
    while i < len(s):
        ch = s[i]
        if ch == "\\" and i + 1 < len(s) and s[i + 1] == '"':
            cur += '"'
            i += 2
            continue
        if ch == '"':
            inq = not inq
        elif ch == " " and not inq:
            if cur:
                out.append(cur)
                cur = ""
        else:
            cur += ch
        i += 1
    if cur:
        out.append(cur)
    return out

targets = sys.argv[1:]
entries = {os.path.basename(e["file"]).replace(".cpp", ".c"): e for e in CC if e["file"].endswith(".c")}

fail = 0
for t in targets:
    e = entries.get(t)
    if not e:
        print("[SKIP] 未在 compile_commands.json 找到", t)
        continue
    parts = tok(e["command"])
    out, src = [], None
    skip = False
    for i, p in enumerate(parts):
        if skip:
            skip = False
            continue
        if p == "-o":
            skip = True
            continue
        if p == "-c":
            continue
        if os.path.basename(p) == t:
            src = p
            continue
        out.append(p)
    if src is None:
        src = os.path.abspath(e["file"])
    cmd = out[:1] + ["-fsyntax-only"] + out[1:] + [src]
    print("[CHECK]", t, "...")
    r = subprocess.run(cmd, cwd=e["directory"], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if r.returncode != 0:
        print(r.stderr[-3000:] if r.stderr else "(无 stderr)")
        print("[FAIL]", t)
        fail += 1
    else:
        print("[OK]", t)
print("RESULT:", "FAIL" if fail else "PASS")
sys.exit(1 if fail else 0)
