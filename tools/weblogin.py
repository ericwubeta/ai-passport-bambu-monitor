#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""本地迷你「拓竹登录页」：在浏览器里输手机号+短信验证码，登录后把 accessToken
直接写入 ~/.bambu_token，供 x2d_badge.py 轮询使用。

为什么需要它：拓竹接口只允许服务端调用（浏览器直连会跨域被拦），所以用本机
Python 服务做中转代理——页面 fetch 到本机，本机再调 api.bambulab.cn。

用法（Linux / macOS；Windows 用户直接双击 get-token.bat 更简单）：
  python weblogin.py              # 启动，默认 http://127.0.0.1:8866
  python weblogin.py --port 9000  # 换端口

绑定 127.0.0.1（仅本机可访问），浏览器打开后：
  1. 输手机号 → 点「发送验证码」
  2. 手机收 6 位短信 → 填验证码 → 点「登录」
  3. 页面提示「已写入 token」即完成，把 token / User ID / 序列号填进设备配网页
     （手机连设备热点 Bambu-Setup 后访问 http://192.168.4.1）。
"""

import argparse
import json
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import requests

TOKEN_FILE = os.path.expanduser("~/.bambu_token")
PROV_FILE = os.path.expanduser("~/.bambu_provision.json")  # token+uid+打印机SN 一体包
TOKEN_CANDIDATES = (TOKEN_FILE,
                    os.path.join(os.getcwd(), ".bambu_token"),
                    os.path.join(os.environ.get("TEMP") or os.getcwd(), ".bambu_token"))
BAMBU = "https://api.bambulab.cn/v1"

PAGE = """<!doctype html>
<html lang="zh-CN"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>拓竹登录</title>
<style>
  :root{--bg:#0d0d12;--card:#16161d;--line:#2a2a36;--txt:#e9e9f0;--sub:#9a9aa8;
        --acc:#27d2bf;--brand:#6f6fff;--err:#e8463a;--ok:#1dc981}
  *{box-sizing:border-box}
  body{margin:0;min-height:100vh;background:var(--bg);color:var(--txt);
       font:15px/1.6"PingFang SC","Microsoft YaHei",system-ui,sans-serif;
       display:flex;align-items:center;justify-content:center}
  .card{width:min(400px,92vw);background:var(--card);border:1px solid var(--line);
        border-radius:16px;padding:28px 26px}
  h1{font-size:19px;margin:0 0 4px}
  .sub{color:var(--sub);font-size:13px;margin-bottom:22px}
  label{display:block;font-size:13px;color:var(--sub);margin:14px 0 6px}
  input{width:100%;padding:11px 12px;border-radius:10px;border:1px solid var(--line);
        background:#20202a;color:var(--txt);font-size:15px;outline:none}
  input:focus{border-color:var(--acc)}
  /* 6 位验证码:大字号 + 居中 + 字间距,独占一行 */
  #code{font-size:26px;font-weight:600;letter-spacing:.45em;text-indent:.45em;
        text-align:center;padding:14px 12px;min-height:56px;box-sizing:border-box}
  .row{display:flex;gap:8px}
  .row input{flex:1}
  button{margin-top:8px;padding:12px;border:0;border-radius:10px;cursor:pointer;
         font-size:15px;font-weight:600}
  #sendBtn{background:var(--brand);color:#fff;width:100%}
  #loginBtn{background:var(--acc);color:#0b1413;width:100%;margin-top:18px}
  button:disabled{opacity:.45;cursor:not-allowed}
  #status{margin-top:18px;font-size:13px;color:var(--sub);min-height:20px;
          white-space:pre-wrap;word-break:break-all}
  #status.ok{color:var(--ok)}#status.err{color:var(--err)}
  #status.inf{color:var(--acc)}
</style>
<div class="card">
  <h1>拓竹 · 登录换取 accessToken</h1>
  <div class="sub">仅在本机使用 · 中国区(api.bambulab.cn)</div>

  <label for="phone">拓竹账号手机号</label>
  <input id="phone" autocomplete="tel" placeholder="输入手机号" inputmode="tel">
  <button id="sendBtn">发送验证码</button>

  <label for="code">短信验证码</label>
  <input id="code" autocomplete="one-time-code" placeholder="6 位数字" inputmode="numeric" maxlength="6">
  <button id="loginBtn">登录</button>

  <div id="status"></div>
