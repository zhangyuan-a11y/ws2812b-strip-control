#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
灯带控制台 —— 桌面小工具，改灯珠数量/亮度/灯效不用重新烧录。

它是干什么的：
    板子里的程序支持「串口命令」。这个小程序在你电脑上开一个只有自己能看的
    网页，你在网页上点一下，它就把命令发给板子。板子自己会把设置记住，
    断电也不会丢。

怎么用：
    双击同目录的「灯带控制台.command」。浏览器会自动弹出页面。
    改完直接叉掉浏览器就行，灯带会保持你设好的样子。
    那个黑色终端窗口别关 —— 关了网页就连不上板子了。
    想彻底退出：回到终端窗口按 Control + C。
"""
import json
import glob
import os
import re
import sys
import threading
import time
import webbrowser
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

BASE = os.path.dirname(os.path.abspath(__file__))


def find_port():
    """找开发板的串口。按优先级试几种常见命名，返回第一个命中的。"""
    for pat in ("/dev/cu.usbmodem*", "/dev/cu.wchusbserial*",
                "/dev/cu.usbserial*", "/dev/cu.SLAB_USBtoUART*"):
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[0]
    return None


HOST = "127.0.0.1"
PORTS = range(8770, 8791)
BAUD = 115200
LOG_MAX = 300
PAGE = os.path.join(BASE, "strip_ui.html")

# 只允许这几种命令，防止乱发东西给板子
# 只放行固件真正认识的命令形状（纯字母+数字，杜绝任何注入）
# 后半段的 g/k/h/y/r/x/z/a 是"治第一颗"的调试开关
ALLOW = re.compile(
    r"^(?:[nN]\d{1,3}|[bB]\d{1,3}|[cC][0-9A-Fa-f]{6}|[fF]\d{1,2}"
    r"|[pP][0-3]|[dD][0-3]|[xX][01]|[hH]\d{1,2}|[yY]\d{4,5}|[rR][1-5]"
    r"|[kKgG][01]|[zZ]\d{1,3}|[aA][0-9A-Fa-f]{6}|[wW]"
    r"|[mM]\d{1,5}|[sS][1-8]"
    r"|[0-7]|[tT]|\?|help)$")
# 从板子的回话里认出「现在什么设置」，用来自动同步界面
# 两种写法都认：「灯珠=74 颗」和「74 颗灯珠」
STATE_RE = re.compile(r"当前：效果=(\S+)\s+亮度=(\d+)\s+(?:灯珠=)?(\d+)\s*颗")
BEAT_RE  = re.compile(r"\[心跳\]\s*效果=(\S+)\s+亮度=(\d+)\s+(?:灯珠=)?(\d+)\s*颗")
COLOR_RE = re.compile(r"颜色\s*[=＝:]\s*([0-9A-Fa-f]{6})")
# 板子心跳里的「这一帧约 XXX 毫安；上限 YYY 毫安」——三种写法都认（含压暗后的括号说明）
MA_RE    = re.compile(r"\[用电\]\s*这一帧约\s*(\d+)\s*毫安(?:（[^）]*）)?\s*[；;]\s*(?:上限\s*(\d+)|电流上限没开)")
FX_RE = re.compile(r"^\s*>> 效果：(\S+)")
N_RE = re.compile(r"^\s*>> 灯珠数量 = (\d+)")
BRI_RE = re.compile(r"^\s*>> 亮度 = (\d+)")
BRS_RE = re.compile(r"呼吸快慢\s*[=＝]\s*(\d+)")
OFF_RE = re.compile(r"^\s*>> 关灯")


class Board:
    """管着一根串口：后台不停读（板子说什么就记下来），随时能往里写命令。"""

    def __init__(self, port_override=None):
        self.override = port_override
        self.lock = threading.RLock()
        self.ser = None
        self.port = None
        self.err = "还没开始连"
        self.lines = deque(maxlen=LOG_MAX)
        self.buf = ""
        self.opened_at = 0.0
        self.helloed = False
        self.got_data = False
        self.silent_notice = False
        self.release_until = 0.0
        self.state = {}
        self.stop = False

    # ---------------------------------------------------------- 日志
    def say(self, text):
        with self.lock:
            self.lines.append("[%s] %s" % (time.strftime("%H:%M:%S"), text))

    def _say_raw(self, line):
        with self.lock:
            self.lines.append("[%s] %s" % (time.strftime("%H:%M:%S"), line))
            hit = COLOR_RE.search(line)
            if hit:
                self.state["color"] = hit.group(1).upper()
            hit = STATE_RE.search(line) or BEAT_RE.search(line)
            if hit:
                self.state.update({"fx": hit.group(1), "bri": int(hit.group(2)),
                                   "n": int(hit.group(3))})
                return
            hit = FX_RE.match(line)
            if hit:
                self.state["fx"] = hit.group(1)
                return
            if OFF_RE.match(line):
                self.state["fx"] = "关灯"
                return
            hit = N_RE.match(line)
            if hit:
                self.state["n"] = int(hit.group(1))
                return
            hit = BRI_RE.match(line)
            if hit:
                self.state["bri"] = int(hit.group(1))
                return
            hit = MA_RE.search(line)
            if hit:
                self.state["ma"] = int(hit.group(1))
                self.state["maCap"] = int(hit.group(2)) if hit.group(2) else 0
                return
            hit = BRS_RE.search(line)
            if hit:
                self.state["brs"] = int(hit.group(1))

    # ------------------------------------------------------ 开关串口
    def _open_locked(self):
        if self.ser is not None:
            return True
        try:
            import serial
        except ImportError:
            self.err = "这台电脑缺 pyserial，跑一下 pip install pyserial"
            return False
        port = self.override or find_port()
        if not port:
            self.err = "没找到开发板（USB 线插稳了吗）"
            return False
        try:
            self.ser = __import__("serial").Serial(port, BAUD, timeout=0.2,
                                                   write_timeout=2)
        except Exception as exc:
            self.ser = None
            self.err = "打不开 %s：%s" % (port, exc)
            return False
        self.port = port
        self.err = ""
        self.buf = ""
        self.opened_at = time.time()
        self.helloed = False
        self.got_data = False
        self.silent_notice = False
        msg = "已连上开发板 %s" % port
        self.say(msg)
        print("  " + msg)
        return True

    def _close_locked(self, why=""):
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None
        self.port = None
        if why:
            self.say(why)
            print("  " + why)

    # -------------------------------------------------------- 发命令
    def send(self, cmd):
        cmd = (cmd or "").strip()[:40]
        if not ALLOW.match(cmd):
            return {"ok": False, "error": "不认识的命令：%s" % cmd}
        with self.lock:
            if time.time() < self.release_until:
                return {"ok": False, "error": "串口正让给烧录用，%d 秒后再点"
                        % int(self.release_until - time.time() + 1)}
            if not self._open_locked():
                return {"ok": False, "error": self.err}
            try:
                self.ser.write((cmd + "\n").encode("ascii", "ignore"))
            except Exception as exc:
                self._close_locked("串口发不出去了：%s" % exc)
                return {"ok": False, "error": str(exc)}
            self.say("你发的 → %s" % cmd)
        return {"ok": True, "port": self.port}

    def release(self, sec):
        """把串口让出去一段时间，好让烧录工具能接进来。"""
        sec = max(5, min(600, int(sec)))
        with self.lock:
            self._close_locked()
            self.release_until = time.time() + sec
            self.say("把串口让给烧录工具 %d 秒，烧完自动收回来" % sec)
        return {"ok": True, "seconds": sec}

    def reconnect(self):
        with self.lock:
            self._close_locked()
            if self._open_locked():
                return {"ok": True, "port": self.port}
            return {"ok": False, "error": self.err}

    def snapshot(self):
        with self.lock:
            left = int(self.release_until - time.time())
            out = {"connected": self.ser is not None, "port": self.port,
                   "err": self.err, "released": left if left > 0 else 0,
                   "log": "\n".join(self.lines)}
            out.update(self.state)
            return out

    # ------------------------------------------------------ 读串口线程
    def loop(self):
        while not self.stop:
            if time.time() < self.release_until:
                time.sleep(0.5)
                continue
            with self.lock:
                ok = self._open_locked()
                ser = self.ser
            if not ok or ser is None:
                time.sleep(1.2)
                continue

            data = b""
            try:
                data = ser.read(4096)
            except Exception as exc:
                with self.lock:
                    if self.ser is ser:
                        self._close_locked("串口断了（%s），3 秒后自己重连" % exc)
                time.sleep(3.0)
                continue

            if data:
                self.got_data = True
                text = data.decode("utf-8", "ignore")
                with self.lock:
                    self.buf += text
                    while "\n" in self.buf:
                        line, self.buf = self.buf.split("\n", 1)
                        line = line.rstrip("\r").rstrip()
                        if line:
                            self._say_raw(line)
            else:
                # 刚连上 1 秒后，自动问一次板子「现在什么设置」，界面就能同步
                with self.lock:
                    if (not self.silent_notice and self.ser is not None
                            and not self.got_data
                            and time.time() - self.opened_at > 8.0):
                        self.silent_notice = True
                        self.say("（连上 8 秒了，板子还没吭声。不影响你用 —— "
                                 "点上面的按钮，灯带有反应就是通的）")
                    if (not self.helloed and self.ser is not None
                            and time.time() - self.opened_at > 1.0):
                        self.helloed = True
                        try:
                            self.ser.write(b"?\n")
                        except Exception:
                            pass

        with self.lock:
            self._close_locked()


class Handler(BaseHTTPRequestHandler):
    board = None

    def log_message(self, *args):
        pass

    def _json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        try:
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
        except Exception:
            pass

    def _page(self):
        try:
            with open(PAGE, "r", encoding="utf-8") as fh:
                body = fh.read().encode("utf-8")
        except Exception as exc:
            self._json({"ok": False, "error": "读不到页面文件：%s" % exc}, 500)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.wfile.write(body)
        except Exception:
            pass

    def do_GET(self):
        url = urlparse(self.path)
        q = parse_qs(url.query)
        try:
            if url.path in ("/", "/index.html"):
                self._page()
            elif url.path == "/api/state":
                self._json(self.board.snapshot())
            elif url.path == "/api/send":
                self._json(self.board.send(q.get("cmd", [""])[0]))
            elif url.path == "/api/reconnect":
                self._json(self.board.reconnect())
            elif url.path == "/api/release":
                self._json(self.board.release(q.get("sec", ["90"])[0]))
            elif url.path == "/api/quit":
                self._json({"ok": True})
                threading.Timer(0.4, lambda: os._exit(0)).start()
            else:
                self._json({"ok": False, "error": "没有这个地址"}, 404)
        except Exception as exc:
            self._json({"ok": False, "error": "控制台内部出错：%s" % exc}, 500)


def find_running():
    """看看是不是已经有一个控制台在跑了（免得开两个抢串口）。"""
    import urllib.request
    for port in PORTS:
        try:
            url = "http://%s:%d/api/state" % (HOST, port)
            with urllib.request.urlopen(url, timeout=0.4) as resp:
                data = json.loads(resp.read().decode("utf-8"))
            if isinstance(data, dict) and "connected" in data and "log" in data:
                return port
        except Exception:
            continue
    return None


def main():
    port_override = os.environ.get("STRIP_PORT") or None
    open_browser = os.environ.get("STRIP_NO_BROWSER") != "1"
    for arg in sys.argv[1:]:
        if arg == "--no-browser":
            open_browser = False
        elif arg.startswith("--port="):
            port_override = arg.split("=", 1)[1]
        elif not arg.startswith("-"):
            port_override = arg

    if not port_override:
        running = find_running()
        if running:
            url = "http://%s:%d" % (HOST, running)
            print()
            print("  已经有一个灯带控制台在跑了，直接给你打开它的页面：")
            print("  %s" % url)
            print("  （不用开第二个，两个一起开会抢串口）")
            if open_browser:
                try:
                    webbrowser.open(url)
                except Exception:
                    pass
            return 0

    httpd = None
    for p in PORTS:
        try:
            httpd = ThreadingHTTPServer((HOST, p), Handler)
            break
        except OSError:
            continue
    if httpd is None:
        print("！端口 8770~8790 都被占用了，关掉别的东西再试")
        return 1

    board = Board(port_override)
    Handler.board = board
    threading.Thread(target=board.loop, daemon=True).start()
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    url = "http://%s:%d" % (HOST, httpd.server_address[1])
    try:
        os.makedirs(os.path.join(BASE, "logs"), exist_ok=True)
        with open(os.path.join(BASE, "logs", "console_port.txt"), "w") as fh:
            fh.write(str(httpd.server_address[1]))
    except Exception:
        pass

    print()
    print("=" * 56)
    print("  灯带控制台 已经开了")
    print("  网页地址：%s" % url)
    print()
    print("  · 浏览器会自动弹出来，在里面改灯珠数量/亮度/灯效")
    print("  · 这个窗口别关！关了网页就连不上板子了")
    print("  · 想彻底退出：在这个窗口按 Control + C")
    print("=" * 56)
    print()

    if open_browser:
        try:
            webbrowser.open(url)
        except Exception:
            pass

    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\n收工。灯带保持现在的样子不变。")
    finally:
        board.stop = True
        try:
            httpd.shutdown()
        except Exception:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
