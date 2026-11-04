# 需求规格映射（REQUIREMENTS）

本文以《工作指导.md》V1.0 为唯一业务依据，记录需求到实现与验证的映射。凡工作指导已冻结的事项均按原文实现；工作指导第 43 节列出的工程细节按 [ENGINEERING_DECISIONS.md](ENGINEERING_DECISIONS.md) 记录。

## 1. 账户与权限

| 需求 | 实现 | 验证 |
|---|---|---|
| 三种角色：超级管理员 / 管理员 / 楼层长 | `account.role` 保存基础角色；`super_admin=1` 的管理员在 API、Session 与审计中呈现为 `super_admin` | `test_super_admin_role_and_admin_accounts` |
| 初始化管理员迁移为超级管理员 | `auth_ensure_super_admin()` 提升最早的管理员，保留 ID/用户名/密码哈希；不重建账户表；旧管理员 Session 失效 | `test_upgrade_to_super_admin` |
| 超级管理员初始化、凭据仅首次输出 | `auth_ensure_admin()` 首次/`ADMIN_RESET=1` 时生成随机密码并打印；数据库仅保存哈希 | 手工启动日志、`test_admin_first_login_gate` |
| `ADMIN_RESET=1` 重置超级管理员且不影响楼层长数据 | 仅更新超级管理员密码/强制改密并失效其会话 | `test_admin_reset_keeps_floor_manager_data` |
| 管理员首次登录强制改密 | `account.must_change_password=1`；HTTP 层拦截除 session/password/logout 外的接口 | `test_admin_first_login_gate` |
| 楼层长首次登录不强制改密 | 创建账户时 `must_change_password=0` | `test_manager_login_and_config_validation` |
| 账户名全局唯一（数据库级） | `account.username_norm UNIQUE` 唯一索引；创建/改名均先检查（管理员与楼层长共用） | `test_account_crud_validation`、`test_super_admin_role_and_admin_accounts` |
| 加盐密码哈希 | PBKDF2-HMAC-SHA256，120000 次迭代，16 字节盐 | `test_admin_first_login_gate`、`--selftest` |
| 无额外密码复杂度 | 仅限制非空与长度上限 | 代码审查 |
| 超级管理员创建/修改/修改密码/启用/禁用/删除管理员 | `/api/super-admin/admins` 系列接口，服务端固定角色为 `admin` | `test_super_admin_role_and_admin_accounts` |
| 普通管理员不能管理管理员、不能访问超级管理员接口 | 路由层 `RF_SUPER_ADMIN` + 目标账户角色校验；忽略请求体角色参数 | `test_super_admin_role_and_admin_accounts` |
| 超级管理员可管理楼层长 | 复用 `/api/admin/accounts` 系列接口 | `test_super_admin_manages_floor_managers` |
| 管理员可创建/修改/启用/禁用/删除/撤销删除楼层长 | `/api/admin/accounts` 系列接口 | `test_account_crud_validation`、`test_disable_enable_delete_restore` |
| 楼层长只能操作本人数据 | 所有资源查询校验 `account_id`；管理员可跨账户 | `test_isolation_and_admin_views` |
| Session 7 天、含账户类型/ID/过期/CSRF | `session` 表：role、account_id、csrf_token、expires_at；Cookie 7 天 | `test_final_session_ttl` |
| 登出使 Session 失效 | 删除 session 行并清除 Cookie | `test_unauthenticated_and_csrf`、手工 |
| 连续 5 次错误锁 10 分钟、仅内存 | 进程内锁表，按账户名计数；不写 SQLite；重启清零 | `test_lockout_memory_only` |
| 管理员与楼层长分别计数 | 账户名全局唯一，锁按账户名独立计数（见工程决策） | `test_lockout_memory_only` |
| 禁用账户禁止登录、保留数据、可重新启用 | `status=disabled`；校验状态；不删除业务数据；启用清锁；立即失效全部会话 | `test_disable_enable_delete_restore`、`test_super_admin_role_and_admin_accounts` |
| 删除账户 72 天倒计时、禁止登录、保留数据 | `status=deleted`、`deleted_at`、`delete_at=deleted_at+72*86400`；撤销需严格早于 `delete_at` | `test_disable_enable_delete_restore` |
| 管理员删除与撤销删除 | 与楼层长相同的 72 天保留机制；不级联删除楼层长账户 | `test_super_admin_role_and_admin_accounts` |
| 最后一个可用超级管理员不能被禁用/删除 | 禁用/删除前统计其余启用超级管理员，否则 `409 last_super_admin` | `test_super_admin_role_and_admin_accounts` |
| 到期自动永久删除、幂等、失败重试 | 后台每 2 秒扫描；条件事务删除普通管理员与楼层长；`deletion_task` 记录失败并重试；超级管理员不参与永久删除 | `test_expiry_purge` |
| 撤销删除与到期清理并发仲裁 | 单进程全局递归互斥锁串行化写操作 + 条件 UPDATE/DELETE | `test_expiry_purge`、代码审查 |
| 删除范围：配置/排除/周/组/勾选/备注/图片/文件 | 外键级联 + 显式图片文件清理 + 账户上传目录清理 | `test_expiry_purge` |

