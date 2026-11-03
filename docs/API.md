# RoomCheck HTTP API 契约（v1）

后端：C / SQLite。所有请求与响应均为 UTF-8。
前端页面由同一服务提供：`GET /` 返回 `frontend/index.html`，静态资源位于 `/css/*`、`/js/*`、`/assets/*`。

## 1. 通用约定

### 1.1 认证

- 登录成功后服务端设置 Cookie：`rc_session=<token>; HttpOnly; SameSite=Lax; Path=/; Max-Age=604800`
- 所有业务 API 需要有效 Session。
- 所有**修改类请求**（POST/PATCH/PUT/DELETE）必须带请求头：
  `X-CSRF-Token: <csrf_token>`
  `csrf_token` 由 `/api/login` 与 `/api/session` 返回，保存在前端内存/localStorage。
- Session 有效期 7 天。`/api/logout` 后失效。
- 管理员首次登录 `must_change_password=true` 时，除下列接口外全部返回
  `403 password_change_required`：
  `GET /api/session`、`POST /api/password`、`POST /api/logout`、`GET /api/health`。

### 1.2 响应格式

成功：HTTP 200/201，JSON 对象（结构见各接口）。
失败：

```json
{ "error": { "code": "invalid_credentials", "message": "账户名或密码错误" } }
```

状态码：`400` 参数错误、`401` 未登录/会话过期、`403` 权限不足或被锁定策略拒绝、`404` 不存在、`409` 冲突（重复、状态冲突）、`413` 文件过大、`422` 业务规则拒绝、`423` 账户锁定、`500` 服务器错误、`503` 维护中。

### 1.3 时间格式

- 所有时间字段除特别说明外，均为 **Unix 秒（整数，UTC 时间戳）**。
- 展示用日期字段（`date`、`week_start`、`week_end`、`checked_at_text` 等）为 `Asia/Shanghai` 的 `YYYY-MM-DD` 或 `YYYY-MM-DD HH:MM:SS` 字符串。

### 1.4 通用对象

**User**

```json
{ "id": 2, "role": "floor_manager", "username": "f1", "display_name": "一层",
  "status": "enabled", "must_change_password": false, "created_at": 1762000000 }
```

`role`: `admin` | `floor_manager`；`status`: `enabled` | `disabled` | `deleted`。
管理员账户管理接口返回的账户对象同样包含 `role` 字段（值固定为 `floor_manager`）。

**Progress**

```json
{ "checked": 3, "total": 33, "percent": 9, "completed": false }
```

`percent` 为 0-100 向下取整。

**Config**

```json
{ "building_name": "秀苑四舍", "start_room": "634", "end_room": "665",
  "start_num": 634, "end_num": 665, "original_count": 32,
  "excluded_count": 2, "excluded_rooms": ["635", "660"], "N": 30,
  "updated_at": 1762000000 }
```

**Room**（`/api/week` 内）

```json
{ "id": 12, "room_key": "秀苑四舍#634", "room_number": "634", "checked": true,
  "checked_at": 1762000000, "notes_count": 2, "notes_chars": 31,
  "images_count": 1, "note_preview": "灯管坏了" }
```

**Group**

```json
{ "id": 3, "period": "noon", "label": "中午", "checked": 1, "total": 3,
  "rooms": [ /* Room */ ] }
```

**Day**

```json
{ "day_index": 0, "date": "2026-11-02", "weekday": "周一", "is_today": true,
  "groups": [ /* Group */ ] }
```

`day_index`：0=周一，1=周二，2=周三，3=周四（**不返回**），4=周五，5=周六，6=周日。
`period`：`noon`（中午）| `evening`（晚上）。

**WeekView**（`GET /api/week`，管理员用 `GET /api/admin/accounts/:id/week` 结构相同，另带 `account` 字段）

```json
{
  "account": { /* User */ },
  "week": { "id": 7, "week_start": "2026-11-02", "week_end": "2026-11-08",
            "iso_year": 2026, "iso_week": 45, "label": "2026 年第 45 周",
            "created_at": 1762000000, "plan_type": "initial" },
  "config": { /* Config，可为 null */ },
  "progress": { "checked": 3, "total": 30, "percent": 10, "completed": false },
  "days": [ /* Day，6 项，周一/二/三/五/六/日 */ ],
  "next_group_id": 5,
  "next_room_id": 21
}
```

