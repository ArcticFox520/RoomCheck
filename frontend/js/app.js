/*
 * RoomCheck 前端应用（纯原生 JavaScript，无框架、无构建步骤）
 * 结构：
 *   1. 状态 / 工具函数 / 主题
 *   2. 提示 Toast / 模态框 / 焦点管理
 *   3. 会话与登录
 *   4. 顶部导航 / 路由
 *   5. 查寝周视图
 *   6. 寝室详情（备注 / 图片）
 *   7. 配置 / 管理员周视图
 *   8. 管理员概览 / 账户 / 备份 / 日志
 *   9. 账户设置 / 事件绑定 / 启动
 */
(function () {
  'use strict';

  var RC = window.RC;
  var api = RC && RC.api;

  var THEME_KEY = 'rc_theme';
  var ROLE_LABELS = { super_admin: '超级管理员', admin: '管理员', floor_manager: '楼层长' };
  var WEEKDAY_LABELS = {
    0: '周一',
    1: '周二',
    2: '周三',
    4: '周五',
    5: '周六',
    6: '周日'
  };
  var STATUS_LABELS = { enabled: '启用', disabled: '禁用', deleted: '已删除' };
  var PERIOD_LABELS = { noon: '中午', evening: '晚上' };
  var RESULT_LABELS = { success: '成功', failure: '失败', warning: '警告' };
  var TARGET_LABELS = {
    account: '账户',
    room: '寝室',
    note: '备注',
    image: '图片',
    week: '周排班',
    config: '配置',
    backup: '备份',
    log: '日志'
  };
  var ACTION_LABELS = {
    login_success: '登录成功',
    login_failed: '登录失败',
    login_locked: '登录锁定',
    logout: '退出登录',
    password_change: '修改密码',
    username_change: '修改账户名',
    account_create: '创建账户',
    account_update: '修改账户',
    account_enable: '启用账户',
    account_disable: '禁用账户',
    account_delete: '删除账户',
    account_restore: '撤销删除',
    account_purge: '永久删除账户',
    config_update: '修改配置',
    schedule_generate: '生成排班',
    schedule_regenerate: '重新生成排班',
    week_reset: '重置本周',
    check_toggle: '勾选变更',
    note_create: '添加备注',
    note_update: '修改备注',
    note_delete: '删除备注',
    note_view: '查看备注',
    image_upload: '上传图片',
    image_delete: '删除图片',
    image_view: '查看原图',
    backup_success: '备份成功',
    backup_failed: '备份失败',
    backup_restore: '恢复备份',
    backup_export: '导出备份',
    log_cleanup: '清理日志',
    deletion_retry_failed: '永久删除重试失败'
  };
  var ACTION_ORDER = [
    'login_success', 'login_failed', 'login_locked', 'logout',
    'password_change', 'username_change', 'account_create', 'account_update',
    'account_enable', 'account_disable', 'account_delete', 'account_restore',
    'account_purge', 'config_update', 'schedule_generate', 'schedule_regenerate',
    'week_reset', 'check_toggle', 'note_create', 'note_update', 'note_delete',
    'note_view', 'image_upload', 'image_delete', 'image_view', 'backup_success',
    'backup_failed', 'backup_restore', 'backup_export', 'log_cleanup',
    'deletion_retry_failed'
  ];

  var els = { app: null, header: null, view: null, toast: null, modal: null, themeColor: null };
  var renderSeq = 0;
  var modalSeq = 0;
  var modalStack = [];
  var pendingChecks = Object.create(null);
  var countdownTimer = null;
  var loginLockTimer = null;

  var state = {
    user: null,
    csrf: '',
    view: 'login',
    theme: 'auto',
    authExpired: false,
    loginNotice: '',
    loginLockUntil: 0,
    loginLockMessage: '',
    week: null,
    config: null,
    configDraft: null,
    roomDetail: null,
    overview: null,
    overviewDate: '',
    accounts: [],
    adminAccounts: [],
    accountFilter: 'all',
    backups: [],
    logs: { items: [], total: 0, page: 1, pageSize: 20, actions: [] },
    logFilters: { action: '', account_id: '', from: '', to: '', page: 1 },
    logsLoading: false,
    adminDetail: null,
    passwordModalOpen: false
  };

  /* ======================================================================
     1. 工具函数 / 主题
     ====================================================================== */

  function q(selector, root) {
    return (root || document).querySelector(selector);
  }

  function qa(selector, root) {
    return Array.prototype.slice.call((root || document).querySelectorAll(selector));
  }

  function esc(value) {
    var map = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
    return String(value === null || value === undefined ? '' : value).replace(/[&<>"']/g, function (ch) {
      return map[ch];
    });
  }

  function icon(name) {
    return '<svg class="icon" viewBox="0 0 24 24" aria-hidden="true" focusable="false"><use href="#icon-' +
      name + '"></use></svg>';
  }

  function charCount(value) {
    return Array.from(String(value === null || value === undefined ? '' : value)).length;
  }

  function num(value, fallback) {
    var n = Number(value);
    return Number.isFinite(n) ? n : (fallback === undefined ? 0 : fallback);
  }

  function clampPercent(value) {
    return Math.max(0, Math.min(100, num(value, 0)));
  }

  function roleLabel(role) {
    return ROLE_LABELS[role] || '用户';
  }

  function statusLabel(status) {
    return STATUS_LABELS[status] || status || '—';
  }

  function statusBadge(status) {
    var key = status === 'enabled' || status === 'disabled' || status === 'deleted' ? status : '';
    var cls = key === 'enabled' ? 'is-enabled' : key === 'disabled' ? 'is-disabled' : key === 'deleted' ? 'is-deleted' : '';
    return '<span class="status-badge ' + cls + '">' + esc(statusLabel(status)) + '</span>';
  }

  function resultBadge(result) {
    var cls = result === 'success' ? 'is-success' : result === 'failure' ? 'is-failure' : result === 'warning' ? 'is-warning' : '';
    return '<span class="status-badge ' + cls + '">' + esc(RESULT_LABELS[result] || result || '—') + '</span>';
  }

  function actionLabel(action) {
    return ACTION_LABELS[action] || action || '未知操作';
  }

  function isoParts(value) {
    var match = /^(\d{4})-(\d{2})-(\d{2})/.exec(String(value || ''));
    if (!match) { return null; }
    return { year: Number(match[1]), month: Number(match[2]), day: Number(match[3]) };
  }

  function formatMonthDay(value) {
    var parts = isoParts(value);
    return parts ? parts.month + '月' + parts.day + '日' : '';
  }

  function formatWeekRange(start, end) {
    var a = formatMonthDay(start);
    var b = formatMonthDay(end);
    if (a && b) { return a + ' — ' + b; }
    return '—';
  }

  /* 服务端未提供 *_text 的少量字段（账户删除时间）在此按上海时区格式化。
     其余时间一律使用服务端返回的 text/date 字段。 */
  function formatShanghaiDateTime(ts) {
    var value = Number(ts);
    if (!Number.isFinite(value) || value <= 0) { return '—'; }
    try {
      var parts = new Intl.DateTimeFormat('zh-CN', {
        timeZone: 'Asia/Shanghai',
        year: 'numeric',
        month: '2-digit',
        day: '2-digit',
        hour: '2-digit',
        minute: '2-digit',
        hour12: false
      }).formatToParts(new Date(value * 1000));
      var bucket = {};
      parts.forEach(function (part) { bucket[part.type] = part.value; });
      return bucket.year + '-' + bucket.month + '-' + bucket.day + ' ' + bucket.hour + ':' + bucket.minute;
    } catch (err) {
      return '—';
    }
  }

  function formatRemaining(totalSeconds) {
    var seconds = Math.max(0, Math.ceil(Number(totalSeconds) || 0));
    if (seconds <= 0) { return '已到期'; }
    var days = Math.floor(seconds / 86400);
    var hours = Math.floor((seconds % 86400) / 3600);
    var minutes = Math.floor((seconds % 3600) / 60);
    var secs = seconds % 60;
    if (days > 0) { return days + '天' + hours + '小时'; }
    if (hours > 0) { return hours + '小时' + minutes + '分'; }
    if (minutes > 0) { return minutes + '分' + secs + '秒'; }
    return secs + '秒';
  }

  function formatFileSize(bytes) {
    var size = Number(bytes);
    if (!Number.isFinite(size) || size <= 0) { return '0 KB'; }
    if (size >= 1024 * 1024) { return (size / (1024 * 1024)).toFixed(1) + ' MB'; }
    if (size >= 1024) { return (size / 1024).toFixed(1) + ' KB'; }
    return size + ' B';
  }

  function pluralCount(count, unit) {
    return String(num(count, 0)) + ' ' + unit;
  }

  function textOf(element, value) {
    if (element) { element.textContent = String(value === null || value === undefined ? '' : value); }
  }

  function setFieldError(element, message) {
    if (!element) { return; }
    if (message) {
      element.textContent = String(message);
      element.hidden = false;
    } else {
      element.textContent = '';
      element.hidden = true;
    }
  }

  function hideElement(element, hidden) {
    if (element) { element.hidden = !!hidden; }
  }

  function errorMessage(err) {
    if (err && typeof err.message === 'string' && err.message) { return err.message; }
    return '操作失败，请稍后重试';
  }

  function reportError(err) {
    if (!err) { return; }
    if (err.status === 401) { return; }
    if (err.code === 'password_change_required') { return; }
    toast(errorMessage(err), 'error');
  }

  function currentHash() {
    return (window.location.hash || '').replace(/^#/, '') || '/';
  }

  function isSuperAdmin() {
    return !!state.user && state.user.role === 'super_admin';
  }

  function isAdmin() {
    return !!state.user && (state.user.role === 'admin' || state.user.role === 'super_admin');
  }

  function isManager() {
    return !!state.user && state.user.role === 'floor_manager';
  }

  function defaultRoute() {
    return isAdmin() ? '/admin' : '/check';
  }

  /* ---- 主题 ---- */
  var darkMedia = null;
  try {
    darkMedia = window.matchMedia ? window.matchMedia('(prefers-color-scheme: dark)') : null;
  } catch (err) {
    darkMedia = null;
  }

  function storedTheme() {
    try {
      var saved = window.localStorage.getItem(THEME_KEY);
      if (saved === 'auto' || saved === 'light' || saved === 'dark') { return saved; }
    } catch (err) {
      /* 忽略 localStorage 异常 */
    }
    return 'auto';
  }

  function resolvedTheme(choice) {
    if (choice === 'light' || choice === 'dark') { return choice; }
    return darkMedia && darkMedia.matches ? 'dark' : 'light';
  }

  function updateThemeColor() {
    if (!els.themeColor) { return; }
    els.themeColor.setAttribute('content', document.documentElement.getAttribute('data-theme') === 'dark' ? '#2b303b' : '#e0e5ec');
  }

  function updateThemeControls() {
    qa('[data-theme-value]').forEach(function (button) {
      var active = button.getAttribute('data-theme-value') === state.theme;
      button.classList.toggle('is-active', active);
      button.setAttribute('aria-pressed', active ? 'true' : 'false');
    });
  }

  function applyTheme(choice, persist) {
    state.theme = choice === 'light' || choice === 'dark' || choice === 'auto' ? choice : 'auto';
    if (persist) {
      try {
        window.localStorage.setItem(THEME_KEY, state.theme);
      } catch (err) {
        /* 忽略 localStorage 异常 */
      }
    }
    document.documentElement.setAttribute('data-theme', resolvedTheme(state.theme));
    updateThemeColor();
    updateThemeControls();
  }

  function initTheme() {
    applyTheme(storedTheme(), false);
    if (darkMedia) {
      var listener = function () {
        if (state.theme === 'auto') { applyTheme('auto', false); }
      };
      if (typeof darkMedia.addEventListener === 'function') {
        darkMedia.addEventListener('change', listener);
      } else if (typeof darkMedia.addListener === 'function') {
        darkMedia.addListener(listener);
      }
    }
  }

  function themeSwitchHtml() {
    var options = [
      { value: 'auto', icon: 'auto', label: '自动主题（跟随系统）' },
      { value: 'light', icon: 'sun', label: '浅色主题' },
      { value: 'dark', icon: 'moon', label: '深色主题' }
    ];
    return '<div class="theme-switch" role="group" aria-label="主题切换">' +
      options.map(function (option) {
        return '<button class="theme-option" type="button" data-action="theme" data-theme-value="' +
          option.value + '" aria-label="' + option.label + '" title="' + option.label + '" aria-pressed="false">' +
          icon(option.icon) + '</button>';
      }).join('') + '</div>';
  }

  /* ---- 倒计时（仅用于删除保留期与锁定提示） ---- */
  function startCountdownTicker() {
    if (countdownTimer) { return; }
    countdownTimer = window.setInterval(updateCountdowns, 1000);
  }

  function updateCountdowns() {
    qa('[data-countdown-at]').forEach(function (element) {
      var until = Number(element.getAttribute('data-countdown-at'));
      if (!Number.isFinite(until) || until <= 0) {
        element.textContent = '—';
        return;
      }
      var remaining = Math.max(0, Math.ceil(until - Date.now() / 1000));
      element.textContent = remaining > 0 ? '剩余 ' + formatRemaining(remaining) : '已到期，等待自动删除';
    });
  }

  /* ---- 骨架屏 / 空状态 / 错误状态 ---- */
  function skeletonLines(count) {
    var html = '';
    for (var i = 0; i < count; i += 1) {
      html += '<div class="skeleton skeleton-line"></div>';
    }
    return html;
  }

  function skeletonCards(count) {
    var html = '';
    for (var i = 0; i < count; i += 1) {
      html += '<div class="skeleton-card">' +
        '<div class="skeleton skeleton-title"></div>' + skeletonLines(4) +
        '</div>';
    }
    return html;
  }

  function loadingPageHtml(eyebrow, title) {
    return '<div class="page">' +
      '<div class="page-head"><div><p class="eyebrow">' + esc(eyebrow) + '</p>' +
      '<h1 class="page-title">' + esc(title) + '</h1></div></div>' +
      '<p class="loading-text">正在加载…</p>' +
      '<div class="skeleton-grid">' + skeletonCards(3) + '</div>' +
      '</div>';
  }

  function emptyStateHtml(iconName, title, message, actionHtml) {
    return '<div class="empty-state">' +
      '<div class="empty-icon">' + icon(iconName) + '</div>' +
      '<h2 class="empty-title">' + esc(title) + '</h2>' +
      '<p class="empty-text">' + esc(message) + '</p>' +
      (actionHtml || '') +
      '</div>';
  }

  function emptyStateCompactHtml(iconName, title, message) {
    return '<div class="empty-state empty-state--compact">' +
      '<div class="empty-icon">' + icon(iconName) + '</div>' +
      '<h3 class="empty-title">' + esc(title) + '</h3>' +
      '<p class="empty-text">' + esc(message) + '</p>' +
      '</div>';
  }

  function renderViewError(title, message) {
    els.view.innerHTML = '<div class="page">' +
      '<div class="error-state">' +
      '<div class="empty-icon">' + icon('warning') + '</div>' +
      '<h1 class="empty-title">' + esc(title) + '</h1>' +
      '<p class="error-message">' + esc(message) + '</p>' +
      '<button class="btn btn-accent" type="button" data-action="retry-view">' +
      icon('refresh') + '重试</button>' +
      '</div></div>';
  }

  function pageHeadHtml(options) {
    return '<div class="page-head">' +
      '<div><p class="eyebrow">' + esc(options.eyebrow || '') + '</p>' +
      '<h1 class="page-title">' + esc(options.title || '') + '</h1>' +
      (options.sub ? '<p class="page-sub">' + options.sub + '</p>' : '') +
      '</div>' +
      (options.actions ? '<div class="page-actions">' + options.actions + '</div>' : '') +
      '</div>';
  }

  function pageWrap(headHtml, bodyHtml) {
    return '<div class="page">' + headHtml + '<div class="page-body">' + bodyHtml + '</div></div>';
  }

  /* ======================================================================
     2. Toast / 模态框 / 焦点管理
     ====================================================================== */

  function toast(message, variant) {
    if (!els.toast) { return; }
    var type = variant === 'success' || variant === 'error' ? variant : 'info';
    var iconName = type === 'success' ? 'check' : type === 'error' ? 'warning' : 'info';
    var toastClasses = {
      success: 'toast toast--success',
      error: 'toast toast--error',
      info: 'toast toast--info'
    };
    var element = document.createElement('div');
    element.className = toastClasses[type];
    element.innerHTML = '<span class="toast-icon">' + icon(iconName) + '</span>' +
      '<p class="toast-text"></p>' +
      '<button class="toast-close" type="button" aria-label="关闭提示">' + icon('close') + '</button>';
    q('.toast-text', element).textContent = String(message === null || message === undefined ? '' : message);
    var timer = null;

    function remove() {
      if (timer) { window.clearTimeout(timer); }
      if (element.parentNode) { element.parentNode.removeChild(element); }
    }

    q('.toast-close', element).addEventListener('click', remove);
    els.toast.appendChild(element);
    timer = window.setTimeout(remove, 4600);
  }

  function getFocusable(modal) {
    return qa('a[href], button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])', modal)
      .filter(function (element) {
        return element.offsetWidth > 0 || element.offsetHeight > 0;
      });
  }

  function openModal(options) {
    var opts = options || {};
    var titleId = 'modal-title-' + (++modalSeq);
    var dismissible = opts.dismissible !== false;
    var backdrop = document.createElement('div');
    backdrop.className = 'modal-backdrop';
    backdrop.innerHTML = '<div class="modal ' + (opts.className || '') + '" role="dialog" aria-modal="true" aria-labelledby="' + titleId + '" tabindex="-1">' +
      '<div class="modal-head">' +
      '<div class="modal-heading">' +
      (opts.eyebrow ? '<p class="modal-eyebrow">' + esc(opts.eyebrow) + '</p>' : '') +
      '<h2 class="modal-title" id="' + titleId + '">' + esc(opts.title || '') + '</h2>' +
      '</div>' +
      (dismissible ? '<button class="icon-btn modal-close" type="button" data-modal-close aria-label="关闭">' + icon('close') + '</button>' : '') +
      '</div>' +
      '<div class="modal-body">' + (opts.body || '') + '</div>' +
      (opts.footer ? '<div class="modal-foot">' + opts.footer + '</div>' : '') +
      '</div>';

    var record = {
      el: backdrop,
      dismissible: dismissible,
      onClose: typeof opts.onClose === 'function' ? opts.onClose : null,
      previousFocus: document.activeElement,
      closed: false
    };

    els.modal.appendChild(backdrop);
    modalStack.push(record);
    document.body.classList.add('modal-open');

    backdrop.addEventListener('click', function (event) {
      if (event.target === backdrop && record.dismissible) {
        closeModal(record);
      }
    });

    var closeButton = q('[data-modal-close]', backdrop);
    if (closeButton) {
      closeButton.addEventListener('click', function () { closeModal(record); });
    }

    window.requestAnimationFrame(function () {
      var dialog = q('.modal', backdrop);
      if (!dialog) { return; }
      var target = q('[autofocus], input:not([type="hidden"]):not([disabled]), textarea:not([disabled]), select:not([disabled]), button:not([disabled])', dialog);
      if (target) {
        target.focus();
      } else {
        dialog.focus();
      }
    });

    return record;
  }

  function closeModal(record) {
    if (!record || record.closed) { return; }
    var index = modalStack.indexOf(record);
    if (index === -1) { return; }
    record.closed = true;
    modalStack.splice(index, 1);
    if (record.el.parentNode) { record.el.parentNode.removeChild(record.el); }
    if (!modalStack.length) {
      document.body.classList.remove('modal-open');
    }
    if (record.onClose) {
      record.onClose();
    }
    if (!modalStack.length && record.previousFocus && typeof record.previousFocus.focus === 'function' &&
      document.documentElement.contains(record.previousFocus)) {
      record.previousFocus.focus();
    }
  }

  function closeTopModal() {
    var top = modalStack[modalStack.length - 1];
    if (top && top.dismissible) { closeModal(top); }
  }

  function closeAllModals() {
    while (modalStack.length) {
      var top = modalStack[modalStack.length - 1];
      top.dismissible = true;
      closeModal(top);
    }
    state.roomDetail = null;
    state.passwordModalOpen = false;
  }

  function confirmDialog(options) {
    var opts = options || {};
    return new Promise(function (resolve) {
      var settled = false;
      var record = openModal({
        title: opts.title || '请确认',
        eyebrow: opts.eyebrow || '操作确认',
        className: opts.danger ? 'modal--danger' : '',
        dismissible: true,
        body: '<p class="modal-text">' + esc(opts.message || '') + '</p>' +
          (opts.note ? '<p class="modal-note">' + esc(opts.note) + '</p>' : ''),
        footer: '<button class="btn" type="button" data-confirm-cancel>' + esc(opts.cancelText || '取消') + '</button>' +
          '<button class="btn ' + (opts.danger ? 'btn-danger' : 'btn-accent') + '" type="button" data-confirm-ok>' +
          esc(opts.confirmText || '确认') + '</button>',
        onClose: function () {
          if (!settled) {
            settled = true;
            resolve(false);
          }
        }
      });
      var cancelButton = q('[data-confirm-cancel]', record.el);
      var okButton = q('[data-confirm-ok]', record.el);
      cancelButton.addEventListener('click', function () {
        if (settled) { return; }
        settled = true;
        closeModal(record);
        resolve(false);
      });
      okButton.addEventListener('click', function () {
        if (settled) { return; }
        settled = true;
        closeModal(record);
        resolve(true);
      });
    });
  }

  function toggleUserMenu(open) {
    var menu = q('.user-menu');
    var button = q('[data-action="toggle-user-menu"]');
    if (!menu) { return; }
    var next = typeof open === 'boolean' ? open : menu.hidden;
    menu.hidden = !next;
    if (button) { button.setAttribute('aria-expanded', next ? 'true' : 'false'); }
    if (next) { toggleMobileMenu(false); }
  }

  function toggleMobileMenu(open) {
    var menu = q('.mobile-menu');
    var button = q('[data-action="toggle-mobile-menu"]');
    if (!menu) { return; }
    var next = typeof open === 'boolean' ? open : menu.hidden;
    menu.hidden = !next;
    if (button) { button.setAttribute('aria-expanded', next ? 'true' : 'false'); }
    if (next) { toggleUserMenu(false); }
  }

  function closeMenus() {
    toggleUserMenu(false);
    toggleMobileMenu(false);
  }

  /* ======================================================================
     3. 会话与登录
     ====================================================================== */

  function setSession(data) {
    state.user = data && data.user ? data.user : null;
    state.csrf = data && data.csrf_token ? data.csrf_token : '';
    api.setCsrf(state.csrf);
    state.authExpired = false;
  }

  function clearSessionState() {
    state.user = null;
    state.csrf = '';
    state.week = null;
    state.config = null;
    state.configDraft = null;
    state.roomDetail = null;
    state.overview = null;
    state.overviewDate = '';
    state.accounts = [];
    state.adminAccounts = [];
    state.backups = [];
    state.logs = { items: [], total: 0, page: 1, pageSize: 20, actions: [] };
    state.adminDetail = null;
    state.accountFilter = 'all';
    state.logFilters = { action: '', account_id: '', from: '', to: '', page: 1 };
    state.loginLockUntil = 0;
    state.loginLockMessage = '';
    if (loginLockTimer) {
      window.clearInterval(loginLockTimer);
      loginLockTimer = null;
    }
    api.clearCsrf();
    renderSeq += 1;
  }

  function hideBoot() {
    var boot = document.getElementById('boot');
    if (boot && boot.parentNode) { boot.parentNode.removeChild(boot); }
    if (els.app) { els.app.hidden = false; }
  }

  function renderLogin(notice) {
    closeMenus();
    state.view = 'login';
    state.user = null;
    state.loginNotice = notice || '';
    renderAppShell();
    els.view.innerHTML = '<div class="login-view">' +
      '<div class="login-theme">' + themeSwitchHtml() + '</div>' +
      '<section class="login-card" aria-labelledby="loginTitle">' +
      '<div class="login-brand">' +
      '<img class="login-logo" src="/assets/logo.svg" alt="">' +
      '<div><p class="brand-name">RoomCheck</p><p class="login-brand-sub">寝室抽查管理系统</p></div>' +
      '</div>' +
      '<h1 class="login-title" id="loginTitle">登录</h1>' +
      '<p class="login-sub">使用楼层长或管理员账户进入系统</p>' +
      '<form class="login-form" id="loginForm" novalidate>' +
      '<label class="field"><span class="field-label">账户名</span>' +
      '<input class="input" id="loginUsername" name="username" type="text" autocomplete="username" autocapitalize="none" spellcheck="false" required autofocus>' +
      '</label>' +
      '<label class="field"><span class="field-label">密码</span>' +
      '<input class="input" id="loginPassword" name="password" type="password" autocomplete="current-password" required>' +
      '</label>' +
      '<p class="form-error" id="loginError" role="alert" ' + (state.loginNotice ? '' : 'hidden') + '>' + esc(state.loginNotice) + '</p>' +
      '<button class="btn btn-accent btn-block" type="submit" id="loginSubmit">登录</button>' +
      '</form>' +
      '<p class="login-foot">Asia/Shanghai · 每周查寝 · 数据保存在本机</p>' +
      '</section></div>';
    updateThemeControls();
    resumeLoginLock();
    window.requestAnimationFrame(function () {
      var input = q('#loginUsername');
      if (input) { input.focus(); }
    });
  }

  function showLoginMessage(message) {
    var error = q('#loginError');
    if (!error) { return; }
    error.textContent = String(message || '');
    error.hidden = !message;
  }

  function setLoginBusy(busy) {
    var button = q('#loginSubmit');
    if (!button) { return; }
    var locked = state.loginLockUntil > Date.now();
    button.disabled = !!(busy || locked);
    button.innerHTML = busy ? '<span class="spinner"></span>登录中…' : '登录';
  }

  function startLoginLock(seconds, message) {
    var wait = Number(seconds);
    if (!Number.isFinite(wait) || wait <= 0) {
      showLoginMessage(message || '账户已锁定，请稍后再试');
      return;
    }
    state.loginLockUntil = Date.now() + wait * 1000;
    state.loginLockMessage = message || '账户已锁定';
    if (loginLockTimer) { window.clearInterval(loginLockTimer); }
    updateLoginLockUI();
    loginLockTimer = window.setInterval(function () {
      if (state.loginLockUntil <= Date.now()) {
        window.clearInterval(loginLockTimer);
        loginLockTimer = null;
        state.loginLockUntil = 0;
        setLoginBusy(false);
        showLoginMessage('锁定时间已结束，可以重新尝试登录。');
      } else {
        updateLoginLockUI();
      }
    }, 1000);
  }

  function updateLoginLockUI() {
    if (!state.loginLockUntil) { return; }
    var remaining = Math.max(0, Math.ceil((state.loginLockUntil - Date.now()) / 1000));
    if (remaining <= 0) { return; }
    var minutes = Math.floor(remaining / 60);
    var seconds = remaining % 60;
    var countdown = (minutes > 0 ? minutes + ' 分 ' : '') + seconds + ' 秒';
    showLoginMessage((state.loginLockMessage || '账户已锁定') + '（' + countdown + '后重试）');
    var button = q('#loginSubmit');
    if (button) { button.disabled = true; }
  }

  function resumeLoginLock() {
    if (state.loginLockUntil > Date.now()) {
      updateLoginLockUI();
      if (!loginLockTimer) {
        loginLockTimer = window.setInterval(function () {
          if (state.loginLockUntil <= Date.now()) {
            window.clearInterval(loginLockTimer);
            loginLockTimer = null;
            state.loginLockUntil = 0;
            setLoginBusy(false);
            showLoginMessage('锁定时间已结束，可以重新尝试登录。');
          } else {
            updateLoginLockUI();
          }
        }, 1000);
      }
    }
  }

  function handleLoginSubmit(form) {
    var usernameInput = q('#loginUsername');
    var passwordInput = q('#loginPassword');
    var username = usernameInput ? usernameInput.value.trim() : '';
    var password = passwordInput ? passwordInput.value : '';

    if (!username || !password) {
      showLoginMessage('请输入账户名和密码。');
      if (usernameInput && !username) { usernameInput.focus(); }
      else if (passwordInput) { passwordInput.focus(); }
      return;
    }
    if (state.loginLockUntil > Date.now()) {
      updateLoginLockUI();
      return;
    }

    showLoginMessage('');
    setLoginBusy(true);
    api.post('/api/login', { username: username, password: password }, { skipAuthHandler: true })
      .then(function (data) {
        if (form) { form.reset(); }
        setSession(data);
        state.loginLockUntil = 0;
        state.loginLockMessage = '';
        if (loginLockTimer) {
          window.clearInterval(loginLockTimer);
          loginLockTimer = null;
        }
        hideBoot();
        renderAppShell();
        if (state.user && state.user.must_change_password) {
          state.view = 'password-required';
          els.view.innerHTML = '<div class="page"><div class="empty-state">' +
            '<div class="empty-icon">' + icon('lock') + '</div>' +
            '<h1 class="empty-title">请先修改初始密码</h1>' +
            '<p class="empty-text">为保障系统安全，管理员首次登录需要修改密码后才能继续使用。</p>' +
            '</div></div>';
          openPasswordModal(true, function () {
            navigateTo(defaultRoute(), true);
          });
        } else {
          navigateTo(defaultRoute(), true);
        }
      })
      .catch(function (err) {
        if (err && err.status === 423) {
          startLoginLock(err.retryAfter, err.message || '账户已锁定');
          setLoginBusy(false);
        } else {
          showLoginMessage(errorMessage(err));
          setLoginBusy(false);
          var passwordNow = q('#loginPassword');
          if (passwordNow) { passwordNow.focus(); }
        }
      });
  }

  function handleLogout() {
    closeMenus();
    api.post('/api/logout', {}, { skipAuthHandler: true }).catch(function () { /* 已失效也继续退出 */ }).then(function () {
      clearSessionState();
      closeAllModals();
      if (window.location.hash !== '#/login') {
        window.history.replaceState(null, '', '#/login');
      }
      renderLogin('已安全退出。');
    });
  }

  function handleUnauthorized() {
    if (state.view === 'login' || state.authExpired) { return; }
    state.authExpired = true;
    closeAllModals();
    clearSessionState();
    state.authExpired = true;
    if (window.location.hash !== '#/login') {
      window.history.replaceState(null, '', '#/login');
    }
    renderLogin('登录已过期，请重新登录。');
  }

  function handlePasswordRequired() {
    if (!state.user) {
      handleUnauthorized();
      return;
    }
    state.user.must_change_password = true;
    renderAppShell();
    if (!state.passwordModalOpen) {
      closeAllModals();
      openPasswordModal(true, function () {
        renderRoute();
      });
    }
  }

  /* ---- 修改密码 ---- */
  function openPasswordModal(mandatory, afterSuccess) {
    if (state.passwordModalOpen) { return; }
    state.passwordModalOpen = true;
    var body = '<form class="config-form" id="passwordForm" novalidate>' +
      (mandatory ? '<p class="modal-text">当前账户需要修改初始密码后才能继续操作。修改成功后其他会话将失效。</p>' : '') +
      '<label class="field"><span class="field-label">旧密码</span>' +
      '<input class="input" id="oldPassword" name="old_password" type="password" autocomplete="current-password" required autofocus>' +
      '</label>' +
      '<label class="field"><span class="field-label">新密码</span>' +
      '<input class="input" id="newPassword" name="new_password" type="password" autocomplete="new-password" required>' +
      '</label>' +
      '<label class="field"><span class="field-label">确认新密码</span>' +
      '<input class="input" id="confirmPassword" name="confirm_password" type="password" autocomplete="new-password" required>' +
      '</label>' +
      '<p class="form-error" id="passwordError" role="alert" hidden></p>' +
      '<div class="form-actions">' +
      (mandatory ? '' : '<button class="btn" type="button" data-action="close-modal">取消</button>') +
      '<button class="btn btn-accent" type="submit" id="passwordSubmit">' + (mandatory ? '修改并继续' : '保存新密码') + '</button>' +
      '</div></form>';

    var record = openModal({
      title: mandatory ? '请修改初始密码' : '修改密码',
      eyebrow: mandatory ? '安全设置' : '账户安全',
      body: body,
      dismissible: !mandatory,
      onClose: function () {
        state.passwordModalOpen = false;
      }
    });
    record.afterSuccess = typeof afterSuccess === 'function' ? afterSuccess : null;
    record.mandatory = !!mandatory;
  }

  function submitPasswordModal(form) {
    var oldInput = q('#oldPassword');
    var newInput = q('#newPassword');
    var confirmInput = q('#confirmPassword');
    var errorEl = q('#passwordError');
    var submit = q('#passwordSubmit');
    var oldPassword = oldInput ? oldInput.value : '';
    var newPassword = newInput ? newInput.value : '';
    var confirmPassword = confirmInput ? confirmInput.value : '';

    setFieldError(errorEl, '');
    if (!oldPassword || !newPassword) {
      setFieldError(errorEl, '请填写旧密码和新密码。');
      return;
    }
    if (newPassword !== confirmPassword) {
      setFieldError(errorEl, '两次输入的新密码不一致。');
      if (confirmInput) { confirmInput.focus(); }
      return;
    }
    if (!submit) { return; }
    var original = submit.innerHTML;
    submit.disabled = true;
    submit.innerHTML = '<span class="spinner"></span>保存中…';

    api.post('/api/password', { old_password: oldPassword, new_password: newPassword })
      .then(function (data) {
        api.setCsrf(data.csrf_token || '');
        state.csrf = data.csrf_token || '';
        if (state.user) { state.user.must_change_password = false; }
        var record = modalStack[modalStack.length - 1];
        var callback = record && record.afterSuccess ? record.afterSuccess : null;
        if (record) { closeModal(record); } else { state.passwordModalOpen = false; }
        state.passwordModalOpen = false;
        toast('密码已修改。', 'success');
        renderAppShell();
        if (callback) { callback(); }
      })
      .catch(function (err) {
        if (submit.isConnected) {
          submit.disabled = false;
          submit.innerHTML = original;
        }
        setFieldError(errorEl, errorMessage(err));
        if (err.status === 403 && err.code === 'password_change_required') {
          handlePasswordRequired();
        }
      });
  }

  /* ======================================================================
     4. 顶部导航 / 路由
     ====================================================================== */

  function navItems() {
    if (isAdmin()) {
      return [
        { href: '#/admin', label: '概览', match: function (hash) { return hash === '/admin' || hash === '/admin/'; } },
        { href: '#/admin/accounts', label: '账户管理', match: function (hash) { return hash.indexOf('/admin/accounts') === 0; } },
        { href: '#/admin/backups', label: '备份', match: function (hash) { return hash === '/admin/backups'; } },
        { href: '#/admin/logs', label: '日志', match: function (hash) { return hash === '/admin/logs'; } }
      ];
    }
    return [
      { href: '#/check', label: '查寝', match: function (hash) { return hash === '/check'; } },
      { href: '#/config', label: '寝室配置', match: function (hash) { return hash === '/config'; } }
    ];
  }

  function renderAppShell() {
    if (!state.user) {
      els.header.innerHTML = '';
      return;
    }
    var hash = currentHash();
    var nav = navItems().map(function (item) {
      var active = item.match(hash);
      return '<a class="nav-link' + (active ? ' is-active' : '') + '" href="' + item.href + '"' +
        (active ? ' aria-current="page"' : '') + '>' + esc(item.label) + '</a>';
    }).join('');
    var mobileNav = navItems().map(function (item) {
      var active = item.match(hash);
      return '<a class="nav-link' + (active ? ' is-active' : '') + '" href="' + item.href + '"' +
        (active ? ' aria-current="page"' : '') + '>' + esc(item.label) + '</a>';
    }).join('');
    var username = state.user.username || '用户';
    var displayName = state.user.display_name || username;
    var isFloorManager = state.user.role === 'floor_manager';

    els.header.innerHTML = '<header class="app-header">' +
      '<div class="header-inner">' +
      '<a class="brand" href="#' + defaultRoute() + '" aria-label="RoomCheck 首页">' +
      '<img class="brand-logo" src="/assets/logo.svg" alt="" width="36" height="36">' +
      '<span class="brand-text"><span class="brand-name">RoomCheck</span>' +
      '<span class="brand-sub">寝室抽查管理系统</span></span></a>' +
      '<nav class="main-nav" aria-label="主导航">' + nav + '</nav>' +
      '<div class="header-tools">' +
      '<div class="theme-slot">' + themeSwitchHtml() + '</div>' +
      '<div class="user-wrap">' +
      '<button class="user-button" type="button" data-action="toggle-user-menu" aria-haspopup="true" aria-expanded="false" aria-label="账户菜单">' +
      icon('users') + '<span class="user-name">' + esc(username) + '</span>' + icon('chevron') +
      '</button>' +
      '<div class="user-menu" hidden>' +
      '<div class="user-menu-head"><span class="user-menu-name">' + esc(displayName) + '</span>' +
      '<span class="user-menu-role">' + esc(roleLabel(state.user.role)) + ' · ' + esc(username) + '</span></div>' +
      '<button class="menu-item" type="button" data-action="open-password">' + icon('lock') + '修改密码</button>' +
      (isFloorManager ? '<button class="menu-item" type="button" data-action="goto-account">' + icon('edit') + '修改账户名</button>' : '') +
      '<button class="menu-item menu-item--danger" type="button" data-action="logout">' + icon('logout') + '退出登录</button>' +
      '</div></div>' +
      '<button class="icon-btn menu-toggle" type="button" data-action="toggle-mobile-menu" aria-label="打开导航菜单" aria-expanded="false">' + icon('menu') + '</button>' +
      '</div></div>' +
      '<div class="mobile-menu" hidden>' +
      '<nav class="mobile-nav" aria-label="移动端导航">' + mobileNav + '</nav>' +
      '<div class="theme-slot">' + themeSwitchHtml() + '</div>' +
      '</div></header>';
    updateThemeControls();
  }

  function parseRoute() {
    var raw = currentHash();
    var parts = raw.split('/').filter(Boolean);
    if (!parts.length) { return { name: 'root' }; }
    if (parts[0] === 'check' && parts.length === 1) { return { name: 'check' }; }
    if (parts[0] === 'config' && parts.length === 1) { return { name: 'config' }; }
    if (parts[0] === 'account' && parts.length === 1) { return { name: 'account' }; }
    if (parts[0] === 'login' && parts.length === 1) { return { name: 'login' }; }
    if (parts[0] === 'admin') {
      if (parts.length === 1) { return { name: 'admin-overview' }; }
      if (parts[1] === 'accounts' && parts.length === 2) { return { name: 'admin-accounts' }; }
      if (parts[1] === 'accounts' && parts.length === 3 && /^\d+$/.test(parts[2])) {
        return { name: 'admin-week', id: Number(parts[2]) };
      }
      if (parts[1] === 'backups' && parts.length === 2) { return { name: 'admin-backups' }; }
      if (parts[1] === 'logs' && parts.length === 2) { return { name: 'admin-logs' }; }
    }
    return { name: 'unknown' };
  }

  function navigateTo(path, forceRender) {
    var target = '#' + (path.charAt(0) === '/' ? path : '/' + path);
    if (window.location.hash === target) {
      if (forceRender) { renderRoute(); }
      return;
    }
    if (forceRender) {
      window.history.replaceState(null, '', target);
      renderRoute();
    } else {
      window.location.hash = target;
    }
  }

  function renderRoute() {
    closeMenus();
    if (!state.user) {
      renderLogin(state.loginNotice || '');
      state.loginNotice = '';
      return;
    }
    var route = parseRoute();
    if (route.name === 'login' || route.name === 'root' || route.name === 'unknown') {
      navigateTo(defaultRoute(), true);
      return;
    }
    if (isAdmin() && (route.name === 'check' || route.name === 'config')) {
      navigateTo('/admin', true);
      return;
    }
    if (isManager() && route.name.indexOf('admin-') === 0) {
      navigateTo('/check', true);
      return;
    }
    state.view = route.name;

    if (route.name === 'admin-week' && (!route.id || route.id <= 0)) {
      navigateTo('/admin/accounts', true);
      return;
    }

    renderAppShell();
    switch (route.name) {
      case 'check':
        renderCheckPage();
        break;
      case 'config':
        renderConfigPage();
        break;
      case 'account':
        renderAccountPage();
        break;
      case 'admin-overview':
        renderOverviewPage();
        break;
      case 'admin-accounts':
        renderAccountsPage();
        break;
      case 'admin-week':
        renderAdminDetailPage(route.id);
        break;
      case 'admin-backups':
        renderBackupsPage();
        break;
      case 'admin-logs':
        renderLogsPage();
        break;
      default:
        navigateTo(defaultRoute(), true);
    }
  }

  /* ======================================================================
     5. 查寝周视图
     ====================================================================== */

  function activeWeek() {
    if (state.roomDetail && state.roomDetail.week) { return state.roomDetail.week; }
    if (state.view === 'admin-week' && state.adminDetail) { return state.adminDetail.week; }
    return state.week;
  }

  function findRoomInWeek(week, roomId) {
    if (!week || !Array.isArray(week.days)) { return null; }
    for (var d = 0; d < week.days.length; d += 1) {
      var groups = week.days[d].groups || [];
      for (var g = 0; g < groups.length; g += 1) {
        var rooms = groups[g].rooms || [];
        for (var r = 0; r < rooms.length; r += 1) {
          if (Number(rooms[r].id) === Number(roomId)) { return rooms[r]; }
        }
      }
    }
    return null;
  }

  function findGroupInWeek(week, groupId) {
    if (!week || !Array.isArray(week.days)) { return null; }
    for (var d = 0; d < week.days.length; d += 1) {
      var groups = week.days[d].groups || [];
      for (var g = 0; g < groups.length; g += 1) {
        if (Number(groups[g].id) === Number(groupId)) { return groups[g]; }
      }
    }
    return null;
  }

  function recalculateWeek(week) {
    if (!week || !Array.isArray(week.days)) { return; }
    var checked = 0;
    var total = 0;
    var nextGroup = null;
    var nextRoom = null;

    week.days.forEach(function (day) {
      (day.groups || []).forEach(function (group) {
        var rooms = group.rooms || [];
        var groupChecked = 0;
        rooms.forEach(function (room) {
          if (room.checked) { groupChecked += 1; }
        });
        group.checked = groupChecked;
        group.total = rooms.length;
        checked += groupChecked;
        total += rooms.length;
        if (!nextGroup && groupChecked < rooms.length) {
          nextGroup = group.id;
          var unchecked = rooms.filter(function (room) { return !room.checked; })[0];
          if (unchecked) { nextRoom = unchecked.id; }
        }
      });
    });

    week.progress = {
      checked: checked,
      total: total,
      percent: total > 0 ? Math.floor((checked / total) * 100) : 0,
      completed: total > 0 && checked >= total
    };
    week.next_group_id = nextGroup;
    week.next_room_id = nextRoom;
  }

  function setRoomCheckedLocal(room, checked) {
    room.checked = !!checked;
    var week = activeWeek();
    if (week) { recalculateWeek(week); }
  }

  function weekSummaryMeta(week) {
    var config = week && week.config ? week.config : null;
    if (!config) {
      return '<span class="meta-item">未配置寝室范围</span>';
    }
    return '<span class="meta-item">' + icon('building') + esc(config.building_name || '未命名楼栋') + '</span>' +
      '<span class="meta-item">范围 ' + esc(config.start_room) + '–' + esc(config.end_room) + '</span>' +
      '<span class="meta-item">应查 ' + esc(config.N) + ' 间</span>';
  }

  function progressHtml(week) {
    var progress = week.progress || { checked: 0, total: 0, percent: 0, completed: false };
    var percent = clampPercent(progress.percent);
    var checked = num(progress.checked);
    var total = num(progress.total);
    var completed = !!progress.completed;
    return '<div class="progress-block">' +
      '<div class="progress-head"><span class="progress-label">总体进度</span>' +
      '<span class="progress-value">已查 <strong data-progress-text>' + checked + '/' + total + '</strong> · ' +
      '<span data-progress-percent>' + percent + '%</span></span></div>' +
      '<div class="progress-track" role="progressbar" aria-label="本周查寝进度" aria-valuemin="0" aria-valuemax="100" aria-valuenow="' + percent + '" data-progress-track>' +
      '<div class="progress-fill" data-progress-fill style="width:' + percent + '%"></div></div>' +
      '<div class="progress-foot"><span class="progress-hint">已查 <strong data-progress-checked>' + checked +
      '</strong> / <span data-progress-total>' + total + '</span> 间</span>' +
      '<button class="btn btn-accent" type="button" data-action="continue-check"' + (completed || !week.next_room_id ? ' disabled' : '') + '>' +
      icon('chevron') + '继续检查</button></div>' +
      '<div class="progress-success" data-progress-success' + (completed ? '' : ' hidden') + '>' +
      icon('check') + '本周已全部完成，共 ' + total + ' 间</div>' +
      '</div>';
  }

  function renderWeekCard(week) {
    var info = week.week || {};
    var account = week.account || null;
    var accountHtml = account ? '<p class="week-range">' + esc(account.display_name || account.username || '') + ' · ' + esc(roleLabel(account.role)) + '</p>' : '';
    return '<section class="card week-card">' +
      '<div class="week-top"><div class="week-title">' +
      '<h2 class="week-label">' + esc(formatWeekRange(info.week_start, info.week_end)) + '</h2>' +
      accountHtml +
      '</div><div class="week-meta">' + weekSummaryMeta(week) + '</div></div>' +
      progressHtml(week) +
      '</section>';
  }

  function roomBadgeHtml(count, kindLabel, iconName, dataAttr, unit) {
    var value = num(count);
    return '<span class="room-badge' + (value === 0 ? ' is-zero' : '') + '" title="' + kindLabel + ' ' + value + ' ' + (unit || '条') + '">' +
      icon(iconName) + '<span ' + dataAttr + '>' + value + '</span></span>';
  }

  function roomCardHtml(room) {
    var checked = !!room.checked;
    var roomNumber = room.room_number === null || room.room_number === undefined ? '' : String(room.room_number);
    return '<div class="room-card' + (checked ? ' is-checked' : '') + '" data-room-id="' + num(room.id) +
      '" data-action="open-room">' +
      '<button class="room-check" type="button" data-action="room-check" aria-pressed="' + (checked ? 'true' : 'false') +
      '" aria-label="' + (checked ? '撤销 ' : '标记 ') + esc(roomNumber) + ' 的查寝勾选">' + icon('check') + '</button>' +
      '<button class="room-open" type="button" data-action="open-room" aria-label="查看 ' + esc(roomNumber) + ' 寝室详情">' +
      '<span class="room-number">' + esc(roomNumber) + '</span>' +
      '<span class="room-badges">' +
      roomBadgeHtml(room.notes_count, '备注', 'note', 'data-note-count', '条') +
      roomBadgeHtml(room.images_count, '图片', 'image', 'data-image-count', '张') +
      '</span></button></div>';
  }

  function groupHtml(group) {
    var period = group.period === 'evening' ? 'evening' : 'noon';
    var iconName = period === 'evening' ? 'moon' : 'sun';
    var label = group.label || PERIOD_LABELS[period] || '';
    var rooms = Array.isArray(group.rooms) ? group.rooms : [];
    return '<div class="group-card" data-group-id="' + num(group.id) + '" id="group-' + num(group.id) + '">' +
      '<div class="group-head"><span class="group-label"><span class="group-period">' + icon(iconName) + '</span>' + esc(label) + '</span>' +
      '<span class="group-count" data-group-count="' + num(group.id) + '">' + num(group.checked) + '/' + num(group.total) + '</span></div>' +
      (rooms.length ? '<div class="room-grid">' + rooms.map(roomCardHtml).join('') + '</div>' :
        '<p class="group-empty">本组无需查寝</p>') +
      '</div>';
  }

  function dayHtml(day) {
    var groups = Array.isArray(day.groups) ? day.groups.slice().sort(function (a, b) {
      var order = { noon: 0, evening: 1 };
      return (order[a.period] || 0) - (order[b.period] || 0);
    }) : [];
    return '<section class="day-card card' + (day.is_today ? ' is-today' : '') + '" data-day-index="' + num(day.day_index) + '">' +
      '<header class="day-head"><div><h3 class="day-name">' + esc(day.weekday || WEEKDAY_LABELS[day.day_index] || '') + '</h3>' +
      '<p class="day-date">' + esc(formatMonthDay(day.date)) + '</p></div>' +
      (day.is_today ? '<span class="today-badge">今天</span>' : '') +
      '</header><div class="group-list">' + groups.map(groupHtml).join('') + '</div></section>';
  }

  function renderDayGrid(week, filter) {
    var order = { 0: 0, 1: 1, 2: 2, 4: 3, 5: 4, 6: 5 };
    var days = Array.isArray(week.days) ? week.days.slice() : [];
    /* 周四绝不显示；按周一、周二、周三、周五、周六、周日的顺序渲染 */
    days = days.filter(function (day) {
      return Number(day.day_index) !== 3 && Object.prototype.hasOwnProperty.call(order, Number(day.day_index));
    }).sort(function (a, b) {
      return order[Number(a.day_index)] - order[Number(b.day_index)];
    });
    if (filter && filter !== 'all') {
      days = days.filter(function (day) { return String(day.day_index) === String(filter); });
    }
    if (!days.length) {
      return emptyStateCompactHtml('calendar', '没有对应的查寝安排', '本周该日期没有排班。');
    }
    return '<div class="day-grid" data-day-grid>' + days.map(dayHtml).join('') + '</div>';
  }

  function renderCheckPage() {
    state.view = 'check';
    renderAppShell();
    els.view.innerHTML = loadingPageHtml('查寝', '本周查寝');
    var token = ++renderSeq;
    api.get('/api/week').then(function (week) {
      if (token !== renderSeq || state.view !== 'check' || !state.user) { return; }
      state.week = week;
      renderCheckContent();
    }).catch(function (err) {
      if (token !== renderSeq || state.view !== 'check' || !state.user) { return; }
      renderViewError('加载本周查寝失败', errorMessage(err));
      reportError(err);
    });
  }

  function renderCheckContent() {
    var week = state.week;
    var head = pageHeadHtml({
      eyebrow: '查寝',
      title: '本周查寝',
      sub: '按排班逐间检查；勾选状态会即时保存到服务端。',
      actions: '<button class="btn" type="button" data-action="refresh-week">' + icon('refresh') + '刷新</button>' +
        '<a class="btn" href="#/config">' + icon('settings') + '寝室配置</a>'
    });

    if (!week || !week.week) {
      var config = week && week.config ? week.config : null;
      var empty = config ?
        emptyStateHtml('check', '当前没有可检查的寝室', '应查寝室数 N 为 0，请调整寝室范围或“无需查寝”列表。',
          '<a class="btn btn-accent" href="#/config">调整配置</a>') :
        emptyStateHtml('building', '尚未配置寝室范围', '先填写楼栋名称和寝室起止编号，系统将生成本周排班。',
          '<a class="btn btn-accent" href="#/config">去配置</a>');
      els.view.innerHTML = pageWrap(head, empty);
      return;
    }

    if (!Array.isArray(week.days) || !week.days.length) {
      els.view.innerHTML = pageWrap(head,
        emptyStateHtml('calendar', '本周还没有排班', '可能正在等待服务端生成排班，请稍后刷新。',
          '<button class="btn btn-accent" type="button" data-action="refresh-week">' + icon('refresh') + '刷新</button>'));
      return;
    }

    els.view.innerHTML = pageWrap(head,
      renderWeekCard(week) +
      '<div data-week-days>' + renderDayGrid(week, 'all') + '</div>');
    updateCheckViewDom();
  }

  function updateCheckViewDom() {
    var week = state.week;
    if (!week || !week.progress) { return; }
    var progress = week.progress;
    var percent = clampPercent(progress.percent);
    textOf(q('[data-progress-text]'), num(progress.checked) + '/' + num(progress.total));
    textOf(q('[data-progress-percent]'), percent + '%');
    textOf(q('[data-progress-checked]'), num(progress.checked));
    textOf(q('[data-progress-total]'), num(progress.total));
    var fill = q('[data-progress-fill]');
    if (fill) { fill.style.width = percent + '%'; }
    var track = q('[data-progress-track]');
    if (track) { track.setAttribute('aria-valuenow', String(percent)); }
    var success = q('[data-progress-success]');
    if (success) { success.hidden = !progress.completed; }
    var continueButton = q('[data-action="continue-check"]');
    if (continueButton) { continueButton.disabled = !!progress.completed || !week.next_room_id; }
    updateWeekCounters();
  }

  function updateWeekCounters() {
    var week = activeWeek();
    if (!week || !Array.isArray(week.days)) { return; }
    qa('[data-group-count]').forEach(function (element) {
      var group = findGroupInWeek(week, Number(element.getAttribute('data-group-count')));
      if (group) {
        element.textContent = num(group.checked) + '/' + num(group.total);
      }
    });
    qa('.room-card[data-room-id]').forEach(function (card) {
      var room = findRoomInWeek(week, Number(card.getAttribute('data-room-id')));
      if (!room) { return; }
      var checked = !!room.checked;
      card.classList.toggle('is-checked', checked);
      var check = q('.room-check', card);
      if (check) {
        check.setAttribute('aria-pressed', checked ? 'true' : 'false');
        var roomNumber = room.room_number === null || room.room_number === undefined ? '' : String(room.room_number);
        check.setAttribute('aria-label', (checked ? '撤销 ' : '标记 ') + roomNumber + ' 的查寝勾选');
      }
      var noteCount = q('[data-note-count]', card);
      if (noteCount) {
        noteCount.textContent = String(num(room.notes_count));
        var noteBadge = noteCount.parentNode;
        if (noteBadge) { noteBadge.classList.toggle('is-zero', num(room.notes_count) === 0); }
      }
      var imageCount = q('[data-image-count]', card);
      if (imageCount) {
        imageCount.textContent = String(num(room.images_count));
        var imageBadge = imageCount.parentNode;
        if (imageBadge) { imageBadge.classList.toggle('is-zero', num(room.images_count) === 0); }
      }
    });
  }

  function updateRoomDetailToggle() {
    if (!state.roomDetail || !state.roomDetail.room || !state.roomDetail.modal) { return; }
    var room = state.roomDetail.room;
    var toggle = q('[data-action="detail-toggle-check"]', state.roomDetail.modal);
    if (!toggle) { return; }
    toggle.classList.toggle('is-on', !!room.checked);
    toggle.setAttribute('aria-pressed', room.checked ? 'true' : 'false');
    toggle.innerHTML = '<span class="check-toggle-mark">' + icon('check') + '</span>' +
      '<span>' + (room.checked ? '本室已查' : '标记为已查') + '</span>';
  }

  function setCheckBusy(roomId, busy) {
    qa('.room-card[data-room-id="' + num(roomId) + '"] .room-check').forEach(function (button) {
      button.disabled = !!busy;
    });
    if (state.roomDetail && state.roomDetail.room && Number(state.roomDetail.room.id) === Number(roomId)) {
      var toggle = q('[data-action="detail-toggle-check"]', state.roomDetail.modal);
      if (toggle) { toggle.disabled = !!busy; }
    }
  }

  function applyCheckedResponse(room, response) {
    if (response && response.room) {
      Object.keys(response.room).forEach(function (key) { room[key] = response.room[key]; });
    }
    var week = state.roomDetail && state.roomDetail.room === room ? state.roomDetail.week : activeWeek();
    if (week) {
      recalculateWeek(week);
      if (response && response.progress) {
        week.progress = {
          checked: num(response.progress.checked),
          total: num(response.progress.total),
          percent: clampPercent(response.progress.percent),
          completed: !!response.progress.completed
        };
      }
      if (response && Object.prototype.hasOwnProperty.call(response, 'next_room_id')) {
        week.next_room_id = response.next_room_id;
      }
    }
    updateProgressDom(week);
    updateWeekCounters();
    updateRoomDetailToggle();
  }

  function submitCheck(room) {
    var roomId = Number(room.id);
    if (pendingChecks[roomId]) { return Promise.resolve(); }
    pendingChecks[roomId] = true;
    setCheckBusy(roomId, true);
    var previousChecked = !!room.checked;
    setRoomCheckedLocal(room, true);
    updateProgressDom(activeWeek());
    updateWeekCounters();
    updateRoomDetailToggle();

    return api.post('/api/checks/' + encodeURIComponent(String(roomId)) + '/check')
      .then(function (response) {
        applyCheckedResponse(room, response);
      })
      .catch(function (err) {
        setRoomCheckedLocal(room, previousChecked);
        updateProgressDom(activeWeek());
        updateWeekCounters();
        updateRoomDetailToggle();
        reportError(err);
      })
      .then(function () {
        delete pendingChecks[roomId];
        setCheckBusy(roomId, false);
      });
  }

  function submitUncheck(room) {
    var roomId = Number(room.id);
    if (pendingChecks[roomId]) { return Promise.resolve(); }
    pendingChecks[roomId] = true;
    setCheckBusy(roomId, true);
    var previousChecked = !!room.checked;

    return api.post('/api/checks/' + encodeURIComponent(String(roomId)) + '/uncheck', { confirm: true })
      .then(function (response) {
        applyCheckedResponse(room, response);
      })
      .catch(function (err) {
        setRoomCheckedLocal(room, previousChecked);
        updateProgressDom(activeWeek());
        updateWeekCounters();
        updateRoomDetailToggle();
        reportError(err);
      })
      .then(function () {
        delete pendingChecks[roomId];
        setCheckBusy(roomId, false);
      });
  }

  function handleRoomCardCheck(button) {
    var card = button.closest('.room-card');
    if (!card) { return; }
    var room = findRoomInWeek(activeWeek(), Number(card.getAttribute('data-room-id')));
    if (!room || pendingChecks[Number(room.id)]) { return; }

    if (room.checked) {
      confirmDialog({
        title: '撤销勾选？',
        message: '确认撤销 ' + (room.room_number || '') + ' 寝室的查寝勾选吗？',
        note: '撤销后该寝室会回到未查状态，此操作会记录到操作日志。',
        confirmText: '撤销勾选',
        danger: true
      }).then(function (ok) {
        if (ok) { submitUncheck(room); }
      });
    } else {
      submitCheck(room);
    }
  }

  function handleRoomDetailCheck() {
    var detail = state.roomDetail;
    if (!detail || !detail.room || pendingChecks[Number(detail.room.id)]) { return; }
    var room = detail.room;
    if (room.checked) {
      confirmDialog({
        title: '撤销勾选？',
        message: '确认撤销 ' + (room.room_number || '') + ' 寝室的查寝勾选吗？',
        note: '撤销后该寝室会回到未查状态，此操作会记录到操作日志。',
        confirmText: '撤销勾选',
        danger: true
      }).then(function (ok) {
        if (ok) { submitUncheck(room); }
      });
    } else {
      submitCheck(room);
    }
  }

  function handleContinueCheck() {
    var week = activeWeek();
    if (!week) { return; }
    var target = null;
    if (week.next_room_id) {
      target = q('.room-card[data-room-id="' + num(week.next_room_id) + '"]');
    }
    if (!target && week.next_group_id) {
      target = q('[data-group-id="' + num(week.next_group_id) + '"]');
    }
    if (!target) {
      target = q('.room-card:not(.is-checked)');
    }
    if (!target) {
      toast('本周已全部完成。', 'success');
      return;
    }
    target.scrollIntoView({ behavior: 'smooth', block: 'center' });
    target.classList.add('is-target');
    window.setTimeout(function () { target.classList.remove('is-target'); }, 2600);
  }

  /* ======================================================================
     6. 寝室详情：备注 / 图片
     ====================================================================== */

  function updateProgressDom(week) {
    if (!week || !week.progress) { return; }
    var progress = week.progress;
    var percent = clampPercent(progress.percent);
    textOf(q('[data-progress-text]'), num(progress.checked) + '/' + num(progress.total));
    textOf(q('[data-progress-percent]'), percent + '%');
    textOf(q('[data-progress-checked]'), num(progress.checked));
    textOf(q('[data-progress-total]'), num(progress.total));
    var fill = q('[data-progress-fill]');
    if (fill) { fill.style.width = percent + '%'; }
    var track = q('[data-progress-track]');
    if (track) { track.setAttribute('aria-valuenow', String(percent)); }
    var success = q('[data-progress-success]');
    if (success) { success.hidden = !progress.completed; }
    var continueButton = q('[data-action="continue-check"]');
    if (continueButton) { continueButton.disabled = !!progress.completed || !week.next_room_id; }
  }

  function detailIsActive(detail) {
    return !!detail && state.roomDetail === detail && !!detail.modal && document.documentElement.contains(detail.modal);
  }

  function markDetailDirty(detail) {
    if (!detail) { return; }
    detail.dirty = true;
    if (detail.room) {
      detail.room.notes_count = detail.notes.length;
      detail.room.images_count = detail.images.length;
    }
    updateWeekCounters();
  }

  function openRoomDetail(roomId) {
    var week = activeWeek();
    var room = findRoomInWeek(week, roomId);
    if (!room || !week) { return; }

    var detail = {
      roomId: Number(room.id),
      room: room,
      week: week,
      modal: null,
      notes: [],
      totalChars: 0,
      limit: 500,
      images: [],
      imageLimit: 50,
      notesLoading: true,
      imagesLoading: true,
      notesError: '',
      imagesError: '',
      editingNoteId: null,
      uploading: false,
      dirty: false
    };
    state.roomDetail = detail;

    var body = '<div class="room-summary">' +
      '<div class="room-summary-main">' +
      '<span class="room-key">' + esc(room.room_key || '') + '</span>' +
      '<span class="badge ' + (room.checked ? 'badge--accent' : 'badge--muted') + '" data-room-status>' +
      (room.checked ? '已查' : '未查') + '</span>' +
      '</div>' +
      '<button class="check-toggle' + (room.checked ? ' is-on' : '') + '" type="button" data-action="detail-toggle-check" aria-pressed="' +
      (room.checked ? 'true' : 'false') + '">' +
      '<span class="check-toggle-mark">' + icon('check') + '</span>' +
      '<span>' + (room.checked ? '本室已查' : '标记为已查') + '</span>' +
      '</button></div>' +
      '<section class="modal-section">' +
      '<div class="section-head"><h2 class="section-title">' + icon('note') + '备注</h2>' +
      '<span class="char-counter" data-note-meta>加载中…</span></div>' +
      '<div data-notes-slot>' + skeletonLines(4) + '</div>' +
      '</section>' +
      '<section class="modal-section">' +
      '<div class="section-head"><h2 class="section-title">' + icon('image') + '图片</h2>' +
      '<span class="badge badge--muted" data-image-meta>加载中…</span></div>' +
      '<div data-images-slot>' + skeletonLines(4) + '</div>' +
      '</section>';

    var record = openModal({
      title: (room.room_number || '寝室') + ' 寝室详情',
      eyebrow: '寝室详情',
      className: 'modal--wide',
      body: body,
      onClose: function () {
        var dirty = detail.dirty;
        if (state.roomDetail === detail) { state.roomDetail = null; }
        if (dirty) { refreshActiveWeekSilently(); }
      }
    });
    detail.modal = record.el;
    var status = q('[data-room-status]', record.el);
    if (status) {
      status.classList.toggle('badge--accent', !!room.checked);
      status.classList.toggle('badge--muted', !room.checked);
      status.textContent = room.checked ? '已查' : '未查';
    }
    loadRoomNotes(detail);
    loadRoomImages(detail);
  }

  function loadRoomNotes(detail) {
    detail.notesLoading = true;
    detail.notesError = '';
    if (detailIsActive(detail)) { renderNotesSection(detail); }
    api.get('/api/rooms/' + encodeURIComponent(String(detail.roomId)) + '/notes')
      .then(function (data) {
        if (!detailIsActive(detail) || state.roomDetail !== detail) { return; }
        detail.notes = Array.isArray(data.notes) ? data.notes : [];
        detail.totalChars = num(data.total_chars);
        detail.limit = num(data.limit, 500);
        detail.notesLoading = false;
        renderNotesSection(detail);
      })
      .catch(function (err) {
        if (!detailIsActive(detail) || state.roomDetail !== detail) { return; }
        detail.notesLoading = false;
        detail.notesError = errorMessage(err);
        renderNotesSection(detail);
        reportError(err);
      });
  }

  function loadRoomImages(detail) {
    detail.imagesLoading = true;
    detail.imagesError = '';
    if (detailIsActive(detail)) { renderImagesSection(detail); }
    api.get('/api/rooms/' + encodeURIComponent(String(detail.roomId)) + '/images')
      .then(function (data) {
        if (!detailIsActive(detail) || state.roomDetail !== detail) { return; }
        detail.images = Array.isArray(data.images) ? data.images : [];
        detail.imageLimit = num(data.limit, 50);
        detail.imagesLoading = false;
        renderImagesSection(detail);
      })
      .catch(function (err) {
        if (!detailIsActive(detail) || state.roomDetail !== detail) { return; }
        detail.imagesLoading = false;
        detail.imagesError = errorMessage(err);
        renderImagesSection(detail);
        reportError(err);
      });
  }

  function renderNotesSection(detail) {
    if (!detailIsActive(detail)) { return; }
    var slot = q('[data-notes-slot]', detail.modal);
    var meta = q('[data-note-meta]', detail.modal);
    if (!slot) { return; }
    if (detail.notesLoading) {
      if (meta) { meta.textContent = '加载中…'; }
      slot.innerHTML = skeletonLines(4);
      return;
    }
    if (detail.notesError) {
      if (meta) { meta.textContent = '加载失败'; }
      slot.innerHTML = '<div class="note-empty">' + icon('warning') +
        '<span>备注加载失败：' + esc(detail.notesError) + '</span>' +
        '<button class="btn btn-sm" type="button" data-action="retry-room-notes">重试</button></div>';
      return;
    }
    if (meta) {
      meta.textContent = '已用 ' + detail.totalChars + ' / ' + detail.limit + ' 字';
      meta.classList.toggle('is-over', detail.totalChars > detail.limit);
    }
    var notesHtml = detail.notes.length ? detail.notes.map(function (note) {
      return noteItemHtml(note, detail);
    }).join('') : '<div class="note-empty">' + icon('note') + '<span>还没有备注，可在下方添加。</span></div>';

    var formHtml = '<form class="note-form" id="roomNoteForm" novalidate>' +
      '<label class="field"><span class="field-label">添加备注</span>' +
      '<textarea class="textarea" id="roomNoteInput" placeholder="记录检查情况，例如：灯管损坏，已登记报修。" rows="3"></textarea>' +
      '</label>' +
      '<div class="note-form-foot"><span class="char-counter" id="roomNoteCounter">已用 ' + detail.totalChars +
      ' / ' + detail.limit + ' 字</span>' +
      '<button class="btn btn-accent" type="submit" id="roomNoteSubmit" disabled>' + icon('plus') + '添加备注</button></div>' +
      '<p class="form-error" id="roomNoteError" hidden></p>' +
      '</form>';

    slot.innerHTML = '<div class="note-list">' + notesHtml + '</div>' + formHtml;
    updateNoteEditCounter(detail);
    updateNewNoteCounter(detail);
  }

  function noteItemHtml(note, detail) {
    var editing = Number(detail.editingNoteId) === Number(note.id);
    var edited = note.updated_at && note.created_at && Number(note.updated_at) !== Number(note.created_at);
    var meta = '<div class="note-meta"><time>' + esc(note.created_at_text || formatShanghaiDateTime(note.created_at)) + '</time>' +
      (edited ? '<span class="badge badge--muted">已编辑</span>' : '') + '</div>';
    if (editing) {
      return '<article class="note-item" data-note-id="' + num(note.id) + '">' + meta +
        '<textarea class="textarea note-edit" id="roomNoteEditInput" rows="3">' + esc(note.content) + '</textarea>' +
        '<div class="note-actions">' +
        '<span class="char-counter" id="roomNoteEditCounter"></span>' +
        '<button class="btn btn-sm" type="button" data-action="cancel-note-edit">取消</button>' +
        '<button class="btn btn-accent btn-sm" type="button" data-action="save-note-edit" disabled>保存</button>' +
        '</div></article>';
    }
    return '<article class="note-item" data-note-id="' + num(note.id) + '">' + meta +
      '<p class="note-content">' + esc(note.content) + '</p>' +
      '<div class="note-actions">' +
      '<button class="btn btn-sm" type="button" data-action="edit-note" data-note-id="' + num(note.id) + '">' +
      icon('edit') + '编辑</button>' +
      '<button class="btn btn-sm text-danger" type="button" data-action="delete-note" data-note-id="' + num(note.id) + '">' +
      icon('trash') + '删除</button>' +
      '</div></article>';
  }

  function updateNewNoteCounter(detail) {
    if (!detailIsActive(detail)) { return; }
    var input = q('#roomNoteInput', detail.modal);
    var counter = q('#roomNoteCounter', detail.modal);
    var submit = q('#roomNoteSubmit', detail.modal);
    var error = q('#roomNoteError', detail.modal);
    if (!input || !counter || !submit) { return; }
    var draft = input.value;
    var projected = detail.totalChars + charCount(draft);
    counter.textContent = '已用 ' + projected + ' / ' + detail.limit + ' 字';
    var over = projected > detail.limit;
    counter.classList.toggle('is-over', over);
    submit.disabled = over || !draft.trim();
    setFieldError(error, over ? '备注合计不能超过 ' + detail.limit + ' 字，请删减后再提交。' : '');
  }

  function updateNoteEditCounter(detail) {
    if (!detailIsActive(detail) || !detail.editingNoteId) { return; }
    var input = q('#roomNoteEditInput', detail.modal);
    var counter = q('#roomNoteEditCounter', detail.modal);
    var save = q('[data-action="save-note-edit"]', detail.modal);
    if (!input || !counter || !save) { return; }
    var note = detail.notes.filter(function (item) {
      return Number(item.id) === Number(detail.editingNoteId);
    })[0];
    if (!note) { return; }
    var projected = detail.totalChars - charCount(note.content) + charCount(input.value);
    counter.textContent = '已用 ' + projected + ' / ' + detail.limit + ' 字';
    var over = projected > detail.limit;
    counter.classList.toggle('is-over', over);
    save.disabled = over || !input.value.trim();
  }

  function editNote(noteId) {
    var detail = state.roomDetail;
    if (!detail) { return; }
    detail.editingNoteId = Number(noteId);
    renderNotesSection(detail);
    window.requestAnimationFrame(function () {
      var input = q('#roomNoteEditInput');
      if (input) {
        input.focus();
        input.setSelectionRange(input.value.length, input.value.length);
      }
    });
  }

  function cancelNoteEdit() {
    var detail = state.roomDetail;
    if (!detail) { return; }
    detail.editingNoteId = null;
    renderNotesSection(detail);
  }

  function saveNoteEdit() {
    var detail = state.roomDetail;
    if (!detail || !detail.editingNoteId) { return; }
    var input = q('#roomNoteEditInput', detail.modal);
    var saveButton = q('[data-action="save-note-edit"]', detail.modal);
    if (!input) { return; }
    var content = input.value.trim();
    if (!content || saveButton && saveButton.disabled) { return; }
    var note = detail.notes.filter(function (item) {
      return Number(item.id) === Number(detail.editingNoteId);
    })[0];
    if (!note) { return; }
    var original = saveButton ? saveButton.innerHTML : '';
    if (saveButton) {
      saveButton.disabled = true;
      saveButton.innerHTML = '<span class="spinner"></span>保存中…';
    }
    api.patch('/api/notes/' + encodeURIComponent(String(note.id)), { content: content })
      .then(function (data) {
        if (!detailIsActive(detail)) { return; }
        if (data.note) {
          for (var i = 0; i < detail.notes.length; i += 1) {
            if (Number(detail.notes[i].id) === Number(note.id)) {
              detail.notes[i] = data.note;
              break;
            }
          }
        }
        detail.totalChars = num(data.total_chars, detail.totalChars);
        detail.editingNoteId = null;
        renderNotesSection(detail);
        markDetailDirty(detail);
        toast('备注已更新。', 'success');
      })
      .catch(function (err) {
        if (saveButton && saveButton.isConnected) {
          saveButton.disabled = false;
          saveButton.innerHTML = original;
        }
        reportError(err);
      });
  }

  function submitNewNote() {
    var detail = state.roomDetail;
    if (!detail) { return; }
    var input = q('#roomNoteInput', detail.modal);
    var submit = q('#roomNoteSubmit', detail.modal);
    var error = q('#roomNoteError', detail.modal);
    if (!input || !submit) { return; }
    var content = input.value.trim();
    if (!content) {
      setFieldError(error, '请输入备注内容。');
      return;
    }
    if (detail.totalChars + charCount(content) > detail.limit) {
      setFieldError(error, '备注合计不能超过 ' + detail.limit + ' 字。');
      return;
    }
    var original = submit.innerHTML;
    submit.disabled = true;
    submit.innerHTML = '<span class="spinner"></span>添加中…';
    api.post('/api/rooms/' + encodeURIComponent(String(detail.roomId)) + '/notes', { content: content })
      .then(function (data) {
        if (!detailIsActive(detail)) { return; }
        if (data.note) { detail.notes.push(data.note); }
        detail.totalChars = num(data.total_chars, detail.totalChars + charCount(content));
        renderNotesSection(detail);
        markDetailDirty(detail);
        toast('备注已添加。', 'success');
      })
      .catch(function (err) {
        if (submit.isConnected) {
          submit.disabled = false;
          submit.innerHTML = original;
        }
        setFieldError(error, errorMessage(err));
        if (err.code === 'note_limit') { reportError(err); }
      });
  }

  function deleteNote(noteId) {
    var detail = state.roomDetail;
    if (!detail) { return; }
    var note = detail.notes.filter(function (item) {
      return Number(item.id) === Number(noteId);
    })[0];
    if (!note) { return; }
    confirmDialog({
      title: '删除备注？',
      message: '确认删除这条备注吗？',
      note: '备注内容不可恢复；若该备注关联图片，图片会一并删除。',
      confirmText: '删除备注',
      danger: true
    }).then(function (ok) {
      if (!ok) { return; }
      api.del('/api/notes/' + encodeURIComponent(String(noteId)))
        .then(function (data) {
          if (!detailIsActive(detail)) { return; }
          detail.notes = detail.notes.filter(function (item) {
            return Number(item.id) !== Number(noteId);
          });
          detail.editingNoteId = null;
          detail.totalChars = num(data.total_chars);
          renderNotesSection(detail);
          if (num(data.deleted_images) > 0) { loadRoomImages(detail); }
          markDetailDirty(detail);
          toast('备注已删除。', 'success');
        })
        .catch(reportError);
    });
  }

  function imageFileUrl(image) {
    var returned = image && typeof image.url === 'string' ? image.url : '';
    if (/^\/api\/images\/[^/]+\/file$/.test(returned)) { return returned; }
    return api.imageUrl(image.id);
  }

  function imageItemHtml(image, deleteAction) {
    var id = num(image.id);
    var name = image.original_name || '图片';
    var action = deleteAction || 'delete-image';
    return '<div class="image-item">' +
      '<button class="image-thumb" type="button" data-action="view-image" data-image-id="' + id +
      '" title="' + esc(name) + ' · ' + esc(image.size_text || '') + '" aria-label="查看原图 ' + esc(name) + '">' +
      '<img src="' + esc(imageFileUrl(image)) + '" alt="' + esc(name) + '" loading="lazy">' +
      '</button>' +
      '<button class="image-delete" type="button" data-action="' + action + '" data-image-id="' + id +
      '" aria-label="删除图片 ' + esc(name) + '">' + icon('trash') + '</button>' +
      '</div>';
  }

  function imagesGridHtml(detail) {
    if (!detail.images.length) {
      return '<div class="note-empty">' + icon('image') + '<span>还没有上传图片。</span></div>';
    }
    return detail.images.map(imageItemHtml).join('');
  }

  function renderImagesSection(detail) {
    if (!detailIsActive(detail)) { return; }
    var slot = q('[data-images-slot]', detail.modal);
    var meta = q('[data-image-meta]', detail.modal);
    if (!slot) { return; }
    if (detail.imagesLoading) {
      if (meta) { meta.textContent = '加载中…'; }
      slot.innerHTML = skeletonLines(4);
      return;
    }
    if (detail.imagesError) {
      if (meta) { meta.textContent = '加载失败'; }
      slot.innerHTML = '<div class="note-empty">' + icon('warning') +
        '<span>图片加载失败：' + esc(detail.imagesError) + '</span>' +
        '<button class="btn btn-sm" type="button" data-action="retry-room-images">重试</button></div>';
      return;
    }
    if (meta) { meta.textContent = detail.images.length + ' / ' + detail.imageLimit + ' 张'; }
    var atLimit = detail.images.length >= detail.imageLimit;
    slot.innerHTML = '<div class="image-grid" data-image-grid>' + imagesGridHtml(detail) + '</div>' +
      '<div class="upload-zone">' +
      '<input class="file-input" id="roomImageInput" type="file" accept="image/*" multiple>' +
      '<button class="btn" type="button" data-action="pick-images" id="roomImageUploadButton"' +
      (atLimit || detail.uploading ? ' disabled' : '') + '>' +
      icon(atLimit ? 'image' : 'plus') + (atLimit ? '已达 ' + detail.imageLimit + ' 张上限' : '上传图片') + '</button>' +
      '<p class="field-hint">单张不超过 10MB，每间寝室最多 ' + detail.imageLimit + ' 张；支持 JPG、PNG、WebP 等图片格式，服务端会校验文件类型。</p>' +
      '<div class="upload-list" data-upload-list></div>' +
      '</div>';
  }

  function refreshImagesGrid(detail) {
    if (!detailIsActive(detail)) { return; }
    var meta = q('[data-image-meta]', detail.modal);
    if (meta) { meta.textContent = detail.images.length + ' / ' + detail.imageLimit + ' 张'; }
    var grid = q('[data-image-grid]', detail.modal);
    if (grid) { grid.innerHTML = imagesGridHtml(detail); }
    var uploadButton = q('#roomImageUploadButton', detail.modal);
    if (uploadButton) {
      var atLimit = detail.images.length >= detail.imageLimit;
      uploadButton.disabled = atLimit || detail.uploading;
      uploadButton.innerHTML = atLimit ?
        icon('image') + '已达 ' + detail.imageLimit + ' 张上限' :
        icon('plus') + '上传图片';
    }
    markDetailDirty(detail);
  }

  function addUploadItem(list, file) {
    var item = document.createElement('div');
    item.className = 'upload-item';
    item.innerHTML = '<div class="upload-name"><span>' + esc(file.name) + '</span>' +
      '<span class="upload-meta">' + esc(formatFileSize(file.size)) + '</span></div>' +
      '<div class="upload-progress"><div class="upload-bar"></div></div>' +
      '<p class="upload-status">等待上传…</p>';
    list.appendChild(item);
    return {
      bar: q('.upload-bar', item),
      status: q('.upload-status', item)
    };
  }

  function markUploadError(item, message) {
    item.status.classList.add('is-error');
    item.status.textContent = message;
    item.bar.style.width = '0%';
  }

  function handleImageFiles(files) {
    var detail = state.roomDetail;
    if (!detail || !detailIsActive(detail) || detail.uploading) { return; }
    var list = q('[data-upload-list]', detail.modal);
    if (!list) { return; }
    var queue = Array.prototype.slice.call(files || []);
    if (!queue.length) { return; }
    detail.uploading = true;
    var uploadButton = q('#roomImageUploadButton', detail.modal);
    if (uploadButton) { uploadButton.disabled = true; }

    var uploadedCount = 0;

    function uploadNext(index) {
      if (index >= queue.length) {
        detail.uploading = false;
        var button = q('#roomImageUploadButton', detail.modal);
        if (button) {
          var atLimit = detail.images.length >= detail.imageLimit;
          button.disabled = atLimit;
          button.innerHTML = atLimit ? icon('image') + '已达 ' + detail.imageLimit + ' 张上限' : icon('plus') + '上传图片';
        }
        if (uploadedCount > 0) {
          toast('已上传 ' + uploadedCount + ' 张图片。', 'success');
        }
        return;
      }
      if (!detailIsActive(detail) || state.authExpired) {
        detail.uploading = false;
        return;
      }
      var file = queue[index];
      var item = addUploadItem(list, file);

      if (!file.type || file.type.indexOf('image/') !== 0) {
        markUploadError(item, '仅支持图片文件。');
        uploadNext(index + 1);
        return;
      }
      if (file.size > 10 * 1024 * 1024) {
        markUploadError(item, '文件超过 10MB，未上传。');
        uploadNext(index + 1);
        return;
      }
      if (detail.images.length >= detail.imageLimit) {
        markUploadError(item, '已达到每间寝室 ' + detail.imageLimit + ' 张上限。');
        uploadNext(index + 1);
        return;
      }

      item.status.textContent = '上传中 0%';
      api.upload('/api/rooms/' + encodeURIComponent(String(detail.roomId)) + '/images', file, {
        onProgress: function (percent) {
          if (!item.bar.isConnected) { return; }
          item.bar.style.width = Math.max(0, Math.min(100, percent)) + '%';
          item.status.textContent = '上传中 ' + percent + '%';
        }
      }).then(function (data) {
        if (data.image) {
          detail.images.push(data.image);
          uploadedCount += 1;
        }
        item.bar.style.width = '100%';
        item.status.textContent = '已上传';
        refreshImagesGrid(detail);
        uploadNext(index + 1);
      }).catch(function (err) {
        markUploadError(item, errorMessage(err));
        reportError(err);
        uploadNext(index + 1);
      });
    }

    uploadNext(0);
  }

  function deleteImage(imageId) {
    var detail = state.roomDetail;
    if (!detail) { return; }
    var image = detail.images.filter(function (item) {
      return Number(item.id) === Number(imageId);
    })[0];
    if (!image) { return; }
    confirmDialog({
      title: '删除图片？',
      message: '确认删除「' + (image.original_name || '图片') + '」吗？',
      note: '图片文件会从服务器删除，此操作不可恢复。',
      confirmText: '删除图片',
      danger: true
    }).then(function (ok) {
      if (!ok) { return; }
      api.del('/api/images/' + encodeURIComponent(String(imageId)))
        .then(function () {
          if (!detailIsActive(detail)) { return; }
          detail.images = detail.images.filter(function (item) {
            return Number(item.id) !== Number(imageId);
          });
          refreshImagesGrid(detail);
          toast('图片已删除。', 'success');
        })
        .catch(reportError);
    });
  }

  function openOriginalImage(imageId) {
    var numericId = Number(imageId);
    if (!Number.isFinite(numericId) || numericId <= 0) { return; }
    var link = document.createElement('a');
    link.href = api.imageUrl(numericId);
    link.target = '_blank';
    link.rel = 'noopener noreferrer';
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
  }

  function refreshActiveWeekSilently() {
    if (!state.user || state.authExpired) { return; }
    if (state.view === 'admin-week' && state.adminDetail) {
      api.get('/api/admin/accounts/' + encodeURIComponent(String(state.adminDetail.accountId)) + '/week')
        .then(function (data) {
          if (state.view !== 'admin-week' || !state.adminDetail) { return; }
          state.adminDetail.week = data;
          if (data.account) { state.adminDetail.account = data.account; }
          updateProgressDom(data);
          updateWeekCounters();
        })
        .catch(function () { /* 静默刷新失败不影响本地数据 */ });
      return;
    }
    if (state.view === 'check') {
      api.get('/api/week')
        .then(function (data) {
          if (state.view !== 'check') { return; }
          state.week = data;
          updateProgressDom(data);
          updateWeekCounters();
        })
        .catch(function () { /* 静默刷新失败不影响本地数据 */ });
    }
  }

  /* ======================================================================
     7. 配置页
     ====================================================================== */

  function configDraftFrom(config) {
    var source = config || {};
    return {
      building_name: source.building_name || '',
      start_room: source.start_room === null || source.start_room === undefined ? '' : String(source.start_room),
      end_room: source.end_room === null || source.end_room === undefined ? '' : String(source.end_room),
      excluded_rooms: Array.isArray(source.excluded_rooms) ? source.excluded_rooms.map(function (room) {
        return String(room);
      }) : []
    };
  }

  function validateConfigDraft(draft) {
    var errors = [];
    var building = String(draft.building_name || '').trim();
    if (!building) { errors.push('请填写楼栋名称。'); }

    var startRaw = String(draft.start_room || '').trim();
    var endRaw = String(draft.end_room || '').trim();
    var startNum = /^\d+$/.test(startRaw) ? Number(startRaw) : null;
    var endNum = /^\d+$/.test(endRaw) ? Number(endRaw) : null;

    if (startNum === null) { errors.push('起始寝室号必须为数字。'); }
    if (endNum === null) { errors.push('结束寝室号必须为数字。'); }
    if (startNum !== null && endNum !== null && endNum < startNum) {
      errors.push('结束寝室号必须大于或等于起始寝室号。');
    }

    var original = startNum !== null && endNum !== null && endNum >= startNum ? endNum - startNum + 1 : null;
    var validExclusions = [];
    var invalidExclusions = [];
    var seen = Object.create(null);

    (Array.isArray(draft.excluded_rooms) ? draft.excluded_rooms : []).forEach(function (raw) {
      var value = String(raw || '').trim();
      var numeric = /^\d+$/.test(value) ? Number(value) : null;
      if (numeric === null) {
        invalidExclusions.push({ value: value, reason: '格式无效' });
        return;
      }
      if (startNum !== null && endNum !== null && (numeric < startNum || numeric > endNum)) {
        invalidExclusions.push({ value: value, reason: '不在当前范围' });
        return;
      }
      if (seen[numeric]) {
        invalidExclusions.push({ value: value, reason: '重复' });
        return;
      }
      seen[numeric] = true;
      validExclusions.push(value);
    });

    var n = original !== null ? original - validExclusions.length : null;
    if (n !== null && n <= 0) {
      errors.push('应查寝室数 N 必须大于 0，请减少无需查寝寝室或调整范围。');
    }
    if (invalidExclusions.length) {
      errors.push('有 ' + invalidExclusions.length + ' 个无需查寝寝室无效，请移除。');
    }

    return {
      valid: errors.length === 0 && n !== null && n > 0,
      errors: errors,
      original: original,
      n: n,
      validExclusions: validExclusions,
      invalidExclusions: invalidExclusions,
      building: building,
      startNum: startNum,
      endNum: endNum,
      startRaw: startRaw,
      endRaw: endRaw
    };
  }

  function chipsHtml(draft, validation) {
    if (!Array.isArray(draft.excluded_rooms) || !draft.excluded_rooms.length) {
      return '<span class="field-hint">当前没有无需查寝的寝室。</span>';
    }
    return draft.excluded_rooms.map(function (room) {
      var value = String(room);
      var invalid = validation.invalidExclusions.some(function (item) {
        return String(item.value) === value;
      });
      return '<span class="chip' + (invalid ? ' is-invalid' : '') + '" title="' + (invalid ? '该寝室号无效或已重复' : '无需查寝') + '">' +
        esc(value) + '<button class="chip-remove" type="button" data-action="excluded-remove" data-room="' + esc(value) +
        '" aria-label="移除 ' + esc(value) + '">' + icon('close') + '</button></span>';
    }).join('');
  }

  function configFormHtml(draft, accountId) {
    return '<form class="config-form" id="configForm" novalidate data-account-id="' + (accountId ? num(accountId) : '') + '">' +
      '<div class="field"><span class="field-label">楼栋名称</span>' +
      '<input class="input" name="building_name" type="text" maxlength="40" placeholder="例如：秀苑四舍" autocomplete="off" value="' + esc(draft.building_name) + '">' +
      '</div>' +
      '<div class="form-grid">' +
      '<label class="field"><span class="field-label">起始寝室号</span>' +
      '<input class="input" name="start_room" type="text" inputmode="numeric" placeholder="例如：634" autocomplete="off" value="' + esc(draft.start_room) + '"></label>' +
      '<label class="field"><span class="field-label">结束寝室号</span>' +
      '<input class="input" name="end_room" type="text" inputmode="numeric" placeholder="例如：665" autocomplete="off" value="' + esc(draft.end_room) + '"></label>' +
      '</div>' +
      '<div class="field"><span class="field-label">无需查寝寝室</span>' +
      '<p class="field-hint">必须位于当前范围内且不能重复；这些寝室不会出现在抽查列表中。</p>' +
      '<div class="excluded-editor">' +
      '<div class="excluded-input-row"><input class="input" id="excludedInput" type="text" inputmode="numeric" placeholder="输入寝室号，例如 635" autocomplete="off">' +
      '<button class="btn" type="button" data-action="excluded-add">' + icon('plus') + '添加</button></div>' +
      '<p class="field-error" id="excludedError" hidden></p>' +
      '<div class="chips" id="excludedChips"></div>' +
      '</div></div>' +
      '<p class="form-error" id="configError" hidden></p>' +
      '<div class="form-actions"><button class="btn btn-accent" type="submit" id="configSave">' + icon('check') + '保存配置</button></div>' +
      '</form>';
  }

  function configPreviewHtml() {
    return '<aside class="card config-preview">' +
      '<div class="section-head"><h2 class="section-title">' + icon('chart') + '应查数量预览</h2></div>' +
      '<div class="preview-formula">' +
      '<div class="preview-part"><span class="preview-number" id="previewOriginal">—</span><span class="preview-label">原始总数</span></div>' +
      '<span class="preview-op">−</span>' +
      '<div class="preview-part"><span class="preview-number" id="previewExcluded">—</span><span class="preview-label">无需查寝</span></div>' +
      '<span class="preview-op">=</span>' +
      '<div class="preview-part preview-part--accent"><span class="preview-number" id="previewN">—</span><span class="preview-label">应查 N</span></div>' +
      '</div>' +
      '<p class="field-hint" id="previewHint">N = 原始总数 − 无需查寝数。</p>' +
      '<p class="field-hint">修改范围只会增量更新本周排班：保留已有寝室的日期、时段与勾选状态，新增寝室补入空位，移出寝室的备注和图片仍保留在库中。保存不会清空查寝进度。</p>' +
      '</aside>';
  }

  function configLayoutHtml(draft, accountId) {
    return '<div class="config-layout">' +
      '<section class="card form-card">' +
      '<div><h2 class="section-title">' + icon('building') + '寝室范围</h2>' +
      '<p class="section-desc">设置本楼层负责的楼栋与寝室编号。范围包含起止两端。</p></div>' +
      configFormHtml(draft, accountId) +
      '</section>' +
      configPreviewHtml() +
      '</div>';
  }

  function weekActionsHtml(accountId, week, account) {
    var progress = week && week.progress ? week.progress : null;
    var scopeText = accountId && account ?
      '以下操作作用于楼层长「' + esc(account.username || '') + '」的本周数据。' :
      '以下操作只影响本周排班与勾选。';
    var progressHint = progress ? '当前进度：已查 ' + num(progress.checked) + ' / ' + num(progress.total) + ' 间。' : '本周尚无排班。';
    return '<section class="card danger-zone">' +
      '<div class="section-head"><div><h2 class="section-title">' + icon('warning') + '本周排班操作</h2>' +
      '<p class="field-hint">' + scopeText + progressHint + '</p></div></div>' +
      '<div class="action-list">' +
      '<div class="action-row"><div class="action-copy"><h3 class="action-title">重新生成本周</h3>' +
      '<p class="action-text">重新随机生成本周排班，并清空本周所有已查勾选；备注与图片保留。此操作必须手动确认，不会自动执行。</p></div>' +
      '<button class="btn" type="button" data-action="regenerate-week">' + icon('refresh') + '重新生成本周</button></div>' +
      '<div class="action-row"><div class="action-copy"><h3 class="action-title">开始新一周 / 重置本周</h3>' +
      '<p class="action-text">清空本周排班与勾选，并开始新一周。备注和图片会迁移到新周，仅新周排班中的寝室保留；旧周图片文件会被删除。此操作不可撤销。</p></div>' +
      '<button class="btn btn-danger" type="button" data-action="reset-week">' + icon('calendar') + '开始新一周</button></div>' +
      '</div></section>';
  }

  function refreshConfigUI() {
    var draft = state.configDraft;
    if (!draft) { return; }
    var validation = validateConfigDraft(draft);
    var chips = q('#excludedChips');
    if (chips) { chips.innerHTML = chipsHtml(draft, validation); }
    var excludedError = q('#excludedError');
    if (excludedError) {
      if (validation.invalidExclusions.length) {
        var messages = validation.invalidExclusions.map(function (item) {
          return item.value + '：' + item.reason;
        });
        setFieldError(excludedError, '无效的寝室：' + messages.join('；'));
      } else {
        setFieldError(excludedError, '');
      }
    }
    var previewOriginal = q('#previewOriginal');
    var previewExcluded = q('#previewExcluded');
    var previewN = q('#previewN');
    var previewHint = q('#previewHint');
    if (previewOriginal) { previewOriginal.textContent = validation.original === null ? '—' : String(validation.original); }
    if (previewExcluded) { previewExcluded.textContent = String(validation.validExclusions.length); }
    if (previewN) { previewN.textContent = validation.n === null ? '—' : String(validation.n); }
    if (previewHint) {
      if (validation.invalidExclusions.length) {
        previewHint.textContent = '存在无效的无需查寝寝室，请移除后再保存。';
      } else if (validation.n !== null && validation.n <= 0) {
        previewHint.textContent = '应查寝室数 N 必须大于 0，当前无法保存。';
      } else {
        previewHint.textContent = 'N = 原始总数 − 无需查寝数。';
      }
    }
    var error = q('#configError');
    if (error) {
      var errorText = validation.errors.join(' ');
      setFieldError(error, errorText);
    }
    var save = q('#configSave');
    if (save) { save.disabled = !validation.valid; }
    var form = q('#configForm');
    if (form) { form.setAttribute('data-valid', validation.valid ? 'true' : 'false'); }
  }

  function configDraftFromForm() {
    var form = q('#configForm');
    if (!form || !state.configDraft) { return state.configDraft; }
    var building = q('[name="building_name"]', form);
    var start = q('[name="start_room"]', form);
    var end = q('[name="end_room"]', form);
    state.configDraft.building_name = building ? building.value : '';
    state.configDraft.start_room = start ? start.value.trim() : '';
    state.configDraft.end_room = end ? end.value.trim() : '';
    return state.configDraft;
  }

  function addExcludedRoom() {
    var input = q('#excludedInput');
    var error = q('#excludedError');
    if (!input || !state.configDraft) { return; }
    var value = input.value.trim();
    setFieldError(error, '');
    if (!value) {
      setFieldError(error, '请输入寝室号。');
      input.focus();
      return;
    }
    if (!/^\d+$/.test(value)) {
      setFieldError(error, '寝室号只能包含数字。');
      input.focus();
      return;
    }
    var draft = configDraftFromForm();
    var validation = validateConfigDraft(draft);
    if (validation.startNum === null || validation.endNum === null || validation.endNum < validation.startNum) {
      setFieldError(error, '请先填写有效的起止寝室号。');
      return;
    }
    var numeric = Number(value);
    if (numeric < validation.startNum || numeric > validation.endNum) {
      setFieldError(error, '寝室 ' + value + ' 不在当前范围内。');
      input.focus();
      return;
    }
    if (draft.excluded_rooms.some(function (room) { return Number(room) === numeric; })) {
      setFieldError(error, '寝室 ' + value + ' 已在无需查寝列表中。');
      input.focus();
      return;
    }
    draft.excluded_rooms.push(value);
    input.value = '';
    refreshConfigUI();
    input.focus();
  }

  function removeExcludedRoom(value) {
    if (!state.configDraft) { return; }
    var numeric = Number(value);
    state.configDraft.excluded_rooms = state.configDraft.excluded_rooms.filter(function (room) {
      return Number(room) !== numeric;
    });
    refreshConfigUI();
    var input = q('#excludedInput');
    if (input) { input.focus(); }
  }

  function saveConfigForm(form) {
    if (!state.configDraft) { return; }
    var draft = configDraftFromForm();
    var validation = validateConfigDraft(draft);
    refreshConfigUI();
    if (!validation.valid) {
      var error = q('#configError');
      setFieldError(error, validation.errors.join(' '));
      return;
    }
    var accountId = Number(form.getAttribute('data-account-id')) || 0;
    var payload = {
      building_name: validation.building,
      start_room: validation.startRaw,
      end_room: validation.endRaw,
      excluded_rooms: validation.validExclusions.slice()
    };
    if (accountId > 0) { payload.account_id = accountId; }
    var saveButton = q('#configSave');
    var original = saveButton ? saveButton.innerHTML : '';
    if (saveButton) {
      saveButton.disabled = true;
      saveButton.innerHTML = '<span class="spinner"></span>保存中…';
    }

    api.put('/api/config', payload)
      .then(function (data) {
        state.config = data.config || null;
        var schedule = data.schedule || {};
        var message = '配置已保存：新增 ' + num(schedule.added) + ' 间，移除 ' + num(schedule.removed) +
          ' 间，保留 ' + num(schedule.retained) + ' 间，应查 ' + num(schedule.N || (data.config ? data.config.N : validation.n)) + ' 间。';
        toast(message, 'success');
        renderRoute();
      })
      .catch(function (err) {
        if (err && err.code === 'no_op') {
          toast('配置没有变化。', 'info');
          if (saveButton && saveButton.isConnected) {
            saveButton.disabled = false;
            saveButton.innerHTML = original;
          }
          return;
        }
        if (saveButton && saveButton.isConnected) {
          saveButton.disabled = false;
          saveButton.innerHTML = original;
        }
        setFieldError(q('#configError'), errorMessage(err));
        reportError(err);
      });
  }

  function regenerateWeek() {
    var accountId = state.view === 'admin-week' && state.adminDetail ? state.adminDetail.accountId : 0;
    var account = state.adminDetail && state.adminDetail.week ? state.adminDetail.week.account : null;
    var owner = accountId && account ? '楼层长「' + account.username + '」的' : '';
    confirmDialog({
      title: '重新生成本周排班？',
      message: '即将重新随机生成' + owner + '本周排班，并清空本周所有已查勾选。',
      note: '备注与图片会保留。此操作必须手动确认，不会自动执行；重新生成后原有勾选无法恢复。',
      confirmText: '重新生成',
      danger: true
    }).then(function (ok) {
      if (!ok) { return; }
      var payload = { confirm: true };
      if (accountId) { payload.account_id = accountId; }
      api.post('/api/schedule/regenerate', payload)
        .then(function () {
          toast('本周排班已重新生成。', 'success');
          renderRoute();
        })
        .catch(reportError);
    });
  }

  function resetWeek() {
    var accountId = state.view === 'admin-week' && state.adminDetail ? state.adminDetail.accountId : 0;
    var account = state.adminDetail && state.adminDetail.week ? state.adminDetail.week.account : null;
    var owner = accountId && account ? '楼层长「' + account.username + '」的' : '';
    confirmDialog({
      title: '开始新一周 / 重置本周？',
      message: '即将开始' + owner + '新的一周：清空本周排班和全部勾选。',
      note: '备注和图片会迁移到新周，但仅新周排班中存在的寝室会保留，其余备注和图片将被删除；旧周图片文件会被删除。此操作不可撤销。',
      confirmText: '开始新一周',
      danger: true
    }).then(function (ok) {
      if (!ok) { return; }
      var payload = { confirm: true };
      if (accountId) { payload.account_id = accountId; }
      api.post('/api/week/reset', payload)
        .then(function () {
          toast('已开始新一周，备注与图片已迁移。', 'success');
          renderRoute();
        })
        .catch(reportError);
    });
  }

  function renderConfigPage() {
    state.view = 'config';
    renderAppShell();
    els.view.innerHTML = loadingPageHtml('配置', '寝室配置');
    var token = ++renderSeq;
    Promise.all([api.get('/api/config'), api.get('/api/week')])
      .then(function (results) {
        if (token !== renderSeq || state.view !== 'config' || !state.user) { return; }
        state.config = results[0] ? results[0].config : null;
        state.week = results[1] || null;
        state.configDraft = configDraftFrom(state.config);
        renderConfigContent();
      })
      .catch(function (err) {
        if (token !== renderSeq || state.view !== 'config' || !state.user) { return; }
        renderViewError('加载寝室配置失败', errorMessage(err));
        reportError(err);
      });
  }

  function renderConfigContent() {
    var head = pageHeadHtml({
      eyebrow: '配置',
      title: '寝室配置',
      sub: '范围变更会增量更新本周排班，不会清空已有查寝勾选。',
      actions: '<button class="btn" type="button" data-action="refresh-week">' + icon('refresh') + '刷新</button>' +
        '<a class="btn" href="#/check">' + icon('check') + '返回查寝</a>'
    });
    els.view.innerHTML = pageWrap(head,
      configLayoutHtml(state.configDraft, 0) +
      weekActionsHtml(0, state.week, null));
    refreshConfigUI();
  }

  /* ======================================================================
     7.1 管理员周视图（复用查寝面板）
     ====================================================================== */

  function renderAdminDetailPage(accountId) {
    state.view = 'admin-week';
    renderAppShell();
    els.view.innerHTML = loadingPageHtml('管理员 · 周视图', '楼层长查寝详情');
    var token = ++renderSeq;
    api.get('/api/admin/accounts/' + encodeURIComponent(String(accountId)) + '/week')
      .then(function (week) {
        if (token !== renderSeq || state.view !== 'admin-week' || !state.user) { return; }
        state.adminDetail = {
          accountId: accountId,
          week: week,
          account: week.account || null,
          orphans: [],
          orphansLoading: true,
          orphansError: '',
          dayFilter: 'all'
        };
        state.configDraft = configDraftFrom(week.config);
        renderAdminDetailContent();
        loadAdminOrphans(accountId);
      })
      .catch(function (err) {
        if (token !== renderSeq || state.view !== 'admin-week' || !state.user) { return; }
        renderViewError('加载楼层长周视图失败', errorMessage(err));
        reportError(err);
      });
  }

  function adminDetailHeadHtml() {
    var detail = state.adminDetail;
    var week = detail.week;
    var account = detail.account || week.account || {};
    var status = statusBadge(account.status || 'enabled');
    var actions = '<button class="btn" type="button" data-action="refresh-week">' + icon('refresh') + '刷新</button>' +
      '<a class="btn" href="#/admin/accounts">' + icon('users') + '账户列表</a>';
    return '<div class="page-head"><div>' +
      '<a class="back-link" href="#/admin/accounts">' + icon('arrow-left') + '返回账户列表</a>' +
      '<h1 class="page-title">' + esc(account.username || '楼层长') + ' 的周视图</h1>' +
      '<p class="page-sub">' + esc(account.display_name || '') + ' · ' + status + ' · ' +
      esc(roleLabel(account.role || 'floor_manager')) + '</p>' +
      '</div><div class="page-actions">' + actions + '</div></div>';
  }

  function adminDayFilterHtml() {
    var current = state.adminDetail ? state.adminDetail.dayFilter : 'all';
    var options = '<option value="all"' + (current === 'all' ? ' selected' : '') + '>全部</option>';
    Object.keys(WEEKDAY_LABELS).forEach(function (key) {
      options += '<option value="' + key + '"' + (String(current) === key ? ' selected' : '') + '>' +
        WEEKDAY_LABELS[key] + '</option>';
    });
    return '<div class="section-head"><div><h2 class="section-title">每日查寝安排</h2>' +
      '<p class="field-hint">按日期筛选下方面板，周四不在查寝日范围内。管理员勾选操作会记录审计日志。</p></div>' +
      '<label class="field field--inline"><span class="field-label">日期筛选</span>' +
      '<select class="select" id="adminDayFilter" aria-label="按查寝日期筛选">' + options + '</select></label></div>';
  }

  function orphansSectionHtml() {
    var detail = state.adminDetail;
    if (!detail) { return ''; }
    if (detail.orphansLoading) {
      return '<section class="card"><div class="section-head"><h2 class="section-title">' +
        icon('trash') + '孤立备注与图片</h2></div>' + skeletonLines(5) + '</section>';
    }
    if (detail.orphansError) {
      return '<section class="card"><div class="section-head"><div><h2 class="section-title">' +
        icon('trash') + '孤立备注与图片</h2><p class="field-hint">已移出范围但数据仍保留的寝室。</p></div></div>' +
        '<div class="error-state"><div class="empty-icon">' + icon('warning') + '</div>' +
        '<p class="error-message">' + esc(detail.orphansError) + '</p>' +
        '<button class="btn" type="button" data-action="refresh-orphans">' + icon('refresh') + '重试</button></div></section>';
    }
    var items = (detail.orphans || []).filter(function (item) {
      return (Array.isArray(item.notes) && item.notes.length) || (Array.isArray(item.images) && item.images.length);
    });
    var content = items.length ? '<div class="orphan-list">' + items.map(function (item) {
      var notes = Array.isArray(item.notes) ? item.notes : [];
      var images = Array.isArray(item.images) ? item.images : [];
      var notesHtml = notes.length ? notes.map(function (note) {
        return '<article class="note-item"><div class="note-meta"><time>' +
          esc(note.created_at_text || formatShanghaiDateTime(note.created_at)) + '</time></div>' +
          '<p class="note-content">' + esc(note.content) + '</p>' +
          '<div class="note-actions"><button class="btn btn-sm text-danger" type="button" data-action="delete-orphan-note" data-note-id="' +
          num(note.id) + '">' + icon('trash') + '删除备注</button></div></article>';
      }).join('') : '<p class="field-hint">没有保留的备注。</p>';
      var imagesHtml = images.length ? '<div class="image-grid">' + images.map(function (image) {
        return imageItemHtml(image, 'delete-orphan-image');
      }).join('') + '</div>' : '<p class="field-hint">没有保留的图片。</p>';
      return '<article class="orphan-card">' +
        '<div class="orphan-head"><div><h3 class="orphan-room">' + esc(item.room_number || '') + '</h3>' +
        '<p class="row-sub">' + esc(item.room_key || '') + '</p></div>' +
        '<span class="badge">备注 ' + notes.length + ' · 图片 ' + images.length + '</span></div>' +
        '<div class="orphan-section"><span class="orphan-label">备注</span>' +
        '<div class="note-list">' + notesHtml + '</div></div>' +
        '<div class="orphan-section"><span class="orphan-label">图片</span>' + imagesHtml + '</div>' +
        '</article>';
    }).join('') + '</div>' : emptyStateCompactHtml('trash', '没有孤立备注或图片', '当前范围调整没有留下孤立数据。');

    return '<section class="card"><div class="section-head"><div><h2 class="section-title">' +
      icon('trash') + '孤立备注与图片</h2><p class="field-hint">这些寝室已移出当前范围，但备注和图片仍保留在数据库；可在此手动删除。</p></div>' +
      '<button class="btn btn-sm" type="button" data-action="refresh-orphans">' + icon('refresh') + '刷新</button></div>' +
      content + '</section>';
  }

  function renderOrphansSlot() {
    var wrap = q('[data-orphans-wrap]');
    if (wrap) { wrap.innerHTML = orphansSectionHtml(); }
  }

  function loadAdminOrphans(accountId) {
    if (!state.adminDetail || Number(state.adminDetail.accountId) !== Number(accountId)) { return; }
    state.adminDetail.orphansLoading = true;
    state.adminDetail.orphansError = '';
    renderOrphansSlot();
    api.get('/api/admin/orphans?account_id=' + encodeURIComponent(String(accountId)))
      .then(function (data) {
        if (!state.adminDetail || Number(state.adminDetail.accountId) !== Number(accountId)) { return; }
        state.adminDetail.orphans = Array.isArray(data.items) ? data.items : [];
        state.adminDetail.orphansLoading = false;
        state.adminDetail.orphansError = '';
        renderOrphansSlot();
      })
      .catch(function (err) {
        if (!state.adminDetail || Number(state.adminDetail.accountId) !== Number(accountId)) { return; }
        state.adminDetail.orphansLoading = false;
        state.adminDetail.orphansError = errorMessage(err);
        renderOrphansSlot();
        reportError(err);
      });
  }

  function renderAdminDetailContent() {
    var detail = state.adminDetail;
    if (!detail || !detail.week) { return; }
    var week = detail.week;
    var account = detail.account || week.account || {};
    var dayFilter = detail.dayFilter || 'all';
    var body = renderWeekCard(week) +
      '<div>' + adminDayFilterHtml() +
      '<div data-admin-days>' + renderDayGrid(week, dayFilter) + '</div></div>' +
      configLayoutHtml(state.configDraft, detail.accountId) +
      weekActionsHtml(detail.accountId, week, account) +
      '<div data-orphans-wrap>' + orphansSectionHtml() + '</div>';
    els.view.innerHTML = '<div class="page">' + adminDetailHeadHtml() +
      '<div class="page-body">' + body + '</div></div>';
    refreshConfigUI();
    updateProgressDom(week);
  }

  function renderAdminDetailDays() {
    if (!state.adminDetail || !state.adminDetail.week) { return; }
    var container = q('[data-admin-days]');
    if (!container) { return; }
    container.innerHTML = renderDayGrid(state.adminDetail.week, state.adminDetail.dayFilter);
  }

  function deleteOrphanNote(noteId) {
    confirmDialog({
      title: '删除孤立备注？',
      message: '确认删除这条孤立备注吗？',
      note: '该寝室已不在当前范围内，删除后备注及关联图片文件将不可恢复。',
      confirmText: '删除备注',
      danger: true
    }).then(function (ok) {
      if (!ok) { return; }
      api.del('/api/notes/' + encodeURIComponent(String(noteId)))
        .then(function () {
          toast('孤立备注已删除。', 'success');
          if (state.adminDetail) { loadAdminOrphans(state.adminDetail.accountId); }
        })
        .catch(reportError);
    });
  }

  function deleteOrphanImage(imageId) {
    confirmDialog({
      title: '删除孤立图片？',
      message: '确认删除这张孤立图片吗？',
      note: '图片文件会从服务器删除，此操作不可恢复。',
      confirmText: '删除图片',
      danger: true
    }).then(function (ok) {
      if (!ok) { return; }
      api.del('/api/images/' + encodeURIComponent(String(imageId)))
        .then(function () {
          toast('孤立图片已删除。', 'success');
          if (state.adminDetail) { loadAdminOrphans(state.adminDetail.accountId); }
        })
        .catch(reportError);
    });
  }

  /* ======================================================================
     8. 管理员：概览 / 账户管理 / 备份 / 日志
     ====================================================================== */

  function setButtonBusy(button, busy, label) {
    if (!button) { return; }
    if (busy) {
      if (!button.getAttribute('data-busy-original')) {
        button.setAttribute('data-busy-original', button.innerHTML);
      }
      button.disabled = true;
      button.innerHTML = '<span class="spinner"></span>' + esc(label || '处理中…');
      return;
    }
    var original = button.getAttribute('data-busy-original');
    if (original) {
      button.innerHTML = original;
      button.removeAttribute('data-busy-original');
    }
    button.disabled = false;
  }

  function findAccountById(accountId) {
    for (var i = 0; i < state.accounts.length; i += 1) {
      if (Number(state.accounts[i].id) === Number(accountId)) { return state.accounts[i]; }
    }
    return null;
  }

  function accountRemainingSeconds(account) {
    if (!account) { return 0; }
    if (account.remaining_seconds !== null && account.remaining_seconds !== undefined) {
      return Math.max(0, num(account.remaining_seconds));
    }
    if (account.delete_at) {
      return Math.max(0, Math.ceil(num(account.delete_at) - Date.now() / 1000));
    }
    return 0;
  }

  function accountConfigSummaryHtml(config) {
    if (!config) {
      return '<span class="row-sub">未配置</span>';
    }
    return '<div class="row-sub">' + esc(config.building_name || '未命名楼栋') + '</div>' +
      '<div class="row-sub">' + esc(config.start_room) + '–' + esc(config.end_room) +
      ' · 应查 ' + esc(config.N) + ' 间</div>';
  }

  function accountProgressHtml(account) {
    var progress = account ? account.progress : null;
    var weekLabel = account && account.week ? formatWeekRange(account.week.week_start, account.week.week_end) : '';
    if (!progress) {
      return '<div class="row-sub">暂无进度</div>' +
        (weekLabel ? '<div class="row-sub">' + esc(weekLabel) + '</div>' : '');
    }
    var percent = clampPercent(progress.percent);
    return '<div class="row-sub">' + (weekLabel ? esc(weekLabel) + ' · ' : '') +
      num(progress.checked) + '/' + num(progress.total) + ' · ' + percent + '%</div>' +
      '<div class="mini-progress"><div class="mini-fill" style="width:' + percent + '%"></div></div>';
  }

  function accountDeletionHtml(account) {
    if (!account || account.status !== 'deleted') {
      return '<span class="muted">—</span>';
    }
    var remaining = accountRemainingSeconds(account);
    var remainingText = remaining > 0 ?
      '剩余 ' + (account.remaining_text || formatRemaining(remaining)) :
      '已到期，等待自动删除';
    var countdownAttr = num(account.delete_at) > 0 ? ' data-countdown-at="' + num(account.delete_at) + '"' : '';
    return '<div>删除于 ' + esc(formatShanghaiDateTime(account.deleted_at)) + '</div>' +
      '<div>预计 ' + esc(formatShanghaiDateTime(account.delete_at)) + ' 永久删除</div>' +
      '<span class="countdown"' + countdownAttr + '>' + esc(remainingText) + '</span>';
  }

  function overviewRowHtml(account) {
    var username = account.username || '';
    var display = account.display_name || '';
    return '<tr class="is-clickable" data-action="open-account" data-account-id="' + num(account.id) +
      '" tabindex="0" role="link" aria-label="查看 ' + esc(username) + ' 的周视图">' +
      '<td data-label="账户"><div class="row-account"><span class="row-name">' + esc(username) + '</span>' +
      '<span class="row-sub">' + esc(display) + ' · ' + esc(roleLabel(account.role || 'floor_manager')) + '</span></div></td>' +
      '<td data-label="状态">' + statusBadge(account.status) + '</td>' +
      '<td data-label="寝室配置">' + accountConfigSummaryHtml(account.config) + '</td>' +
      '<td class="cell-progress" data-label="本周进度">' + accountProgressHtml(account) + '</td>' +
      '<td class="cell-countdown" data-label="删除倒计时">' + accountDeletionHtml(account) + '</td>' +
      '<td class="cell-chevron">' + icon('chevron') + '</td>' +
      '</tr>';
  }

  function overviewStatsHtml(overview) {
    var accounts = overview && Array.isArray(overview.accounts) ? overview.accounts : [];
    var totals = overview && overview.totals ? overview.totals : {};
    var deleted = accounts.filter(function (account) { return account.status === 'deleted'; }).length;
    var enabled = accounts.filter(function (account) { return account.status === 'enabled'; }).length;
    var totalAccounts = totals.accounts !== undefined ? num(totals.accounts) : accounts.length;
    var checked = num(totals.checked);
    var total = num(totals.total);
    var percent = clampPercent(totals.percent);
    return '<div class="stat-grid">' +
      '<article class="card stat-card"><span class="stat-icon">' + icon('users') + '</span>' +
      '<div class="stat-info"><span class="stat-label">楼层长</span>' +
      '<span class="stat-value">' + totalAccounts + '</span>' +
      '<span class="stat-hint">启用 ' + enabled + ' 位</span></div></article>' +
      '<article class="card stat-card"><span class="stat-icon">' + icon('check') + '</span>' +
      '<div class="stat-info"><span class="stat-label">已查 / 应查</span>' +
      '<span class="stat-value">' + checked + ' / ' + total + '</span>' +
      '<span class="stat-hint">本周全部楼层长合计</span></div></article>' +
      '<article class="card stat-card"><span class="stat-icon">' + icon('chart') + '</span>' +
      '<div class="stat-info"><span class="stat-label">完成百分比</span>' +
      '<span class="stat-value">' + percent + '%</span>' +
      '<span class="stat-hint">按已查 / 应查计算</span></div></article>' +
      '<article class="card stat-card"><span class="stat-icon">' + icon('clock') + '</span>' +
      '<div class="stat-info"><span class="stat-label">删除中账户</span>' +
      '<span class="stat-value">' + deleted + '</span>' +
      '<span class="stat-hint">72 天保留期内</span></div></article>' +
      '</div>';
  }

  function renderOverviewPage() {
    state.view = 'admin-overview';
    renderAppShell();
    els.view.innerHTML = loadingPageHtml('管理员', '管理概览');
    var token = ++renderSeq;
    var path = '/api/admin/overview';
    if (state.overviewDate) { path += '?date=' + encodeURIComponent(state.overviewDate); }
    api.get(path)
      .then(function (data) {
        if (token !== renderSeq || state.view !== 'admin-overview' || !state.user) { return; }
        state.overview = data || {};
        if (Array.isArray(data.accounts)) { state.accounts = data.accounts; }
        renderOverviewContent();
      })
      .catch(function (err) {
        if (token !== renderSeq || state.view !== 'admin-overview' || !state.user) { return; }
        renderViewError('加载管理概览失败', errorMessage(err));
        reportError(err);
      });
  }

  function overviewFilterHtml() {
    return '<section class="card"><form class="filter-grid filter-grid--single" id="overviewFilterForm" novalidate>' +
      '<label class="field"><span class="field-label">按日期统计</span>' +
      '<input class="input" type="date" id="overviewDate" name="date" value="' + esc(state.overviewDate) + '"></label>' +
      '<div class="filter-actions">' +
      '<button class="btn btn-accent" type="submit">' + icon('search') + '按日期统计</button>' +
      '<button class="btn" type="button" data-action="clear-overview-date">' + icon('close') + '查看本周</button>' +
      '</div></form></section>';
  }

  function renderOverviewContent() {
    var overview = state.overview || {};
    var accounts = Array.isArray(overview.accounts) ? overview.accounts : [];
    var head = pageHeadHtml({
      eyebrow: '管理员',
      title: '管理概览',
      sub: '统计日期：' + esc(overview.date || '—') + ' · 点击账户行可查看该楼层长的本周查寝详情。',
      actions: '<button class="btn" type="button" data-action="refresh-view">' + icon('refresh') + '刷新</button>' +
        '<a class="btn btn-accent" href="#/admin/accounts">' + icon('users') + '账户管理</a>'
    });
    var table;
    if (!accounts.length) {
      table = '<section class="card table-card">' +
        emptyStateCompactHtml('users', '还没有楼层长账户', '创建楼层长账户并配置寝室范围后，即可在这里查看整体进度。') +
        '<div class="form-actions"><a class="btn btn-accent" href="#/admin/accounts">新建楼层长</a></div>' +
        '</section>';
    } else {
      table = '<section class="card table-card">' +
        '<div class="section-head"><div><h2 class="section-title">楼层长本周进度</h2>' +
        '<p class="field-hint">点击任意账户行进入其周视图，可查看备注与图片、修改配置或调整排班。</p></div></div>' +
        '<div class="table-wrap"><table class="data-table">' +
        '<thead><tr><th>账户</th><th>状态</th><th>寝室配置</th><th>本周进度</th><th>删除倒计时</th><th></th></tr></thead>' +
        '<tbody>' + accounts.map(overviewRowHtml).join('') + '</tbody>' +
        '</table></div></section>';
    }
    els.view.innerHTML = pageWrap(head, overviewFilterHtml() + overviewStatsHtml(overview) + table);
  }

  /* ---- 账户管理 ---- */
  function accountsByFilter() {
    if (state.accountFilter === 'all') { return state.accounts.slice(); }
    return state.accounts.filter(function (account) { return account.status === state.accountFilter; });
  }

  function accountActionsHtml(account) {
    var id = num(account.id);
    var viewButton = '<button class="btn btn-sm" type="button" data-action="open-account" data-account-id="' + id + '">' +
      icon('eye') + '查看</button>';
    if (account.status === 'deleted') {
      var remaining = accountRemainingSeconds(account);
      return viewButton + '<button class="btn btn-sm" type="button" data-action="restore-account" data-account-id="' + id + '"' +
        (remaining > 0 ? '' : ' disabled') + '>' + icon('refresh') + (remaining > 0 ? '撤销删除' : '已到期') + '</button>';
    }
    var buttons = viewButton + '<button class="btn btn-sm" type="button" data-action="edit-account" data-account-id="' + id + '">' +
      icon('edit') + '编辑</button>';
    if (account.status === 'enabled') {
      buttons += '<button class="btn btn-sm" type="button" data-action="disable-account" data-account-id="' + id + '">禁用</button>';
    } else if (account.status === 'disabled') {
      buttons += '<button class="btn btn-sm" type="button" data-action="enable-account" data-account-id="' + id + '">' + icon('check') + '启用</button>';
    }
    buttons += '<button class="btn btn-sm text-danger" type="button" data-action="delete-account" data-account-id="' + id + '">' +
      icon('trash') + '删除</button>';
    return buttons;
  }

  function accountsTableHtml(accounts) {
    if (!accounts.length) {
      return '<div class="empty-state empty-state--compact"><div class="empty-icon">' + icon('users') + '</div>' +
        '<h3 class="empty-title">没有符合条件的账户</h3>' +
        '<p class="empty-text">切换筛选条件或新建楼层长账户。</p>' +
        '<button class="btn" type="button" data-action="account-filter" data-filter="all">查看全部账户</button></div>';
    }
    return '<div class="table-wrap"><table class="data-table">' +
      '<thead><tr><th>账户</th><th>状态</th><th>寝室配置</th><th>本周进度</th><th>删除信息</th><th>操作</th></tr></thead>' +
      '<tbody>' + accounts.map(function (account) {
        return '<tr>' +
          '<td data-label="账户"><div class="row-account"><span class="row-name">' + esc(account.username) + '</span>' +
          '<span class="row-sub">' + esc(account.display_name || '') + ' · ' + esc(roleLabel('floor_manager')) + '</span></div></td>' +
          '<td data-label="状态">' + statusBadge(account.status) + '</td>' +
          '<td data-label="寝室配置">' + accountConfigSummaryHtml(account.config) + '</td>' +
          '<td class="cell-progress" data-label="本周进度">' + accountProgressHtml(account) + '</td>' +
          '<td class="cell-countdown" data-label="删除信息">' + accountDeletionHtml(account) + '</td>' +
          '<td class="cell-actions" data-label="操作"><div class="row-actions">' + accountActionsHtml(account) + '</div></td>' +
          '</tr>';
      }).join('') + '</tbody></table></div>';
  }

  function findAdminAccountById(accountId) {
    for (var i = 0; i < state.adminAccounts.length; i += 1) {
      if (Number(state.adminAccounts[i].id) === Number(accountId)) { return state.adminAccounts[i]; }
    }
    return null;
  }

  function adminAccountActionsHtml(account) {
    var id = num(account.id);
    var enabledSuperAdmins = state.adminAccounts.filter(function (item) {
      return item.role === 'super_admin' && item.status === 'enabled';
    }).length;
    var protectedSuper = account.role === 'super_admin' && enabledSuperAdmins <= 1;
    if (account.status === 'deleted') {
      var remaining = accountRemainingSeconds(account);
      return '<button class="btn btn-sm" type="button" data-action="restore-admin" data-account-id="' + id + '"' +
        (remaining > 0 ? '' : ' disabled') + '>' + icon('refresh') + (remaining > 0 ? '撤销删除' : '已到期') + '</button>';
    }
    var buttons = '<button class="btn btn-sm" type="button" data-action="edit-admin" data-account-id="' + id + '">' +
      icon('edit') + '编辑</button>';
    if (account.status === 'enabled') {
      buttons += '<button class="btn btn-sm" type="button" data-action="disable-admin" data-account-id="' + id + '"' +
        (protectedSuper ? ' disabled' : '') + '>禁用</button>';
    } else if (account.status === 'disabled') {
      buttons += '<button class="btn btn-sm" type="button" data-action="enable-admin" data-account-id="' + id + '">' + icon('check') + '启用</button>';
    }
    buttons += '<button class="btn btn-sm text-danger" type="button" data-action="delete-admin" data-account-id="' + id + '"' +
      (protectedSuper ? ' disabled' : '') + '>' + icon('trash') + '删除</button>';
    return buttons;
  }

  function adminAccountsTableHtml(accounts) {
    if (!accounts.length) {
      return '<div class="empty-state empty-state--compact"><div class="empty-icon">' + icon('lock') + '</div>' +
        '<h3 class="empty-title">还没有管理员账户</h3>' +
        '<p class="empty-text">使用上方的表单创建管理员账户。</p></div>';
    }
    return '<div class="table-wrap"><table class="data-table">' +
      '<thead><tr><th>账户</th><th>账户类型</th><th>状态</th><th>最近登录</th><th>删除信息</th><th>操作</th></tr></thead>' +
      '<tbody>' + accounts.map(function (account) {
        return '<tr>' +
          '<td data-label="账户"><div class="row-account"><span class="row-name">' + esc(account.username) + '</span>' +
          '<span class="row-sub">' + esc(account.display_name || '—') + '</span></div></td>' +
          '<td data-label="账户类型"><span class="badge' + (account.role === 'super_admin' ? ' badge--accent' : '') + '">' +
          esc(roleLabel(account.role)) + '</span></td>' +
          '<td data-label="状态">' + statusBadge(account.status) + '</td>' +
          '<td data-label="最近登录">' + esc(account.last_login_at ? formatShanghaiDateTime(account.last_login_at) : '—') + '</td>' +
          '<td class="cell-countdown" data-label="删除信息">' + accountDeletionHtml(account) + '</td>' +
          '<td class="cell-actions" data-label="操作"><div class="row-actions">' + adminAccountActionsHtml(account) + '</div></td>' +
          '</tr>';
      }).join('') + '</tbody></table></div>';
  }

  function adminCreateCardHtml() {
    return '<section class="card form-card">' +
      '<div class="section-head"><div><h2 class="section-title">' + icon('plus') + '新建管理员</h2>' +
      '<p class="field-hint">账户名全局唯一，可与楼层长账户名不同；初始密码不强制修改。</p></div></div>' +
      '<form class="inline-form" id="createAdminForm" novalidate>' +
      '<label class="field"><span class="field-label">账户名</span>' +
      '<input class="input" name="username" type="text" autocomplete="off" maxlength="40" required placeholder="例如：admin2"></label>' +
      '<label class="field"><span class="field-label">初始密码</span>' +
      '<input class="input" name="password" type="password" autocomplete="new-password" required placeholder="设置初始密码"></label>' +
      '<label class="field"><span class="field-label">显示名</span>' +
      '<input class="input" name="display_name" type="text" maxlength="40" placeholder="例如：二号管理员"></label>' +
      '<button class="btn btn-accent" type="submit" id="createAdminSubmit">' + icon('plus') + '创建管理员</button>' +
      '</form></section>';
  }

  function adminTableCardHtml() {
    var accounts = state.adminAccounts || [];
    return '<section class="card table-card">' +
      '<div class="section-head"><div><h2 class="section-title">管理员账户</h2>' +
      '<p class="field-hint">超级管理员可以管理管理员账户；最后一个可用超级管理员不能被禁用或删除。</p></div>' +
      '<span class="badge">' + accounts.length + ' 个</span></div>' +
      adminAccountsTableHtml(accounts) +
      '</section>';
  }

  function renderAccountsPage() {
    state.view = 'admin-accounts';
    renderAppShell();
    els.view.innerHTML = loadingPageHtml('管理员', '账户管理');
    var token = ++renderSeq;
    var requests = [api.get('/api/admin/accounts')];
    if (isSuperAdmin()) { requests.push(api.get('/api/super-admin/admins')); }
    Promise.all(requests)
      .then(function (results) {
        if (token !== renderSeq || state.view !== 'admin-accounts' || !state.user) { return; }
        state.accounts = Array.isArray(results[0].accounts) ? results[0].accounts : [];
        if (isSuperAdmin()) {
          state.adminAccounts = Array.isArray(results[1].accounts) ? results[1].accounts : [];
        }
        renderAccountsContent();
      })
      .catch(function (err) {
        if (token !== renderSeq || state.view !== 'admin-accounts' || !state.user) { return; }
        renderViewError('加载账户列表失败', errorMessage(err));
        reportError(err);
      });
  }

  function renderAccountsContent() {
    var all = state.accounts;
    var filtered = accountsByFilter();
    var counts = {
      all: all.length,
      enabled: all.filter(function (a) { return a.status === 'enabled'; }).length,
      disabled: all.filter(function (a) { return a.status === 'disabled'; }).length,
      deleted: all.filter(function (a) { return a.status === 'deleted'; }).length
    };
    var filters = [
      { value: 'all', label: '全部' },
      { value: 'enabled', label: '启用' },
      { value: 'disabled', label: '禁用' },
      { value: 'deleted', label: '已删除' }
    ];
    var pills = filters.map(function (filter) {
      return '<button class="pill' + (state.accountFilter === filter.value ? ' is-active' : '') +
        '" type="button" data-action="account-filter" data-filter="' + filter.value +
        '" aria-pressed="' + (state.accountFilter === filter.value ? 'true' : 'false') + '">' +
        esc(filter.label) + '<span class="filter-count">' + counts[filter.value] + '</span></button>';
    }).join('');

    var head = pageHeadHtml({
      eyebrow: '管理员',
      title: '账户管理',
      sub: isSuperAdmin() ?
        '创建和管理楼层长账户，并管理管理员账户；删除账户后进入 72 天保留期，到期自动永久删除。' :
        '创建和管理楼层长账户；删除账户后进入 72 天保留期，到期自动永久删除。',
      actions: '<button class="btn" type="button" data-action="refresh-view">' + icon('refresh') + '刷新</button>' +
        '<a class="btn" href="#/admin">' + icon('chart') + '返回概览</a>'
    });

    var createCard = '<section class="card form-card">' +
      '<div class="section-head"><div><h2 class="section-title">' + icon('plus') + '新建楼层长</h2>' +
      '<p class="field-hint">账户名全局唯一，可与管理员账户名不同；初始密码不强制修改。</p></div></div>' +
      '<form class="inline-form" id="createAccountForm" novalidate>' +
      '<label class="field"><span class="field-label">账户名</span>' +
      '<input class="input" name="username" type="text" autocomplete="off" maxlength="40" required placeholder="例如：f1"></label>' +
      '<label class="field"><span class="field-label">初始密码</span>' +
      '<input class="input" name="password" type="password" autocomplete="new-password" required placeholder="设置初始密码"></label>' +
      '<label class="field"><span class="field-label">显示名</span>' +
      '<input class="input" name="display_name" type="text" maxlength="40" placeholder="例如：一层" required></label>' +
      '<button class="btn btn-accent" type="submit" id="createAccountSubmit">' + icon('plus') + '创建账户</button>' +
      '</form></section>';

    var adminCards = isSuperAdmin() ? adminCreateCardHtml() + adminTableCardHtml() : '';

    var tableCard = '<section class="card table-card">' +
      '<div class="section-head"><div><h2 class="section-title">楼层长账户</h2>' +
      '<p class="field-hint">当前显示 ' + filtered.length + ' 个账户。删除后 72 天内可撤销删除。</p></div></div>' +
      '<div class="filter-pills" role="group" aria-label="账户状态筛选">' + pills + '</div>' +
      accountsTableHtml(filtered) +
      '</section>';

    els.view.innerHTML = pageWrap(head, createCard + adminCards + tableCard);
    startCountdownTicker();
  }

  function reloadAccountsData() {
    return api.get('/api/admin/accounts')
      .then(function (data) {
        state.accounts = Array.isArray(data.accounts) ? data.accounts : [];
        if (state.view === 'admin-accounts') { renderAccountsContent(); }
      })
      .catch(function (err) { reportError(err); });
  }

  function createAccount(form) {
    var usernameInput = q('[name="username"]', form);
    var passwordInput = q('[name="password"]', form);
    var displayInput = q('[name="display_name"]', form);
    var username = usernameInput ? usernameInput.value.trim() : '';
    var password = passwordInput ? passwordInput.value : '';
    var displayName = displayInput ? displayInput.value.trim() : '';
    if (!username || !password || !displayName) {
      toast('请填写账户名、初始密码和显示名。', 'error');
      if (!username && usernameInput) { usernameInput.focus(); }
      else if (!password && passwordInput) { passwordInput.focus(); }
      else if (displayInput) { displayInput.focus(); }
      return;
    }
    var button = q('#createAccountSubmit', form);
    setButtonBusy(button, true, '创建中…');
    api.post('/api/admin/accounts', { username: username, password: password, display_name: displayName })
      .then(function () {
        toast('楼层长账户「' + username + '」已创建。', 'success');
        return reloadAccountsData();
      })
      .catch(function (err) {
        setButtonBusy(button, false);
        reportError(err);
      });
  }

  function openAccountEditModal(accountId) {
    var account = findAccountById(accountId);
    if (!account) { return; }
    var body = '<form class="config-form" id="accountEditForm" data-account-id="' + num(account.id) + '" novalidate>' +
      '<div class="form-grid">' +
      '<label class="field"><span class="field-label">账户名</span>' +
      '<input class="input" name="username" type="text" autocomplete="off" maxlength="40" required value="' + esc(account.username) + '"></label>' +
      '<label class="field"><span class="field-label">显示名</span>' +
      '<input class="input" name="display_name" type="text" maxlength="40" required value="' + esc(account.display_name || '') + '"></label>' +
      '</div>' +
      '<label class="field"><span class="field-label">重置密码（可选）</span>' +
      '<input class="input" name="password" type="password" autocomplete="new-password" placeholder="留空则不修改密码"></label>' +
      '<p class="field-hint">修改密码后该账户的其他登录会话将失效。</p>' +
      '<p class="form-error" id="accountEditError" role="alert" hidden></p>' +
      '<div class="form-actions">' +
      '<button class="btn" type="button" data-action="close-modal">取消</button>' +
      '<button class="btn btn-accent" type="submit" id="accountEditSubmit">保存修改</button>' +
      '</div></form>';
    openModal({
      title: '编辑楼层长账户',
      eyebrow: account.username || '',
      body: body
    });
  }

  function saveAccountEdit(form) {
    var accountId = Number(form.getAttribute('data-account-id'));
    var usernameInput = q('[name="username"]', form);
    var displayInput = q('[name="display_name"]', form);
    var passwordInput = q('[name="password"]', form);
    var errorEl = q('#accountEditError', form);
    var username = usernameInput ? usernameInput.value.trim() : '';
    var displayName = displayInput ? displayInput.value.trim() : '';
    var password = passwordInput ? passwordInput.value : '';
    setFieldError(errorEl, '');
    if (!username || !displayName) {
      setFieldError(errorEl, '账户名和显示名不能为空。');
      return;
    }
    var payload = { username: username, display_name: displayName };
    if (password) { payload.password = password; }
    var button = q('#accountEditSubmit', form);
    setButtonBusy(button, true, '保存中…');
    api.patch('/api/admin/accounts/' + encodeURIComponent(String(accountId)), payload)
      .then(function () {
        closeTopModal();
        toast('账户信息已更新。', 'success');
        return reloadAccountsData();
      })
      .catch(function (err) {
        setButtonBusy(button, false);
        setFieldError(errorEl, errorMessage(err));
        if (err.status !== 409) { reportError(err); }
      });
  }

  function handleAccountAction(action, button) {
    var accountId = Number(button.getAttribute('data-account-id'));
    var account = findAccountById(accountId);
    if (!account) { return; }
    var username = account.username || '';

    function request(path, body, successMessage) {
      setButtonBusy(button, true, '处理中…');
      api.post(path, body)
        .then(function () {
          toast(successMessage, 'success');
          return reloadAccountsData();
        })
        .catch(function (err) {
          setButtonBusy(button, false);
          if (err.code === 'restore_not_allowed') {
            toast(errorMessage(err), 'error');
            reloadAccountsData();
          } else {
            reportError(err);
          }
        });
    }

    if (action === 'enable-account') {
      request('/api/admin/accounts/' + accountId + '/enable', {}, '账户「' + username + '」已启用。');
      return;
    }
    if (action === 'disable-account') {
      confirmDialog({
        title: '禁用账户？',
        message: '确认禁用楼层长「' + username + '」吗？',
        note: '禁用后该账户会立即失效全部登录会话并禁止登录；账户数据完整保留，可随时重新启用。',
        confirmText: '禁用账户',
        danger: true
      }).then(function (ok) {
        if (!ok) { return; }
        request('/api/admin/accounts/' + accountId + '/disable', { confirm: true },
          '账户「' + username + '」已禁用。');
      });
      return;
    }
    if (action === 'delete-account') {
      confirmDialog({
        title: '删除账户？',
        message: '确认删除楼层长「' + username + '」吗？',
        note: '删除后账户立即禁止登录，进入 72 天保留期；期间可撤销删除。保留期到期后，系统将永久删除该账户及其全部关联数据（排班、勾选、备注、图片），该操作不可恢复。',
        confirmText: '删除账户',
        danger: true
      }).then(function (ok) {
        if (!ok) { return; }
        request('/api/admin/accounts/' + accountId + '/delete', { confirm: true },
          '账户「' + username + '」已删除，进入 72 天保留期。');
      });
      return;
    }
    if (action === 'restore-account') {
      var remaining = accountRemainingSeconds(account);
      if (remaining <= 0) {
        toast('该账户已超过撤销期限，无法恢复。', 'error');
        return;
      }
      confirmDialog({
        title: '撤销删除？',
        message: '确认撤销删除楼层长「' + username + '」吗？',
        note: '剩余保留时间：' + (account.remaining_text || formatRemaining(remaining)) +
          '。撤销后账户恢复为启用状态，全部数据保留。',
        confirmText: '撤销删除',
        danger: false
      }).then(function (ok) {
        if (!ok) { return; }
        request('/api/admin/accounts/' + accountId + '/restore', { confirm: true },
          '账户「' + username + '」已恢复启用。');
      });
    }
  }

  function reloadAdminAccountsData() {
    return api.get('/api/super-admin/admins')
      .then(function (data) {
        state.adminAccounts = Array.isArray(data.accounts) ? data.accounts : [];
        if (state.view === 'admin-accounts') { renderAccountsContent(); }
      })
      .catch(function (err) { reportError(err); });
  }

  function createAdminAccount(form) {
    var usernameInput = q('[name="username"]', form);
    var passwordInput = q('[name="password"]', form);
    var displayInput = q('[name="display_name"]', form);
    var username = usernameInput ? usernameInput.value.trim() : '';
    var password = passwordInput ? passwordInput.value : '';
    var displayName = displayInput ? displayInput.value.trim() : '';
    if (!username || !password) {
      toast('请填写管理员账户名和初始密码。', 'error');
      if (!username && usernameInput) { usernameInput.focus(); }
      else if (!password && passwordInput) { passwordInput.focus(); }
      return;
    }
    var button = q('#createAdminSubmit', form);
    setButtonBusy(button, true, '创建中…');
    api.post('/api/super-admin/admins', { username: username, password: password, display_name: displayName })
      .then(function () {
        toast('管理员账户「' + username + '」已创建。', 'success');
        return reloadAdminAccountsData();
      })
      .catch(function (err) {
        setButtonBusy(button, false);
        reportError(err);
      });
  }

  function openAdminEditModal(accountId) {
    var account = findAdminAccountById(accountId);
    if (!account) { return; }
    var body = '<form class="config-form" id="adminEditForm" data-account-id="' + num(account.id) + '" novalidate>' +
      '<p class="field-hint">账户类型：' + esc(roleLabel(account.role)) + '</p>' +
      '<div class="form-grid">' +
      '<label class="field"><span class="field-label">账户名</span>' +
      '<input class="input" name="username" type="text" autocomplete="off" maxlength="40" required value="' + esc(account.username) + '"></label>' +
      '<label class="field"><span class="field-label">显示名</span>' +
      '<input class="input" name="display_name" type="text" maxlength="40" value="' + esc(account.display_name || '') + '"></label>' +
      '</div>' +
      '<label class="field"><span class="field-label">重置密码（可选）</span>' +
      '<input class="input" name="password" type="password" autocomplete="new-password" placeholder="留空则不修改密码"></label>' +
      '<p class="field-hint">修改密码后该账户的其他登录会话将失效。</p>' +
      '<p class="form-error" id="adminEditError" role="alert" hidden></p>' +
      '<div class="form-actions">' +
      '<button class="btn" type="button" data-action="close-modal">取消</button>' +
      '<button class="btn btn-accent" type="submit" id="adminEditSubmit">保存修改</button>' +
      '</div></form>';
    openModal({
      title: '编辑管理员账户',
      eyebrow: account.username || '',
      body: body
    });
  }

  function saveAdminEdit(form) {
    var accountId = Number(form.getAttribute('data-account-id'));
    var usernameInput = q('[name="username"]', form);
    var displayInput = q('[name="display_name"]', form);
    var passwordInput = q('[name="password"]', form);
    var errorEl = q('#adminEditError', form);
    var username = usernameInput ? usernameInput.value.trim() : '';
    var displayName = displayInput ? displayInput.value.trim() : '';
    var password = passwordInput ? passwordInput.value : '';
    setFieldError(errorEl, '');
    if (!username) {
      setFieldError(errorEl, '账户名不能为空。');
      return;
    }
    var payload = { username: username, display_name: displayName };
    if (password) { payload.password = password; }
    var button = q('#adminEditSubmit', form);
    setButtonBusy(button, true, '保存中…');
    api.patch('/api/super-admin/admins/' + encodeURIComponent(String(accountId)), payload)
      .then(function () {
        closeTopModal();
        toast('管理员账户已更新。', 'success');
        return reloadAdminAccountsData();
      })
      .catch(function (err) {
        setButtonBusy(button, false);
        setFieldError(errorEl, errorMessage(err));
        if (err.status !== 409) { reportError(err); }
      });
  }

  function handleAdminAccountAction(action, button) {
    var accountId = Number(button.getAttribute('data-account-id'));
    var account = findAdminAccountById(accountId);
    if (!account) { return; }
    var username = account.username || '';

    function request(path, body, successMessage) {
      setButtonBusy(button, true, '处理中…');
      api.post(path, body)
        .then(function () {
          toast(successMessage, 'success');
          return reloadAdminAccountsData();
        })
        .catch(function (err) {
          setButtonBusy(button, false);
          if (err.code === 'restore_not_allowed' || err.code === 'last_super_admin') {
            toast(errorMessage(err), 'error');
            reloadAdminAccountsData();
          } else {
            reportError(err);
          }
        });
    }

    if (action === 'enable-admin') {
      request('/api/super-admin/admins/' + accountId + '/enable', {}, '账户「' + username + '」已启用。');
      return;
    }
    if (action === 'disable-admin') {
      confirmDialog({
        title: '禁用账户？',
        message: '确认禁用管理员「' + username + '」吗？',
        note: '禁用后该账户会立即失效全部登录会话并禁止登录；账户数据完整保留，可随时重新启用。',
        confirmText: '禁用账户',
        danger: true
      }).then(function (ok) {
        if (!ok) { return; }
        request('/api/super-admin/admins/' + accountId + '/disable', { confirm: true },
          '账户「' + username + '」已禁用。');
      });
      return;
    }
    if (action === 'delete-admin') {
      confirmDialog({
        title: '删除账户？',
        message: '确认删除管理员「' + username + '」吗？',
        note: '删除后账户立即禁止登录，进入 72 天保留期；期间可撤销删除。保留期到期后，系统将永久删除该账户，该操作不可恢复。',
        confirmText: '删除账户',
        danger: true
      }).then(function (ok) {
        if (!ok) { return; }
        request('/api/super-admin/admins/' + accountId + '/delete', { confirm: true },
          '账户「' + username + '」已删除，进入 72 天保留期。');
      });
      return;
    }
    if (action === 'restore-admin') {
      var remaining = accountRemainingSeconds(account);
      if (remaining <= 0) {
        toast('该账户已超过撤销期限，无法恢复。', 'error');
        return;
      }
      confirmDialog({
        title: '撤销删除？',
        message: '确认撤销删除管理员「' + username + '」吗？',
        note: '剩余保留时间：' + (account.remaining_text || formatRemaining(remaining)) +
          '。撤销后账户恢复为启用状态，全部数据保留。',
        confirmText: '撤销删除',
        danger: false
      }).then(function (ok) {
        if (!ok) { return; }
        request('/api/super-admin/admins/' + accountId + '/restore', { confirm: true },
          '账户「' + username + '」已恢复启用。');
      });
    }
  }

  /* ---- 备份 ---- */
  function backupStatusBadge(status) {
    if (status === 'success') {
      return '<span class="status-badge is-success">成功</span>';
    }
    if (status === 'failed' || status === 'failure') {
      return '<span class="status-badge is-failure">失败</span>';
    }
    return '<span class="status-badge">' + esc(status || '—') + '</span>';
  }

  function renderBackupsPage() {
    state.view = 'admin-backups';
    renderAppShell();
    els.view.innerHTML = loadingPageHtml('管理员', '系统备份');
    var token = ++renderSeq;
    api.get('/api/admin/backups')
      .then(function (data) {
        if (token !== renderSeq || state.view !== 'admin-backups' || !state.user) { return; }
        state.backups = Array.isArray(data.backups) ? data.backups : [];
        renderBackupsContent();
      })
      .catch(function (err) {
        if (token !== renderSeq || state.view !== 'admin-backups' || !state.user) { return; }
        renderViewError('加载备份列表失败', errorMessage(err));
        reportError(err);
      });
  }

  function renderBackupsContent() {
    var backups = state.backups;
    var head = pageHeadHtml({
      eyebrow: '管理员',
      title: '系统备份',
      sub: '系统每日自动备份，仅保留最新三份；可手动立即创建备份，或在需要时恢复。',
      actions: '<button class="btn btn-accent" type="button" data-action="immediate-backup" id="backupNowButton">' +
        icon('backup') + '立即备份</button>' +
        '<button class="btn" type="button" data-action="refresh-view">' + icon('refresh') + '刷新</button>'
    });
    var list = backups.length ? '<div class="backup-list">' + backups.map(function (backup) {
      var canRestore = backup.status === 'success';
      var exportButton = isSuperAdmin() ?
        '<button class="btn btn-sm" type="button" data-action="download-backup" data-backup-id="' + num(backup.id) + '">' +
        icon('backup') + '导出到本地</button>' : '';
      return '<div class="backup-item">' +
        '<span class="backup-icon">' + icon('backup') + '</span>' +
        '<div class="backup-info"><p class="backup-name">' + esc(backup.dir_name || '未命名备份') + '</p>' +
        '<p class="backup-meta">' + esc(backup.created_at_text || formatShanghaiDateTime(backup.created_at)) +
        ' · ' + esc(backup.size_text || '') + '</p></div>' +
        backupStatusBadge(backup.status) +
        '<div class="backup-actions">' +
        exportButton +
        '<button class="btn btn-sm" type="button" data-action="restore-backup" data-backup-id="' + num(backup.id) + '"' +
        (canRestore ? '' : ' disabled') + '>' + icon('refresh') + '恢复此备份</button>' +
        '</div></div>';
    }).join('') + '</div>' :
      emptyStateCompactHtml('backup', '暂无备份', '还没有可用的系统备份，可点击“立即备份”创建第一份。');
    var card = '<section class="card"><div class="section-head"><div><h2 class="section-title">' +
      icon('backup') + '备份记录</h2><p class="field-hint">恢复会覆盖当前数据库与图片并失效全部登录会话。' +
      (isSuperAdmin() ? '导出会把数据库与图片打包为本地文件。' : '') + '</p></div>' +
      '<span class="badge">' + backups.length + ' 份</span></div>' + list + '</section>';
    els.view.innerHTML = pageWrap(head, card);
  }

  function refreshBackupsList() {
    return api.get('/api/admin/backups')
      .then(function (data) {
        state.backups = Array.isArray(data.backups) ? data.backups : [];
        if (state.view === 'admin-backups') { renderBackupsContent(); }
      })
      .catch(function (err) { reportError(err); });
  }

  function handleBackupNow(button) {
    setButtonBusy(button, true, '备份中…');
    api.post('/api/admin/backups', {})
      .then(function (data) {
        toast(data && data.backup ? '备份已创建：' + (data.backup.dir_name || '') : '备份已创建。', 'success');
        return refreshBackupsList();
      })
      .catch(function (err) {
        setButtonBusy(button, false);
        reportError(err);
      });
  }

  function restoreBackup(button, backupId) {
    var backup = null;
    for (var i = 0; i < state.backups.length; i += 1) {
      if (Number(state.backups[i].id) === Number(backupId)) { backup = state.backups[i]; break; }
    }
    if (!backup) { return; }
    confirmDialog({
      title: '恢复此备份？',
      message: '即将使用 ' + (backup.created_at_text || formatShanghaiDateTime(backup.created_at)) +
        ' 的备份覆盖当前数据库与图片。',
      note: '恢复会失效所有登录会话，完成后需要重新登录。当前数据库中尚未备份的数据将无法找回，请确认后继续。',
      confirmText: '恢复并重新登录',
      danger: true
    }).then(function (ok) {
      if (!ok) { return; }
      setButtonBusy(button, true, '恢复中…');
      api.post('/api/admin/backups/' + encodeURIComponent(String(backupId)) + '/restore', { confirm: true })
        .then(function (data) {
          closeAllModals();
          clearSessionState();
          if (window.location.hash !== '#/login') {
            window.history.replaceState(null, '', '#/login');
          }
          renderLogin('');
          toast(data && data.message ? data.message : '备份已恢复，请重新登录。', 'success');
        })
        .catch(function (err) {
          setButtonBusy(button, false);
          reportError(err);
        });
    });
  }

  function findBackupById(backupId) {
    for (var i = 0; i < state.backups.length; i += 1) {
      if (Number(state.backups[i].id) === Number(backupId)) { return state.backups[i]; }
    }
    return null;
  }

  function responseErrorMessage(status, text) {
    var data = null;
    try { data = JSON.parse(text); } catch (err) { data = null; }
    if (data && data.error && typeof data.error.message === 'string' && data.error.message) {
      return data.error.message;
    }
    return '导出失败（HTTP ' + status + '）';
  }

  function downloadBackup(backup, button) {
    if (!backup) { return; }
    var path = '/api/super-admin/backups/' + encodeURIComponent(String(backup.id)) + '/download';
    setButtonBusy(button, true, '导出中…');
    window.fetch(path, {
      method: 'GET',
      credentials: 'same-origin',
      headers: { 'X-CSRF-Token': api.getCsrf() }
    }).then(function (response) {
      if (!response.ok) {
        return response.text().then(function (text) {
          var err = { status: response.status, message: responseErrorMessage(response.status, text) };
          if (response.status === 401) {
            handleUnauthorized();
            return;
          }
          throw err;
        });
      }
      return response.blob().then(function (blob) {
        var objectUrl = window.URL.createObjectURL(blob);
        var link = document.createElement('a');
        link.href = objectUrl;
        link.download = (backup.dir_name || 'backup') + '.tar';
        document.body.appendChild(link);
        link.click();
        document.body.removeChild(link);
        window.setTimeout(function () { window.URL.revokeObjectURL(objectUrl); }, 2000);
        toast('备份已导出：' + link.download, 'success');
      });
    }).catch(function (err) {
      reportError(err);
    }).then(function () {
      setButtonBusy(button, false);
    });
  }

  /* ---- 日志 ---- */
  function normaliseLogs(data) {
    var source = data || {};
    return {
      items: Array.isArray(source.items) ? source.items : [],
      total: num(source.total),
      page: Math.max(1, num(source.page, 1)),
      pageSize: Math.max(1, num(source.page_size, 20)),
      actions: Array.isArray(source.actions) ? source.actions : []
    };
  }

  function logsQueryString() {
    var params = new window.URLSearchParams();
    params.set('page', String(state.logFilters.page || 1));
    params.set('page_size', String(state.logs.pageSize || 20));
    if (state.logFilters.action) { params.set('action', state.logFilters.action); }
    if (state.logFilters.account_id) { params.set('account_id', state.logFilters.account_id); }
    if (state.logFilters.from) { params.set('from', state.logFilters.from); }
    if (state.logFilters.to) { params.set('to', state.logFilters.to); }
    return params.toString();
  }

  function targetText(log) {
    var type = log.target_type ? (TARGET_LABELS[log.target_type] || log.target_type) : '';
    var id = log.target_id === null || log.target_id === undefined ? '' : String(log.target_id);
    if (!type && !id) { return '—'; }
    return type + (id ? ' #' + id : '');
  }

  function logsResultsHtml() {
    var logs = state.logs;
    var totalPages = Math.max(1, Math.ceil(logs.total / logs.pageSize));
    var currentPage = Math.max(1, Math.min(logs.page, totalPages));
    if (!logs.items.length) {
      return '<section class="card table-card"><div class="section-head"><div><h2 class="section-title">' +
        icon('log') + '日志记录</h2><p class="field-hint">共 ' + logs.total + ' 条记录</p></div></div>' +
        emptyStateCompactHtml('log', '没有匹配的日志', '更换筛选条件或清理筛选后重试。') + '</section>';
    }
    var rows = logs.items.map(function (log) {
      return '<tr>' +
        '<td class="log-time" data-label="时间">' + esc(log.created_at_text || formatShanghaiDateTime(log.created_at)) + '</td>' +
        '<td data-label="操作人"><div class="log-actor">' + esc(log.actor_name || '—') + '</div>' +
        '<div class="row-sub">' + esc(ROLE_LABELS[log.actor_type] || log.actor_type || '系统') + '</div></td>' +
        '<td data-label="动作"><span class="log-action" title="' + esc(log.action || '') + '">' +
        esc(actionLabel(log.action)) + '</span></td>' +
        '<td data-label="目标"><span class="log-target">' + esc(targetText(log)) + '</span></td>' +
        '<td data-label="结果">' + resultBadge(log.result) + '</td>' +
        '<td class="cell-detail" data-label="详情"><div class="log-detail">' + esc(log.detail || '—') + '</div></td>' +
        '</tr>';
    }).join('');
    var pagination = '<div class="pagination">' +
      '<span class="page-info">共 ' + logs.total + ' 条 · 第 ' + currentPage + ' / ' + totalPages + ' 页</span>' +
      '<div class="page-buttons">' +
      '<button class="btn btn-sm" type="button" data-action="logs-page" data-page="' + (currentPage - 1) + '"' +
      (currentPage <= 1 ? ' disabled' : '') + '>' + icon('arrow-left') + '上一页</button>' +
      '<button class="btn btn-sm" type="button" data-action="logs-page" data-page="' + (currentPage + 1) + '"' +
      (currentPage >= totalPages ? ' disabled' : '') + '>下一页' + icon('chevron') + '</button>' +
      '</div></div>';
    return '<section class="card table-card">' +
      '<div class="section-head"><div><h2 class="section-title">' + icon('log') + '日志记录</h2>' +
      '<p class="field-hint">仅保留最近一个自然周的日志。管理员查看备注和原图也会记录审计。</p></div>' +
      '<span class="page-info">共 ' + logs.total + ' 条</span></div>' +
      '<div class="table-wrap"><table class="data-table">' +
      '<thead><tr><th>时间</th><th>操作人</th><th>动作</th><th>目标</th><th>结果</th><th>详情</th></tr></thead>' +
      '<tbody>' + rows + '</tbody></table></div>' + pagination + '</section>';
  }

  function logsErrorHtml(message) {
    return '<section class="card table-card"><div class="error-state">' +
      '<div class="empty-icon">' + icon('warning') + '</div>' +
      '<h3 class="empty-title">日志加载失败</h3>' +
      '<p class="error-message">' + esc(message) + '</p>' +
      '<button class="btn btn-accent" type="button" data-action="refresh-logs">' + icon('refresh') + '重新加载</button>' +
      '</div></section>';
  }

  function logFilterCardHtml() {
    var actionOptions = '<option value="">全部操作</option>' + ACTION_ORDER.map(function (action) {
      return '<option value="' + action + '"' + (state.logFilters.action === action ? ' selected' : '') + '>' +
        esc(actionLabel(action)) + '</option>';
    }).join('');
    var accountOptions = '<option value="">全部账户</option>' + state.accounts.map(function (account) {
      return '<option value="' + num(account.id) + '"' + (String(state.logFilters.account_id) === String(account.id) ? ' selected' : '') + '>' +
        esc(account.username) + (account.display_name ? ' · ' + esc(account.display_name) : '') + '</option>';
    }).join('');
    return '<section class="card"><form class="filter-grid" id="logFilterForm" novalidate>' +
      '<label class="field"><span class="field-label">操作类型</span>' +
      '<select class="select" name="action">' + actionOptions + '</select></label>' +
      '<label class="field"><span class="field-label">账户</span>' +
      '<select class="select" name="account_id">' + accountOptions + '</select></label>' +
      '<label class="field"><span class="field-label">起始日期</span>' +
      '<input class="input" type="date" name="from" value="' + esc(state.logFilters.from) + '"></label>' +
      '<label class="field"><span class="field-label">结束日期</span>' +
      '<input class="input" type="date" name="to" value="' + esc(state.logFilters.to) + '"></label>' +
      '<div class="filter-actions">' +
      '<button class="btn btn-accent" type="submit">' + icon('search') + '查询</button>' +
      '<button class="btn" type="button" data-action="refresh-logs">' + icon('refresh') + '刷新</button>' +
      '<button class="btn" type="button" data-action="reset-logs">' + icon('close') + '重置</button>' +
      '</div></form></section>';
  }

  function renderLogsContent() {
    var head = pageHeadHtml({
      eyebrow: '管理员',
      title: '操作日志',
      sub: '筛选和查看管理员与楼层长的操作审计记录，日志按上海时区显示。',
      actions: '<a class="btn" href="#/admin/accounts">' + icon('users') + '账户管理</a>'
    });
    els.view.innerHTML = pageWrap(head, logFilterCardHtml() +
      '<div data-log-results>' + logsResultsHtml() + '</div>');
    startCountdownTicker();
  }

  function renderLogsPage() {
    state.view = 'admin-logs';
    renderAppShell();
    els.view.innerHTML = loadingPageHtml('管理员', '操作日志');
    var token = ++renderSeq;
    var accountsPromise = state.accounts.length ? Promise.resolve(state.accounts) :
      api.get('/api/admin/accounts').then(function (data) {
        return Array.isArray(data.accounts) ? data.accounts : [];
      }).catch(function () { return []; });
    var logsPromise = api.get('/api/admin/logs?' + logsQueryString());
    Promise.all([accountsPromise, logsPromise])
      .then(function (results) {
        if (token !== renderSeq || state.view !== 'admin-logs' || !state.user) { return; }
        state.accounts = results[0] || [];
        state.logs = normaliseLogs(results[1]);
        renderLogsContent();
      })
      .catch(function (err) {
        if (token !== renderSeq || state.view !== 'admin-logs' || !state.user) { return; }
        renderViewError('加载操作日志失败', errorMessage(err));
        reportError(err);
      });
  }

  function loadLogsResults() {
    if (state.view !== 'admin-logs') { return; }
    var wrapper = q('[data-log-results]');
    if (!wrapper) { return; }
    state.logsRequestSeq = (state.logsRequestSeq || 0) + 1;
    var requestId = state.logsRequestSeq;
    wrapper.innerHTML = '<section class="card table-card">' +
      '<div class="skeleton skeleton-title"></div>' + skeletonLines(5) + '</section>';
    api.get('/api/admin/logs?' + logsQueryString())
      .then(function (data) {
        if (requestId !== state.logsRequestSeq || state.view !== 'admin-logs') { return; }
        state.logs = normaliseLogs(data);
        state.logFilters.page = state.logs.page;
        var currentWrapper = q('[data-log-results]');
        if (currentWrapper) { currentWrapper.innerHTML = logsResultsHtml(); }
      })
      .catch(function (err) {
        if (requestId !== state.logsRequestSeq || state.view !== 'admin-logs') { return; }
        var currentWrapper = q('[data-log-results]');
        if (currentWrapper) { currentWrapper.innerHTML = logsErrorHtml(errorMessage(err)); }
        reportError(err);
      });
  }

  function applyLogFilters(form) {
    var actionSelect = q('[name="action"]', form);
    var accountSelect = q('[name="account_id"]', form);
    var fromInput = q('[name="from"]', form);
    var toInput = q('[name="to"]', form);
    var from = fromInput ? fromInput.value : '';
    var to = toInput ? toInput.value : '';
    if (from && to && from > to) {
      toast('起始日期不能晚于结束日期。', 'error');
      return;
    }
    state.logFilters = {
      action: actionSelect ? actionSelect.value : '',
      account_id: accountSelect ? accountSelect.value : '',
      from: from,
      to: to,
      page: 1
    };
    loadLogsResults();
  }

  function resetLogFilters() {
    state.logFilters = { action: '', account_id: '', from: '', to: '', page: 1 };
    renderLogsPage();
  }

  function goLogsPage(page) {
    var totalPages = Math.max(1, Math.ceil(state.logs.total / state.logs.pageSize));
    var nextPage = Math.max(1, Math.min(Number(page) || 1, totalPages));
    if (nextPage === state.logFilters.page) { return; }
    state.logFilters.page = nextPage;
    loadLogsResults();
  }

  /* ======================================================================
     9. 账户设置
     ====================================================================== */

  function renderAccountPage() {
    state.view = 'account';
    renderAppShell();
    var user = state.user;
    var head = pageHeadHtml({
      eyebrow: '账户',
      title: '账户设置',
      sub: '管理账户信息和登录密码。账户名修改后全局唯一性由服务端校验。'
    });
    var profileCard;
    if (isManager()) {
      profileCard = '<section class="card form-card">' +
        '<div><h2 class="section-title">' + icon('users') + '账户信息</h2>' +
        '<p class="section-desc">楼层长可以修改本人账户名和显示名。</p></div>' +
        '<form class="config-form" id="profileForm" novalidate>' +
        '<label class="field"><span class="field-label">账户名</span>' +
        '<input class="input" name="username" type="text" autocomplete="username" maxlength="40" required value="' + esc(user.username) + '"></label>' +
        '<label class="field"><span class="field-label">显示名</span>' +
        '<input class="input" name="display_name" type="text" maxlength="40" required value="' + esc(user.display_name || '') + '"></label>' +
        '<p class="field-hint">账户名全局唯一，不能与管理员或其他楼层长重复。</p>' +
        '<p class="form-error" id="profileError" role="alert" hidden></p>' +
        '<div class="form-actions"><button class="btn btn-accent" type="submit" id="profileSubmit">保存账户信息</button></div>' +
        '</form></section>';
    } else {
      profileCard = '<section class="card form-card">' +
        '<div><h2 class="section-title">' + icon('users') + '账户信息</h2>' +
        '<p class="section-desc">管理员账户名不可在此修改；如需更换密码请使用下方密码表单。</p></div>' +
        '<div class="readonly-grid">' +
        '<div class="readonly-item"><span class="readonly-label">账户名</span><span class="readonly-value">' + esc(user.username) + '</span></div>' +
        '<div class="readonly-item"><span class="readonly-label">显示名</span><span class="readonly-value">' + esc(user.display_name || '') + '</span></div>' +
        '<div class="readonly-item"><span class="readonly-label">角色</span><span class="readonly-value">' + esc(roleLabel(user.role)) + '</span></div>' +
        '</div></section>';
    }
    var passwordCard = '<section class="card form-card">' +
      '<div><h2 class="section-title">' + icon('lock') + '修改密码</h2>' +
      '<p class="section-desc">修改成功后其他登录会话将失效，当前会话保留。</p></div>' +
      '<form class="config-form" id="accountPasswordForm" novalidate>' +
      '<label class="field"><span class="field-label">旧密码</span>' +
      '<input class="input" name="old_password" type="password" autocomplete="current-password" required></label>' +
      '<label class="field"><span class="field-label">新密码</span>' +
      '<input class="input" name="new_password" type="password" autocomplete="new-password" required></label>' +
      '<label class="field"><span class="field-label">确认新密码</span>' +
      '<input class="input" name="confirm_password" type="password" autocomplete="new-password" required></label>' +
      '<p class="form-error" id="accountPasswordError" role="alert" hidden></p>' +
      '<div class="form-actions"><button class="btn btn-accent" type="submit" id="accountPasswordSubmit">修改密码</button></div>' +
      '</form></section>';
    els.view.innerHTML = pageWrap(head, '<div class="account-grid">' + profileCard + passwordCard + '</div>');
  }

  function saveProfile(form) {
    var usernameInput = q('[name="username"]', form);
    var displayInput = q('[name="display_name"]', form);
    var errorEl = q('#profileError', form);
    var submit = q('#profileSubmit', form);
    var username = usernameInput ? usernameInput.value.trim() : '';
    var displayName = displayInput ? displayInput.value.trim() : '';
    setFieldError(errorEl, '');
    if (!username) {
      setFieldError(errorEl, '账户名不能为空。');
      return;
    }
    if (!displayName) {
      setFieldError(errorEl, '显示名不能为空。');
      return;
    }
    setButtonBusy(submit, true, '保存中…');
    api.patch('/api/profile', { username: username, display_name: displayName })
      .then(function (data) {
        if (data && data.user) {
          state.user = data.user;
        } else if (state.user) {
          state.user.username = username;
          state.user.display_name = displayName;
        }
        setButtonBusy(submit, false);
        renderAppShell();
        toast('账户信息已保存。', 'success');
      })
      .catch(function (err) {
        setButtonBusy(submit, false);
        setFieldError(errorEl, errorMessage(err));
        reportError(err);
      });
  }

  function saveOwnPassword(form) {
    var oldInput = q('[name="old_password"]', form);
    var newInput = q('[name="new_password"]', form);
    var confirmInput = q('[name="confirm_password"]', form);
    var errorEl = q('#accountPasswordError', form);
    var submit = q('#accountPasswordSubmit', form);
    var oldPassword = oldInput ? oldInput.value : '';
    var newPassword = newInput ? newInput.value : '';
    var confirmPassword = confirmInput ? confirmInput.value : '';
    setFieldError(errorEl, '');
    if (!oldPassword || !newPassword) {
      setFieldError(errorEl, '请填写旧密码和新密码。');
      return;
    }
    if (newPassword !== confirmPassword) {
      setFieldError(errorEl, '两次输入的新密码不一致。');
      return;
    }
    setButtonBusy(submit, true, '保存中…');
    api.post('/api/password', { old_password: oldPassword, new_password: newPassword })
      .then(function (data) {
        api.setCsrf(data.csrf_token || '');
        state.csrf = data.csrf_token || '';
        if (state.user) { state.user.must_change_password = false; }
        if (form) { form.reset(); }
        setButtonBusy(submit, false);
        toast('密码已修改。', 'success');
      })
      .catch(function (err) {
        setButtonBusy(submit, false);
        setFieldError(errorEl, errorMessage(err));
        reportError(err);
      });
  }

  /* ======================================================================
     10. 全局事件
     ====================================================================== */

  function handleAction(actionEl) {
    var action = actionEl.getAttribute('data-action');
    switch (action) {
      case 'theme':
        applyTheme(actionEl.getAttribute('data-theme-value'), true);
        break;
      case 'toggle-user-menu':
        toggleUserMenu();
        break;
      case 'toggle-mobile-menu':
        toggleMobileMenu();
        break;
      case 'close-modal':
        closeTopModal();
        break;
      case 'open-password':
        closeMenus();
        openPasswordModal(false);
        break;
      case 'goto-account':
        closeMenus();
        navigateTo('/account');
        break;
      case 'logout':
        handleLogout();
        break;
      case 'retry-view':
      case 'refresh-view':
      case 'refresh-week':
        renderRoute();
        break;
      case 'continue-check':
        handleContinueCheck();
        break;
      case 'room-check':
        handleRoomCardCheck(actionEl);
        break;
      case 'open-room': {
        var card = actionEl.closest('.room-card');
        if (card) { openRoomDetail(Number(card.getAttribute('data-room-id'))); }
        break;
      }
      case 'detail-toggle-check':
        handleRoomDetailCheck();
        break;
      case 'retry-room-notes':
        if (state.roomDetail) { loadRoomNotes(state.roomDetail); }
        break;
      case 'retry-room-images':
        if (state.roomDetail) { loadRoomImages(state.roomDetail); }
        break;
      case 'excluded-add':
        addExcludedRoom();
        break;
      case 'excluded-remove':
        removeExcludedRoom(actionEl.getAttribute('data-room'));
        break;
      case 'regenerate-week':
        regenerateWeek();
        break;
      case 'reset-week':
        resetWeek();
        break;
      case 'pick-images': {
        var fileInput = q('#roomImageInput');
        if (fileInput) { fileInput.click(); }
        break;
      }
      case 'view-image':
        openOriginalImage(Number(actionEl.getAttribute('data-image-id')));
        break;
      case 'delete-image':
        deleteImage(Number(actionEl.getAttribute('data-image-id')));
        break;
      case 'edit-note':
        editNote(Number(actionEl.getAttribute('data-note-id')));
        break;
      case 'cancel-note-edit':
        cancelNoteEdit();
        break;
      case 'save-note-edit':
        saveNoteEdit();
        break;
      case 'delete-note':
        deleteNote(Number(actionEl.getAttribute('data-note-id')));
        break;
      case 'delete-orphan-note':
        deleteOrphanNote(Number(actionEl.getAttribute('data-note-id')));
        break;
      case 'delete-orphan-image':
        deleteOrphanImage(Number(actionEl.getAttribute('data-image-id')));
        break;
      case 'refresh-orphans':
        if (state.adminDetail) { loadAdminOrphans(state.adminDetail.accountId); }
        break;
      case 'open-account':
        navigateTo('/admin/accounts/' + Number(actionEl.getAttribute('data-account-id')));
        break;
      case 'account-filter':
        state.accountFilter = actionEl.getAttribute('data-filter') || 'all';
        if (state.view === 'admin-accounts') { renderAccountsContent(); }
        break;
      case 'clear-overview-date':
        state.overviewDate = '';
        renderOverviewPage();
        break;
      case 'edit-account':
        openAccountEditModal(Number(actionEl.getAttribute('data-account-id')));
        break;
      case 'enable-account':
      case 'disable-account':
      case 'delete-account':
      case 'restore-account':
        handleAccountAction(action, actionEl);
        break;
      case 'edit-admin':
        openAdminEditModal(Number(actionEl.getAttribute('data-account-id')));
        break;
      case 'enable-admin':
      case 'disable-admin':
      case 'delete-admin':
      case 'restore-admin':
        handleAdminAccountAction(action, actionEl);
        break;
      case 'immediate-backup':
        handleBackupNow(actionEl);
        break;
      case 'download-backup':
        downloadBackup(findBackupById(actionEl.getAttribute('data-backup-id')), actionEl);
        break;
      case 'restore-backup':
        restoreBackup(actionEl, actionEl.getAttribute('data-backup-id'));
        break;
      case 'refresh-logs':
        loadLogsResults();
        break;
      case 'reset-logs':
        resetLogFilters();
        break;
      case 'logs-page':
        goLogsPage(actionEl.getAttribute('data-page'));
        break;
      default:
        break;
    }
  }

  function onDocumentClick(event) {
    var target = event.target;
    if (!(target instanceof Element)) { return; }
    var actionEl = target.closest('[data-action]');
    if (actionEl) {
      handleAction(actionEl);
    }
    if (!target.closest('.user-wrap')) {
      toggleUserMenu(false);
    }
    if (!target.closest('.app-header')) {
      toggleMobileMenu(false);
    }
  }

  function onDocumentSubmit(event) {
    var form = event.target;
    if (!(form instanceof HTMLFormElement)) { return; }
    event.preventDefault();
    switch (form.id) {
      case 'loginForm':
        handleLoginSubmit(form);
        break;
      case 'configForm':
        saveConfigForm(form);
        break;
      case 'roomNoteForm':
        submitNewNote();
        break;
      case 'passwordForm':
        submitPasswordModal(form);
        break;
      case 'createAccountForm':
        createAccount(form);
        break;
      case 'accountEditForm':
        saveAccountEdit(form);
        break;
      case 'createAdminForm':
        createAdminAccount(form);
        break;
      case 'adminEditForm':
        saveAdminEdit(form);
        break;
      case 'logFilterForm':
        applyLogFilters(form);
        break;
      case 'overviewFilterForm':
        var overviewDate = form.elements.date ? form.elements.date.value : '';
        if (overviewDate) {
          var dateParts = overviewDate.split('-');
          var weekday = new Date(Number(dateParts[0]), Number(dateParts[1]) - 1, Number(dateParts[2])).getDay();
          if (weekday === 4) {
            toast('周四为隐藏查寝日，不参与统计。', 'error');
            return;
          }
        }
        state.overviewDate = overviewDate;
        renderOverviewPage();
        break;
      case 'profileForm':
        saveProfile(form);
        break;
      case 'accountPasswordForm':
        saveOwnPassword(form);
        break;
      default:
        break;
    }
  }

  function onDocumentInput(event) {
    var target = event.target;
    if (!(target instanceof Element)) { return; }
    if (target.id === 'roomNoteInput' && state.roomDetail) {
      updateNewNoteCounter(state.roomDetail);
      return;
    }
    if (target.id === 'roomNoteEditInput' && state.roomDetail) {
      updateNoteEditCounter(state.roomDetail);
      return;
    }
    if (target.closest('#configForm') && state.configDraft) {
      if (target.name === 'building_name' || target.name === 'start_room' || target.name === 'end_room') {
        state.configDraft[target.name] = target.value;
        refreshConfigUI();
      }
    }
  }

  function onDocumentChange(event) {
    var target = event.target;
    if (!(target instanceof Element)) { return; }
    if (target.id === 'adminDayFilter' && state.adminDetail) {
      state.adminDetail.dayFilter = target.value;
      renderAdminDetailDays();
      return;
    }
    if (target.id === 'roomImageInput') {
      var files = target.files;
      handleImageFiles(files);
      target.value = '';
    }
  }

  function trapModalFocus(event) {
    if (!modalStack.length) { return; }
    var top = modalStack[modalStack.length - 1];
    var dialog = q('.modal', top.el);
    if (!dialog) { return; }
    var focusables = getFocusable(dialog);
    if (!focusables.length) {
      event.preventDefault();
      dialog.focus();
      return;
    }
    var first = focusables[0];
    var last = focusables[focusables.length - 1];
    var active = document.activeElement;
    if (!dialog.contains(active)) {
      event.preventDefault();
      first.focus();
      return;
    }
    if (event.shiftKey && active === first) {
      event.preventDefault();
      last.focus();
    } else if (!event.shiftKey && active === last) {
      event.preventDefault();
      first.focus();
    }
  }

  function onDocumentKeydown(event) {
    if (event.key === 'Escape') {
      if (modalStack.length) {
        var top = modalStack[modalStack.length - 1];
        if (top.dismissible) {
          closeTopModal();
          event.preventDefault();
        }
        return;
      }
      closeMenus();
      return;
    }
    if (event.key === 'Tab') {
      trapModalFocus(event);
      return;
    }
    if (event.key === 'Enter' && event.target instanceof Element && event.target.id === 'excludedInput') {
      event.preventDefault();
      addExcludedRoom();
      return;
    }
    if ((event.key === 'Enter' || event.key === ' ') && event.target instanceof Element) {
      var row = event.target.closest('[data-action="open-account"][role="link"]');
      if (row) {
        event.preventDefault();
        handleAction(row);
      }
    }
  }

  function bindGlobalEvents() {
    document.addEventListener('click', onDocumentClick);
    document.addEventListener('submit', onDocumentSubmit);
    document.addEventListener('input', onDocumentInput);
    document.addEventListener('change', onDocumentChange);
    document.addEventListener('keydown', onDocumentKeydown);
    window.addEventListener('hashchange', function () {
      closeMenus();
      renderRoute();
    });
  }

  /* ======================================================================
     11. 启动
     ====================================================================== */

  function bootSession() {
    api.get('/api/session', { skipAuthHandler: true })
      .then(function (data) {
        if (!data || !data.user) {
          hideBoot();
          renderLogin('');
          return;
        }
        setSession(data);
        hideBoot();
        renderAppShell();
        if (state.user.must_change_password) {
          state.view = 'password-required';
          els.view.innerHTML = '<div class="page"><div class="empty-state">' +
            '<div class="empty-icon">' + icon('lock') + '</div>' +
            '<h1 class="empty-title">请先修改初始密码</h1>' +
            '<p class="empty-text">为保障系统安全，管理员首次登录需要修改密码后才能继续使用。</p>' +
            '</div></div>';
          openPasswordModal(true, function () {
            navigateTo(defaultRoute(), true);
          });
        } else {
          renderRoute();
        }
      })
      .catch(function (err) {
        hideBoot();
        renderAppShell();
        if (err.status === 401) {
          renderLogin('');
        } else if (err.status === 503) {
          renderLogin(err.message || '系统维护中，请稍后重试。');
        } else if (err.status === 0) {
          renderLogin('无法连接服务器，请确认后端服务已启动。');
        } else {
          renderLogin(errorMessage(err));
        }
      });
  }

  function start() {
    els.app = document.getElementById('app');
    els.header = document.getElementById('appHeader');
    els.view = document.getElementById('viewRoot');
    els.toast = document.getElementById('toastRegion');
    els.modal = document.getElementById('modalRoot');
    els.themeColor = document.getElementById('themeColorMeta');

    if (!els.app || !els.header || !els.view || !els.toast || !els.modal || !api) {
      var boot = document.getElementById('boot');
      if (boot) {
        boot.innerHTML = '<p class="boot-text">前端资源加载失败，请刷新页面重试。</p>';
      }
      return;
    }

    initTheme();
    bindGlobalEvents();
    api.onUnauthorized(handleUnauthorized);
    api.onPasswordRequired(handlePasswordRequired);
    startCountdownTicker();
    bootSession();
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', start);
  } else {
    start();
  }
})();
