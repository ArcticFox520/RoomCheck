#!/usr/bin/env python3
"""
RoomCheck API acceptance tests.

Starts its own server with a temporary data directory, exercises the full
business flow, restarts the server where persistence matters and manipulates
the SQLite database only for time-based scenarios (session expiry, 72-day
deletion, log retention).
"""
import hashlib
import http.cookiejar
import io
import json
import os
import shutil
import socket
import sqlite3
import struct
import subprocess
import sys
import tarfile
import tempfile
import time
import traceback
import urllib.error
import urllib.request
import zlib
from datetime import datetime, timedelta, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "RoomCheck")
ADMIN_INIT_PW = "InitAdmin#2026"
ADMIN_PW = "AdminPass#2026"

PASS = 0
FAIL = 0
FAILURES = []


def check(cond, message):
    if not cond:
        raise AssertionError(message)


def expect_status(result, code, label):
    status = result[0]
    if status != code:
        body = result[1] if len(result) > 1 else None
        raise AssertionError("%s: 期望 HTTP %s，实际 %s，响应=%s" % (label, code, status, body))
    return result[1]


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def png_bytes(w=2, h=2):
    raw = b"".join(b"\x00" + b"\xff\x00\x00" * w for _ in range(h))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def multipart(fields, files):
    boundary = "----RoomCheckTest%s" % os.urandom(8).hex()
    out = []
    for name, value in fields.items():
        out.append(("--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n"
                    % (boundary, name, value)).encode())
    for name, filename, content, ctype in files:
        out.append(("--%s\r\nContent-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"
                    "Content-Type: %s\r\n\r\n" % (boundary, name, filename, ctype)).encode())
        out.append(content)
        out.append(b"\r\n")
    out.append(("--%s--\r\n" % boundary).encode())
    return b"".join(out), "multipart/form-data; boundary=%s" % boundary


class Server:
    def __init__(self):
        self.port = free_port()
        self.base = "http://127.0.0.1:%d" % self.port
        self.data_dir = tempfile.mkdtemp(prefix="roomcheck-test-")
        self.log_path = os.path.join(self.data_dir, "server.log")
        self.proc = None

    def start(self, backup_disabled=True, reset_admin=False):
        env = os.environ.copy()
        env.pop("ROOMCHECK_DISABLE_BACKUP", None)
        env.pop("ADMIN_RESET", None)
        env.update({
            "PORT": str(self.port),
            "ROOMCHECK_DATA_DIR": self.data_dir,
            "ROOMCHECK_WEB_ROOT": os.path.join(ROOT, "frontend"),
            "ADMIN_INIT_PASSWORD": ADMIN_INIT_PW,
        })
        if backup_disabled:
            env["ROOMCHECK_DISABLE_BACKUP"] = "1"
        if reset_admin:
            env["ADMIN_RESET"] = "1"
        log = open(self.log_path, "ab")
        self.proc = subprocess.Popen([BIN], cwd=ROOT, env=env, stdout=log, stderr=log)
        log.close()
        deadline = time.time() + 8
        while time.time() < deadline:
            try:
                with urllib.request.urlopen(self.base + "/api/health", timeout=1) as r:
                    if r.status == 200:
                        return
            except Exception:
                time.sleep(0.1)
        self.dump_log()
        raise RuntimeError("服务启动失败")

    def stop(self):
        if not self.proc:
            return
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.proc = None

    def restart(self, **kwargs):
        self.stop()
        self.start(**kwargs)

    def dump_log(self):
        try:
            with open(self.log_path, "r", errors="replace") as f:
                lines = f.readlines()[-80:]
            sys.stderr.write("---- server log ----\n" + "".join(lines))
        except Exception:
            pass

    def db(self):
        conn = sqlite3.connect(os.path.join(self.data_dir, "roomcheck.db"), timeout=10)
        conn.row_factory = sqlite3.Row
        return conn

    def sql(self, query, params=()):
        with self.db() as conn:
            cur = conn.execute(query, params)
            return cur.fetchall()

    def sql_one(self, query, params=()):
        rows = self.sql(query, params)
        return rows[0] if rows else None

    def cleanup(self):
        self.stop()
        shutil.rmtree(self.data_dir, ignore_errors=True)


class Client:
    def __init__(self, server):
        self.base = server.base
        self.jar = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(
            urllib.request.HTTPCookieProcessor(self.jar))
        self.csrf = None
        self.user = None

    def call(self, method, path, json_body=None, data=None, headers=None,
             csrf=True, timeout=20):
        hdrs = dict(headers or {})
        if json_body is not None:
            data = json.dumps(json_body, ensure_ascii=False).encode("utf-8")
            hdrs["Content-Type"] = "application/json; charset=utf-8"
        if data is None:
            data = b"" if method in ("POST", "PUT", "PATCH", "DELETE") else None
        if csrf and self.csrf and method in ("POST", "PUT", "PATCH", "DELETE"):
            hdrs["X-CSRF-Token"] = self.csrf
        req = urllib.request.Request(self.base + path, data=data, headers=hdrs,
                                     method=method)
        try:
            resp = self.opener.open(req, timeout=timeout)
            status = resp.status
            body = resp.read()
            ctype = resp.headers.get("Content-Type", "")
            hdrs_out = dict(resp.headers)
        except urllib.error.HTTPError as e:
            status = e.code
            body = e.read()
            ctype = e.headers.get("Content-Type", "")
            hdrs_out = dict(e.headers)
        obj = None
        if "application/json" in ctype and body:
            obj = json.loads(body.decode("utf-8"))
        return status, obj, hdrs_out, body

    def login(self, username, password):
        st, obj, _, _ = self.call("POST", "/api/login",
                                  {"username": username, "password": password},
                                  csrf=False)
        if st == 200:
            self.csrf = obj["csrf_token"]
            self.user = obj["user"]
        return st, obj

    def sync(self):
        st, obj, _, _ = self.call("GET", "/api/session")
        if st == 200:
            self.csrf = obj["csrf_token"]
            self.user = obj["user"]
        return st, obj

    def add_note(self, room_id, content):
        return self.call("POST", "/api/rooms/%d/notes" % room_id, {"content": content})

    def upload(self, room_id, filename="photo.png", content=None, ctype="image/png",
               note_id=None):
        content = content if content is not None else png_bytes()
        fields = {}
        if note_id is not None:
            fields["note_id"] = str(note_id)
        body, ct = multipart(fields, [("file", filename, content, ctype)])
        return self.call("POST", "/api/rooms/%d/images" % room_id, data=body,
                         headers={"Content-Type": ct})


class TestContext:
    def __init__(self):
        self.server = Server()
        self.admin = Client(self.server)
        self.m1 = Client(self.server)
        self.m2 = Client(self.server)
        self.ids = {}

    def week(self, client, path="/api/week"):
        st, obj, _, _ = client.call("GET", path)
        expect_status((st, obj), 200, "读取周视图")
        check(obj.get("week") is not None, "周计划应存在")
        return obj

    def room(self, week, number):
        for day in week["days"]:
            for group in day["groups"]:
                for room in group["rooms"]:
                    if room["room_number"] == number:
                        return room
        raise AssertionError("周视图中找不到寝室 %s" % number)

    def rooms(self, week):
        return [r for d in week["days"] for g in d["groups"] for r in g["rooms"]]

    def set_config(self, client, building, start, end, excluded, account_id=None):
        body = {"building_name": building, "start_room": start, "end_room": end,
                "excluded_rooms": excluded}
        if account_id is not None:
            body["account_id"] = account_id
        return client.call("PUT", "/api/config", body)

    def find_account(self, username):
        st, obj, _, _ = self.admin.call("GET", "/api/admin/accounts")
        expect_status((st, obj), 200, "账户列表")
        for a in obj["accounts"]:
            if a["username"] == username:
                return a
        raise AssertionError("账户不存在: %s" % username)


def run_test(fn):
    global PASS, FAIL
    try:
        fn()
        PASS += 1
        print("PASS  %s" % fn.__name__)
    except Exception as exc:
        FAIL += 1
        FAILURES.append((fn.__name__, exc))
        print("FAIL  %s: %s" % (fn.__name__, exc))
        traceback.print_exc(limit=2)
        try:
            t.server.dump_log()
        except Exception:
            pass


t = None


# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------
def test_health_and_static():
    st, obj, headers, _ = t.admin.call("GET", "/api/health")
    expect_status((st, obj), 200, "health")
    check(obj["ok"] is True and obj["version"], "health 字段")
    st, obj, _, body = t.admin.call("GET", "/api/unknown")
    expect_status((st, obj), 404, "未知 API")
    index = os.path.join(ROOT, "frontend", "index.html")
    if os.path.exists(index):
        st, obj, headers, body = t.admin.call("GET", "/")
        expect_status((st, obj), 200, "首页")
        check("text/html" in headers.get("Content-Type", ""), "首页 content-type")
        st, obj, headers, body = t.admin.call("GET", "/css/style.css")
        expect_status((st, obj), 200, "样式文件")
        check("text/css" in headers.get("Content-Type", ""), "CSS content-type")
    st, obj, _, _ = t.admin.call("GET", "/%2e%2e/%2e%2e/etc/passwd")
    check(st == 404, "路径穿越应被拒绝")