## 2. 寝室配置与 N

| 需求 | 实现 | 验证 |
|---|---|---|
| 楼栋名称 + 寝室编号，room_key | `room_key = building_name + "#" + room_number` | `test_manager_login_and_config_validation` |
| 不同楼栋允许相同数字 | 楼栋参与 room_key | 代码审查 |
| 原始总数包含两端 | `end_num - start_num + 1` | 测试 N 值 |
| N = 原始总数 - 无需查寝数 | 服务端计算；N≤0 拒绝保存 | `test_manager_login_and_config_validation` |
| 无需查寝必须位于范围、不重复 | `room_number_canonical` 校验并在事务内重写列表 | `test_manager_login_and_config_validation` |
| 无需查寝不出现在抽查列表 | 构造寝室集合时排除 | 同上 |
| 范围修改保留/移出/新增 | `week_rebuild()` 比较旧排班与新寝室集合，增量写入 | `test_incremental_schedule` |
| 保留寝室排班不变 | 增量算法固定 day/period，校验器再次校验 | `test_incremental_schedule` |
| 新增优先填空位、余数平均、随机 | 空位随机匹配；无空位选最轻日期/时段 | `test_incremental_schedule` |
| 校验通过后保存 | 每次写入前 `schedule_validate()`；失败回滚事务 | 测试 + 代码审查 |

## 3. 排班与查寝

| 需求 | 实现 | 验证 |
|---|---|---|
| 自然周周一至周日，Asia/Shanghai | 固定 UTC+8 时间函数；自然周计算 | 测试日期输出 |
| 周标题显示真实日期范围 | `week_start` / `week_end` 由 `rc_fmt_week_range()` 生成 `x月x日 — y月y日`；前端查寝页、管理员周视图与账户列表统一使用 | `test_week_date_range_display` |
| 周四完全隐藏 | 只创建 6 个查寝日；周四无 group、无 room_check；校验拒绝周四 | `test_manager_login_and_config_validation` |
| 每天中午/晚上两组 | `check_group` 12 行；`days[].groups` | 周视图测试 |
| 六天平均、余数随机 | 初次排班先分六天，再分时段；余数随机 | 天数/组数差值断言 |
| 每组数量不固定为 3 | 平均分配，无固定大小 | N=2/11/13 等测试 |
| 固定排班，勾选/撤销/备注不重排 | 勾选仅更新 `checked`；备注独立表 | `test_check_and_csrf`、`test_weekly_reset_and_regenerate` |
| 主动重新生成二次确认并清空勾选 | `/api/schedule/regenerate` 要求 `confirm:true`，删除并重建 room_check | `test_weekly_reset_and_regenerate` |
| 勾选持久化（刷新/重启/跨周） | SQLite `room_check.checked` | 重启测试 `test_lockout_memory_only` |
| 撤销必须二次确认 | 服务端校验 `confirm:true` | `test_check_and_csrf` |
| 进度、百分比、已查 X/N | `progress_json`；周视图聚合 | 多次测试 |
| 继续检查定位第一个未勾选组 | `next_group_id` / `next_room_id` | 前端交互 + API 返回 |
| 今天高亮 | `is_today` | 前端渲染 + 截图 |
| 跨周未查完保留 | 不自动创建新周；仅手动重置 | 代码审查 |