</div>
<script>
  const $=s=>document.querySelector(s), st=$("#status");
  function set(t,m){st.className=t;st.textContent=m}
  async function post(p,b){const r=await fetch(p,{method:"POST",
    headers:{"Content-Type":"application/json"},body:JSON.stringify(b)});
    return {s:r.status, j:await r.json().catch(()=>({}))}}

  $("#sendBtn").onclick=async()=>{
    const phone=$("#phone").value.trim();
    if(!/^\\d{6,15}$/.test(phone))return set("err","请输入正确的手机号");
    const b=$("#sendBtn"); b.disabled=true; set("inf","正在向拓竹发送验证码…");
    const {s,j}=await post("/api/sendcode",{phone});
    if(s===200&&j.success){set("inf","验证码已发送到手机（1~2 分钟内有效）。请往下，在「短信验证码」输入框填 6 位码，再点「登录」。");$("#code").focus();}
    else{set("err","发送失败："+(j.msg||j.message||s));}
    b.disabled=false;
  };

  $("#loginBtn").onclick=async()=>{
    const account=$("#phone").value.trim(), code=$("#code").value.trim();
    if(!/^\\d{6,15}$/.test(account))return set("err","手机号不对");
    if(!/^\\d{6}$/.test(code))return set("err","请输入 6 位验证码");
    const b=$("#loginBtn"); b.disabled=true; set("inf","正在登录…");
    const {s,j}=await post("/api/login",{account,code});
    if(s===200&&j.ok){
      let m="登录成功 \\u2705\\n";
      m+="User ID : "+(j.uid||"(空)")+"\\n";
      m+="打印机  : "+(j.sn_list||"(无)")+"\\n";
      m+="token/uid/SN 已写入 "+j.prov_path+"\\n";
      m+="→ 复制这三项填进设备网页 192.168.4.1(还差家里 WiFi 账号密码)";
      set("ok",m);
    }
    else{set("err","登录失败："+(j.msg||j.message||s));}
    b.disabled=false;
  };