def test_unauthenticated_and_csrf():
    c = Client(t.server)
    st, obj, _, _ = c.call("GET", "/api/week")
    expect_status((st, obj), 401, "未登录访问")
    st, obj, _, _ = c.call("POST", "/api/login", {"username": "admin", "password": "x"})
    expect_status((st, obj), 401, "错误密码")
    st, obj, _, _ = c.call("POST", "/api/login", data=b"{bad json",
                            headers={"Content-Type": "application/json"}, csrf=False)
    expect_status((st, obj), 400, "非法 JSON")


def test_admin_first_login_gate():
    st, obj = t.admin.login("admin", ADMIN_INIT_PW)
    expect_status((st, obj), 200, "管理员登录")
    check(obj["user"]["must_change_password"] is True, "首次登录应强制改密")
    st, obj, _, _ = t.admin.call("GET", "/api/admin/accounts")
    expect_status((st, obj), 403, "未改密应被拦截")
    check(obj["error"]["code"] == "password_change_required", "改密拦截错误码")
    st, obj, _, _ = t.admin.call("POST", "/api/password",
                                 {"old_password": ADMIN_INIT_PW,
                                  "new_password": ADMIN_PW})
    expect_status((st, obj), 200, "修改管理员密码")
    t.admin.csrf = obj["csrf_token"]
    st, obj = t.admin.sync()
    expect_status((st, obj), 200, "改密后 session")
    check(obj["user"]["must_change_password"] is False, "改密标志应清除")
    expires = obj["expires_at"]
    t.ids["admin_session_ttl"] = expires - int(time.time())


def test_super_admin_role_and_admin_accounts():
    st, obj, _, _ = t.admin.call("GET", "/api/session")
    expect_status((st, obj), 200, "超级管理员会话")
    check(obj["user"]["role"] == "super_admin", "初始化管理员应迁移为超级管理员")
    t.ids["super_admin"] = obj["user"]["id"]
    row = t.server.sql_one("SELECT role, super_admin FROM account WHERE id=?",
                           (t.ids["super_admin"],))
    check(row["role"] == "admin" and row["super_admin"] == 1, "超级管理员存储结构")

    st, obj, _, _ = t.admin.call("GET", "/api/super-admin/admins")
    expect_status((st, obj), 200, "管理员账户列表")
    check(any(a["id"] == t.ids["super_admin"] and a["role"] == "super_admin"
              for a in obj["accounts"]), "列表包含超级管理员")

    st, obj, _, _ = t.admin.call("POST", "/api/super-admin/admins",
                                 {"username": "adm1", "password": "Admin1#2026",
                                  "display_name": "管理员一", "role": "super_admin",
                                  "super_admin": True})
    expect_status((st, obj), 201, "超级管理员创建管理员")
    check(obj["account"]["role"] == "admin", "伪造 role 参数不能提升权限")
    t.ids["adm1"] = obj["account"]["id"]
    check(t.server.sql_one("SELECT super_admin FROM account WHERE id=?",
                           (t.ids["adm1"],))["super_admin"] == 0, "新管理员不是超级管理员")

    st, obj, _, _ = t.admin.call("POST", "/api/super-admin/admins",
                                 {"username": "adm1", "password": "x"})
    expect_status((st, obj), 409, "管理员账户名重复拒绝")
    st, obj, _, _ = t.admin.call("POST", "/api/admin/accounts",
                                 {"username": "adm1", "password": "x", "display_name": "x"})
    expect_status((st, obj), 409, "楼层长不能与管理员重名")

    adm1 = Client(t.server)
    st, obj = adm1.login("adm1", "Admin1#2026")
    expect_status((st, obj), 200, "普通管理员登录")
    check(obj["user"]["role"] == "admin", "普通管理员角色")
    st, obj, _, _ = adm1.call("GET", "/api/admin/accounts")
    expect_status((st, obj), 200, "普通管理员保留楼层长管理权限")
    st, obj, _, _ = adm1.call("GET", "/api/super-admin/admins")
    expect_status((st, obj), 403, "普通管理员不能访问管理员管理接口")
    st, obj, _, _ = adm1.call("POST", "/api/super-admin/admins",
                              {"username": "evil", "password": "x"})
    expect_status((st, obj), 403, "普通管理员不能创建管理员")
    st, obj, _, _ = adm1.call("PATCH", "/api/admin/accounts/%d" % t.ids["adm1"],
                              {"display_name": "提权", "role": "super_admin"})
    expect_status((st, obj), 404, "楼层长接口不能修改管理员账户")
    st, obj, _, _ = adm1.call("POST",
                              "/api/super-admin/admins/%d/disable" % t.ids["super_admin"],
                              {"confirm": True})
    expect_status((st, obj), 403, "普通管理员不能禁用超级管理员")
    st, obj, _, _ = adm1.call("PATCH", "/api/profile",
                              {"display_name": "管理员一", "role": "super_admin"})
    expect_status((st, obj), 200, "普通管理员修改本人资料")
    check(obj["user"]["role"] == "admin", "资料接口不能提升角色")

    st, obj, _, _ = t.admin.call("PATCH", "/api/super-admin/admins/%d" % t.ids["adm1"],
                                 {"username": "adm1b", "password": "Admin1New#2026",
                                  "role": "super_admin"})
    expect_status((st, obj), 200, "超级管理员修改管理员账户")
    check(obj["account"]["username"] == "adm1b" and obj["account"]["role"] == "admin",
          "管理员修改结果")
    st, obj, _, _ = adm1.call("GET", "/api/session")
    expect_status((st, obj), 401, "修改管理员密码后旧会话失效")
    st, obj = adm1.login("adm1", "Admin1#2026")
    expect_status((st, obj), 401, "管理员旧密码失效")
    st, obj = adm1.login("adm1b", "Admin1New#2026")
    expect_status((st, obj), 200, "管理员新账户名密码可登录")
    st, obj, _, _ = t.admin.call("PATCH", "/api/super-admin/admins/%d" % t.ids["adm1"],
                                 {"username": "adm1", "password": "Admin1#2026"})
    expect_status((st, obj), 200, "恢复管理员账户名与密码")

    adm1 = Client(t.server)
    expect_status(adm1.login("adm1", "Admin1#2026"), 200, "管理员重新登录")
    st, obj, _, _ = t.admin.call("POST",
                                 "/api/super-admin/admins/%d/disable" % t.ids["adm1"],
                                 {"confirm": True})
    expect_status((st, obj), 200, "超级管理员禁用管理员")
    check(obj["account"]["status"] == "disabled", "管理员禁用状态")
    st, obj, _, _ = adm1.call("GET", "/api/session")
    expect_status((st, obj), 401, "禁用管理员后会话立即失效")
    st, obj = adm1.login("adm1", "Admin1#2026")
    expect_status((st, obj), 403, "禁用管理员后禁止登录")
    check(obj["error"]["code"] == "account_disabled", "管理员禁用登录错误码")
    st, obj, _, _ = t.admin.call("POST",
                                 "/api/super-admin/admins/%d/enable" % t.ids["adm1"],
                                 {"confirm": True})
    expect_status((st, obj), 200, "超级管理员启用管理员")
    adm1 = Client(t.server)
    expect_status(adm1.login("adm1", "Admin1#2026"), 200, "管理员启用后可登录")

    fm_count = t.server.sql_one(
        "SELECT COUNT(*) c FROM account WHERE role='floor_manager'")["c"]
    st, obj, _, _ = t.admin.call("POST",
                                 "/api/super-admin/admins/%d/delete" % t.ids["adm1"],
                                 {"confirm": True})
    expect_status((st, obj), 200, "超级管理员删除管理员")
    check(obj["account"]["status"] == "deleted", "管理员删除状态")
    check(t.server.sql_one(
        "SELECT COUNT(*) c FROM account WHERE role='floor_manager'")["c"] == fm_count,
        "删除管理员不影响楼层长账户")
    st, obj = adm1.login("adm1", "Admin1#2026")
    expect_status((st, obj), 403, "管理员删除后禁止登录")
    st, obj, _, _ = t.admin.call("POST",
                                 "/api/super-admin/admins/%d/restore" % t.ids["adm1"],
                                 {"confirm": True})
    expect_status((st, obj), 200, "超级管理员撤销删除管理员")
    check(obj["account"]["status"] == "enabled", "管理员恢复启用")

    st, obj, _, _ = t.admin.call("POST",
                                 "/api/super-admin/admins/%d/disable" % t.ids["super_admin"],
                                 {"confirm": True})
    expect_status((st, obj), 409, "不能禁用最后一个可用超级管理员")
    check(obj["error"]["code"] == "last_super_admin", "最后超级管理员禁用错误码")
    st, obj, _, _ = t.admin.call("POST",
                                 "/api/super-admin/admins/%d/delete" % t.ids["super_admin"],
                                 {"confirm": True})
    expect_status((st, obj), 409, "不能删除最后一个可用超级管理员")
    st, obj, _, _ = t.admin.call("GET", "/api/session")
    expect_status((st, obj), 200, "拒绝后超级管理员会话仍然有效")


def test_account_crud_validation():
    st, obj, _, _ = t.admin.call("POST", "/api/admin/accounts",
                                 {"username": "m1", "password": "Manager1#2026",
                                  "display_name": "一楼"})
    expect_status((st, obj), 201, "创建楼层长 m1")
    t.ids["m1"] = obj["account"]["id"]
    st, obj, _, _ = t.admin.call("POST", "/api/admin/accounts",
                                 {"username": "M1", "password": "other"})
    expect_status((st, obj), 409, "账户名全局唯一（大小写不敏感）")
    st, obj, _, _ = t.admin.call("POST", "/api/admin/accounts",
                                 {"username": "admin", "password": "other"})
    expect_status((st, obj), 409, "不能与管理員重名")
    st, obj, _, _ = t.admin.call("POST", "/api/admin/accounts",
                                 {"username": "m2", "password": "Manager2#2026"})
    expect_status((st, obj), 201, "创建楼层长 m2")
    t.ids["m2"] = obj["account"]["id"]
    st, obj, _, _ = t.admin.call("POST", "/api/admin/accounts",
                                 {"username": "m4", "password": "Manager4#2026"})
    expect_status((st, obj), 201, "创建楼层长 m4")
    t.ids["m4"] = obj["account"]["id"]