## 4. 备注与图片

| 需求 | 实现 | 验证 |
|---|---|---|
| 每间寝室多条备注，可增改删 | `/api/rooms/:id/notes`、`/api/notes/:id` | `test_notes_and_limits` |
| 单寝室备注合计 ≤500 字 | 新建/修改时按 UTF-8 字符统计；超限 422 | `test_notes_and_limits` |
| 备注标识 | 周视图 `notes_count`、`note_preview` | 前端卡片 |
| 每间寝室 ≤50 张图片 | 上传前计数 | `test_image_count_limit` |
| 单图 ≤10MB | 请求体/文件长度校验 | `test_image_security` |
| 原文件存储、Docker 卷 | `uploads/<account>/<week>/<random>.<ext>`，容器 `/data` 卷 | 测试文件路径 |
| 扩展名 + MIME + 签名三层黑名单 | 扩展名表、MIME 表、magic 检查（ELF/PE/Mach-O/ZIP/脚本/HTML/SVG 等） | `test_image_security` |
| 上传仅文件选择/相册 | 原生 `<input type=file accept=image/*>` | 前端代码 |
| 删除备注级联关联图片 | 上传可带 `note_id`；删除备注先删文件再删记录 | `test_notes_and_limits` |
| 管理员查看备注/原图审计 | `note_view`、`image_view`（原图仅记录动作） | `test_isolation_and_admin_views` |

## 5. 周重置与生命周期

| 需求 | 实现 | 验证 |
|---|---|---|
| 重置二次确认 | `/api/week/reset` 要求 `confirm:true` | `test_weekly_reset_and_regenerate` |
| 清空本周排班/勾选 | 新建周计划并重建 room_check | 同上 |
| 保留范围与无需查寝 | 配置表不动 | 同上 |
| 备注图片复制到新周、仅新周寝室保留 | 事务内复制 note/note_image 及文件；旧周记录级联删除 | 同上 |
| 旧周图片文件删除 | 提交后删除旧周目录 | 同上（目录不存在断言） |
| 数据库记录与图片一致 | 先文件后记录或回滚清理；旧目录清理失败写审计警告 | 代码审查 + 测试 |
| 72 天倒计时字段 | `deleted_at` / `delete_at` | `test_disable_enable_delete_restore` |
| 撤销删除清空倒计时 | 条件更新 + 清空字段；不恢复旧 Session | 同上 |
| 到期永久删除不可恢复 | 删除账户行 + 数据 + 文件；撤销 404 | `test_expiry_purge` |

## 6. 备份与审计

| 需求 | 实现 | 验证 |
|---|---|---|
| 每日备份、保留 3 份 | 03:00 上海边界 + 启动补偿；`prune_backups(3)` | 手工 + `test_backup_retention` |
| 备份内容覆盖 DB + 图片 | `roomcheck.db`、`uploads/`、`manifest.json` | `test_backup_and_restore` |
| 备份失败只写 WARNING | `backup_failed` 审计 warning；失败 5 分钟后重试 | 代码审查 |
| 管理员主动恢复、二次确认 | `/api/admin/backups/:id/restore` | `test_backup_and_restore` |
| 恢复前校验 + 快照 + 失败回滚 | integrity_check + schema 检查；`pre_restore_*` 快照；自动回滚 | 同上 |
| 超级管理员导出备份到本地 | `/api/super-admin/backups/:id/download` 校验备份完整性后输出 `manifest.json + roomcheck.db + uploads/` 的 `.tar` | `test_backup_export` |
| 导出接口权限与 CSRF | 仅 `super_admin`；GET 同样校验 `X-CSRF-Token`；失败返回明确错误且不输出部分文件 | `test_backup_export` |
| 备份/图片/数据库不作为静态资源暴露 | 静态服务只允许 SPA、`/css/`、`/js/`、`/assets/` | `test_backup_export` |
| 恢复防并发写入 | 全局 DB 锁串行化 | 代码审查 |
| 操作日志覆盖规定动作 | 登录、账户、配置、随机排班、勾选、备注、图片、重置、恢复、导出、永久删除 | `test_admin_log_filters`、`test_backup_export`、多次断言 |
| 日志保留一个自然周 | 周一 00:00 边界删除更早日志 | `test_audit_log_retention` |
| 日志不含密码/Token | 审计内容仅动作与对象 | 代码审查 |

