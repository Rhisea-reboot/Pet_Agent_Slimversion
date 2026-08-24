/**
 * api.js — 后端 REST/SSE 访问封装
 *
 * token 从 URL 查询参数读取一次，之后所有 API 请求经 X-DAG-Token 头携带。
 */
(function (global) {
    'use strict';

    const params = new URLSearchParams(global.location.search);
    const TOKEN = params.get('token') || '';

    function headers(extra) {
        const base = { 'X-DAG-Token': TOKEN };
        if (extra && extra['Content-Type']) {
            base['Content-Type'] = extra['Content-Type'];
        }
        return base;
    }

    async function request(method, path, body, querySuffix) {
        let url = path;
        if (querySuffix) {
            url += (path.includes('?') ? '&' : '?') + querySuffix;
        }

        const options = {
            method,
            headers: headers(body ? { 'Content-Type': 'application/json' } : null),
        };
        if (body !== undefined && body !== null) {
            options.body = typeof body === 'string' ? body : JSON.stringify(body);
        }

        const response = await fetch(url, options);

        let payload = null;
        const text = await response.text();
        try { payload = text ? JSON.parse(text) : null; } catch (e) { payload = { raw: text }; }

        return { status: response.status, ok: response.ok, data: payload };
    }

    global.DagApi = {
        token: TOKEN,

        meta() { return request('GET', '/api/meta'); },
        objectInfo() { return request('GET', '/api/object_info'); },
        getGraph() { return request('GET', '/api/graph'); },

        /** 保存文档；返回 {status, data}，409 时 data 含服务器最新图。 */
        putGraph(documentJson, baseRevision) {
            return request(
                'PUT',
                '/api/graph',
                documentJson,
                'baseRevision=' + encodeURIComponent(baseRevision));
        },

        validate(documentJson) { return request('POST', '/api/graph/validate', documentJson); },
        reload() { return request('POST', '/api/graph/reload', {}); },
        runtimeStatus() { return request('GET', '/api/runtime/status'); },

        /** 建立 SSE 连接；onEvent(name, payload)。 */
        connectEvents(onEvent) {
            const source = new EventSource('/api/events?token=' + encodeURIComponent(TOKEN));
            const names = [
                'graph.saved', 'graph.external_changed',
                'graph.reloaded', 'graph.reload_failed',
                'runtime.busy', 'runtime.idle',
            ];
            names.forEach((name) => {
                source.addEventListener(name, (ev) => {
                    let payload = {};
                    try { payload = ev.data ? JSON.parse(ev.data) : {}; } catch (e) { /* 忽略坏帧 */ }
                    onEvent(name, payload);
                });
            });
            source.onerror = () => {
                // 断线后 EventSource 自动重连；这里仅更新 UI 状态。
                onEvent('connection.lost', {});
            };
            return source;
        },
    };
})(window);