def test_super_admin_manages_floor_managers():
    st, obj, _, _ = t.admin.call("POST", "/api/admin/accounts",
                                 {"username": "safm", "password": "SaFm#2026",
                                  "display_name": "超级管理员创建"})
    expect_status((st, obj), 201, "超级管理员创建楼层长")
    check(obj["account"]["role"] == "floor_manager", "楼层长角色")
    fid = obj["account"]["id"]
    st, obj, _, _ = t.admin.call("PATCH", "/api/admin/accounts/%d" % fid,
                                 {"username": "safm2", "password": "SaFm2#2026",
                                  "display_name": "已修改"})
    expect_status((st, obj), 200, "超级管理员修改楼层长")
    check(obj["account"]["username"] == "safm2", "楼层长账户名修改")
    st, obj, _, _ = t.admin.call("POST", "/api/admin/accounts/%d/disable" % fid,
                                 {"confirm": True})
    expect_status((st, obj), 200, "超级管理员禁用楼层长")
    st, obj, _, _ = t.admin.call("POST", "/api/admin/accounts/%d/enable" % fid,
                                 {"confirm": True})
    expect_status((st, obj), 200, "超级管理员启用楼层长")
    st, obj, _, _ = t.admin.call("POST", "/api/admin/accounts/%d/delete" % fid,
                                 {"confirm": True})
    expect_status((st, obj), 200, "超级管理员删除楼层长")
    st, obj, _, _ = t.admin.call("POST", "/api/admin/accounts/%d/restore" % fid,
                                 {"confirm": True})
    expect_status((st, obj), 200, "超级管理员恢复楼层长")
    c = Client(t.server)
    expect_status(c.login("safm2", "SaFm2#2026"), 200, "楼层长恢复后正常登录")


def test_manager_login_and_config_validation():
    st, obj = t.m1.login("m1", "Manager1#2026")
    expect_status((st, obj), 200, "楼层长登录")
    check(obj["user"]["role"] == "floor_manager", "角色")
    st, obj, _, _ = t.m1.call("GET", "/api/config")
    expect_status((st, obj), 200, "初始配置")
    check(obj["config"] is None, "初始无配置")

    cases = [
        ({"building_name": "测试楼", "start_room": "120", "end_room": "110",
          "excluded_rooms": []}, 422, "起始大于结束"),
        ({"building_name": "测试楼", "start_room": "101", "end_room": "112",
          "excluded_rooms": ["120"]}, 422, "排除寝室越界"),
        ({"building_name": "测试楼", "start_room": "101", "end_room": "112",
          "excluded_rooms": ["105", "105"]}, 422, "排除寝室重复"),
        ({"building_name": "测试楼", "start_room": "101", "end_room": "101",
          "excluded_rooms": ["101"]}, 422, "N=0"),
        ({"building_name": "", "start_room": "101", "end_room": "112",
          "excluded_rooms": []}, 422, "空楼栋名"),
    ]
    for body, code, label in cases:
        st, obj, _, _ = t.m1.call("PUT", "/api/config", body)
        expect_status((st, obj), code, label)

    st, obj, _, _ = t.set_config(t.m1, "测试楼", "101", "112", ["105"])
    expect_status((st, obj), 200, "保存配置")
    check(obj["config"]["N"] == 11, "N 计算")
    check(obj["schedule"]["week_updated"] is True, "首次配置应生成排班")
    check(obj["schedule"]["N"] == 11, "排班数量")

    week = t.week(t.m1)
    check(week["progress"]["total"] == 11, "周计划总数")
    check(len(week["days"]) == 6, "六个查寝日")
    check(all(d["day_index"] != 3 for d in week["days"]), "周四必须隐藏")
    counts = [sum(len(g["rooms"]) for g in d["groups"]) for d in week["days"]]
    check(max(counts) - min(counts) <= 1, "六天平均分配: %s" % counts)
    for d in week["days"]:
        gcounts = [len(g["rooms"]) for g in d["groups"]]
        check(abs(gcounts[0] - gcounts[1]) <= 1, "中午/晚上平均: %s" % gcounts)
    all_rooms = t.rooms(week)
    check(len(all_rooms) == len(set(r["room_number"] for r in all_rooms)), "无重复寝室")
    check("105" not in [r["room_number"] for r in all_rooms], "无需查寝寝室不出现")


def test_week_date_range_display():
    st, obj, _, _ = t.admin.call("POST", "/api/admin/accounts",
                                 {"username": "weekfm", "password": "WeekFm#2026",
                                  "display_name": "周显示"})
    expect_status((st, obj), 201, "创建周显示账户")
    wid = obj["account"]["id"]
    wf = Client(t.server)
    expect_status(wf.login("weekfm", "WeekFm#2026"), 200, "周显示账户登录")
    st, obj, _, _ = t.set_config(wf, "周显示楼", "501", "506", [])
    expect_status((st, obj), 200, "周显示账户配置")

    week = t.week(wf)
    info = week["week"]
    start = datetime.strptime(info["week_start"], "%Y-%m-%d").date()
    end = datetime.strptime(info["week_end"], "%Y-%m-%d").date()
    check(start.weekday() == 0, "周起始必须是周一")
    check((end - start).days == 6, "周结束必须是周日")
    check(info["label"] == "%d月%d日 — %d月%d日" %
          (start.month, start.day, end.month, end.day),
          "周标题应为真实日期范围: %s" % info["label"])
    check("年第" not in info["label"], "周标题不再包含年份周数")

    week_id = t.server.sql_one(
        "SELECT id FROM week_plan WHERE account_id=? ORDER BY id DESC LIMIT 1",
        (wid,))["id"]

    def shanghai_epoch(y, m, d):
        return int(datetime(y, m, d, tzinfo=timezone(timedelta(hours=8))).timestamp())

    def set_week_range(start_text, end_text):
        start_epoch = shanghai_epoch(*[int(x) for x in start_text.split("-")])
        end_epoch = shanghai_epoch(*[int(x) for x in end_text.split("-")]) + 86399
        with t.server.db() as conn:
            conn.execute("UPDATE week_plan SET week_start=?, week_end=? WHERE id=?",
                         (start_epoch, end_epoch, week_id))

    set_week_range("2026-11-30", "2026-12-06")
    week = t.week(wf)
    check(week["week"]["week_start"] == "2026-11-30", "跨月周起始")
    check(week["week"]["week_end"] == "2026-12-06", "跨月周结束")
    check(week["week"]["label"] == "11月30日 — 12月6日", "跨月周日期范围")
    check(week["days"][0]["date"] == "2026-11-30", "周内日期来自真实周起始")

    set_week_range("2025-12-29", "2026-01-04")
    week = t.week(wf)
    check(week["week"]["week_start"] == "2025-12-29", "跨年周起始")
    check(week["week"]["week_end"] == "2026-01-04", "跨年周结束")
    check(week["week"]["label"] == "12月29日 — 1月4日", "跨年周日期范围")

    st, obj, _, _ = t.admin.call("GET", "/api/admin/accounts/%d/week" % wid)
    expect_status((st, obj), 200, "管理员周视图")
    check(obj["week"]["label"] == "12月29日 — 1月4日", "管理员周视图日期范围")
    check(obj["week"]["week_start"] == "2025-12-29", "管理员周视图真实日期")

    st, obj, _, _ = t.admin.call("GET", "/api/admin/accounts")
    expect_status((st, obj), 200, "管理员账户列表")
    entry = [a for a in obj["accounts"] if a["username"] == "weekfm"][0]
    check(entry["week"]["week_start"] == "2025-12-29", "账户列表周起始")
    check(entry["week"]["week_end"] == "2026-01-04", "账户列表周结束")
    check(entry["week"]["label"] == "12月29日 — 1月4日", "账户列表日期范围")

    app_js_path = os.path.join(ROOT, "frontend", "js", "app.js")
    with open(app_js_path, "r", encoding="utf-8") as fh:
        app_js = fh.read()
    check("年第" not in app_js, "前端不得再出现年份周数标题")
    check("info.label" not in app_js and "week.label" not in app_js,
          "前端不再使用周标签字段")
    check("formatWeekRange(info.week_start, info.week_end)" in app_js,
          "查寝周视图使用真实周起止日期")
    check("formatWeekRange(account.week.week_start, account.week.week_end)" in app_js,
          "管理员账户列表使用真实周起止日期")


