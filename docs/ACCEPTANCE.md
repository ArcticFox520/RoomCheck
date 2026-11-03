# 验收报告（ACCEPTANCE）

**项目：** RoomCheck 寝室抽查管理系统  
**版本：** 1.0.0  
**验收日期：** 2026-11-03（Asia/Shanghai）  
**依据：** 《工作指导.md》V1.0、《新拟物.md》

---

## 1. 验收环境

| 项目 | 值 |
|---|---|
| 操作系统 | Debian GNU/Linux 12 (bookworm) |
| 架构 | x86_64 |
| 编译器 | GCC 12.2.0；musl-gcc；aarch64-linux-musl-gcc 11.2.1 |
| SQLite | 3.46.1 amalgamation（静态编译，运行时报告 3.46.1） |
| 浏览器 | Playwright 1.64.1 + Chromium headless（桌面 1440×1000，移动 390×844） |
| QEMU | qemu-aarch64 7.2.22（用于 arm64 静态二进制运行验证） |
| Python | 3.11.2（测试脚本） |
| Docker | **本机无 Docker daemon**；多架构构建由 CI workflow 定义，未在本机执行 |

---

## 2. 自动化测试结果

### 2.1 后端端到端测试

命令：

```bash
make dev
make test
```

结果：**24 / 24 通过，0 失败**。

补充验证：

- **UBSan 构建**（`-fsanitize=undefined`，`halt_on_error=1`）运行同一套测试：**24 / 24 通过**，未报告未定义行为。
- **ASan 构建**在主服务进程内执行的用例未报告内存错误；涉及同时启动第二个服务进程/重启用例时，容器内 ASan 运行时出现 `DEADLYSIGNAL`（应用未输出 ASan 错误报告），因此以 UBSan 全量结果为发布依据。

| # | 用例 | 覆盖内容 |
|---:|---|---|
| 1 | `test_health_and_static` | 健康检查、静态页面、SPA 回退、路径穿越拒绝 |
| 2 | `test_unauthenticated_and_csrf` | 未登录 401、错误密码、非法 JSON |
| 3 | `test_admin_first_login_gate` | 首次强制改密、改密拦截、Session 7 天 |
| 4 | `test_account_crud_validation` | 账户名全局唯一（大小写不敏感）、管理员重名拒绝 |
| 5 | `test_manager_login_and_config_validation` | 范围/排除/N 校验、六天平均、中午晚上平均、周四隐藏、N=11 |
| 6 | `test_check_and_csrf` | CSRF 缺省拒绝、勾选、撤销二次确认、进度 |
| 7 | `test_notes_and_limits` | 多条备注、编辑删除、500 字上限、关联图片级联删除 |
| 8 | `test_image_security` | 扩展名黑名单、ELF 签名、HTML MIME、非图片、10MB 限制、原图访问 |
| 9 | `test_weekly_reset_and_regenerate` | 重置确认、勾选清空、备注/图片迁移、旧周目录删除、重新生成 |
| 10 | `test_incremental_schedule` | 范围缩小/扩大、保留位置与勾选、新增数量、孤儿备注 |
| 11 | `test_isolation_and_admin_views` | 数据隔离、管理员周视图、查看备注审计、管理员修改勾选 |
| 12 | `test_disable_enable_delete_restore` | 禁用/启用、会话失效、删除 72 天倒计时、撤销删除、数据保留 |
| 13 | `test_password_change_session_invalidation` | 改密后其它会话失效、旧密码拒绝 |
| 14 | `test_lockout_memory_only` | 5 次锁定 10 分钟、锁定期正确密码拒绝、重启清零、勾选持久化 |
| 15 | `test_image_count_limit` | 50 张上限、第 51 张拒绝 |
| 16 | `test_backup_and_restore` | 手动备份结构、恢复、会话失效、恢复后数据回滚 |
| 17 | `test_session_expiry` | 伪造过期 Session 被拒绝 |
| 18 | `test_audit_log_retention` | 自然周日志清理、本周日志保留、清理审计 |
| 19 | `test_expiry_purge` | 到期永久删除账户/配置/排班/备注/图片/文件、审计保留、撤销拒绝、重复扫描幂等 |
| 20 | `test_backup_retention` | 只保留最新 3 份备份与目录 |
| 21 | `test_admin_log_filters` | 分页、动作筛选、日期筛选、管理员周视图日期参数 |
| 22 | `test_admin_reset_keeps_floor_manager_data` | ADMIN_RESET 重置管理员、旧会话失效、强制改密、楼层长数据不变 |
| 23 | `test_automatic_daily_backup` | 启动补偿的每日自动备份、DB/图片/清单、审计 |
| 24 | `test_final_session_ttl` | 登录 Session 有效期约 7 天 |

### 2.2 前端静态检查

