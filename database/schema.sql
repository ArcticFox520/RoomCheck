-- RoomCheck 1.0.0 SQLite schema
-- 运行时由 backend/src/db.c 内嵌执行；本文件为可读参考副本。
-- 修改表结构时必须同步 backend/src/db.c。

PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA foreign_keys = ON;
PRAGMA user_version = 2;

-- super_admin = 1 且 role = 'admin' 表示超级管理员；role 列保存账户的基础角色。
CREATE TABLE IF NOT EXISTS account (
    id                   INTEGER PRIMARY KEY AUTOINCREMENT,
    role                 TEXT NOT NULL CHECK (role IN ('admin', 'floor_manager')),
    username             TEXT NOT NULL,
    username_norm        TEXT NOT NULL UNIQUE,
    password_hash        TEXT NOT NULL,
    must_change_password INTEGER NOT NULL DEFAULT 0,
    status               TEXT NOT NULL DEFAULT 'enabled'
                         CHECK (status IN ('enabled', 'disabled', 'deleted')),
    deleted_at           INTEGER,
    delete_at            INTEGER,
    display_name         TEXT NOT NULL DEFAULT '',
    created_at           INTEGER NOT NULL,
    updated_at           INTEGER NOT NULL,
    last_login_at        INTEGER,
    super_admin          INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS floor_config (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id     INTEGER NOT NULL UNIQUE REFERENCES account(id) ON DELETE CASCADE,
    building_name  TEXT NOT NULL,
    start_room     TEXT NOT NULL,
    end_room       TEXT NOT NULL,
    start_num      INTEGER NOT NULL,
    end_num        INTEGER NOT NULL,
    original_count INTEGER NOT NULL,
    created_at     INTEGER NOT NULL,
    updated_at     INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS excluded_room (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id  INTEGER NOT NULL REFERENCES account(id) ON DELETE CASCADE,
    room_number TEXT NOT NULL,
    created_at  INTEGER NOT NULL,
    UNIQUE (account_id, room_number)
);

CREATE TABLE IF NOT EXISTS week_plan (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id INTEGER NOT NULL REFERENCES account(id) ON DELETE CASCADE,
    week_start INTEGER NOT NULL,
    week_end   INTEGER NOT NULL,
    iso_year   INTEGER NOT NULL,
    iso_week   INTEGER NOT NULL,
    plan_type  TEXT NOT NULL DEFAULT 'initial',
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS week_plan_account_idx ON week_plan (account_id);

CREATE TABLE IF NOT EXISTS check_group (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    week_id    INTEGER NOT NULL REFERENCES week_plan(id) ON DELETE CASCADE,
    day_index  INTEGER NOT NULL CHECK (day_index BETWEEN 0 AND 6),
    period     TEXT NOT NULL CHECK (period IN ('noon', 'evening')),
    created_at INTEGER NOT NULL,
    UNIQUE (week_id, day_index, period)
);

CREATE TABLE IF NOT EXISTS room_check (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    week_id     INTEGER NOT NULL REFERENCES week_plan(id) ON DELETE CASCADE,
    group_id    INTEGER NOT NULL REFERENCES check_group(id) ON DELETE CASCADE,
    account_id  INTEGER NOT NULL REFERENCES account(id) ON DELETE CASCADE,
    room_key    TEXT NOT NULL,
    room_number TEXT NOT NULL,
    position    INTEGER NOT NULL DEFAULT 0,
    checked     INTEGER NOT NULL DEFAULT 0,
    checked_at  INTEGER,
    created_at  INTEGER NOT NULL,
    UNIQUE (week_id, room_key)
);
CREATE INDEX IF NOT EXISTS room_check_group_idx ON room_check (group_id, position);
CREATE INDEX IF NOT EXISTS room_check_week_idx ON room_check (week_id);

CREATE TABLE IF NOT EXISTS note (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id  INTEGER NOT NULL REFERENCES account(id) ON DELETE CASCADE,
    week_id     INTEGER NOT NULL REFERENCES week_plan(id) ON DELETE CASCADE,
    room_key    TEXT NOT NULL,
    room_number TEXT NOT NULL,
    content     TEXT NOT NULL,
    created_at  INTEGER NOT NULL,
    updated_at  INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS note_room_idx ON note (week_id, room_key);

CREATE TABLE IF NOT EXISTS note_image (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    note_id       INTEGER REFERENCES note(id) ON DELETE CASCADE,
    account_id    INTEGER NOT NULL REFERENCES account(id) ON DELETE CASCADE,
    week_id       INTEGER NOT NULL REFERENCES week_plan(id) ON DELETE CASCADE,
    room_key      TEXT NOT NULL,
    room_number   TEXT NOT NULL,
    rel_path      TEXT NOT NULL,
    original_name TEXT NOT NULL,
    mime          TEXT NOT NULL,
    size          INTEGER NOT NULL,
    sha256        TEXT NOT NULL,
    created_at    INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS note_image_room_idx ON note_image (week_id, room_key);
CREATE INDEX IF NOT EXISTS note_image_note_idx ON note_image (note_id);

CREATE TABLE IF NOT EXISTS session (
    token_hash  TEXT PRIMARY KEY,
    account_id  INTEGER NOT NULL REFERENCES account(id) ON DELETE CASCADE,
    role        TEXT NOT NULL,
    csrf_token  TEXT NOT NULL,
    created_at  INTEGER NOT NULL,
    expires_at  INTEGER NOT NULL,
    revoked     INTEGER NOT NULL DEFAULT 0,
    ip          TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS session_account_idx ON session (account_id);

CREATE TABLE IF NOT EXISTS audit_log (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    actor_type  TEXT NOT NULL,
    actor_id    INTEGER,
    actor_name  TEXT NOT NULL DEFAULT '',
    action      TEXT NOT NULL,
    target_type TEXT NOT NULL DEFAULT '',
    target_id   TEXT NOT NULL DEFAULT '',
    detail      TEXT NOT NULL DEFAULT '',
    result      TEXT NOT NULL DEFAULT 'success',
    ip          TEXT NOT NULL DEFAULT '',
    created_at  INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS audit_created_idx ON audit_log (created_at);
CREATE INDEX IF NOT EXISTS audit_action_idx ON audit_log (action);

CREATE TABLE IF NOT EXISTS deletion_task (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id INTEGER NOT NULL UNIQUE,
    status     TEXT NOT NULL DEFAULT 'pending',
    attempts   INTEGER NOT NULL DEFAULT 0,
    last_error TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS backup_record (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    dir_name   TEXT NOT NULL UNIQUE,
    path       TEXT NOT NULL,
    size       INTEGER NOT NULL DEFAULT 0,
    status     TEXT NOT NULL DEFAULT 'success',
    created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
