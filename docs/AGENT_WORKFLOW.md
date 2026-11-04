# Agent 工作流与实现映射（AGENT_WORKFLOW）

RoomCheck 采用“中心编排 + 业务分工”的 Agent 设计。生产环境的业务规则全部由确定性 C 代码执行；Agent 只负责流程编排、权限检查、异常识别和交接，不直接修改 SQLite 或图片文件。HTTP API 是唯一数据写入入口。

## 1. 总体架构

```text
浏览器（HTML/CSS/JS）
      │  HTTP + Session Cookie + CSRF Token
      ▼
C HTTP 服务（backend/src/http.c）
      │  路由 / 鉴权 / 归属校验 / 二次确认参数
      ▼
业务模块（确定性 C 代码）
  auth.c      config.c     schedule.c   week.c
  api_*.c     backup.c     scheduler.c  audit.c
      │
      ├── SQLite（单连接 + 全局递归互斥锁 + 事务）
      └── 本地图片卷 / 备份卷（/data）
```

- 单一进程、单 SQLite 写连接；全局互斥锁串行化所有 API 写操作与后台任务，避免并发竞态。
- 关键写操作使用 `BEGIN IMMEDIATE … COMMIT`；排班/配置/重置在事务内校验失败即回滚。
- 后台调度线程负责 72 天到期清理、每日备份、日志清理、会话清理、孤儿图片目录清理。

## 2. Agent 清单与实现映射

| Agent | 职责 | 实现位置 | 不负责 |
|---|---|---|---|
| Orchestrator Agent | 工作流编排、路由选择、权限阶段化 | `http.c` 路由与 `routes.c` | 不直接写库 |
| Auth Agent | 登录、Session、CSRF、锁定、角色权限、归属校验 | `auth.c`、`api_auth.c`、`http.c` | 不生成排班 |
| Config Agent | 寝室范围、排除列表、N、增量差异 | `config.c`、`api_config.c` | 不直接生成最终排班 |
| Schedule Agent | 初始/增量随机排班、完整性校验 | `schedule.c`、`week.c` | 不负责认证 |
| Check Agent | 勾选、撤销、进度、继续检查 | `api_check.c`、`week.c` | 不改变排班 |
| Note Agent | 备注增删改、500 字限制 | `api_note.c` | 不管理账户 |
| Image Agent | 扩展名/MIME/签名校验、存储、删除 | `api_image.c` | 不决定权限 |
| Lifecycle Agent | 管理员/楼层长禁用、删除倒计时、撤销、重置、到期清理 | `api_admin.c`、`api_week.c`、`scheduler.c` | 不负责日常勾选 |
| Backup Agent | 备份、校验、保留 3 份、恢复/回滚、超级管理员导出 | `backup.c`、`scheduler.c` | 不修改业务规则 |
| Audit Agent | 审计事件写入、按自然周保留 | `audit.c`、`scheduler.c` | 不决定权限 |
| DevOps Agent | 多架构镜像、musl 静态、Tag、发布 | `.github/workflows/`、`Dockerfile`、`scripts/` | 不修改业务规则 |
| QA Agent | 端到端测试、静态检查、验收 | `tests/`、`scripts/check_frontend.py` | 不修改生产数据 |

## 3. 端到端流程

### 流程 A：系统初始化

| 步骤 | 输入 | 动作 | 决策/输出 | 异常 |
|---|---|---|---|---|
| A1 | 启动 | 升级账户角色结构并查询超级管理员 | 旧库补 `super_admin` 列并提升最早管理员；无管理员→创建唯一超级管理员 | 失败停止启动 |
| A2 | 管理员凭据 | 登录 | 密码正确→创建 7 天 Session；响应角色为 `super_admin` | 错误计数 |
| A3 | `must_change_password` | 首次登录 | 强制改密，未改密仅可访问 session/password/logout | 403 `password_change_required` |
| A4 | `ADMIN_RESET=1` | 重置超级管理员密码 | 打印新凭据；不修改楼层长 | 失败停止 |

实现：`auth_ensure_super_admin()`、`auth_ensure_admin()`、`h_login`、`h_password`、`http.c` 门禁。

### 流程 B：认证与权限

`POST /api/login` → 查账户 → 校验锁 → 校验状态 → 验哈希 → 创建 Session → 设置 Cookie。
每个受保护请求：Cookie Token SHA-256 查 `session` → 过期/撤销/状态 → 角色校验（`admin`/`super_admin` → 管理员接口，`super_admin` → 超级管理员接口）→ 归属校验 → 修改类校验 CSRF。
失败处理：连续 5 次错误锁 10 分钟（进程内计数）；禁用/删除账户立即拒绝；越权 403。

### 流程 C：寝室配置