命令：

```bash
python3 scripts/check_frontend.py
```

结果：**通过**。

- CSS 类数量 271；HTML 静态类 12。
- 无内联 `<script>`、无内联事件属性、无 `javascript:` URL（CSP `script-src 'self'`）。
- 无渐变、无 `translate`、无标准 `shadow-*`、无 `rounded-none`、无粗边框（<2px 实线边框）。
- 无纯白/纯黑背景（白色仅用于高光阴影）。
- 全部 `transition` 使用 `300ms ease-in-out` 令牌。
- 无 CSS 孤儿类；HTML 使用类均存在；无外部资源；图标引用存在。

### 2.3 浏览器 UI 端到端冒烟

使用真实后端 + Playwright（非 mock）执行：

- 管理员首次登录 → 强制改密 → 管理概览；
- 通过 UI 创建楼层长；
- 楼层长登录 → 配置寝室范围（601–620，排除 605/610，N=18）；
- 周视图渲染 18 间寝室，勾选、继续检查；
- 打开寝室详情、添加备注、上传图片并确认图片实际渲染；
- 撤销勾选确认弹窗、备注/图片操作；
- 暗色主题切换；
- 移动端 390px 布局；
- 管理员概览、账户列表、账户周视图、立即备份、备份列表、操作日志。

结果：**通过，无页面异常（pageerror）或非预期控制台错误**。

> 发现并修复的 UI 缺陷：管理员首次改密成功后，强制密码弹窗因 `closeTopModal()` 不关闭不可取消弹窗而导致遮罩残留、页面不可点击。已改为直接关闭当前弹窗，并回归验证。

---

## 3. 视觉验收

| 规范 | 结果 |
|---|---|
| 新拟物双阴影、左上高光/右下阴影 | 通过（CSS 变量、截图） |
| 未勾选外凸、已勾选内凹 | 通过（截图） |
| 按钮按下内凹、无位移 | 通过（CSS `:active` inset；静态检查无 translate） |
| Hover 阴影减小 | 通过（CSS 审查） |
| 输入框默认内凹、Focus 减小内阴影 | 通过（截图与 CSS） |
| 进度条轨道内凹、强调色填充 | 通过（截图） |
| 背景 #e0e5ec / 暗色 #2b303b | 通过 |
| 强调色 #6d5dfc / 暗色 #7c6cff | 通过 |
| 圆角、无直角、无粗边框、无渐变 | 通过 |
| 过渡 300ms ease-in-out | 通过（静态检查） |
| 触控区域 ≥44px | 通过（CSS/截图） |
| 移动端与 PC 端 | 通过（1440px、390px 截图） |
| 自动/亮色/暗色主题 | 通过（截图） |
| 无孤儿 CSS | 通过（静态检查） |

截图见 [docs/screenshots/](screenshots/)：

- [登录页](screenshots/login-desktop.png)
- [楼层长周视图](screenshots/manager-week-desktop.png)
- [寝室详情（备注与图片）](screenshots/room-detail-desktop.png)
- [寝室配置](screenshots/config-desktop.png)
- [暗色主题](screenshots/manager-week-dark.png)
- [移动端周视图](screenshots/manager-week-mobile.png)
- [管理概览](screenshots/admin-overview-with-data.png)
- [账户管理](screenshots/admin-accounts-desktop.png)
- [管理员查看楼层长周视图](screenshots/admin-week-desktop.png)
- [备份管理](screenshots/admin-backups-desktop.png)
- [操作日志](screenshots/admin-logs-desktop.png)
- [首次登录强制改密](screenshots/admin-first-login-password.png)

---

## 4. 静态二进制验收

命令：

```bash
scripts/build-musl.sh all
scripts/verify-static.sh dist/linux-amd64-musl/RoomCheck amd64 --run
scripts/verify-static.sh dist/linux-arm64-musl/RoomCheck arm64 --run
```

| 架构 | 文件类型 | 动态依赖 | 运行验证 | SHA-256 | 大小 |
|---|---|---|---|---|---|
| amd64 | ELF 64-bit LSB executable, x86-64, statically linked, stripped | 无 `INTERP` / `NEEDED` / ldd 动态依赖 | 本机 `--version`、`--selftest` | `68f526c99e27c263de9558c07dcefdc9185db3e7c283d80836810b3b520716b6` | 1,248,104 B |
| arm64 | ELF 64-bit LSB executable, ARM aarch64, statically linked, stripped | 无 `INTERP` / `NEEDED` / ldd 动态依赖 | QEMU 下 `--version`、`--selftest`，并启动服务验证 `/api/health` 与管理员登录 | `0ff4338d73a35ffa235ef6662e22bbb02b797949669c31996904653913aa5676` | 1,194,792 B |