def test_check_and_csrf():
    week = t.week(t.m1)
    room = t.room(week, "101")
    rid = room["id"]
    saved = t.m1.csrf
    t.m1.csrf = None
    st, obj, _, _ = t.m1.call("POST", "/api/checks/%d/check" % rid)
    expect_status((st, obj), 403, "缺少 CSRF")
    t.m1.csrf = saved
    st, obj, _, _ = t.m1.call("POST", "/api/checks/%d/check" % rid)
    expect_status((st, obj), 200, "勾选")
    check(obj["room"]["checked"] is True and obj["progress"]["checked"] == 1, "勾选状态")
    st, obj, _, _ = t.m1.call("POST", "/api/checks/%d/uncheck" % rid, {})
    expect_status((st, obj), 400, "撤销未确认")
    check(t.room(t.week(t.m1), "101")["checked"] is True, "未确认不得修改")
    st, obj, _, _ = t.m1.call("POST", "/api/checks/%d/uncheck" % rid, {"confirm": True})
    expect_status((st, obj), 200, "撤销勾选")
    check(obj["room"]["checked"] is False, "撤销结果")
    st, obj, _, _ = t.m1.call("POST", "/api/checks/%d/check" % rid)
    expect_status((st, obj), 200, "再次勾选")
    t.ids["m1_checked_room"] = "101"


def test_notes_and_limits():
    week = t.week(t.m1)
    room = t.room(week, "101")
    rid = room["id"]
    st, obj, _, _ = t.m1.add_note(rid, "灯管坏了")
    expect_status((st, obj), 201, "新增备注")
    note_id = obj["note"]["id"]
    t.ids["note_101"] = note_id
    st, obj, _, _ = t.m1.add_note(rid, "第二條備註")
    expect_status((st, obj), 201, "多条备注")
    st, obj, _, _ = t.m1.call("GET", "/api/rooms/%d/notes" % rid)
    expect_status((st, obj), 200, "备注列表")
    check(len(obj["notes"]) == 2 and obj["limit"] == 500, "备注条数/限制")
    st, obj, _, _ = t.m1.call("PATCH", "/api/notes/%d" % note_id, {"content": "已更换灯管"})
    expect_status((st, obj), 200, "修改备注")
    check(obj["note"]["content"] == "已更换灯管", "备注内容")
    st, obj, _, _ = t.m1.call("DELETE", "/api/notes/%d" % note_id)
    expect_status((st, obj), 200, "删除备注")

    room3 = t.room(week, "103")
    st, obj, _, _ = t.m1.add_note(room3["id"], "字" * 501)
    expect_status((st, obj), 422, "备注超 500 字")
    st, obj, _, _ = t.m1.add_note(room3["id"], "字" * 500)
    expect_status((st, obj), 201, "恰好 500 字")
    st, obj, _, _ = t.m1.add_note(room3["id"], "字")
    expect_status((st, obj), 422, "超过 500 字拒绝")
    st, obj, _, _ = t.m1.call("GET", "/api/rooms/%d/notes" % room3["id"])
    check(obj["total_chars"] == 500, "备注总字数")

    # linked image deletion cascade
    st, obj, _, _ = t.m1.upload(rid)
    expect_status((st, obj), 201, "上传图片")
    t.ids["image_101"] = obj["image"]["id"]
    room4 = t.room(week, "104")
    st, obj, _, _ = t.m1.add_note(room4["id"], "关联图片的备注")
    expect_status((st, obj), 201, "关联备注")
    linked_note = obj["note"]["id"]
    st, obj, _, _ = t.m1.upload(room4["id"], note_id=linked_note)
    expect_status((st, obj), 201, "上传关联图片")
    check(obj["image"]["note_id"] == linked_note, "图片关联备注")
    linked_image = obj["image"]["id"]
    st, obj, _, _ = t.m1.call("DELETE", "/api/notes/%d" % linked_note)
    expect_status((st, obj), 200, "删除备注并级联删除图片")
    check(obj["deleted_images"] == 1, "级联删除图片数量")
    st, obj, _, _ = t.m1.call("GET", "/api/images/%d" % linked_image)
    expect_status((st, obj), 404, "关联图片已删除")
    st, obj, _, _ = t.m1.call("GET", "/api/images/%d/file" % linked_image)
    expect_status((st, obj), 404, "关联图片文件已删除")


def test_image_security():
    week = t.week(t.m1)
    rid = t.room(week, "101")["id"]
    st, obj, _, _ = t.m1.upload(rid, filename="evil.exe", ctype="image/png")
    expect_status((st, obj), 422, "扩展名黑名单")
    st, obj, _, _ = t.m1.upload(rid, filename="evil.jpg", content=b"\x7fELF\x02\x01",
                                ctype="image/jpeg")
    expect_status((st, obj), 422, "ELF 文件签名")
    st, obj, _, _ = t.m1.upload(rid, filename="page.html", ctype="text/html")
    expect_status((st, obj), 422, "HTML MIME")
    st, obj, _, _ = t.m1.upload(rid, filename="x.png", content=b"not an image",
                                ctype="text/plain")
    expect_status((st, obj), 422, "非图片")
    big = png_bytes() + b"\x00" * (10 * 1024 * 1024)
    st, obj, _, _ = t.m1.upload(rid, filename="big.png", content=big)
    check(st in (413, 400), "超大文件应拒绝，实际 %s" % st)
    st, obj, _, _ = t.m1.upload(rid, filename="ok.png", ctype="image/png")
    expect_status((st, obj), 201, "合法 PNG")
    img_id = obj["image"]["id"]
    st, obj, headers, body = t.m1.call("GET", "/api/images/%d/file" % img_id)
    expect_status((st, obj), 200, "查看原图")
    check(body[:8] == b"\x89PNG\r\n\x1a\n", "原图内容")
    st, obj, _, _ = t.m1.call("DELETE", "/api/images/%d" % img_id)
    expect_status((st, obj), 200, "删除图片")


def test_weekly_reset_and_regenerate():
    week = t.week(t.m1)
    room = t.room(week, "101")
    rid = room["id"]
    st, obj, _, _ = t.m1.add_note(rid, "重置前备注")
    expect_status((st, obj), 201, "重置前备注")
    st, obj, _, _ = t.m1.upload(rid)
    expect_status((st, obj), 201, "重置前图片")
    old_week = t.server.sql_one(
        "SELECT week_start, id FROM week_plan WHERE account_id=? ORDER BY id DESC LIMIT 1",
        (t.ids["m1"],))
    before_checked = t.week(t.m1)["progress"]["checked"]
    check(before_checked > 0, "重置前应有勾选")
    old_dir = os.path.join(t.server.data_dir, "uploads", str(t.ids["m1"]), str(old_week["id"]))
    check(os.path.isdir(old_dir), "旧周图片目录应存在")

    st, obj, _, _ = t.m1.call("POST", "/api/week/reset", {"confirm": True})
    expect_status((st, obj), 200, "重置本周")
    check(obj["progress"]["checked"] == 0, "重置后勾选清空")
    check(obj["week"]["plan_type"] == "reset", "新周计划类型")
    check(not os.path.exists(old_dir), "旧周图片目录应删除")
    rooms_with_notes = [r for r in t.rooms(obj) if r["notes_count"]]
    check(len(rooms_with_notes) >= 2, "备注应迁移到新周")
    new_room = t.room(obj, "101")
    check(new_room["notes_count"] >= 2, "101 备注数量迁移")
    check(new_room["images_count"] >= 1, "101 图片应迁移到新周")
    st, notes, _, _ = t.m1.call("GET", "/api/rooms/%d/notes" % new_room["id"])
    expect_status((st, notes), 200, "新周备注")
    check(any(n["content"] == "重置前备注" for n in notes["notes"]), "备注内容保留")

    st, obj, _, _ = t.m1.call("POST", "/api/schedule/regenerate", {"confirm": True})
    expect_status((st, obj), 200, "重新生成排班")
    check(obj["progress"]["checked"] == 0, "重新生成后勾选清空")
    check(obj["week"]["plan_type"] == "regenerate", "重新生成类型")
    check(t.room(obj, "101")["notes_count"] >= 2, "重新生成保留备注")
    st, obj, _, _ = t.m1.call("POST", "/api/schedule/regenerate", {})
    expect_status((st, obj), 400, "重新生成缺少确认")


def test_incremental_schedule():
    week = t.week(t.m1)
    room101 = t.room(week, "101")
    rid = room101["id"]
    t.m1.call("POST", "/api/checks/%d/check" % rid)
    positions_before = {r["room_number"]: (d["day_index"], g["period"])
                        for d in week["days"] for g in d["groups"] for r in g["rooms"]}

    # a note on a room that will leave the range becomes an orphan
    room112 = t.room(week, "112")
    st, obj, _, _ = t.m1.add_note(room112["id"], "移出范围的备注")
    expect_status((st, obj), 201, "移出范围备注")
    orphan_note_id = obj["note"]["id"]

    st, obj, _, _ = t.set_config(t.m1, "测试楼", "101", "106", ["105"])
    expect_status((st, obj), 200, "缩小范围")
    check(obj["schedule"]["removed"] > 0, "应有移出寝室")
    check(obj["schedule"]["retained"] > 0, "应有保留寝室")
    week2 = t.week(t.m1)
    check(week2["progress"]["total"] == 5, "缩小后 N=5，实际 %s" %
          week2["progress"]["total"])
    check(week2["progress"]["checked"] == 1, "保留寝室勾选不变")
    check(t.room(week2, "101")["checked"] is True, "已勾选寝室保留状态")
    for num in ("107", "108", "109", "110", "111", "112"):
        check(num not in [r["room_number"] for r in t.rooms(week2)], "移出寝室不显示: %s" % num)
    pos_after = {r["room_number"]: (d["day_index"], g["period"])
                 for d in week2["days"] for g in d["groups"] for r in g["rooms"]}
    for num in ("101", "102", "103", "104", "106"):
        check(pos_after[num] == positions_before[num], "保留寝室位置不变: %s" % num)
    st, obj, _, _ = t.admin.call("GET", "/api/admin/orphans?account_id=%d" % t.ids["m1"])
    expect_status((st, obj), 200, "孤儿备注列表")
    keys = [item["room_number"] for item in obj["items"]]
    check("112" in keys, "移出寝室备注应保留")
    st, obj, _, _ = t.admin.call("DELETE", "/api/notes/%d" % orphan_note_id)
    expect_status((st, obj), 200, "管理员删除孤儿备注")

    st, obj, _, _ = t.set_config(t.m1, "测试楼", "101", "114", ["105"])
    expect_status((st, obj), 200, "扩大范围")
    check(obj["schedule"]["added"] == 8, "新增数量，实际 %s" % obj["schedule"]["added"])
    week3 = t.week(t.m1)
    check(week3["progress"]["total"] == 13, "扩大后 N=13")
    check(t.room(week3, "101")["checked"] is True, "扩大后勾选保留")
    counts = [sum(len(g["rooms"]) for g in d["groups"]) for d in week3["days"]]
    check(max(counts) - min(counts) <= 2, "新增分配相对均衡: %s" % counts)


