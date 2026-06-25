
//----------------------------------------------------------------------------

const csNumber = "number";
const csString = "string";

function isNumber(n) {
    return typeof n === csNumber;
};
function isString(s) {
    return typeof s === csString;
};
function isObject(obj) {
    return obj instanceof Object;
};
function isFunction(f) {
    return f instanceof Function;
};

exports.isNumber = isNumber;
exports.isString = isString;
exports.isObject = isObject;
exports.isFunction = isFunction;

//----------------------------------------------------------------------------

function dom(tagName, styles, attributes, events) {
    var el = document.createElement(tagName && (typeof tagName === csString) ? tagName : "div");

    if (attributes && (attributes instanceof Object)) {
        for (var key of Object.keys(attributes)) {
            el.setAttribute(key, attributes[key]);
        }
    }

    if (events && (events instanceof Object)) {
        for (var key of Object.keys(events)) {
            if (events[key] instanceof Function) {
                if (key.indexOf("click") >= 0 || key.indexOf("mouse") >= 0) {
                    el.style.cursor = "pointer";
                }
                el.addEventListener(key, events[key]);
            }
        }
    }

    if (styles && (styles instanceof Object)) {
        for (var key of Object.keys(styles)) {
            el.style[key] = styles[key];
        }
    }

    return el;
};

function setStyle(el, styles) {
    if (styles && (styles instanceof Object)) {
        for (var key of Object.keys(styles)) {
            el.style[key] = styles[key];
        }
    }

    return el;
};

function setText(el, text) {
    if (isString(text)) {
        el.innerText = text;
    }
    return el;
};

function setHTML(el, html) {
    if (isString(html)) {
        el.innerHTML = html;
    }
    return el;
};

function img(src, styles, events) {
    var el = dom("img", styles, null, events);
    el.src = src;
    return el;
};

function clear(el) {
    while (el.firstChild) {
        el.removeChild(el.firstChild);
    }
    return el;
};

exports.dom      = dom;
exports.setStyle = setStyle;
exports.setText  = setText;
exports.setHTML  = setHTML;
exports.img      = img;
exports.clear    = clear;

//----------------------------------------------------------------------------