## 7. 视觉与 CSS

| 需求 | 实现 | 验证 |
|---|---|---|
| 以《新拟物.md》为主 | CSS 变量双阴影、同色、外凸/内凹、固定光源 | `scripts/check_frontend.py` + 截图 |
| 背景 #e0e5ec / 暗色非纯黑 | 亮色 `#e0e5ec`，暗色 `#2b303b` | 截图 |
| 强调色 #6d5dfc | `--accent` | 截图 |
| 圆角、无直角 | 12–24px；无 `rounded-none` | 静态检查 |
| 不使用标准 shadow-*、渐变、纯白/纯黑背景、粗边框 | 静态检查禁用模式 | `scripts/check_frontend.py` |
| 按钮按下内凹、无 translate | CSS active inset；静态检查 | 静态检查 |
| Hover 阴影减小 | 阴影数值下降 | 代码审查 |
| 输入框内凹、Focus 减小内阴影 | CSS 输入样式 | 截图 |
| 进度条轨道内凹、填充强调色 | CSS | 截图 |
| 过渡 300ms ease-in-out | 全局 transition | 静态检查 |
| 触控 ≥44px | 按钮/卡片最小尺寸 | 截图 + CSS |
| 无孤儿 CSS | `scripts/check_frontend.py` | 静态检查 |
| 移动端/PC | 响应式断点 | 截图（桌面/移动） |
| 日夜主题自动/手动 | `rc_theme` + `prefers-color-scheme` | 截图 |
| 44px、可读对比度 | 变量与字号 | 截图 |

## 8. 部署与 CI/CD

| 需求 | 实现 | 验证 |
|---|---|---|
| Dockerfile / Compose | 多阶段 Alpine 构建静态二进制；`/data` 卷 | 本地无 daemon，CI 构建 |
| amd64 / arm64 镜像 | CI 双架构 buildx + manifest | workflow 定义 |
| Docker Hub `ArcticFox520/RoomCheck` | 镜像 `arcticfox520/roomcheck`（Docker 引用小写） | workflow |
| Tag 规则 | v 版本→版本/次版本/主版本/latest/sha；main/develop→分支/sha；PR 不推送；dispatch 自定义 tag | workflow |
| latest 仅稳定版本 | 仅 `refs/tags/v*` 且不含 `-` 时启用 | workflow |
| musl amd64/arm64 静态 | `scripts/build-musl.sh`、CI Alpine arm64 容器 | 本地 amd64/arm64 构建 + 校验 |
| 无动态依赖 | 校验 `INTERP`、`NEEDED`、`ldd` | `verify-static.sh` |
| 产物 SHA 可追溯 | Tag `sha-<short>`、build-info、SHA256SUMS | workflow |
| PR 只构建不推送 | `push` 由事件决定 | workflow |
| 生产禁止 main | 文档 + README | 文档 |

## 9. 验收清单状态

完整结果见 [ACCEPTANCE.md](ACCEPTANCE.md)。当前：端到端测试 29/29；前端静态检查通过；amd64/arm64 musl 静态二进制本机构建验证通过；Docker 镜像构建由 CI 执行（本环境无 Docker daemon）。