- 使用内置 SQLite 3.46.1 amalgamation 静态链接。
- 构建参数 `-static -no-pie`；`file` 显示 `statically linked`，不是 static-pie。
- amd64 二进制已验证可启动 HTTP 服务并完成登录。
- arm64 二进制通过 `qemu-aarch64-static` 启动服务并成功返回健康检查与登录响应。

---

## 5. 工作指导第 45 节验收清单

### 业务

- [x] 管理员初始化
- [x] 楼层长管理
- [x] 账户名全局唯一
- [x] 登录锁定
- [x] Session 7 天
- [x] 权限隔离
- [x] 寝室配置
- [x] N 计算
- [x] 无需查寝
- [x] 增量排班
- [x] 六天平均排班
- [x] 中午/晚上平均
- [x] 周四隐藏
- [x] 勾选和撤销
- [x] 进度统计
- [x] 继续检查
- [x] 备注
- [x] 图片
- [x] 周重置
- [x] 禁用
- [x] 账户删除、72 天内撤销删除恢复启用、到期自动永久删除及并发边界
- [x] 到期自动永久删除账户及关联数据、图片

### 安全

- [x] 加盐密码哈希
- [x] CSRF
- [x] Session 校验
- [x] MIME 校验
- [x] 文件签名校验
- [x] 可执行文件拒绝
- [x] 日志审计
- [x] 数据隔离

### 运维

- [x] 每日备份（启动补偿 + 03:00 后检查）
- [x] 保留 3 份
- [x] 管理员恢复
- [x] 恢复二次确认
- [x] Docker 卷持久化（Compose/Dockerfile 定义）
- [x] 重启数据保留（端到端测试重启验证）

> Docker 卷/镜像的容器级运行验证依赖 CI（本机无 Docker daemon），见“待 CI 验证”。

### 视觉

- [x] 新拟物
- [x] 双阴影
- [x] 外凸/内凹
- [x] 移动端
- [x] PC 端
- [x] 日夜主题
- [x] 44px 触控区域
- [x] 无孤儿样式

### CI/CD

- [x] PR 只构建不推送（workflow 条件）
- [x] main 推送 main 和 SHA（metadata + digest manifest）
- [x] develop 推送 develop 和 SHA
- [x] semver 推送版本 Tag
- [x] latest 仅跟随稳定版本
- [x] workflow_dispatch 支持自定义 Tag
- [x] Docker amd64（配置）
- [x] Docker arm64（配置）
- [x] musl static amd64（本机验证）
- [x] musl static arm64（本机验证）
- [x] 所有发布产物可追溯（SHA Tag、SHA256SUMS、build-info）
- [x] 生产禁止使用 main（README/部署文档）

> 标注“配置”的项为 workflow 代码审查通过，实际 GitHub Actions 执行需在仓库推送后验证。

---

## 6. P0 / P1 / P2 状态

| 优先级 | 状态 | 说明 |
|---|---|---|
| P0 核心业务与数据安全 | **全部完成并通过测试** | 数据库、唯一性、登录/Session、锁定、CSRF、密码哈希、配置/N、排班、勾选、备注、图片、重置、审计、备份、权限与核心测试 |
| P1 管理功能、界面、部署 | **代码完成并验证** | 启用/禁用、72 天、到期清理、聚合页、查寝 UI、新拟物、移动端、日夜主题、继续检查、备份恢复、Compose、Dockerfile、musl、CSS 检查 |
| P2 交付与发布体系 | **产物准备完成** | Actions 双 workflow、Tag、Docker Hub 推送配置、musl 双架构、SHA 追溯、README、验收报告、已知限制、截图。Docker 镜像构建与 Release 需 CI 实际运行 |

---

## 7. 待 CI 验证项（不阻塞代码验收）

1. GitHub Actions 双架构 Docker 构建、manifest push、Docker Hub 登录与 Tag 规则。
2. 容器以非 root、`read_only`、命名卷启动及 Healthcheck。
3. Alpine arm64 容器内 musl 构建（本地已用交叉工具链与 QEMU 验证等价产物）。
4. GitHub Release 创建（仅版本 Tag）。
5. Docker Hub 仓库实际可见性（`ArcticFox520/RoomCheck`，镜像引用小写）。

这些项目在 CI 中执行后，补齐构建报告即可达到无条件发布状态。

---

## 8. 发布结论

- **P0：通过**，不允许发布的问题未发现。
- **P1：通过**（本地端到端、UI、静态与静态二进制验证）。
- **P2：准备完成，待 CI 镜像/Release 验证**。
- **建议：** 推送仓库并运行 `docker-build` 与 `linux-musl` workflow；确认 manifest 同时包含 `linux/amd64` 与 `linux/arm64`、musl 产物在 arm64 容器内自检通过后，即可按固定版本 Tag 发布。