`next_group_id` / `next_room_id`：第一个未完成组的 id 与该组第一个未勾选寝室 id；全部完成时为 `null`。

**AccountSummary**（管理员账户列表）

```json
{ "id": 2, "username": "f1", "display_name": "一层", "status": "enabled",
  "deleted_at": null, "delete_at": null, "remaining_seconds": null,
  "remaining_text": null, "created_at": 1762000000, "last_login_at": 1762000000,
  "config": { /* Config 或 null */ },
  "progress": { /* Progress 或 null */ },
  "week": { "week_start": "2026-11-02", "week_end": "2026-11-08", "label": "2026 年第 45 周" } }
```

`remaining_text` 形如 `71天23小时`；`status=deleted` 时 `remaining_seconds/remaining_text` 非空。

**AuditLog**

```json
{ "id": 8, "actor_type": "admin", "actor_id": 1, "actor_name": "admin",
  "action": "account_delete", "target_type": "account", "target_id": "2",
  "detail": "删除楼层长 f1，72 天后永久删除", "result": "success",
  "ip": "1.2.3.4", "created_at": 1762000000, "created_at_text": "2026-11-03 19:00:00" }
```

**Backup**

```json
{ "id": 1, "dir_name": "backup_20261103_030000", "created_at": 1762000000,
  "created_at_text": "2026-11-03 03:00:00", "size": 123456,
  "size_text": "120.6 KB", "status": "success" }
```

---

## 2. 认证与个人账户

### `GET /api/health`
无需认证。`{ "ok": true, "version": "1.0.0", "time": 1762000000 }`

### `POST /api/login`
```json
{ "username": "admin", "password": "..." }
```
200：
```json
{ "user": { /* User */ }, "csrf_token": "hex", "expires_at": 1762600000 }
```
失败：`401 invalid_credentials`；`423 account_locked`（`message` 含剩余分钟，另返回 `retry_after` 秒）；`403 account_disabled` / `403 account_deleted`。

### `GET /api/session`
200：`{ "user": {...}, "csrf_token": "...", "expires_at": 1762600000 }`
未登录：401。

### `POST /api/logout`
200：`{ "ok": true }`

### `POST /api/password`
```json
{ "old_password": "...", "new_password": "..." }
```
200：`{ "ok": true, "csrf_token": "..." }`（修改后其它会话失效，当前会话保留，CSRF Token 轮换）
错误：`422 invalid_password`（旧密码错误）。

### `PATCH /api/profile`
楼层长修改本人账户名 / 显示名；管理员也可修改本人显示名。
```json
{ "username": "new_name", "display_name": "新名字" }
```
两字段均可选。200：`{ "user": {...} }`。
错误：`409 username_taken`。

---

## 3. 寝室配置

### `GET /api/config`
楼层长读取本人配置。管理员查询时需带 `?account_id=<楼层长ID>`。
200：`{ "config": { /* Config 或 null */ } }`

### `PUT /api/config`
楼层长修改本人配置；管理员可额外传 `account_id` 修改指定楼层长配置。
```json
{ "account_id": 2,
  "building_name": "秀苑四舍",
  "start_room": "634",
  "end_room": "665",
  "excluded_rooms": ["635", "660"] }
```
200：
```json
{ "config": { /* Config */ },
  "schedule": { "added": 0, "removed": 2, "retained": 28, "N": 30,
                "week_updated": true, "plan_type": "incremental" } }
```
业务错误：`422 invalid_range` / `422 room_out_of_range` / `422 duplicate_excluded` / `422 N_zero` / `422 no_op`（配置完全未变，允许不报错也可，前端按成功处理）。

---

## 4. 本周查寝

### `GET /api/week`
200：`{ /* WeekView */ }`。无配置或 N=0 时 `week` 为 null，`config` 可能为 null，`days: []`。