def test_isolation_and_admin_views():
    # m2 has no config; make sure it cannot read m1 rooms
    st, obj = t.m2.login("m2", "Manager2#2026")
    expect_status((st, obj), 200, "m2 登录")
    week = t.week(t.m1)
    rid = t.room(week, "101")["id"]
    st, obj, _, _ = t.m2.call("GET", "/api/rooms/%d/notes" % rid)
    expect_status((st, obj), 403, "楼层长数据隔离")
    st, obj, _, _ = t.m2.call("GET", "/api/admin/accounts")
    expect_status((st, obj), 403, "非管理员不能访问管理接口")

    st, obj, _, _ = t.admin.call("GET", "/api/admin/overview")
    expect_status((st, obj), 200, "管理员概览")
    check(obj["totals"]["total"] > 0, "概览统计")
    st, obj, _, _ = t.admin.call("GET", "/api/admin/accounts/%d/week" % t.ids["m1"])
    expect_status((st, obj), 200, "管理员查看周视图")
    check(obj["account"]["username"] == "m1", "管理员周视图账户")

    st, obj, _, _ = t.admin.call("GET", "/api/rooms/%d/notes" % rid)
    expect_status((st, obj), 200, "管理员查看备注")
    st, obj, _, _ = t.admin.call("GET", "/api/admin/logs?action=note_view&page_size=5")
    expect_status((st, obj), 200, "审计日志")
    check(any(i["action"] == "note_view" and i["actor_name"] == "admin"
              for i in obj["items"]), "管理员查看备注必须记录日志")

    # admin modifies check state of a manager room
    target = t.room(t.week(t.m1), "102")
    before = t.week(t.m1)["progress"]["checked"]
    st, obj, _, _ = t.admin.call("POST", "/api/checks/%d/check" % target["id"])
    expect_status((st, obj), 200, "管理员勾选楼层长寝室")
    check(obj["progress"]["checked"] == before + 1, "管理员勾选生效")
    st, obj, _, _ = t.admin.call("POST", "/api/checks/%d/uncheck" % target["id"],
                                 {"confirm": True})
    expect_status((st, obj), 200, "管理员撤销勾选")


def test_disable_enable_delete_restore():
    st, obj, _, _ = t.set_config(t.m2, "二栋", "201", "206", [])
    expect_status((st, obj), 200, "m2 保存配置")
    week = t.week(t.m2)
    check(week["progress"]["total"] == 6, "m2 排班")

    st, obj, _, _ = t.admin.call("POST", "/api/admin/accounts/%d/disable" % t.ids["m2"],
                                 {"confirm": True})
    expect_status((st, obj), 200, "禁用账户")
    check(obj["account"]["status"] == "disabled", "禁用状态")
    st, obj, _, _ = t.m2.call("GET", "/api/week")
    expect_status((st, obj), 401, "禁用后会话失效")
    st, obj = t.m2.login("m2", "Manager2#2026")
    expect_status((st, obj), 403, "禁用后禁止登录")
    check(obj["error"]["code"] == "account_disabled", "禁用登录错误码")
    check(t.server.sql_one("SELECT COUNT(*) c FROM room_check WHERE account_id=? AND week_id IN "
                           "(SELECT id FROM week_plan WHERE account_id=?)",
                           (t.ids["m2"], t.ids["m2"]))["c"] > 0, "禁用保留数据")

    st, obj, _, _ = t.admin.call("POST", "/api/admin/accounts/%d/enable" % t.ids["m2"],
                                 {"confirm": True})
    expect_status((st, obj), 200, "启用账户")
    check(obj["account"]["status"] == "enabled", "启用状态")
    st, obj = t.m2.login("m2", "Manager2#2026")
    expect_status((st, obj), 200, "启用后可登录")
    t.m2.sync()
    check(t.week(t.m2)["progress"]["total"] == 6, "启用后数据完整")

    st, obj, _, _ = t.admin.call("POST", "/api/admin/accounts/%d/delete" % t.ids["m2"],
                                 {"confirm": True})
    expect_status((st, obj), 200, "删除账户")
    acct = obj["account"]
    check(acct["status"] == "deleted", "删除状态")
    check(acct["deleted_at"] and acct["delete_at"], "删除倒计时字段")
    check(acct["remaining_seconds"] and abs(acct["remaining_seconds"] - 72 * 86400) < 10,
          "72 天倒计时")
    check("72" in acct["remaining_text"], "剩余时间文本")
    check(acct["delete_at"] - acct["deleted_at"] == 72 * 86400, "delete_at=+72天")
    st, obj, _, _ = t.m2.call("GET", "/api/week")
    expect_status((st, obj), 401, "删除后会话失效")
    st, obj = t.m2.login("m2", "Manager2#2026")
    expect_status((st, obj), 403, "删除后禁止登录")
    check(obj["error"]["code"] == "account_deleted", "删除登录错误码")
    check(t.server.sql_one("SELECT COUNT(*) c FROM week_plan WHERE account_id=?",
                           (t.ids["m2"],))["c"] == 1, "删除保留数据")

    st, obj, _, _ = t.admin.call("POST", "/api/admin/accounts/%d/restore" % t.ids["m2"],
                                 {"confirm": True})
    expect_status((st, obj), 200, "撤销删除")
    check(obj["account"]["status"] == "enabled", "恢复启用")
    check(obj["account"]["deleted_at"] is None and obj["account"]["delete_at"] is None,
          "清空倒计时字段")
    st, obj = t.m2.login("m2", "Manager2#2026")
    expect_status((st, obj), 200, "恢复后可登录")
    t.m2.sync()
    check(t.week(t.m2)["progress"]["total"] == 6, "恢复后数据完整")

    st, obj, _, _ = t.admin.call("POST", "/api/admin/accounts/%d/restore" % t.ids["m2"],
                                 {"confirm": True})
    expect_status((st, obj), 409, "非删除状态不能撤销")


def test_password_change_session_invalidation():
    c1 = Client(t.server)
    c2 = Client(t.server)
    expect_status(c1.login("m4", "Manager4#2026"), 200, "m4 会话1")
    expect_status(c2.login("m4", "Manager4#2026"), 200, "m4 会话2")
    st, obj, _, _ = c1.call("POST", "/api/password",
                            {"old_password": "Manager4#2026",
                             "new_password": "Manager4New#2026"})
    expect_status((st, obj), 200, "修改密码")
    c1.csrf = obj["csrf_token"]
    st, obj, _, _ = c2.call("GET", "/api/session")
    expect_status((st, obj), 401, "其它会话应失效")
    st, obj, _, _ = c1.call("GET", "/api/session")
    expect_status((st, obj), 200, "当前会话保留")
    c3 = Client(t.server)
    expect_status(c3.login("m4", "Manager4New#2026"), 200, "新密码登录")
    st, obj, _, _ = c1.call("POST", "/api/password",
                            {"old_password": "wrong", "new_password": "x"})
    expect_status((st, obj), 422, "旧密码错误")


def test_lockout_memory_only():
    c = Client(t.server)
    for i in range(4):
        st, obj = c.login("m2", "wrong")
        expect_status((st, obj), 401, "第 %d 次错误密码" % (i + 1))
    st, obj = c.login("m2", "wrong")
    expect_status((st, obj), 423, "第 5 次错误锁定")
    check(obj["error"]["code"] == "account_locked", "锁定错误码")
    check(obj["error"]["retry_after"] >= 590, "锁定剩余时间")
    st, obj = c.login("m2", "Manager2#2026")
    expect_status((st, obj), 423, "锁定期内正确密码也拒绝")
    # other accounts are unaffected
    st, obj = t.m1.login("m1", "Manager1#2026")
    expect_status((st, obj), 200, "锁定按账户独立")
    # lockout lives in memory only: restart clears it
    t.server.restart()
    t.admin.sync()
    t.m1.sync()
    t.m2.sync()
    st, obj = c.login("m2", "Manager2#2026")
    expect_status((st, obj), 200, "重启后解锁")
    # checked state survived restart
    week = t.week(t.m1)
    check(t.room(week, "101")["checked"] is True, "勾选状态重启后保留")