`PUT /api/config`：
1. 解析并校验楼栋、起止、排除列表（范围/重复/N>0）。
2. 事务开始，写入配置与排除表。
3. 构造新寝室集合，与旧集合比较。
4. 首次配置 → 初始排班；集合变化 → 增量排班；无变化 → 仅保存配置。
5. 排班校验失败 → 回滚配置；成功 → 提交并审计。

### 流程 D：范围变化后的增量排班

1. 读取旧 `room_check`（日期、时段、位置、勾选）。
2. 计算保留 / 移出 / 新增集合。
3. 保留寝室日期/时段/勾选不变。
4. 移出寝室产生 `(day, period)` 空位。
5. 新增寝室随机填空位；多余的新增按最轻日期/时段平衡分配。
6. 校验：数量=N、无重复、无遗漏、无周四、保留位置不变。
7. 删除并重写该周 room_check（事务内，勾选按 room_key 保留），提交并审计随机结果。

### 流程 E：主动生成每周排班

`POST /api/schedule/regenerate` 要求 `confirm:true` → 校验配置/N → 六天平均（余数随机） → 每天中午/晚上平均（余数随机） → 校验 → 事务内重建 room_check（清空勾选）→ 审计。
未确认直接 400，不修改数据。

### 流程 F：日常查寝

`GET /api/week` → Session → 本人账户 → 周计划/组/寝室/备注图片计数 + 进度 + 下一未完成组。
`POST /api/checks/:id/check` → 归属 → 条件更新 `checked=1` → 进度 → 审计。
`POST /api/checks/:id/uncheck` → 必须 `confirm:true` → 条件更新 `checked=0` → 审计。
勾选/撤销/备注均不触发排班重算。

### 流程 G：备注与图片

备注：校验内容 → 统计该寝室该周合计字符 → 超 500 拒绝 → 增/改/删 → 审计。
图片：
1. 校验数量（≤50）。
2. 解析 multipart，校验大小（≤10MB）。
3. 扩展名黑名单 → MIME 黑名单 → 文件签名。
4. 写入随机文件名 → 记录 `note_image`（可选 `note_id`）。
5. 失败补偿删除文件；删除时先删文件再删记录。
管理员访问备注/原图分别写 `note_view` / `image_view`。

### 流程 H：周重置

`POST /api/week/reset` 要求 `confirm:true`：
1. 读取配置，生成新周排班（随机，清空勾选）。
2. 事务内创建新周计划/组/room_check。
3. 复制旧周备注与图片记录/文件（仅新周排班中存在的寝室；`note_id` 映射）。
4. 旧周计划删除（级联旧备注/图片/勾选），提交。
5. 删除旧周图片目录；失败写审计警告。
审计包含迁移数量。

### 流程 I：禁用、删除倒计时与到期永久删除

- 禁用：二次确认 → `status=disabled` → 失效 Session → 保留数据 → 审计。
- 删除：二次确认 → `status=deleted`、`deleted_at=now`、`delete_at=now+72d` → 失效 Session → 审计。
- 管理员账户（超级管理员通过 `/api/super-admin/admins`）与楼层长账户（管理员通过 `/api/admin/accounts`）使用相同的禁用、删除、撤销与到期机制；删除管理员不会级联删除楼层长账户。
- 禁用/删除最后一个可用超级管理员：`409 last_super_admin`，不做任何修改。
- 撤销：二次确认 → 条件更新（`status=deleted` 且 `now<delete_at` 且未开始清理）→ 清空倒计时字段 → 审计；否则 409。
- 到期：后台扫描 → 再次条件检查 → 删除图片文件 → 事务内条件删除账户（级联业务数据，排除 `super_admin`）→ 审计 → 删除图片目录；失败写入 `deletion_task` 并重试。
- 撤销与到期由同一全局锁串行化，避免竞态。

### 流程 J：备份与恢复

- 每日 03:00 后检查当日是否已有成功备份；无则使用 `sqlite3_backup` 复制数据库到 `backups/backup_*`，复制图片目录，写 manifest，做 integrity_check，保留最新 3 份。
- 获取失败仅写 `backup_failed` 警告，5 分钟后重试。
- 管理员恢复：二次确认 → 校验备份 DB 完整性与 schema → 创建 `pre_restore_*` 快照 → 关闭连接、覆盖数据库与图片目录 → 重新打开、校验超级管理员、删除全部 Session → 审计；失败自动回滚快照。
- 超级管理员导出：GET `/api/super-admin/backups/:id/download` + `X-CSRF-Token` → 校验备份数据库完整性、manifest SHA-256、图片数量与数据库图片引用 → 生成 `manifest.json + roomcheck.db + uploads/` 的 tar 响应 → 审计；失败返回明确错误且不输出部分文件。
- 恢复与导出期间全局锁阻塞其他 API 写入。

