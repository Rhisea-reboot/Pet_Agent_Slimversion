/**
 * app.js — DAG 编辑器主程序：装载、保存、SSE、撤销/重做、校验联动
 */
(function (global) {
    'use strict';

    const Api = global.DagApi;
    const Adapter = global.DagGraphAdapter;
    const Library = global.DagLibraryPanel;
    const PropertyPanel = global.DagPropertyPanel;

    /** @type {number} 服务器当前修订（乐观锁基准） */
    let serverRevision = 0;
    /** @type {Object|null} 最近一次 GET 的文档 */
    let currentDocument = null;

    /** 撤销/重做栈：存放文档 JSON 字符串快照 */
    const undoStack = [];
    const redoStack = [];
    const UNDO_LIMIT = 50;

    let dirty = false;          // 图上有未保存改动
    let saving = false;
    let loadingDocument = false; // 载入期间不记录撤销快照

    // ------------------------------------------------------------------
    // 状态栏
    // ------------------------------------------------------------------
    function setStatus(text, kind) {
        const element = document.getElementById('save-status');
        element.textContent = text;
        element.className = 'status-' + (kind || 'idle');
    }

    function setReloadStatus(text) {
        document.getElementById('reload-status').textContent = text || '';
    }

    // ------------------------------------------------------------------
    // 文档装载与渲染
    // ------------------------------------------------------------------
    async function loadFromServer(fitAfterLoad) {
        const response = await Api.getGraph();
        if (!response.ok) {
            setStatus('载入失败: ' + response.status, 'error');
            return;
        }
        serverRevision = response.data.revision;
        currentDocument = response.data.graph;
        loadingDocument = true;
        Adapter.loadDocument(currentDocument);
        loadingDocument = false;
        if (fitAfterLoad) Adapter.fitView();
        PropertyPanel.show(null, onConfigChanged);
        setStatus('已载入 revision ' + serverRevision, 'ok');
        scheduleLint();
    }

    // ------------------------------------------------------------------
    // 快照与撤销
    // ------------------------------------------------------------------
    function snapshot() {
        return JSON.stringify(Adapter.toDocument(currentDocument));
    }

    function pushUndo() {
        undoStack.push(lastSnapshot);
        if (undoStack.length > UNDO_LIMIT) undoStack.shift();
        redoStack.length = 0;
        lastSnapshot = snapshot();
    }

    let lastSnapshot = null;

    function restoreSnapshot(json) {
        const doc = JSON.parse(json);
        Adapter.loadDocument(doc);
        currentDocument = doc;
        markDirty();
        scheduleLint();
    }

    function undo() {
        if (!undoStack.length) return;
        redoStack.push(snapshot());
        restoreSnapshot(undoStack.pop());
    }

    function redo() {
        if (!redoStack.length) return;
        undoStack.push(snapshot());
        restoreSnapshot(redoStack.pop());
    }

    function markDirty() {
        dirty = true;
        setStatus('未保存', 'saving');
    }

    // ------------------------------------------------------------------
    // 保存
    // ------------------------------------------------------------------
    async function save() {
        if (saving) return;
        saving = true;
        setStatus('保存中…', 'saving');
        setReloadStatus('');

        const documentJson = Adapter.toDocument(currentDocument);
        const response = await Api.putGraph(documentJson, serverRevision);

        if (response.status === 200) {
            dirty = false;
            serverRevision = response.data.revision;
            const reload = response.data.reload || {};

            if (reload.applied === true) {
                setReloadStatus('热重载：已应用');
            } else if (reload.reason === 'deferred') {
                setReloadStatus('热重载：运行中，稍后自动生效');
            } else if (reload.reason && reload.reason !== 'runtime_unavailable') {
                setReloadStatus('热重载失败: ' + reload.reason);
            } else {
                setReloadStatus('');
            }

            setStatus('已保存 revision ' + serverRevision, 'ok');
            runLint(); // 保存响应里带 issues
        } else if (response.status === 409) {
            setStatus('保存冲突', 'error');
            showModal(
                '配置已被其他人修改',
                '服务器上的最新版本是 revision ' + response.data.revision
                + '。可以重新加载磁盘内容（丢弃本地修改），或用当前内容覆盖。',
                [
                    { label: '加载磁盘内容', value: 'adopt' },
                    { label: '覆盖服务器', value: 'overwrite', primary: true },
                    { label: '取消', value: null },
                ],
                async (choice) => {
                    if (choice === 'adopt') {
                        await loadFromServer(true);
                    } else if (choice === 'overwrite') {
                        serverRevision = response.data.revision;
                        await save();
                    }
                });
        } else if (response.status === 400) {
            setStatus('校验失败，未保存', 'error');
            renderIssues(response.data.issues || []);
        } else {
            setStatus('保存失败: ' + response.status, 'error');
        }

        saving = false;
    }

    // ------------------------------------------------------------------
    // 校验
    // ------------------------------------------------------------------
    let lintTimer = null;

    function scheduleLint() {
        clearTimeout(lintTimer);
        lintTimer = setTimeout(runLint, 300);
    }

    async function runLint() {
        const documentJson = Adapter.toDocument(currentDocument);
        const response = await Api.validate(documentJson);

        if (response.ok) {
            renderIssues(response.data.issues || []);
        }
    }

    function renderIssues(issues) {
        Adapter.applyIssues(issues);

        const summary = document.getElementById('issue-summary');
        const list = document.getElementById('issue-list');
        list.innerHTML = '';

        const errors = issues.filter((i) => i.severity === 'error');
        const warnings = issues.filter((i) => i.severity !== 'error');

        if (!issues.length) {
            summary.textContent = '校验：通过';
            summary.className = 'clean';
            return;
        }

        summary.textContent = '校验：' + errors.length + ' 个错误，'
            + warnings.length + ' 个警告';
        summary.className = errors.length ? 'has-errors' : 'has-warnings';

        issues.forEach((issue) => {
            const item = document.createElement('li');
            item.className = issue.severity;
            item.textContent = '[' + issue.code + '] '
                + (issue.nodeId ? issue.nodeId + ': ' : '') + issue.message;
            item.addEventListener('click', () => {
                if (issue.nodeId) Adapter.selectNodeById(issue.nodeId);
            });
            list.appendChild(item);
        });
    }

    // ------------------------------------------------------------------
    // 节点增删
    // ------------------------------------------------------------------
    function nextNodeId(type) {
        const existing = new Set(currentDocument.nodes.map((n) => n.id));
        const base = type.split('.').join('_');

        for (let index = 1; ; ++index) {
            const candidate = base + '_' + index;
            if (!existing.has(candidate)) return candidate;
        }
    }

    /** 库面板与拖放共用的新建入口；pos 可为空（放到视口中心）。 */
    function createNodeWithType(type, pos) {
        pushUndo();
        const id = nextNodeId(type);
        const lgNode = pos
            ? (() => {
                  const created = Adapter.addNodeAtCenter(type, id);
                  if (created) created.pos = pos;
                  return created;
              })()
            : Adapter.addNodeAtCenter(type, id);

        if (!lgNode) return null;

        currentDocument.nodes.push({ id, type, config: {} });
        markDirty();
        PropertyPanel.show(lgNode, onConfigChanged);
        scheduleLint();
        return lgNode;
    }

    /**
     * 删除选中节点（带受影响边提示）。
     * 拦截 Delete/Backspace：全局 keydown 弹确认框，litegraph 默认删除被屏蔽。
     */
    function installDeleteGuard() {
        const originalProcessKey = LGraphCanvas.prototype.processKey;

        // 屏蔽 litegraph 对 Delete 的默认立即删除（由全局 keydown 接管）。
        LGraphCanvas.prototype.processKey = function (key, e) {
            if ((key === 'Delete' || key === 'backspace' || key === 'Backspace')
                && this.selected_nodes
                && Object.keys(this.selected_nodes).length > 0
                && ev_targetIsEditable(e)) {
                return true;
            }
            return originalProcessKey.apply(this, arguments);
        };

        global.document.addEventListener('keydown', (ev) => {
            if (ev.key !== 'Delete' && ev.key !== 'Backspace') return;
            if (ev.target.matches('input, textarea')) return;

            const canvasInstance = Adapter.getCanvas();
            const selectedIds = Object.values(canvasInstance.selected_nodes || {})
                .map((node) => node._docId)
                .filter(Boolean);

            if (!selectedIds.length) return;

            ev.preventDefault();

            const affectedEdges = currentDocument.edges.filter(
                (edge) => selectedIds.includes(edge.from)
                    || selectedIds.includes(edge.to));

            const message = '删除 ' + selectedIds.length + ' 个节点'
                + (affectedEdges.length
                    ? '，将同时移除 ' + affectedEdges.length + ' 条连线'
                    : '');

            showModal('确认删除', message,
                [{ label: '删除', value: true, primary: true },
                 { label: '取消', value: false }],
                (confirmed) => { if (confirmed) deleteNodes(selectedIds); });
        });
    }

    function ev_targetIsEditable(ev) {
        return !(ev.target && ev.target.matches
            && ev.target.matches('input, textarea'));
    }

    function deleteNodes(nodeIds) {
        pushUndo();
        const graph = Adapter.getGraph();

        nodeIds.forEach((id) => {
            const lgNode = graph._nodes.find((n) => n._docId === id);
            if (lgNode) graph.remove(lgNode);
        });

        currentDocument.nodes = currentDocument.nodes.filter(
            (node) => !nodeIds.includes(node.id));
        currentDocument.edges = currentDocument.edges.filter(
            (edge) => !nodeIds.includes(edge.from) && !nodeIds.includes(edge.to));

        PropertyPanel.show(null, onConfigChanged);
        markDirty();
        scheduleLint();
    }

    // ------------------------------------------------------------------
    // 连线变化检测（包装 LGraphNode 原型方法，跨版本可靠）
    // ------------------------------------------------------------------
    let connectionNotifyInstalled = false;

    function installConnectionTracking() {
        if (connectionNotifyInstalled) return;
        connectionNotifyInstalled = true;

        const wrap = (object, name) => {
            const original = object[name];
            object[name] = function () {
                const result = original.apply(this, arguments);
                onTopologyChanged();
                return result;
            };
        };

        wrap(LGraphNode.prototype, 'connect');
        wrap(LGraphNode.prototype, 'disconnectOutput');
        wrap(LGraphNode.prototype, 'disconnectInput');
    }

    function onTopologyChanged() {
        if (loadingDocument) return;
        // toDocument 会从运行时图重建 edges
        pushUndo();
        markDirty();
        scheduleLint();
    }

    function onConfigChanged() {
        markDirty();
        scheduleLint();
    }

    /**
     * litegraph 只负责选择状态；属性编辑统一桥接到右侧 schema 面板，
     * 避免左键双击再弹出原版 litegraph 的第二套属性菜单。
     */
    function installCanvasSelectionBridge() {
        const graphCanvas = Adapter.getCanvas();
        graphCanvas.onSelectionChange = (selectedNodes) => {
            const selected = Object.values(selectedNodes || {});
            PropertyPanel.show(selected.length === 1 ? selected[0] : null, onConfigChanged);
        };
    }

    // ------------------------------------------------------------------
    // SSE
    // ------------------------------------------------------------------
    function connectEvents() {
        Api.connectEvents((name, payload) => {
            switch (name) {
            case 'graph.saved':
                serverRevision = payload.revision || serverRevision;
                break;

            case 'graph.external_changed':
                serverRevision = payload.revision || serverRevision;
                showExternalBanner(payload.revision);
                break;

            case 'graph.reload_failed':
                setReloadStatus('热重载失败: ' + (payload.reason || '未知原因'));
                break;

            case 'graph.reloaded':
                if (payload.applied) setReloadStatus('热重载：已应用');
                else setReloadStatus('热重载：稍后生效');
                break;

            case 'runtime.busy':
                setReloadStatus('Agent 运行中：保存将在本轮结束后生效');
                break;

            case 'runtime.idle':
                setReloadStatus('');
                break;

            case 'connection.lost':
                setStatus('事件流断开，尝试重连…', 'error');
                break;
            }
        });
    }

    function showExternalBanner(revision) {
        const banner = document.getElementById('external-banner');
        banner.classList.remove('hidden');
        document.getElementById('external-banner-text').textContent =
            '配置文件已被外部修改（revision ' + revision + '）。';

        document.getElementById('btn-adopt-external').onclick = async () => {
            banner.classList.add('hidden');
            await loadFromServer(true);
        };

        document.getElementById('btn-force-overwrite').onclick = async () => {
            banner.classList.add('hidden');
            serverRevision = revision || serverRevision;
            await save();
        };
    }

    // ------------------------------------------------------------------
    // 模态框
    // ------------------------------------------------------------------
    function showModal(title, message, buttons, onChoice) {
        const backdrop = document.getElementById('modal-backdrop');
        document.getElementById('modal-title').textContent = title;
        document.getElementById('modal-message').textContent = message;

        const buttonHost = document.getElementById('modal-buttons');
        buttonHost.innerHTML = '';

        buttons.forEach((buttonSpec) => {
            const button = document.createElement('button');
            button.textContent = buttonSpec.label;
            if (buttonSpec.primary) button.style.borderColor = 'var(--accent)';
            button.addEventListener('click', () => {
                backdrop.classList.add('hidden');
                onChoice(buttonSpec.value);
            });
            buttonHost.appendChild(button);
        });

        backdrop.classList.remove('hidden');
    }

    // ------------------------------------------------------------------
    // 工具栏与快捷键
    // ------------------------------------------------------------------
    function installToolbar() {
        document.getElementById('btn-save').addEventListener('click', save);
        document.getElementById('btn-undo').addEventListener('click', undo);
        document.getElementById('btn-redo').addEventListener('click', redo);
        document.getElementById('btn-fit').addEventListener('click',
            () => Adapter.fitView());

        document.getElementById('btn-export').addEventListener('click', () => {
            const canvasElement = Adapter.getCanvas().canvas;
            const link = document.createElement('a');
            link.download = 'dag_editor.png';
            link.href = canvasElement.toDataURL('image/png');
            link.click();
        });

        global.document.addEventListener('keydown', (ev) => {
            if (!(ev.ctrlKey || ev.metaKey)) return;
            const key = ev.key.toLowerCase();

            if (key === 's') { ev.preventDefault(); save(); }
            else if (key === 'z' && !ev.shiftKey) { ev.preventDefault(); undo(); }
            else if (key === 'y' || (key === 'z' && ev.shiftKey)) {
                ev.preventDefault();
                redo();
            }
        });
    }

    // ------------------------------------------------------------------
    // 启动
    // ------------------------------------------------------------------
    async function boot() {
        try {
            await runBootSequence();
        } catch (error) {
            setStatus('初始化失败: '
                + (error && error.message ? error.message : error), 'error');
            console.error('[DagEditor] boot 失败:', error);
        }
    }

    async function runBootSequence() {
        const objectInfoResponse = await Api.objectInfo();

        if (!objectInfoResponse.ok) {
            setStatus('无法获取节点目录: ' + objectInfoResponse.status, 'error');
            return;
        }

        Adapter.init(objectInfoResponse.data,
                     document.getElementById('editor-canvas'));

        Library.init(createNodeWithType);
        installToolbar();
        installDeleteGuard();
        installCanvasSelectionBridge();
        installConnectionTracking();

        // 节点添加入口：左侧节点库、拖放或画布右键菜单；左键不弹菜单。

        await loadFromServer(true);
        lastSnapshot = snapshot();
        connectEvents();

        // 关闭前提醒未保存改动。
        global.addEventListener('beforeunload', (ev) => {
            if (dirty) ev.preventDefault();
        });
    }

    global.DagEditorApp = {
        boot,
        createNodeWithType,
    };

    global.document.addEventListener('DOMContentLoaded', boot);
})(window);
