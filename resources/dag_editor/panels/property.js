/**
 * panels/property.js — 右侧属性面板：按节点 schema 渲染 config 编辑器
 */
(function (global) {
    'use strict';

    const DagGraphAdapter = global.DagGraphAdapter;
    const DagWidgets = global.DagWidgets;

    /** @type {Object|null} 当前编辑中的 litegraph 节点 */
    let currentNode = null;

    /**
     * 渲染选中节点的属性面板。
     * @param {Object} lgNode litegraph 节点（null 表示未选中）
     * @param {Function} onConfigChanged (newConfig) => void
     */
    function show(lgNode, onConfigChanged) {
        currentNode = lgNode;
        const body = document.getElementById('property-body');
        body.innerHTML = '';

        if (!lgNode) {
            const hint = document.createElement('p');
            hint.className = 'hint';
            hint.textContent = '选中一个节点以编辑其 config。';
            body.appendChild(hint);
            return;
        }

        const spec = lgNode.spec || {};
        const catalogEntry = DagGraphAdapter.getCatalog()[lgNode.type_key] || spec;

        // ---- 标识信息 -------------------------------------------------------
        const header = document.createElement('div');
        header.innerHTML =
            '<div><strong>' + escapeHtml(spec.display_name || lgNode.type_key) + '</strong></div>'
            + '<div class="hint">' + escapeHtml(lgNode.type_key) + '</div>'
            + '<div class="property-row" style="margin-top:6px">'
            + '<label>显示名（可选）</label></div>';
        body.appendChild(header);

        const titleInput = document.createElement('input');
        titleInput.type = 'text';
        titleInput.value = lgNode.title !== lgNode._origTitle ? lgNode.title : '';
        titleInput.placeholder = lgNode._origTitle || '';
        titleInput.addEventListener('change', () => {
            lgNode.title = titleInput.value.trim() || (lgNode._origTitle || '');
            lgNode.setDirtyCanvas(true, true);
            if (onConfigChanged) onConfigChanged(lgNode.getConfig());
        });
        header.lastChild.appendChild(titleInput);

        if (spec.description) {
            const desc = document.createElement('p');
            desc.className = 'hint';
            desc.textContent = spec.description;
            body.appendChild(desc);
        }

        // 上下文键说明
        appendContextSection(body, '读取（reads）', spec.reads);
        appendContextSection(body, '写入（writes）', spec.writes);

        // ---- config 控件区 ---------------------------------------------------
        const inputs = catalogEntry.input || {};
        const configObject = lgNode.getConfig();

        appendInputs(body, '必填', inputs.required || {}, configObject, onConfigChanged);
        appendInputs(body, '可选', inputs.optional || {}, configObject, onConfigChanged);
    }

    function appendContextSection(container, title, keys) {
        if (!keys || !keys.length) return;
        const sectionTitle = document.createElement('div');
        sectionTitle.className = 'property-section-title';
        sectionTitle.textContent = title;
        container.appendChild(sectionTitle);

        keys.forEach((key) => {
            const chip = document.createElement('span');
            chip.className = 'context-key-chip';
            chip.textContent = key;
            container.appendChild(chip);
        });
    }

    function appendInputs(container, sectionTitleText, inputMap, configObject, onConfigChanged) {
        const keys = Object.keys(inputMap);
        if (!keys.length && !configHasKeysFor(configObject, inputMap)) return;

        const sectionTitle = document.createElement('div');
        sectionTitle.className = 'property-section-title';
        sectionTitle.textContent = sectionTitleText;
        container.appendChild(sectionTitle);

        // schema 中没有但 config 里已有的键也一并展示，避免数据不可见。
        const extraKeys = Object.keys(configObject).filter(
            (key) => !inputMap[key]);

        keys.concat(extraKeys.map((key) => [key, { key, type: 'text' }]))
            .forEach((entry) => {
                const key = Array.isArray(entry) ? entry[0] : entry;
                const inputSpec = Array.isArray(entry) ? entry[1] : inputMap[key];
                const row = DagWidgets.buildInputRow(
                    normalizeSpec(key, inputSpec),
                    configObject[key],
                    (newValue) => {
                        const config = currentNode.getConfig();
                        applyValue(config, key, newValue);
                        currentNode.setConfig(config);
                        if (onConfigChanged) onConfigChanged(config);
                    });

                if (!Array.isArray(entry)) {
                    container.appendChild(row.element);
                } else {
                    // 额外键：带删除按钮
                    const wrap = document.createElement('div');
                    wrap.appendChild(row.element);
                    const removeButton = document.createElement('button');
                    removeButton.textContent = '删除该键';
                    removeButton.addEventListener('click', () => {
                        const config = currentNode.getConfig();
                        delete config[key];
                        currentNode.setConfig(config);
                        show(currentNode, onConfigChanged);
                        if (onConfigChanged) onConfigChanged(config);
                    });
                    wrap.appendChild(removeButton);
                    container.appendChild(wrap);
                }
            });
    }

    function configHasKeysFor(configObject, inputMap) {
        return Object.keys(configObject || {}).some((key) => !inputMap[key]);
    }

    function applyValue(config, key, value) {
        if (value === undefined) {
            delete config[key];
        } else {
            config[key] = value;
        }
    }

    function normalizeSpec(key, inputSpec) {
        return {
            key,
            label: inputSpec.label || key,
            required: !!inputSpec.required,
            min: inputSpec.min,
            max: inputSpec.max,
            step: inputSpec.step,
            choices: inputSpec.choices,
            tooltip: inputSpec.tooltip,
            type: inputSpec.type || 'text',
        };
    }

    function escapeHtml(text) {
        const div = document.createElement('div');
        div.textContent = text === undefined || text === null ? '' : String(text);
        return div.innerHTML;
    }

    global.DagPropertyPanel = { show };
})(window);