### 流程 K：GitHub Actions 发布

- PR：构建 amd64/arm64 镜像（不推送），amd64 容器冒烟测试；musl 双架构构建与校验。
- main/develop：推送分支 Tag + `sha-<short>`，不更新 latest。
- `vX.Y.Z`：推送 `X.Y.Z`、`X.Y`、`X`、`latest`、`sha-<short>`，创建 GitHub Release。
- `workflow_dispatch`：支持自定义 Tag。
- 所有产物可追溯到 Git SHA。

### 流程 L：Linux musl 静态构建

1. amd64：Debian + `musl-gcc`，编译内置 SQLite amalgamation。
2. arm64：arm64 Alpine 容器内 `gcc -static -no-pie`，或在具备 `aarch64-linux-musl-gcc` 的环境交叉编译。
3. 校验 ELF 架构、`INTERP`、`NEEDED`、`ldd`、可执行性。
4. 生成 `dist/linux-{amd64,arm64}-musl/RoomCheck`、SHA256SUMS、构建信息。
5. 上传 Actions Artifact；版本 Tag 创建 Release。

## 4. 二次确认矩阵

| 操作 | 前端确认 | 服务端强制 |
|---|---|---|
| 撤销勾选 | 是 | `confirm:true` |
| 主动重新生成排班 | 是 | `confirm:true` |
| 重置本周 / 开始新一周 | 是 | `confirm:true` |
| 禁用账户 | 是 | `confirm:true` |
| 删除账户 | 是 | `confirm:true` |
| 撤销删除 | 是 | `confirm:true` |
| 恢复备份 | 是 | `confirm:true` |
| 备份导出 | 否（直接下载） | 超级管理员 + `X-CSRF-Token` |
| 删除备注/图片 | 是 | 资源归属 + CSRF（单条删除） |

## 5. 审计事件

`login_success`、`login_failed`、`login_locked`、`logout`、`password_change`、`username_change`、`account_create`、`account_update`、`account_enable`、`account_disable`、`account_delete`、`account_restore`、`account_purge`、`config_update`、`schedule_generate`、`schedule_regenerate`、`week_reset`、`check_toggle`、`note_create`、`note_update`、`note_delete`、`note_view`、`image_upload`、`image_delete`、`image_view`、`backup_success`、`backup_failed`、`backup_restore`、`backup_export`、`log_cleanup`、`deletion_retry_failed`、`upload_cleanup`。

- 每条记录操作者类型/ID/名称、动作、目标、结果、IP、时间。
- 不记录密码、Token、备注正文、图片内容。
- 管理员查看原图仅记录“查看原图”动作。

## 6. 异常处理与质量门禁

| 异常 | 处理 |
|---|---|
| 登录错误 5 次 | 锁 10 分钟，前端显示剩余时间 |
| Session 过期 | 401，前端回登录页 |
| 越权 | 403，记录审计失败 |
| N≤0 / 排除越界 / 重复 | 422，不保存 |
| 排班重复/遗漏/周四 | 禁止保存并回滚事务 |
| 备注 >500 / 图片 >50 / >10MB / 黑名单 | 422/413，不写库 |
| 上传写入或 DB 失败 | 清理孤儿文件，返回错误 |
| 删除文件失败 | 不删除数据库记录，返回错误 |
| 重置迁移失败 | 回滚事务并清理新周目录 |
| 永久删除失败 | 记录错误、保留数据、定时重试 |
| 备份失败 | WARNING，5 分钟后重试 |
| 恢复失败 | 自动回滚快照并记录失败 |
| 日志写入失败（关键删除） | 回滚关键事务 |
| musl 静态校验失败 | 阻断产物上传/发布 |

## 7. 人工复核

- 首次启动输出超级管理员凭据并强制改密；旧版本数据库升级后原管理员账户可直接登录且角色为超级管理员。
- 普通管理员看不到管理员账户管理与备份导出入口，直接请求对应 API 返回 403。
- 超级管理员连续尝试禁用/删除自己（最后一个可用超级管理员）均被拒绝，会话保持有效。
- 旧范围 201–233 → 新范围 201–220：保留寝室位置不变、移出寝室消失、备注保留。
- 反向扩展 201–220 → 201–233：新增寝室优先填空位。
- 新增数量大于/小于空位数量。
- 原有寝室已勾选、已有多条备注时修改范围。
- 删除账户第 1 天、第 71 天 23 小时可撤销；恰好到 `delete_at` 拒绝。
- 到期清理重复运行不重复删除。
- 备份恢复后记录与图片一致，全部会话失效。
