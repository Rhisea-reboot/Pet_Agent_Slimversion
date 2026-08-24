/**
 * graph_adapter.js — 文档模型 ↔ litegraph 图模型的双向转换
 *
 * 语义约定：
 * - 每个文档节点对应一个 litegraph 节点（类型 "dag/<type>"）；
 * - 每个节点至多一个入向 slot 与一个出向 slot（边是控制依赖，不是数据端口）；
 * - is_source 节点没有入向 slot；
 * - config 编辑在右侧属性面板完成，节点框上只显示摘要。
 */
(function (global) {
    'use strict';

    /** @type {Object<string, Object>} 类型键 → object_info 条目 */
    let catalog = {};
    /** @type {LGraph} */
    let graph = null;
    /** @type {LGraphCanvas} */
    let canvas = null;

    const WIDGET_TYPE_LABELS = {
        text: 'text', multiline: 'text', int: 'number',
        float: 'number', bool: 'toggle', enum: 'combo',
        string_list: 'string_list',
    };

    /**
     * 初始化目录与图；为每个节点类型注册一个通用 litegraph 节点类。
     */
    function init(objectInfo, hostCanvasElement) {
        catalog = objectInfo || {};
        hostElement = hostCanvasElement.parentElement || document.body;
        installLiteGraphInputPatches();

        graph = new LGraph();
        canvas = new LGraphCanvas(hostCanvasElement, graph);
        canvas._dagHiDpiEnabled = true;

        canvas.background_image = null;
        canvas.render_shadows = false;
        canvas.allow_dragcanvas = true;
        canvas.allow_dragnodes = true;
        // 左键只承担选中/拖拽/连线，不使用原版 litegraph 的双击搜索菜单。
        canvas.allow_searchbox = false;
        // 双击节点不打开 litegraph 自带的浮动属性窗，统一由右侧属性面板编辑。
        canvas.onShowNodePanel = () => {};
        canvas.show_info = false; // 关闭 litegraph 自带的 T/FPS 调试浮层

        installHiDpiSupport();
        installMousePolicy();

        // 清除原版 litegraph 自带的演示节点（basic/math/audio/...），
        // 右键菜单只保留 dag/* 节点。
        Object.keys(LiteGraph.registered_node_types).forEach((key) => {
            if (key.indexOf('dag/') !== 0) {
                delete LiteGraph.registered_node_types[key];
            }
        });
        LiteGraph.searchbox_extras = {};
        // 松开左键连线不再唤起默认菜单；需要新节点可由节点库或右键添加。
        LiteGraph.release_link_on_empty_shows_menu = false;

        window.addEventListener('resize', applyCanvasResolution);
        if (typeof ResizeObserver === 'function' && hostElement) {
            resizeObserver = new ResizeObserver(() => applyCanvasResolution());
            resizeObserver.observe(hostElement);
        }

        Object.keys(catalog).forEach((type) => registerNodeType(type));
        return canvas;
    }

    let hostElement = null;
    let resizeObserver = null;

    /**
     * 当前设备像素比。
     */
    function getDpr() {
        return Math.max(1, window.devicePixelRatio || 1);
    }

    /**
     * 让画布后备缓冲区跟随宿主容器尺寸与 DPR，
     * 避免“小画布被 CSS 拉伸”导致的模糊与巨型调试文字。
     */
    function applyCanvasResolution() {
        if (!hostElement) return;

        const dpr = getDpr();
        const previousDpr = canvas._dagDevicePixelRatio || 1;
        const width = Math.max(320, hostElement.clientWidth);
        const height = Math.max(240, hostElement.clientHeight);
        const targetWidth = Math.round(width * dpr);
        const targetHeight = Math.round(height * dpr);

        // ds.scale 是物理画布比例；DPR 变化时维持用户看到的 CSS 缩放级别。
        if (canvas._dagHiDpiEnabled && canvas.ds && previousDpr !== dpr) {
            canvas.ds.scale = canvas.ds.scale / previousDpr * dpr;
        }
        canvas._dagDevicePixelRatio = dpr;

        if (canvas.canvas.width !== targetWidth
            || canvas.canvas.height !== targetHeight) {
            canvas.canvas.width = targetWidth;
            canvas.canvas.height = targetHeight;

            // litegraph 将连线背景缓存在独立 bgcanvas；同步调整避免背景被拉伸。
            if (canvas.bgcanvas) {
                canvas.bgcanvas.width = targetWidth;
                canvas.bgcanvas.height = targetHeight;
            }
            canvas.setDirty(true, true);
        }

        canvas.canvas.style.width = width + 'px';
        canvas.canvas.style.height = height + 'px';
    }

    let liteGraphInputPatched = false;

    /**
     * 判断一个鼠标事件是否为右键；同时兼容 button 与旧版 which。
     */
    function isRightMouseButton(event) {
        return event && (event.button === 2 || event.which === 3);
    }

    /**
     * 临时把左键/中键事件的 client 坐标换为画布物理像素坐标。
     *
     * litegraph 0.7 的拖拽增量直接使用 clientX/clientY；在 DPR 画布上，
     * 仅重算 canvasX/canvasY 会导致节点移动距离偏小。右键事件必须保留
     * CSS 坐标，否则浏览器菜单的定位会被放大。
     */
    function withDevicePixelCoordinates(instance, event, callback) {
        if (!instance._dagHiDpiEnabled || isRightMouseButton(event)) {
            return callback.call(instance, event);
        }

        const rect = instance.canvas.getBoundingClientRect();
        const ratioX = rect.width > 0 ? instance.canvas.width / rect.width : 1;
        const ratioY = rect.height > 0 ? instance.canvas.height / rect.height : 1;

        if (ratioX === 1 && ratioY === 1) {
            return callback.call(instance, event);
        }

        const savedClientX = Object.getOwnPropertyDescriptor(event, 'clientX');
        const savedClientY = Object.getOwnPropertyDescriptor(event, 'clientY');
        const savedMarker = Object.getOwnPropertyDescriptor(event, '__dagPhysicalCoordinates');

        try {
            Object.defineProperties(event, {
                clientX: {
                    configurable: true,
                    value: (event.clientX - rect.left) * ratioX,
                },
                clientY: {
                    configurable: true,
                    value: (event.clientY - rect.top) * ratioY,
                },
                __dagPhysicalCoordinates: {
                    configurable: true,
                    value: true,
                },
            });
        } catch (_error) {
            // 某些浏览器若将原生事件字段锁定，仍可走 canvasX/canvasY 映射；
            // 只是无法额外校正 litegraph 内部的拖拽增量，不能让左键直接失效。
            restoreOwnProperty(event, 'clientX', savedClientX);
            restoreOwnProperty(event, 'clientY', savedClientY);
            restoreOwnProperty(event, '__dagPhysicalCoordinates', savedMarker);
            return callback.call(instance, event);
        }

        try {
            return callback.call(instance, event);
        } finally {
            restoreOwnProperty(event, 'clientX', savedClientX);
            restoreOwnProperty(event, 'clientY', savedClientY);
            restoreOwnProperty(event, '__dagPhysicalCoordinates', savedMarker);
        }
    }

    /** 还原临时覆盖的原生事件属性。 */
    function restoreOwnProperty(object, name, descriptor) {
        if (descriptor) {
            Object.defineProperty(object, name, descriptor);
        } else {
            delete object[name];
        }
    }

    /**
     * 在创建 LGraphCanvas 前安装一次全局输入补丁。
     * 这样构造函数绑定到的 down/move/up/wheel 回调已经是 DPR 安全版本。
     */
    function installLiteGraphInputPatches() {
        if (liteGraphInputPatched) return;
        liteGraphInputPatched = true;

        const originalAdjustMouseEvent = LGraphCanvas.prototype.adjustMouseEvent;
        const originalProcessMouseDown = LGraphCanvas.prototype.processMouseDown;
        const originalProcessMouseMove = LGraphCanvas.prototype.processMouseMove;
        const originalProcessMouseUp = LGraphCanvas.prototype.processMouseUp;
        const originalProcessMouseWheel = LGraphCanvas.prototype.processMouseWheel;

        LGraphCanvas.prototype.adjustMouseEvent = function (event) {
            if (!this._dagHiDpiEnabled || !this.canvas) {
                return originalAdjustMouseEvent.call(this, event);
            }

            const rect = this.canvas.getBoundingClientRect();
            const ratioX = rect.width > 0 ? this.canvas.width / rect.width : 1;
            const ratioY = rect.height > 0 ? this.canvas.height / rect.height : 1;
            const x = event.__dagPhysicalCoordinates
                ? event.clientX
                : (event.clientX - rect.left) * ratioX;
            const y = event.__dagPhysicalCoordinates
                ? event.clientY
                : (event.clientY - rect.top) * ratioY;

            this.last_mouse_position[0] = x;
            this.last_mouse_position[1] = y;
            event.canvasX = x / this.ds.scale - this.ds.offset[0];
            event.canvasY = y / this.ds.scale - this.ds.offset[1];
        };

        LGraphCanvas.prototype.processMouseDown = function (event) {
            return withDevicePixelCoordinates(this, event, originalProcessMouseDown);
        };
        LGraphCanvas.prototype.processMouseMove = function (event) {
            return withDevicePixelCoordinates(this, event, originalProcessMouseMove);
        };
        LGraphCanvas.prototype.processMouseUp = function (event) {
            return withDevicePixelCoordinates(this, event, originalProcessMouseUp);
        };
        LGraphCanvas.prototype.processMouseWheel = function (event) {
            return withDevicePixelCoordinates(this, event, originalProcessMouseWheel);
        };
    }

    /**
     * HiDPI 支持：后备缓冲区按 DPR 分配，图形以物理像素绘制。
     * 输入坐标的映射由 installLiteGraphInputPatches() 统一处理。
     */
    function installHiDpiSupport() {
        applyCanvasResolution();
    }

    /**
     * 交互约定：仅右键可以打开菜单。
     * 左键只用于选择、框选、拖拽、平移和连接端口。
     */
    function installMousePolicy() {
        const originalProcessContextMenu = canvas.processContextMenu;
        const originalShowLinkMenu = canvas.showLinkMenu;

        canvas.processContextMenu = function (node, event) {
            if (!isRightMouseButton(event)) return false;
            return originalProcessContextMenu.call(this, node, event);
        };

        canvas.showLinkMenu = function (link, event) {
            if (!isRightMouseButton(event)) return false;
            return originalShowLinkMenu.call(this, link, event);
        };
    }

    function getGraph() { return graph; }
    function getCanvas() { return canvas; }
    function getCatalog() { return catalog; }

    /**
     * 为单个节点类型注册通用节点类。
     */
    function registerNodeType(type) {
        const spec = catalog[type];

        class DagNode extends LGraphNode {
            static title = spec.display_name;
            constructor() {
                super();
                this.type_key = type;
                this.spec = spec;

                // 原版 litegraph 的 _ctor 会把标题置为 "Unnamed"，
                // 必须在 super() 之后用实例字段覆盖。
                this.title = spec.display_name;
                this.color = '#2b2c33';
                this.bgcolor = '#31333c';
                this.boxcolor = spec.color || '#4aa3ff';

                if (!spec.is_source) {
                    this.addInput('in', 'control');
                }
                this.addOutput('out', 'control');

                this.size = [260, 96];
                this.resizable = false;

                this.serialize_widgets = false;
            }

            /** 节点上显示 config 摘要（按节点宽度截断，避免溢出）。 */
            onDrawForeground(ctx) {
                const summary = summarizeConfig(this.getConfig());
                ctx.save();
                ctx.font = '11px sans-serif';
                ctx.fillStyle = '#8a8b92';
                ctx.textAlign = 'left';

                const maxWidth = this.size[0] - 16;
                summary.slice(0, 4).forEach((line, i) => {
                    ctx.fillText(truncateToWidth(ctx, line, maxWidth), 8, 36 + i * 14);
                });
                ctx.restore();
            }

            getConfig() { return this._docConfig || {}; }
            setConfig(configObject) {
                this._docConfig = configObject || {};
                this.setDirtyCanvas(true, true);
            }
        }

        DagNode.title = spec.display_name;
        DagNode.desc = spec.description || '';
        // 让双击搜索能按中文名与类型键命中
        DagNode.searchable = () => [spec.display_name, type].join(' ');

        LiteGraph.registerNodeType('dag/' + type, DagNode);
    }

    /**
     * 按像素宽度截断文本（末尾加省略号）。
     */
    function truncateToWidth(ctx, text, maxWidth) {
        if (ctx.measureText(text).width <= maxWidth) {
            return text;
        }

        let truncated = text;

        while (truncated.length > 1
               && ctx.measureText(truncated + '…').width > maxWidth) {
            truncated = truncated.slice(0, -1);
        }

        return truncated + '…';
    }

    /**
     * 生成 config 的单行摘要行。
     */
    function summarizeConfig(configObject) {
        const keys = Object.keys(configObject || {});
        if (!keys.length) return ['(config 为空)'];
        return keys.map((key) => {
            let value = configObject[key];
            if (typeof value === 'object') value = JSON.stringify(value);
            const text = String(value);
            return key + ': ' + (text.length > 26 ? text.slice(0, 25) + '…' : text);
        });
    }

    /**
     * 拓扑分层自动布局：x 按深度，y 按层内顺序。
     */
    function autoLayout(doc) {
        const nodes = doc.nodes || [];
        const edges = doc.edges || [];
        const depth = {};
        nodes.forEach((node) => { depth[node.id] = 0; });

        for (let round = 0; round < nodes.length; ++round) {
            edges.forEach((edge) => {
                const candidate = depth[edge.from] + 1;
                if ((depth[edge.to] || 0) < candidate && candidate < nodes.length) {
                    depth[edge.to] = candidate;
                }
            });
        }

        const perLayer = {};
        nodes.forEach((node) => {
            const layer = depth[node.id] || 0;
            perLayer[layer] = (perLayer[layer] || 0) + 1;
            node.__layout = {
                x: 80 + layer * 280,
                y: 60 + (perLayer[layer] - 1) * 150,
            };
        });
        return doc;
    }

    /**
     * 文档 → litegraph 图（全量重建）。
     */
    function loadDocument(doc) {
        graph.clear();

        const byId = {};

        (doc.nodes || []).forEach((docNode) => {
            const created = LiteGraph.createNode('dag/' + docNode.type);
            if (!created) {
                console.warn('[adapter] unknown type skipped:', docNode.type);
                return;
            }
            created.id = undefined; // 由 litegraph 重新分配运行时 id
            const editorMeta = (doc.editor && doc.editor.nodes && doc.editor.nodes[docNode.id]) || {};
            if (Array.isArray(editorMeta.pos)) {
                created.pos = [editorMeta.pos[0], editorMeta.pos[1]];
            } else {
                if (!doc.__autoLayoutDone) autoLayout(doc);
                created.pos = [docNode.__layout.x, docNode.__layout.y];
            }
            if (Array.isArray(editorMeta.size) && editorMeta.size.length === 2) {
                created.size = [editorMeta.size[0], editorMeta.size[1]];
            }
            if (editorMeta.collapsed) created.collapse();
            created._docId = docNode.id;
            created._origTitle = created.title;
            if (editorMeta.title) created.title = editorMeta.title;
            created.setConfig(docNode.config || {});
            graph.add(created);
            byId[docNode.id] = created;
        });

        (doc.edges || []).forEach((edge) => {
            const fromNode = byId[edge.from];
            const toNode = byId[edge.to];
            if (!fromNode || !toNode) return;
            if (!toNode.inputs || !toNode.inputs.length) return;
            fromNode.connect(0, toNode, 0);
        });

        if (doc.editor && typeof doc.editor.zoom === 'number' && canvas.ds) {
            // zoom 需按 DPR 折算；offset 是图坐标，不能随 DPR 放大。
            const dpr = getDpr();
            canvas.ds.scale = (doc.editor.zoom || 1) * dpr;
            if (Array.isArray(doc.editor.scroll)) {
                canvas.ds.offset = [doc.editor.scroll[0], doc.editor.scroll[1]];
            }
        }
        canvas.setDirty(true, true);
    }

    /**
     * litegraph 图 → 文档（保留原 editor 布局字段之外的信息）。
     */
    function toDocument(previousDoc) {
        const dpr = getDpr();
        const doc = {
            version: 2,
            nodes: [],
            edges: [],
            editor: {
                revision: previousDoc && previousDoc.editor ? previousDoc.editor.revision : 0,
                // offset 已经是图坐标；仅 zoom 是物理像素比例，需除以 DPR。
                scroll: canvas.ds
                    ? [canvas.ds.offset[0], canvas.ds.offset[1]]
                    : [0, 0],
                zoom: canvas.ds ? canvas.ds.scale / dpr : 1,
                nodes: {},
            },
        };

        const runtimeToDocId = {};

        graph._nodes.forEach((lgNode) => {
            if (!lgNode._docId) return;
            runtimeToDocId[String(lgNode.id)] = lgNode._docId;
            doc.nodes.push({
                id: lgNode._docId,
                type: lgNode.type_key,
                config: lgNode.getConfig(),
            });
            doc.editor.nodes[lgNode._docId] = {
                pos: [Math.round(lgNode.pos[0]), Math.round(lgNode.pos[1])],
                size: [Math.round(lgNode.size[0]), Math.round(lgNode.size[1])],
                collapsed: !!lgNode.collapsed,
            };
            const customTitle = lgNode.title !== lgNode._origTitle ? lgNode.title : undefined;
            if (customTitle) doc.editor.nodes[lgNode._docId].title = customTitle;
        });

        eachLink(graph, (link) => {
            const fromId = runtimeToDocId[String(link.origin_id)];
            const toId = runtimeToDocId[String(link.target_id)];
            if (fromId && toId && fromId !== toId) {
                doc.edges.push({ from: fromId, to: toId });
            }
        });

        return doc;
    }

    /**
     * 版本兼容的连线枚举：graph.links 可能是数组、对象或 Map。
     */
    function eachLink(graph, callback) {
        const links = graph.links;

        if (!links) return;

        if (typeof links.forEach === 'function') {
            links.forEach((link) => { if (link) callback(link); });
            return;
        }

        Object.keys(links).forEach((key) => {
            const link = links[key];
            if (link) callback(link);
        });
    }

    /**
     * 按 id 定位并选中节点。
     */
    function selectNodeById(nodeId) {
        const found = graph._nodes.find((n) => n._docId === nodeId);
        if (found) {
            canvas.selectNodes([found]);
            canvas.centerOnNode(found);
        }
    }

    /**
     * 高亮校验问题节点（红色描边）；清除旧标记。
     */
    function applyIssues(issues) {
        graph._nodes.forEach((n) => { n.boxcolor = (n.spec && n.spec.color) || '#4aa3ff'; });
        (issues || []).forEach((issue) => {
            if (issue.severity !== 'error' && issue.severity !== 'warning') return;
            const target = graph._nodes.find((n) => n._docId === issue.nodeId);
            if (target) {
                target.boxcolor = issue.severity === 'error' ? '#ef5350' : '#ffb300';
            }
        });
        canvas.setDirty(true, true);
    }

    /**
     * 视口适配全部节点。
     */
    function fitView() {
        if (!graph._nodes.length) return;
        let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
        graph._nodes.forEach((n) => {
            minX = Math.min(minX, n.pos[0]);
            minY = Math.min(minY, n.pos[1]);
            maxX = Math.max(maxX, n.pos[0] + n.size[0]);
            maxY = Math.max(maxY, n.pos[1] + n.size[1]);
        });

        const dpr = getDpr();
        // 视觉尺寸按 CSS 像素计算，再折算成物理像素比例。
        const cssWidth = canvas.canvas.width / dpr;
        const cssHeight = canvas.canvas.height / dpr;
        const scale = Math.min(1.2, Math.min(
            cssWidth / Math.max(200, maxX - minX + 160),
            cssHeight / Math.max(200, maxY - minY + 160))) * dpr;

        canvas.ds.scale = scale;
        // DragAndScale 的 offset 使用图坐标：physical = (graph + offset) * scale。
        canvas.ds.offset = [80 * dpr / scale - minX, 60 * dpr / scale - minY];
        canvas.setDirty(true, true);
    }

    /**
     * 在视口中心添加指定类型的节点。
     */
    function addNodeAtCenter(type, newId) {
        const created = LiteGraph.createNode('dag/' + type);
        if (!created) return null;
        const viewCenter = canvas.convertCanvasToOffset([
            canvas.canvas.width / 2,
            canvas.canvas.height / 2,
        ]);
        created.pos = [viewCenter[0] - 110, viewCenter[1] - 40];
        created._docId = newId;
        graph.add(created);
        canvas.selectNodes([created]);
        return created;
    }

    global.DagGraphAdapter = {
        init, getGraph, getCanvas, getCatalog,
        loadDocument, toDocument,
        selectNodeById, applyIssues, fitView, addNodeAtCenter,
        autoLayout,
    };
})(window);