def test_image_count_limit():
    st, obj, _, _ = t.admin.call("POST", "/api/admin/accounts",
                                 {"username": "m5", "password": "Manager5#2026"})
    expect_status((st, obj), 201, "创建 m5")
    t.ids["m5"] = obj["account"]["id"]
    m5 = Client(t.server)
    expect_status(m5.login("m5", "Manager5#2026"), 200, "m5 登录")
    st, obj, _, _ = t.set_config(m5, "三栋", "301", "302", [])
    expect_status((st, obj), 200, "m5 配置")
    week = t.week(m5)
    rid = t.room(week, "301")["id"]
    for i in range(50):
        st, obj, _, _ = m5.upload(rid, filename="p%d.png" % i)
        if st != 201:
            raise AssertionError("第 %d 张上传失败: %s %s" % (i + 1, st, obj))
    st, obj, _, _ = m5.upload(rid, filename="p51.png")
    expect_status((st, obj), 422, "第 51 张图片拒绝")
    check(obj["error"]["code"] == "too_many_images", "图片数量错误码")
    st, obj, _, _ = m5.call("GET", "/api/rooms/%d/images" % rid)
    check(len(obj["images"]) == 50, "图片数量为 50")


def test_backup_and_restore():
    st, obj, _, _ = t.admin.call("POST", "/api/admin/backups")
    expect_status((st, obj), 201, "创建备份")
    backup = obj["backup"]
    t.ids["backup"] = backup["id"]
    backup_dir = os.path.join(t.server.data_dir, "backups", backup["dir_name"])
    check(os.path.isfile(os.path.join(backup_dir, "roomcheck.db")), "备份数据库")
    check(os.path.isdir(os.path.join(backup_dir, "uploads")), "备份图片目录")
    check(os.path.isfile(os.path.join(backup_dir, "manifest.json")), "备份清单")
    st, obj, _, _ = t.admin.call("GET", "/api/admin/backups")
    expect_status((st, obj), 200, "备份列表")
    check(any(x["id"] == backup["id"] for x in obj["backups"]), "备份记录")

    week = t.week(t.m1)
    rid = t.room(week, "101")["id"]
    st, obj, _, _ = t.m1.add_note(rid, "备份之后的备注")
    expect_status((st, obj), 201, "备份后新增备注")
    sentinel = obj["note"]["id"]

    st, obj, _, _ = t.admin.call("POST", "/api/admin/backups/%d/restore" % backup["id"],
                                 {"confirm": True})
    expect_status((st, obj), 200, "恢复备份")
    # all sessions are invalidated
    st, obj, _, _ = t.admin.call("GET", "/api/session")
    expect_status((st, obj), 401, "恢复后会话失效")
    st, obj = t.admin.login("admin", ADMIN_PW)
    expect_status((st, obj), 200, "恢复后重新登录")
    st, obj = t.m1.login("m1", "Manager1#2026")
    expect_status((st, obj), 200, "m1 恢复后登录")
    st, obj, _, _ = t.m1.call("GET", "/api/notes/%d" % sentinel)
    expect_status((st, obj), 404, "恢复后备份后的数据消失")
    check(t.server.sql_one("SELECT COUNT(*) c FROM note WHERE content='备份之后的备注'")["c"] == 0,
          "恢复后备注回滚")


def test_backup_export():
    week = t.week(t.m1)
    room = t.room(week, "101")
    st, obj, _, _ = t.m1.upload(room["id"], filename="export.png")
    expect_status((st, obj), 201, "导出前上传图片")

    st, obj, _, _ = t.admin.call("POST", "/api/admin/backups")
    expect_status((st, obj), 201, "创建待导出备份")
    backup = obj["backup"]
    path = "/api/super-admin/backups/%d/download" % backup["id"]

    anon = Client(t.server)
    st, obj, _, _ = anon.call("GET", path)
    expect_status((st, obj), 401, "未登录不能导出备份")
    adm1 = Client(t.server)
    expect_status(adm1.login("adm1", "Admin1#2026"), 200, "普通管理员导出测试登录")
    st, obj, _, _ = adm1.call("GET", path)
    expect_status((st, obj), 403, "普通管理员不能导出备份")
    st, obj, _, _ = t.m1.call("GET", path)
    expect_status((st, obj), 403, "楼层长不能导出备份")
    st, obj, _, _ = t.admin.call("GET", path)
    expect_status((st, obj), 403, "导出缺少 CSRF Token 应被拒绝")
    check(obj["error"]["code"] == "csrf_invalid", "导出 CSRF 错误码")

    st, obj, headers, body = t.admin.call("GET", path,
                                          headers={"X-CSRF-Token": t.admin.csrf})
    expect_status((st, obj), 200, "超级管理员导出备份")
    check("application/x-tar" in headers.get("Content-Type", ""), "导出内容类型")
    disposition = headers.get("Content-Disposition", "")
    check("attachment" in disposition, "导出附件响应头")
    check(disposition.endswith('.tar"'), "导出文件名为 .tar: %s" % disposition)

    with tarfile.open(fileobj=io.BytesIO(body), mode="r:") as archive:
        members = {m.name: m for m in archive.getmembers()}
        check("roomcheck.db" in members, "导出包含 SQLite 数据库")
        check("manifest.json" in members, "导出包含备份清单")
        manifest = json.loads(archive.extractfile("manifest.json").read().decode("utf-8"))
        db_bytes = archive.extractfile("roomcheck.db").read()
        check(hashlib.sha256(db_bytes).hexdigest() == manifest["database_sha256"],
              "导出数据库与清单校验一致")
        upload_names = sorted(n for n, m in members.items()
                              if n.startswith("uploads/") and m.isfile())
        check(manifest["image_files"] == len(upload_names), "导出图片数量与清单一致")
        check(len(upload_names) > 0, "导出必须包含图片文件")
        tmpdir = tempfile.mkdtemp(prefix="roomcheck-export-")
        try:
            for member in archive.getmembers():
                check(not member.name.startswith("/") and ".." not in member.name.split("/"),
                      "导出路径安全: %s" % member.name)
                archive.extract(member, tmpdir)
            extracted_db = os.path.join(tmpdir, "roomcheck.db")
            conn = sqlite3.connect(extracted_db)
            try:
                check(conn.execute("PRAGMA integrity_check").fetchone()[0] == "ok",
                      "导出数据库可正常识别")
                image_rows = conn.execute("SELECT COUNT(*) FROM note_image").fetchone()[0]
                check(image_rows > 0, "导出数据库包含图片记录")
                rels = [r[0] for r in conn.execute("SELECT rel_path FROM note_image")]
            finally:
                conn.close()
            backup_dir = os.path.join(t.server.data_dir, "backups", backup["dir_name"])
            for rel in rels:
                check(os.path.isfile(os.path.join(tmpdir, "uploads", rel)),
                      "导出文件包含数据库记录的图片: %s" % rel)
                check(os.path.isfile(os.path.join(backup_dir, "uploads", rel)),
                      "服务器备份包含数据库记录的图片: %s" % rel)
        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)

    st, obj, _, _ = t.admin.call("GET", "/api/super-admin/backups/999999/download",
                                 headers={"X-CSRF-Token": t.admin.csrf})
    expect_status((st, obj), 404, "不存在的备份不能导出")

    st, obj, _, _ = t.admin.call("GET", "/uploads/1/1/x.png")
    expect_status((st, obj), 404, "图片目录不能作为静态资源访问")
    st, obj, _, _ = t.admin.call(
        "GET", "/backups/%s/roomcheck.db" % backup["dir_name"])
    expect_status((st, obj), 404, "备份目录不能作为静态资源访问")

    st, obj, _, _ = t.admin.call("POST", "/api/admin/backups")
    expect_status((st, obj), 201, "创建不完整备份")
    broken = obj["backup"]
    broken_dir = os.path.join(t.server.data_dir, "backups", broken["dir_name"])
    removed = False
    for root, _dirs, files in os.walk(os.path.join(broken_dir, "uploads")):
        for name in files:
            os.unlink(os.path.join(root, name))
            removed = True
            break
        if removed:
            break
    check(removed, "不完整备份测试需要至少一个图片文件")
    st, obj, _, _ = t.admin.call(
        "GET", "/api/super-admin/backups/%d/download" % broken["id"],
        headers={"X-CSRF-Token": t.admin.csrf})
    expect_status((st, obj), 500, "不完整备份不能导出")
    check(obj["error"]["code"] == "backup_export_failed", "不完整备份导出错误码")
    with t.server.db() as conn:
        conn.execute("DELETE FROM backup_record WHERE id=?", (broken["id"],))
    shutil.rmtree(broken_dir, ignore_errors=True)

    st, obj, _, _ = t.admin.call("POST", "/api/admin/backups")
    expect_status((st, obj), 201, "创建篡改测试备份")
    tampered = obj["backup"]
    tampered_dir = os.path.join(t.server.data_dir, "backups", tampered["dir_name"])
    with open(os.path.join(tampered_dir, "roomcheck.db"), "r+b") as fh:
        fh.seek(0)
        fh.write(b"corrupted")
    st, obj, _, _ = t.admin.call(
        "GET", "/api/super-admin/backups/%d/download" % tampered["id"],
        headers={"X-CSRF-Token": t.admin.csrf})
    expect_status((st, obj), 500, "被篡改的备份不能导出")
    with t.server.db() as conn:
        conn.execute("DELETE FROM backup_record WHERE id=?", (tampered["id"],))
    shutil.rmtree(tampered_dir, ignore_errors=True)

    st, obj, _, _ = t.admin.call("GET", "/api/admin/logs?action=backup_export")
    expect_status((st, obj), 200, "导出审计日志")
    check(any(i["result"] == "success" for i in obj["items"]), "成功导出写入审计")
    check(any(i["result"] == "failure" for i in obj["items"]), "失败导出写入审计")