### `POST /api/checks/:id/check`
`:id` 为 Room id。无需请求体。200：
```json
{ "room": { /* Room */ }, "progress": { /* Progress */ }, "next_room_id": 22 }
```

### `POST /api/checks/:id/uncheck`
必须二次确认，前端先弹确认框，确认后发送：
```json
{ "confirm": true }
```
未确认：`400 confirm_required`。200：`{ "room": {...}, "progress": {...} }`。

### `POST /api/schedule/regenerate`
主动重新生成本周排班（清空本周勾选，保留备注与图片）。二次确认：
```json
{ "confirm": true }
```
200：`{ /* WeekView */ }`。

### `POST /api/week/reset`
开始新一周（重置）：清空排班与勾选，保留范围与无需查寝列表，备注和图片迁移到新周（仅新周排班存在的寝室保留，其余删除），旧周图片文件删除。二次确认：
```json
{ "confirm": true }
```
200：`{ /* WeekView */ }`。

---

## 5. 备注

`:id` 为 Room id（管理员同样可用，操作会记录审计）。

### `GET /api/rooms/:id/notes`
```json
{ "notes": [ { "id": 3, "content": "灯管坏了", "created_at": 1762000000,
               "updated_at": 1762000000, "created_at_text": "2026-11-03 19:00:00" } ],
  "total_chars": 4, "limit": 500 }
```

### `POST /api/rooms/:id/notes`
```json
{ "content": "灯管坏了" }
```
201：`{ "note": {...}, "total_chars": 4 }`。超 500 字：`422 note_limit`。

### `PATCH /api/notes/:id`
```json
{ "content": "已更换灯管" }
```
200：`{ "note": {...}, "total_chars": 5 }`。

### `DELETE /api/notes/:id`
200：`{ "ok": true, "deleted_images": 2, "total_chars": 0 }`（关联图片与其文件一并删除）。

---

## 6. 图片

### `GET /api/rooms/:id/images`
```json
{ "images": [ { "id": 5, "original_name": "IMG_0001.jpg", "mime": "image/jpeg",
                "size": 82211, "size_text": "80.3 KB", "created_at": 1762000000,
                "url": "/api/images/5/file", "note_id": null } ],
  "limit": 50 }
```

### `POST /api/rooms/:id/images`
`Content-Type: multipart/form-data`，字段名 `file`（也接受 `image`），一个文件。
可选字段 `note_id`：若提供，则必须属于同一寝室且同一周；图片会关联该备注，删除备注时级联删除。
- 单文件 ≤ 10MB，每寝室最多 50 张。
- 服务端校验扩展名、MIME、文件签名，命中黑名单则拒绝。
201：`{ "image": {...} }`。
错误：`413 file_too_large`、`422 too_many_images`、`422 forbidden_file_type`、`400 no_file`。

### `GET /api/images/:id/file`
返回原图二进制（`Content-Type` 为存储 MIME）。管理员访问会记录“查看原图”审计。

### `DELETE /api/images/:id`
200：`{ "ok": true }`。

---

## 7. 管理员

以下接口仅管理员可用（否则 403）。

### `GET /api/admin/accounts`
200：`{ "accounts": [ /* AccountSummary，包含已删除账户 */ ] }`

### `POST /api/admin/accounts`
```json
{ "username": "f1", "password": "123456", "display_name": "一层" }
```
201：`{ "account": { /* User */ } }`。重复：`409 username_taken`。

### `PATCH /api/admin/accounts/:id`
```json
{ "username": "f1b", "password": "newpass", "display_name": "一层" }
```
200：`{ "account": {...} }`。管理员不可被此接口修改（除自身密码走 `/api/password`）。

### `POST /api/admin/accounts/:id/enable`
200：`{ "account": {...} }`。状态 `disabled → enabled`。

### `POST /api/admin/accounts/:id/disable`
二次确认 `{ "confirm": true }`。200：`{ "account": {...} }`。立即失效该账户全部 Session。

