/*
 * routes.c - HTTP route table and SPA entry points
 */
#include "common.h"

const route_t g_routes[] = {
    {"GET", "/api/health", h_health, RF_PUBLIC},
    {"POST", "/api/login", h_login, RF_PUBLIC},
    {"POST", "/api/logout", h_logout, RF_AUTH},
    {"GET", "/api/session", h_session, RF_AUTH},
    {"POST", "/api/password", h_password, RF_AUTH},
    {"PATCH", "/api/profile", h_profile, RF_AUTH},

    {"GET", "/api/config", h_config_get, RF_AUTH},
    {"PUT", "/api/config", h_config_put, RF_AUTH},

    {"GET", "/api/week", h_week_get, RF_AUTH},
    {"POST", "/api/schedule/regenerate", h_week_regenerate, RF_AUTH},
    {"POST", "/api/week/reset", h_week_reset, RF_AUTH},

    {"POST", "/api/checks/:id/check", h_check, RF_AUTH},
    {"POST", "/api/checks/:id/uncheck", h_uncheck, RF_AUTH},

    {"GET", "/api/rooms/:id/notes", h_notes_list, RF_AUTH},
    {"POST", "/api/rooms/:id/notes", h_note_create, RF_AUTH},
    {"PATCH", "/api/notes/:id", h_note_update, RF_AUTH},
    {"DELETE", "/api/notes/:id", h_note_delete, RF_AUTH},

    {"GET", "/api/rooms/:id/images", h_images_list, RF_AUTH},
    {"POST", "/api/rooms/:id/images", h_image_upload, RF_AUTH},
    {"GET", "/api/images/:id/file", h_image_file, RF_AUTH},
    {"DELETE", "/api/images/:id", h_image_delete, RF_AUTH},

    {"GET", "/api/admin/accounts", h_admin_accounts, RF_AUTH | RF_ADMIN},
    {"POST", "/api/admin/accounts", h_admin_account_create, RF_AUTH | RF_ADMIN},
    {"PATCH", "/api/admin/accounts/:id", h_admin_account_update, RF_AUTH | RF_ADMIN},
    {"POST", "/api/admin/accounts/:id/enable", h_admin_account_enable, RF_AUTH | RF_ADMIN},
    {"POST", "/api/admin/accounts/:id/disable", h_admin_account_disable, RF_AUTH | RF_ADMIN},
    {"POST", "/api/admin/accounts/:id/delete", h_admin_account_delete, RF_AUTH | RF_ADMIN},
    {"POST", "/api/admin/accounts/:id/restore", h_admin_account_restore, RF_AUTH | RF_ADMIN},
    {"GET", "/api/admin/accounts/:id/week", h_admin_account_week, RF_AUTH | RF_ADMIN},
    {"GET", "/api/admin/overview", h_admin_overview, RF_AUTH | RF_ADMIN},
    {"GET", "/api/admin/logs", h_admin_logs, RF_AUTH | RF_ADMIN},
    {"GET", "/api/admin/backups", h_admin_backups, RF_AUTH | RF_ADMIN},
    {"POST", "/api/admin/backups", h_admin_backup_create, RF_AUTH | RF_ADMIN},
    {"POST", "/api/admin/backups/:id/restore", h_admin_backup_restore,
     RF_AUTH | RF_ADMIN},
    {"GET", "/api/admin/orphans", h_admin_orphans, RF_AUTH | RF_ADMIN},

    {"GET", "/api/super-admin/admins", h_admin_admins, RF_AUTH | RF_SUPER_ADMIN},
    {"POST", "/api/super-admin/admins", h_admin_admin_create, RF_AUTH | RF_SUPER_ADMIN},
    {"PATCH", "/api/super-admin/admins/:id", h_admin_admin_update, RF_AUTH | RF_SUPER_ADMIN},
    {"POST", "/api/super-admin/admins/:id/enable", h_admin_admin_enable,
     RF_AUTH | RF_SUPER_ADMIN},
    {"POST", "/api/super-admin/admins/:id/disable", h_admin_admin_disable,
     RF_AUTH | RF_SUPER_ADMIN},
    {"POST", "/api/super-admin/admins/:id/delete", h_admin_admin_delete,
     RF_AUTH | RF_SUPER_ADMIN},
    {"POST", "/api/super-admin/admins/:id/restore", h_admin_admin_restore,
     RF_AUTH | RF_SUPER_ADMIN},
    {"GET", "/api/super-admin/backups/:id/download", h_admin_backup_export,
     RF_AUTH | RF_SUPER_ADMIN},

    {NULL, NULL, NULL, 0}};

const char *g_spa_routes[] = {"/", "/login", "/admin", "/manager", NULL};