def test_session_expiry():
    c = Client(t.server)
    expect_status(c.login("m1", "Manager1#2026"), 200, "会话登录")
    token = None
    for cookie in c.jar:
        if cookie.name == "rc_session":
            token = cookie.value
    check(token, "会话 Cookie")
    token_hash = hashlib.sha256(token.encode()).hexdigest()
    past = int(time.time()) - 10
    with t.server.db() as conn:
        conn.execute("UPDATE session SET expires_at=? WHERE token_hash=?", (past, token_hash))
    st, obj, _, _ = c.call("GET", "/api/session")
    expect_status((st, obj), 401, "过期会话")
    st, obj, _, _ = c.call("GET", "/api/week")
    expect_status((st, obj), 401, "过期访问拒绝")
    t.m1.sync()
    t.admin.sync()


def test_audit_log_retention():
    # insert a log from before this natural week and one from now
    now = int(time.time())
    local = datetime.fromtimestamp(now, tz=timezone(timedelta(hours=8)))
    monday = (local - timedelta(days=local.weekday())).replace(
        hour=0, minute=0, second=0, microsecond=0)
    week_start = int(monday.timestamp())
    with t.server.db() as conn:
        conn.execute("INSERT INTO audit_log(actor_type, actor_id, actor_name, action, "
                     "target_type, target_id, detail, result, ip, created_at) "
                     "VALUES('system',NULL,'test','test_old_log','','','old','success','',?)",
                     (week_start - 3600,))
        conn.execute("INSERT INTO audit_log(actor_type, actor_id, actor_name, action, "
                     "target_type, target_id, detail, result, ip, created_at) "
                     "VALUES('system',NULL,'test','test_new_log','','','new','success','',?)",
                     (now,))
    with t.server.db() as conn:
        conn.execute("DELETE FROM meta WHERE key='last_log_cleanup_week'")
    t.server.restart()
    t.admin.sync()
    deadline = time.time() + 5
    old_count = 1
    while time.time() < deadline:
        old_count = t.server.sql_one(
            "SELECT COUNT(*) c FROM audit_log WHERE action='test_old_log'")["c"]
        if old_count == 0:
            break
        time.sleep(0.2)
    check(old_count == 0, "按自然周清理过期日志")
    check(t.server.sql_one("SELECT COUNT(*) c FROM audit_log WHERE action='test_new_log'")["c"] == 1,
          "本周日志保留")
    check(t.server.sql_one("SELECT COUNT(*) c FROM audit_log WHERE action='log_cleanup'")["c"] >= 1,
          "日志清理审计")


def test_expiry_purge():
    st, obj, _, _ = t.admin.call("POST", "/api/admin/accounts",
                                 {"username": "m3", "password": "Manager3#2026"})
    expect_status((st, obj), 201, "创建 m3")
    m3_id = obj["account"]["id"]
    t.ids["m3"] = m3_id
    m3 = Client(t.server)
    expect_status(m3.login("m3", "Manager3#2026"), 200, "m3 登录")
    st, obj, _, _ = t.set_config(m3, "四栋", "401", "410", [])
    expect_status((st, obj), 200, "m3 配置")
    week = t.week(m3)
    rid = t.room(week, "401")["id"]
    st, obj, _, _ = m3.add_note(rid, "到期删除备注")
    expect_status((st, obj), 201, "m3 备注")
    st, obj, _, _ = m3.upload(rid, filename="m3.png")
    expect_status((st, obj), 201, "m3 图片")
    image_path = t.server.sql_one(
        "SELECT rel_path FROM note_image WHERE account_id=?", (m3_id,))["rel_path"]
    account_dir = os.path.join(t.server.data_dir, "uploads", str(m3_id))
    check(os.path.isdir(account_dir), "m3 图片目录")

    st, obj, _, _ = t.admin.call("POST", "/api/admin/accounts/%d/delete" % m3_id,
                                 {"confirm": True})
    expect_status((st, obj), 200, "删除 m3")
    t.server.stop()
    with t.server.db() as conn:
        conn.execute("UPDATE account SET delete_at=? WHERE id=?", (int(time.time()) - 10, m3_id))
    t.server.start()
    t.admin.sync()
    deadline = time.time() + 8
    remaining = 1
    while time.time() < deadline:
        remaining = t.server.sql_one("SELECT COUNT(*) c FROM account WHERE id=?", (m3_id,))["c"]
        if remaining == 0:
            break
        time.sleep(0.3)
    check(remaining == 0, "72 天期满自动永久删除")
    for table in ("floor_config", "excluded_room", "week_plan", "note", "note_image"):
        count = t.server.sql_one("SELECT COUNT(*) c FROM %s WHERE account_id=?" % table,
                                 (m3_id,))["c"]
        check(count == 0, "%s 关联数据删除" % table)
    check(t.server.sql_one("SELECT COUNT(*) c FROM room_check WHERE account_id=?",
                           (m3_id,))["c"] == 0, "排班记录删除")
    check(t.server.sql_one("SELECT COUNT(*) c FROM deletion_task WHERE account_id=?",
                           (m3_id,))["c"] == 0, "删除任务清理")
    check(not os.path.exists(account_dir), "图片目录删除")
    check(t.server.sql_one("SELECT COUNT(*) c FROM audit_log WHERE action='account_purge' "
                           "AND detail LIKE '%m3%'")["c"] == 1, "永久删除审计保留")
    st, obj, _, _ = t.admin.call("POST", "/api/admin/accounts/%d/restore" % m3_id,
                                 {"confirm": True})
    check(st == 404, "过期后不能撤销删除")

    # idempotent: a second scan must not duplicate the audit entry or corrupt data
    t.server.restart()
    t.admin.sync()
    time.sleep(2.5)
    check(t.server.sql_one("SELECT COUNT(*) c FROM audit_log WHERE action='account_purge' "
                           "AND detail LIKE '%m3%'")["c"] == 1, "重复扫描幂等")


def test_backup_retention():
    for _ in range(4):
        st, obj, _, _ = t.admin.call("POST", "/api/admin/backups")
        expect_status((st, obj), 201, "创建备份")
        time.sleep(0.05)
    st, obj, _, _ = t.admin.call("GET", "/api/admin/backups")
    expect_status((st, obj), 200, "备份列表")
    check(len(obj["backups"]) == 3, "只保留最新 3 份，实际 %d" % len(obj["backups"]))
    dirs = [d for d in os.listdir(os.path.join(t.server.data_dir, "backups"))
            if d.startswith("backup_")]
    check(len(dirs) == 3, "备份目录保留 3 份")


def test_admin_log_filters():
    st, obj, _, _ = t.admin.call("GET", "/api/admin/logs?page=1&page_size=5")
    expect_status((st, obj), 200, "日志列表")
    check(obj["total"] > 0 and len(obj["items"]) <= 5, "分页")
    st, obj, _, _ = t.admin.call("GET", "/api/admin/logs?action=config_update")
    expect_status((st, obj), 200, "按动作筛选")
    check(all(i["action"] == "config_update" for i in obj["items"]), "动作筛选结果")
    st, obj, _, _ = t.admin.call("GET", "/api/admin/logs?from=2020-01-01&to=2030-01-01")
    check(obj["total"] > 0, "日期筛选")

    st, obj, _, _ = t.admin.call("GET", "/api/admin/accounts/%d/week?date=2026-11-03"
                                 % t.ids["m1"])
    expect_status((st, obj), 200, "管理员周视图日期参数")
    st, obj, _, _ = t.admin.call("GET", "/api/admin/overview?date=2026-11-03")
    expect_status((st, obj), 200, "管理员概览日期筛选")
    st, obj, _, _ = t.admin.call("GET", "/api/admin/overview?date=2026-11-05")
    expect_status((st, obj), 422, "周四不参与统计")
    check(obj["error"]["code"] == "thursday_hidden", "周四隐藏错误码")




def test_admin_reset_keeps_floor_manager_data():
    m1_week = t.server.sql_one("SELECT COUNT(*) c FROM week_plan WHERE account_id=?", (t.ids["m1"],))["c"]
    m1_notes = t.server.sql_one("SELECT COUNT(*) c FROM note WHERE account_id=?", (t.ids["m1"],))["c"]
    check(m1_week == 1 and m1_notes >= 0, "重置前 m1 数据")
    t.server.restart(reset_admin=True)
    st, obj, _, _ = t.admin.call("GET", "/api/session")
    expect_status((st, obj), 401, "ADMIN_RESET 应失效旧管理员会话")
    st, obj = t.admin.login("admin", ADMIN_INIT_PW)
    expect_status((st, obj), 200, "ADMIN_RESET 后使用初始化密码登录")
    check(obj["user"]["must_change_password"] is True, "重置后强制改密")
    st, obj, _, _ = t.admin.call("GET", "/api/admin/accounts")
    expect_status((st, obj), 403, "重置后未改密仍被拦截")
    st, obj = t.m1.login("m1", "Manager1#2026")
    expect_status((st, obj), 200, "ADMIN_RESET 不影响楼层长登录")
    check(t.week(t.m1)["progress"]["total"] > 0, "ADMIN_RESET 不修改楼层长排班")
    check(t.server.sql_one("SELECT COUNT(*) c FROM week_plan WHERE account_id=?", (t.ids["m1"],))["c"] == m1_week,
          "ADMIN_RESET 不修改楼层长周计划")
    check(t.server.sql_one("SELECT COUNT(*) c FROM note WHERE account_id=?", (t.ids["m1"],))["c"] == m1_notes,
          "ADMIN_RESET 不修改楼层长备注")
    st, obj, _, _ = t.admin.call("POST", "/api/password",
                                 {"old_password": ADMIN_INIT_PW, "new_password": ADMIN_PW})
    expect_status((st, obj), 200, "重置后恢复管理员密码")
    t.admin.csrf = obj["csrf_token"]


