/**
 * panels/library.js — 左侧节点库（按 category 分组 + 搜索 + 点击/拖拽添加）
 */
(function (global) {
    'use strict';

    const DagGraphAdapter = global.DagGraphAdapter;

    /**
     * 渲染库面板；onAdd(type) 在用户点击或拖放时回调。
     */
    function render(filterText, onAdd) {
        const tree = document.getElementById('library-tree');
        tree.innerHTML = '';

        const catalog = DagGraphAdapter.getCatalog();
        const groups = {};

        Object.keys(catalog).forEach((type) => {
            const spec = catalog[type];
            const category = spec.category || '其他';
            (groups[category] = groups[category] || []).push({ type, spec });
        });

        const needle = (filterText || '').trim().toLowerCase();

        Object.keys(groups).sort().forEach((category) => {
            const matches = groups[category].filter((entry) =>
                !needle
                || entry.type.toLowerCase().includes(needle)
                || entry.spec.display_name.toLowerCase().includes(needle)
                || (entry.spec.description || '').toLowerCase().includes(needle));

            if (!matches.length) return;

            const title = document.createElement('div');
            title.className = 'library-category';
            title.textContent = category;
            tree.appendChild(title);

            matches.forEach((entry) => {
                const item = document.createElement('div');
                item.className = 'library-item';
                item.textContent = entry.spec.display_name;
                item.title = entry.type + '\n' + (entry.spec.description || '');
                item.style.setProperty('--node-color', entry.spec.color || '#666');

                item.addEventListener('click', () => onAdd(entry.type));
                item.addEventListener('dragstart', (ev) => {
                    ev.dataTransfer.setData('text/dag-node-type', entry.type);
                    ev.dataTransfer.effectAllowed = 'copy';
                });
                item.draggable = true;

                tree.appendChild(item);
            });
        });
    }

    function init(onAdd, onFilterChanged) {
        const search = document.getElementById('library-search');

        let debounce = null;
        search.addEventListener('input', () => {
            clearTimeout(debounce);
            debounce = setTimeout(() => {
                render(search.value, onAdd);
                if (onFilterChanged) onFilterChanged(search.value);
            }, 120);
        });

        render('', onAdd);

        // 拖放到画布添加：drop 坐标换算为图坐标。
        const hostCanvas = document.getElementById('editor-canvas');
        hostCanvas.addEventListener('dragover', (ev) => {
            ev.preventDefault();
            ev.dataTransfer.dropEffect = 'copy';
        });
        hostCanvas.addEventListener('drop', (ev) => {
            ev.preventDefault();
            const type = ev.dataTransfer.getData('text/dag-node-type');
            if (!type) return;

            const lgCanvas = DagGraphAdapter.getCanvas();
            const rect = hostCanvas.getBoundingClientRect();
            const dpr = Math.max(1, window.devicePixelRatio || 1);
            // convertCanvasToOffset 期望画布物理像素坐标
            const local = [(ev.clientX - rect.left) * dpr,
                           (ev.clientY - rect.top) * dpr];
            const graphPos = lgCanvas.convertCanvasToOffset(local);
            const created = global.DagEditorApp.createNodeWithType(
                type, [graphPos[0] - 110, graphPos[1] - 30]);
            if (created && onAdd) onAdd(type, created);
        });
    }

    global.DagLibraryPanel = { init, render };
})(window);