</script>
</html>"""


def save_token(token):
    for p in TOKEN_CANDIDATES:
        try:
            with open(p, "w", encoding="utf-8") as fh:
                json.dump({"region": "china", "token": token}, fh)
            try:
                os.chmod(p, 0o600)
            except Exception:
                pass
            return p
        except Exception:
            continue
    raise RuntimeError("无法写入任一 token 位置")


def _write(h, code, payload, ctype="application/json"):
    body = json.dumps(payload).encode("utf-8") if isinstance(payload, dict) else payload
    h.send_response(code)
    h.send_header("Content-Type", ctype)
    h.send_header("Content-Length", str(len(body)))
    h.end_headers()
    h.wfile.write(body)


def _req(path, payload):
    r = requests.post(BAMBU + path, json=payload, timeout=25)
    try:
        data = r.json()
    except ValueError:
        data = {}  # 拓竹成功发送验证码时返回 HTTP 200 + 空 body，不做 JSON 解析
    return r.status_code, data


def send_code(phone):
    status, data = _req("/user-service/user/sendsmscode",
                        {"phone": phone, "type": "codeLogin"})
    if status == 200:
        return True, ""
    return False, data.get("error") or data.get("errmessage") or "HTTP %d" % status


def _dump_raw(data):
    """把登录原始响应(token 打码)落盘,用于探查真实字段名。"""
    try:
        import copy
        raw = copy.deepcopy(data) if isinstance(data, dict) else {"_raw": data}
        if isinstance(raw, dict):
            for k in list(raw.keys()):
                if "token" in str(k).lower():
                    raw[k] = "***"
        with open(os.path.expanduser("~/.bambu_login_raw.json"), "w", encoding="utf-8") as fh:
            json.dump(raw, fh, ensure_ascii=False)
    except Exception:
        pass


def login_with_code(account, code):
    status, data = _req("/user-service/user/login",
                        {"account": account, "code": code})
    d = data if isinstance(data, dict) else {}
    dd = d.get("data") if isinstance(d.get("data"), dict) else {}
    token = d.get("accessToken") or dd.get("accessToken")
    _dump_raw(data)
    if not token:
        return False, data, ""          # 失败也是 3 元组,便于上层统一解包
    # User ID 在登录响应里,字段名不定,广撒网(优先数值型 id)
    uid = (d.get("userId") or d.get("user_id") or d.get("uid") or d.get("userNo")
           or d.get("memberId") or d.get("accountId") or d.get("openid") or d.get("sub")
           or dd.get("userId") or dd.get("uid") or dd.get("user_id") or "")
    return True, token, uid


def fetch_devices(token):
    """用 token 调拓竹绑定接口,列出账号下的打印机(SN + 名称)。"""
    H = {"Authorization": "Bearer " + token, "Content-Type": "application/json"}
    try:
        r = requests.get(BAMBU + "/iot-service/api/user/bind", headers=H, timeout=25)
        data = r.json()
        devs = data.get("devices") or []
        out = []
        for d in devs:
            out.append({"sn": d.get("dev_id", ""),
                        "name": d.get("name", ""),
                        "model": d.get("dev_product_name") or d.get("dev_model_name", "")})
        return out
    except Exception as e:
        return [{"error": str(e)}]


def _find_uid(o):
    """递归在响应里找 "uid" 键(拓竹把 User ID 藏在 preferences 接口的深层)。"""
    if isinstance(o, dict):
        if "uid" in o:
            return o["uid"]
        for v in o.values():
            r = _find_uid(v)
            if r is not None:
                return r
    elif isinstance(o, list):
        for x in o:
            r = _find_uid(x)
            if r is not None:
                return r
    return None


def fetch_uid(token):
    """调拓竹 preferences 接口,取出账号 User ID(数字)。登录响应里没有 uid,
    只能从这里拿(同项目 x2d_badge.py 的已验证做法)。"""
    H = {"Authorization": "Bearer " + token, "Content-Type": "application/json"}
    try:
        r = requests.get(BAMBU + "/design-user-service/my/preference", headers=H, timeout=25)
        return _find_uid(r.json())
    except Exception:
        return None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path == "/" or self.path == "/index.html":
            b = PAGE.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)
        else:
            _write(self, 404, {"msg": "not found"})

    def do_POST(self):
        ln = int(self.headers.get("Content-Length") or 0)
        try:
            body = json.loads(self.rfile.read(ln).decode("utf-8"))
        except Exception:
            _write(self, 400, {"msg": "bad json"})
            return
        if self.path == "/api/sendcode":
            try:
                ok, msg = send_code(body.get("phone", ""))
            except Exception as e:
                ok, msg = False, "请求拓竹失败：" + str(e)
            _write(self, 200, {"success": ok, "msg": msg})
        elif self.path == "/api/login":
            try:
                ok, payload, uid = login_with_code(body.get("account", ""), body.get("code", ""))
            except Exception as e:
                _write(self, 200, {"ok": False, "msg": "请求拓竹失败：" + str(e)})
                return
            if ok:
                try:
                    p = save_token(payload)
                except Exception as e:
                    _write(self, 200, {"ok": False, "msg": "已拿到 token，但写盘失败：" + str(e)})
                    return
                devs = fetch_devices(payload)
                uid = fetch_uid(payload) or uid   # 优先从 preferences 接口取真实 User ID
                prov = {"region": "china", "token": payload,
                        "uid": str(uid) if uid else "", "devices": devs}
                try:
                    with open(PROV_FILE, "w", encoding="utf-8") as fh:
                        json.dump(prov, fh, ensure_ascii=False)
                    prov_path = PROV_FILE
                except Exception:
                    prov_path = "(写盘失败)"
                sn_list = ", ".join("%s(%s)" % (d.get("sn", ""), d.get("name", ""))
                                    for d in devs if d.get("sn"))
                _write(self, 200, {"ok": True, "path": p, "uid": uid,
                                   "sn_list": sn_list or "(无打印机)",
                                   "prov_path": prov_path,
                                   "next": "把 token / uid / SN 填进设备网页 192.168.4.1"})
            else:
                msg = payload.get("error") or payload.get("errmessage") or json.dumps(payload, ensure_ascii=False)[:120]
                _write(self, 200, {"ok": False, "msg": msg})
        else:
            _write(self, 404, {"msg": "not found"})


def main():
    p = argparse.ArgumentParser(description="本地拓竹登录页中转")
    p.add_argument("--port", type=int, default=8866)
    a = p.parse_args()
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    ln = srv.server_address[1]
    print("拓竹登录页已就绪，请用浏览器打开：")
    print("  http://127.0.0.1:%d" % ln)
    print("（仅本机可访问，关闭此窗口即停止服务）")
    threading.Timer(0.1, _open_browser, args=(ln,)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


def _open_browser(port):
    import webbrowser
    try:
        webbrowser.open("http://127.0.0.1:%d" % port)
    except Exception:
        pass


if __name__ == "__main__":
    sys.exit(main())