def test_upgrade_to_super_admin():
    s = Server()
    try:
        s.start()
        c = Client(s)
        st, obj = c.login("admin", ADMIN_INIT_PW)
        expect_status((st, obj), 200, "升级前管理员登录")
        check(obj["user"]["role"] == "super_admin", "新安装即为超级管理员")
        st, obj, _, _ = c.call("POST", "/api/password",
                               {"old_password": ADMIN_INIT_PW,
                                "new_password": "Upgrade#2026"})
        expect_status((st, obj), 200, "升级前修改管理员密码")
        c.csrf = obj["csrf_token"]
        st, obj, _, _ = c.call("POST", "/api/admin/accounts",
                               {"username": "legacyfm", "password": "LegacyFm#2026",
                                "display_name": "旧楼层长"})
        expect_status((st, obj), 201, "升级前创建楼层长")
        fid = obj["account"]["id"]
        st, obj, _, _ = c.call("PUT", "/api/config",
                               {"account_id": fid, "building_name": "旧楼",
                                "start_room": "101", "end_room": "104",
                                "excluded_rooms": []})
        expect_status((st, obj), 200, "升级前配置楼层长")
        week_before = s.sql_one("SELECT id FROM week_plan WHERE account_id=?",
                                (fid,))["id"]
        fm = Client(s)
        expect_status(fm.login("legacyfm", "LegacyFm#2026"), 200, "升级前楼层长登录")
        st, week, _, _ = fm.call("GET", "/api/week")
        expect_status((st, week), 200, "升级前楼层长周视图")
        room = [r for d in week["days"] for g in d["groups"] for r in g["rooms"]
                if r["room_number"] == "101"][0]
        st, obj, _, _ = fm.add_note(room["id"], "升级前备注")
        expect_status((st, obj), 201, "升级前备注")
        st, obj, _, _ = fm.upload(room["id"], filename="legacy.png")
        expect_status((st, obj), 201, "升级前图片")
        notes_before = s.sql_one("SELECT COUNT(*) c FROM note WHERE account_id=?",
                                 (fid,))["c"]
        images_before = s.sql_one("SELECT COUNT(*) c FROM note_image WHERE account_id=?",
                                  (fid,))["c"]
        audit_before = s.sql_one("SELECT COUNT(*) c FROM audit_log")["c"]
        check(notes_before >= 1 and images_before >= 1, "升级前备注与图片已写入")
        s.stop()

        with s.db() as conn:
            conn.execute("ALTER TABLE account DROP COLUMN super_admin")
            conn.execute("PRAGMA user_version=1")
        columns = [row["name"] for row in s.sql("PRAGMA table_info(account)")]
        check("super_admin" not in columns, "模拟旧库缺少超级管理员字段")

        s.start()
        st, obj, _, _ = c.call("GET", "/api/session")
        expect_status((st, obj), 401, "升级后旧管理员会话失效")
        st, obj = c.login("admin", "Upgrade#2026")
        expect_status((st, obj), 200, "升级后保留原密码")
        check(obj["user"]["role"] == "super_admin", "升级后管理员成为超级管理员")
        check(obj["user"]["must_change_password"] is False, "升级不强制修改密码")
        c.csrf = obj["csrf_token"]
        columns = [row["name"] for row in s.sql("PRAGMA table_info(account)")]
        check("super_admin" in columns, "升级迁移账户结构")
        check(s.sql_one("SELECT COUNT(*) c FROM account WHERE role='admin'")["c"] == 1,
              "升级不产生重复管理员")
        check(s.sql_one("SELECT COUNT(*) c FROM account WHERE role='admin' "
                        "AND super_admin=1")["c"] == 1, "升级只提升一个超级管理员")
        check(s.sql_one("SELECT COUNT(*) c FROM week_plan WHERE account_id=?",
                        (fid,))["c"] == 1, "升级保留楼层长周计划")
        st, obj, _, _ = c.call("GET", "/api/admin/accounts/%d/week" % fid)
        expect_status((st, obj), 200, "升级后超级管理员可查看楼层长周视图")
        check(obj["week"]["id"] == week_before, "升级保留原周计划 ID")
        fm = Client(s)
        expect_status(fm.login("legacyfm", "LegacyFm#2026"), 200, "升级后楼层长正常登录")
        check(s.sql_one("SELECT COUNT(*) c FROM note WHERE account_id=?",
                        (fid,))["c"] == notes_before, "升级保留备注")
        check(s.sql_one("SELECT COUNT(*) c FROM note_image WHERE account_id=?",
                        (fid,))["c"] == images_before, "升级保留图片记录")
        check(s.sql_one("SELECT COUNT(*) c FROM note WHERE content='升级前备注'")["c"] == 1,
              "升级保留备注内容")
        check(s.sql_one("SELECT COUNT(*) c FROM audit_log")["c"] >= audit_before,
              "升级保留审计记录")
        rel = s.sql_one("SELECT rel_path FROM note_image WHERE account_id=?",
                        (fid,))["rel_path"]
        check(os.path.isfile(os.path.join(s.data_dir, "uploads", rel)),
              "升级保留图片文件")
        st, week, _, _ = fm.call("GET", "/api/week")
        expect_status((st, week), 200, "升级后楼层长周视图")
        new_room = [r for d in week["days"] for g in d["groups"] for r in g["rooms"]
                    if r["room_number"] == "101"][0]
        check(new_room["notes_count"] >= 1 and new_room["images_count"] >= 1,
              "升级后周视图包含原备注与图片")

        s.stop()
        s.start()
        check(s.sql_one("SELECT COUNT(*) c FROM account WHERE role='admin'")["c"] == 1,
              "迁移可重复执行")
        check(s.sql_one("SELECT COUNT(*) c FROM account WHERE role='admin' "
                        "AND super_admin=1")["c"] == 1, "重复迁移不产生额外超级管理员")
        st, obj = c.login("admin", "Upgrade#2026")
        expect_status((st, obj), 200, "重复迁移后正常登录")
        check(obj["user"]["role"] == "super_admin", "重复迁移后角色不变")
    finally:
        s.cleanup()


def test_automatic_daily_backup():
    """启动补偿逻辑：当天 03:00 后启动且无当日备份时自动创建一份。"""
    s2 = Server()
    try:
        s2.start(backup_disabled=False)
        deadline = time.time() + 10
        record = None
        while time.time() < deadline:
            rows = s2.sql("SELECT dir_name, size, created_at FROM backup_record")
            if rows:
                record = rows[0]
                break
            time.sleep(0.25)
        check(record is not None, "启动后应自动创建每日备份")
        backup_dir = os.path.join(s2.data_dir, "backups", record["dir_name"])
        check(os.path.isfile(os.path.join(backup_dir, "roomcheck.db")), "自动备份数据库")
        check(os.path.isdir(os.path.join(backup_dir, "uploads")), "自动备份图片目录")
        check(os.path.isfile(os.path.join(backup_dir, "manifest.json")), "自动备份清单")
        check(record["size"] > 0, "自动备份大小")
        check(s2.sql_one("SELECT COUNT(*) c FROM audit_log WHERE action='backup_success'")["c"] >= 1,
              "自动备份审计")
    finally:
        s2.cleanup()


def test_final_session_ttl():
    ttl = t.ids.get("admin_session_ttl")
    check(ttl is not None, "会话有效期")
    check(abs(ttl - 7 * 86400) <= 120, "Session 有效期为 7 天，实际 %s" % ttl)


TEST_ORDER = [
    test_health_and_static,
    test_unauthenticated_and_csrf,
    test_admin_first_login_gate,
    test_super_admin_role_and_admin_accounts,
    test_account_crud_validation,
    test_super_admin_manages_floor_managers,
    test_manager_login_and_config_validation,
    test_week_date_range_display,
    test_check_and_csrf,
    test_notes_and_limits,
    test_image_security,
    test_weekly_reset_and_regenerate,
    test_incremental_schedule,
    test_isolation_and_admin_views,
    test_disable_enable_delete_restore,
    test_password_change_session_invalidation,
    test_lockout_memory_only,
    test_image_count_limit,
    test_backup_and_restore,
    test_backup_export,
    test_session_expiry,
    test_audit_log_retention,
    test_expiry_purge,
    test_backup_retention,
    test_admin_log_filters,
    test_admin_reset_keeps_floor_manager_data,
    test_upgrade_to_super_admin,
    test_automatic_daily_backup,
    test_final_session_ttl,
]


def main():
    global t
    if not os.path.isfile(BIN):
        sys.stderr.write("未找到 %s，请先执行 make dev\n" % BIN)
        return 2
    os.chmod(BIN, 0o755)
    t = TestContext()
    try:
        t.server.start()
        for fn in TEST_ORDER:
            run_test(fn)
    finally:
        t.server.cleanup()
    print("\n%d passed, %d failed" % (PASS, FAIL))
    if FAILURES:
        print("失败用例：")
        for name, exc in FAILURES:
            print("  - %s: %s" % (name, exc))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
