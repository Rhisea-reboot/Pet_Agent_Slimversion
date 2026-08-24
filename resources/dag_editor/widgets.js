/**
 * widgets.js — DagWidgetType → HTML 控件工厂
 *
 * 每个控件以 {get(), set(value)} 形式暴露读写；变更通过 onChange 回调上报，
 * 由属性面板统一写回文档并触发防抖 lint。
 */
(function (global) {
    'use strict';

    /**
     * 创建一个属性行（label + 控件 + tooltip）。
     */
    function buildInputRow(inputSpec, currentValue, onChange) {
        const row = document.createElement('div');
        row.className = 'property-row';

        const label = document.createElement('label');
        label.textContent = inputSpec.label || inputSpec.key;
        if (inputSpec.required) label.textContent += ' *';
        row.appendChild(label);

        let control;
        switch (inputSpec.type) {
        case 'int': control = numberControl(intConstraints(inputSpec), parseIntSafe(currentValue)); break;
        case 'float': control = numberControl(floatConstraints(inputSpec), parseFloatSafe(currentValue)); break;
        case 'bool': control = boolControl(currentValue); break;
        case 'enum': control = enumControl(inputSpec, currentValue); break;
        case 'multiline': control = multilineControl(currentValue); break;
        case 'string_list': control = stringListControl(currentValue); break;
        default: control = textControl(currentValue); break;
        }

        control.element.addEventListener('change', () => {
            onChange(control.read());
        });

        row.appendChild(control.element);

        if (inputSpec.tooltip) {
            const tip = document.createElement('div');
            tip.className = 'property-tooltip';
            tip.textContent = inputSpec.tooltip;
            row.appendChild(tip);
        }

        return { element: row, read: control.read };
    }

    function parseIntSafe(value) {
        const parsed = parseInt(value, 10);
        return Number.isFinite(parsed) ? parsed : undefined;
    }

    function parseFloatSafe(value) {
        const parsed = parseFloat(value);
        return Number.isFinite(parsed) ? parsed : undefined;
    }

    function intConstraints(spec) {
        return { min: spec.min, max: spec.max, step: 1 };
    }

    function floatConstraints(spec) {
        return { min: spec.min, max: spec.max, step: spec.step || 0.01 };
    }

    function textControl(value) {
        const element = document.createElement('input');
        element.type = 'text';
        element.value = value === undefined || value === null ? '' : String(value);
        return {
            element,
            read: () => element.value,
        };
    }

    function multilineControl(value) {
        const element = document.createElement('textarea');
        element.value = value === undefined || value === null ? '' : String(value);
        return {
            element,
            read: () => element.value,
        };
    }

    function numberControl(constraints, value) {
        const element = document.createElement('input');
        element.type = 'number';
        if (Number.isFinite(constraints.min)) element.min = constraints.min;
        if (Number.isFinite(constraints.max)) element.max = constraints.max;
        if (Number.isFinite(constraints.step)) element.step = constraints.step;
        element.value = value === undefined ? '' : value;

        return {
            element,
            read: () => {
                const parsed = parseFloat(element.value);
                return Number.isFinite(parsed)
                    ? (constraints.step === 1 && Number.isInteger(parsed) ? parsed : parsed)
                    : undefined;
            },
        };
    }

    function boolControl(value) {
        const element = document.createElement('select');
        [true, false].forEach((optionValue) => {
            const option = document.createElement('option');
            option.value = optionValue ? 'true' : 'false';
            option.textContent = optionValue ? '启用' : '停用';
            element.appendChild(option);
        });
        element.value = value === undefined || value === null || value ? 'true' : 'false';

        return {
            element,
            read: () => element.value === 'true',
        };
    }

    function enumControl(inputSpec, value) {
        const element = document.createElement('select');
        (inputSpec.choices || []).forEach((choice) => {
            const option = document.createElement('option');
            option.value = choice;
            option.textContent = choice;
            element.appendChild(option);
        });
        const normalized = value === undefined || value === null
            ? (inputSpec.choices && inputSpec.choices[0])
            : String(value);
        element.value = normalized;

        // 值不在选项里时保留原值，避免静默改写用户配置。
        if (element.selectedIndex < 0 && inputSpec.choices && inputSpec.choices.length) {
            const extraOption = document.createElement('option');
            extraOption.value = normalized;
            extraOption.textContent = normalized + '（当前值）';
            element.appendChild(extraOption);
            element.value = normalized;
        }

        return {
            element,
            read: () => element.value,
        };
    }

    function stringListControl(value) {
        const element = document.createElement('input');
        element.type = 'text';
        const initial = Array.isArray(value) ? value.join(', ') : (value || '');
        element.value = initial;
        element.placeholder = '逗号分隔';

        return {
            element,
            read: () => element.value
                .split(',')
                .map((part) => part.trim())
                .filter(Boolean),
        };
    }

    global.DagWidgets = { buildInputRow };
})(window);
