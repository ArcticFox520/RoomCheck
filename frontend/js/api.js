/*
 * RoomCheck API 客户端
 * - 统一 fetch 封装、错误归一化、CSRF 注入、401/403 全局回调
 * - 图片上传使用 XHR，以便展示单文件上传进度
 * 契约：docs/API.md v1
 */
(function () {
  'use strict';

  var RC = (window.RC = window.RC || {});
  var CSRF_KEY = 'rc_csrf';
  var memoryToken = '';
  var handlers = { unauthorized: null, passwordRequired: null };

  try {
    memoryToken = window.localStorage.getItem(CSRF_KEY) || '';
  } catch (err) {
    memoryToken = '';
  }

  function setCsrf(token) {
    memoryToken = token || '';
    try {
      if (token) {
        window.localStorage.setItem(CSRF_KEY, token);
      } else {
        window.localStorage.removeItem(CSRF_KEY);
      }
    } catch (err) {
      /* localStorage 不可用时仅保留内存态 */
    }
  }

  function getCsrf() {
    return memoryToken;
  }

  function makeError(status, payload, fallbackMessage) {
    var code = 'request_failed';
    var message = fallbackMessage || '请求失败，请稍后重试';
    var retryAfter = null;

    if (payload && typeof payload === 'object') {
      var error = payload.error;
      if (error && typeof error === 'object') {
        if (typeof error.code === 'string' && error.code) { code = error.code; }
        if (typeof error.message === 'string' && error.message) { message = error.message; }
        if (typeof error.retry_after === 'number') { retryAfter = error.retry_after; }
      } else if (typeof error === 'string' && error) {
        message = error;
      }
      if (typeof payload.code === 'string' && payload.code) { code = payload.code; }
      if (typeof payload.message === 'string' && payload.message) { message = payload.message; }
      if (typeof payload.retry_after === 'number') { retryAfter = payload.retry_after; }
    }

    return {
      status: status,
      code: code,
      message: message,
      retryAfter: retryAfter,
      data: payload || null
    };
  }

  function handleAuthError(err, options) {
    if (!options || !options.skipAuthHandler) {
      if (err.status === 401 && typeof handlers.unauthorized === 'function') {
        handlers.unauthorized(err);
      }
    }
    if (err.status === 403 && err.code === 'password_change_required' && typeof handlers.passwordRequired === 'function') {
      handlers.passwordRequired(err);
    }
  }

  function parseText(text) {
    if (!text) { return null; }
    try {
      return JSON.parse(text);
    } catch (err) {
      return null;
    }
  }

  async function request(method, path, body, options) {
    var opts = options || {};
    var headers = {};
    var init = {
      method: method,
      credentials: 'same-origin',
      headers: headers
    };

    if (body !== undefined) {
      headers['Content-Type'] = 'application/json';
      init.body = JSON.stringify(body);
    }
    if (method !== 'GET') {
      headers['X-CSRF-Token'] = getCsrf();
    }

    var response;
    try {
      response = await window.fetch(path, init);
    } catch (err) {
      var networkError = makeError(0, null, '网络连接失败，请检查网络后重试');
      networkError.code = 'network_error';
      throw networkError;
    }

    var text = '';
    try {
      text = await response.text();
    } catch (err) {
      text = '';
    }
    var data = parseText(text);

    if (!response.ok) {
      var apiError = makeError(response.status, data, '请求失败（HTTP ' + response.status + '）');
      handleAuthError(apiError, opts);
      throw apiError;
    }

    return data === null ? {} : data;
  }

  function upload(path, file, options) {
    var opts = options || {};
    return new Promise(function (resolve, reject) {
      var xhr = new window.XMLHttpRequest();
      xhr.open('POST', path, true);
      xhr.withCredentials = true;
      xhr.setRequestHeader('X-CSRF-Token', getCsrf());

      if (typeof opts.onProgress === 'function') {
        xhr.upload.addEventListener('progress', function (event) {
          if (event.lengthComputable) {
            opts.onProgress(Math.round((event.loaded / event.total) * 100), event.loaded, event.total);
          }
        });
      }

      xhr.onload = function () {
        var data = parseText(xhr.responseText);
        if (xhr.status >= 200 && xhr.status < 300) {
          resolve(data === null ? {} : data);
          return;
        }
        var err = makeError(xhr.status, data, '上传失败（HTTP ' + xhr.status + '）');
        handleAuthError(err, opts);
        reject(err);
      };

      xhr.onerror = function () {
        var err = makeError(0, null, '图片上传失败，请检查网络连接');
        err.code = 'network_error';
        reject(err);
      };

      xhr.onabort = function () {
        var err = makeError(0, null, '图片上传已取消');
        err.code = 'upload_aborted';
        reject(err);
      };

      var form = new window.FormData();
      form.append('file', file);
      xhr.send(form);
    });
  }

  var api = {
    get: function (path, options) { return request('GET', path, undefined, options); },
    post: function (path, body, options) { return request('POST', path, body === undefined ? {} : body, options); },
    patch: function (path, body, options) { return request('PATCH', path, body || {}, options); },
    put: function (path, body, options) { return request('PUT', path, body || {}, options); },
    del: function (path, body, options) { return request('DELETE', path, body, options); },
    upload: upload,
    setCsrf: setCsrf,
    getCsrf: getCsrf,
    clearCsrf: function () { setCsrf(''); },
    imageUrl: function (id) {
      return '/api/images/' + encodeURIComponent(String(id)) + '/file';
    },
    onUnauthorized: function (handler) { handlers.unauthorized = handler; },
    onPasswordRequired: function (handler) { handlers.passwordRequired = handler; }
  };

  RC.api = api;
})();