### `POST /api/admin/accounts/:id/delete`
二次确认 `{ "confirm": true }`。200：
```json
{ "account": { "id": 2, "status": "deleted", "deleted_at": 1762000000,
               "delete_at": 1768220800, "remaining_seconds": 6220800,
               "remaining_text": "72天0小时" } }
```
立即禁止登录并失效 Session，72 天后自动永久删除。

### `POST /api/admin/accounts/:id/restore`
撤销删除，二次确认 `{ "confirm": true }`。200：`{ "account": {...} }`。
条件：`status=deleted` 且当前时间 < `delete_at` 且未开始永久删除。否则 `409 restore_not_allowed`。

### `GET /api/admin/accounts/:id/week?date=YYYY-MM-DD`
管理员查看指定楼层长周视图（结构同 WeekView，带 `account`）。`date` 为可选参数；服务端返回完整一周，前端按日期筛选面板。

### `GET /api/admin/overview?date=YYYY-MM-DD`
200：
```json
{ "date": "2026-11-03", "totals": { "accounts": 3, "checked": 12, "total": 90, "percent": 13 },
  "accounts": [ /* AccountSummary；progress 按 date 统计，省略 date 时按本周统计 */ ] }
```
（概览只返回账户级进度；点击账户进入 `.../week` 查看每日面板。）

### `GET /api/admin/logs?page=1&page_size=50&account_id=&action=&from=YYYY-MM-DD&to=YYYY-MM-DD`
200：
```json
{ "total": 120, "page": 1, "page_size": 50, "items": [ /* AuditLog */ ],
  "actions": ["login_success", "config_update", "..."] }
```

### `GET /api/admin/backups`
200：`{ "backups": [ /* Backup */ ] }`

### `POST /api/admin/backups`
立即备份。201：`{ "backup": { /* Backup */ } }`。

### `POST /api/admin/backups/:id/restore`
二次确认 `{ "confirm": true }`。200：
```json
{ "ok": true, "message": "恢复完成，请重新登录" }
```
恢复会覆盖数据库与图片并失效全部 Session；当前前端必须清除本地状态并回到登录页。

### `GET /api/admin/orphans?account_id=2`
查看被移出范围、但备注/图片仍保留的寝室。列表中的备注可用 `DELETE /api/notes/:id` 删除，图片可用 `DELETE /api/images/:id` 删除（均需 CSRF）。
```json
{ "items": [ { "room_key": "秀苑四舍#640", "room_number": "640",
               "notes": [ /* Note */ ], "images": [ /* Image */ ] } ] }
```

---

## 8. 审计动作名（action）

`login_success`、`login_failed`、`login_locked`、`logout`、`password_change`、`username_change`、
`account_create`、`account_update`、`account_enable`、`account_disable`、`account_delete`、
`account_restore`、`account_purge`、`config_update`、`schedule_generate`、`schedule_regenerate`、
`week_reset`、`check_toggle`、`note_create`、`note_update`、`note_delete`、`note_view`、
`image_upload`、`image_delete`、`image_view`、`backup_success`、`backup_failed`、`backup_restore`、
`log_cleanup`、`deletion_retry_failed`、`upload_cleanup`。

`result`: `success` | `failure` | `warning`。

## 9. 静态页面路由

- `GET /` → `index.html`
- `GET /login`、`GET /admin`、`GET /manager` → `index.html`（SPA 回退）
- `GET /css/*`、`GET /js/*`、`GET /assets/*` → 静态资源
- 其它 → 404

## 10. 前端约定

- `localStorage.rc_theme`：`auto` | `light` | `dark`。
- `localStorage.rc_csrf`：CSRF Token；登录/会话刷新时更新。
- 所有修改操作在 UI 上需要二次确认：撤销勾选、重新生成、重置本周、禁用、删除、撤销删除、恢复备份。
- 登录后根据 `user.role` 渲染：`floor_manager` → 查寝页；`admin` → 管理概览。
- `must_change_password=true` 时强制展示改密弹窗，不可关闭，成功后重新进入。
- 响应头启用严格 CSP（`script-src 'self'`），因此不能使用内联 `<script>`、`onclick` 等内联事件或 `javascript:` URL；所有脚本必须位于 `/js/*.js`。
