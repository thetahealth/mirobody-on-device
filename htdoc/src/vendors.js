// Vendor account management — the web client's calls to /vendors/*.
//   - showVendorsModal(): GET /vendors, then show ALL vendors grouped by category
//     (devices / platforms). Connected ones get a red "Disconnect"; the rest get
//     "Connect".
//   - unlinkVendor(): POST /vendors/{id}/unlink — the server revokes the grant at
//     the vendor and deletes the stored link + tokens.
// The OAuth/token work is server-side. NOTE: the web "Connect" OAuth flow (authorize
// -> callback -> token exchange) is not wired yet, so Connect is a placeholder.

const ui = require("./ui");
const net = require("./net");
const i18n = require("./i18n");
const config = require("./config");
const color = config.color;
const widgets = require("./widgets");
const button = widgets.button;

const t = i18n.t;

// Display names for known vendor ids (src/health/vendor/); unknown ids show the id.
var NAMES = {
    oura: "Oura", whoop: "WHOOP", polar: "Polar", fitbit: "Fitbit",
    withings: "Withings", dexcom: "Dexcom", garmin: "Garmin", huawei: "Huawei Health",
    terra: "Terra", validic: "Validic", rook: "Rook", spike: "Spike",
    junction: "Junction", wefitter: "WeFitter", thryve: "Thryve", human_api: "Human API",
    vitalera: "Vitalera", metriport: "Metriport", open_wearables: "Open Wearables",
    redox: "Redox", particle_health: "Particle Health", healthconnect: "HealthConnect CoPilot",
    lexisnexis: "LexisNexis"
};
// Monogram background per brand (readable with white text). Fallback = neutral grey.
var COLORS = {
    oura: "#4c6ef5", whoop: "#0ca678", polar: "#e8590c", fitbit: "#0c8599",
    withings: "#1c7ed6", dexcom: "#7048e8", garmin: "#2f9e44"
};
// Icon bundle from GET /vendors/icons: { id: "data:<mime>;base64,<...>" }. The server
// resolves each vendor's real site icon once and holds it in memory, so the browser
// makes ONE same-origin request and never calls a third party. Fetched once, cached
// here; null = not loaded yet. Ids missing from the map fall back to the monogram.
var ICONS = null;

// The full catalog, grouped. Mirrors src/health/vendor/ (device/ + phone/ under
// "devices"; platform/ under "platforms"). EHR is managed via "Connect EHR".
var CATALOG = [
    { labelKey: "vendorGroupDevices", ids: [
        "oura", "whoop", "polar", "fitbit", "withings", "dexcom", "garmin", "huawei"
    ] },
    { labelKey: "vendorGroupPlatforms", ids: [
        "terra", "validic", "rook", "spike", "junction", "wefitter", "thryve",
        "human_api", "vitalera", "metriport", "open_wearables", "redox",
        "particle_health", "healthconnect", "lexisnexis"
    ] }
];

function nameOf(id) { return NAMES[id] || id; }

// A colored circle with the brand's initial — the offline / unknown-brand fallback.
function monogram(id) {
    var el = ui.dom("span", {
        width: "24px", height: "24px", borderRadius: "50%",
        background: COLORS[id] || "#868e96", color: "#fff",
        display: "inline-flex", alignItems: "center", justifyContent: "center",
        fontSize: "0.72rem", fontWeight: "600", flexShrink: "0"
    });
    return ui.setText(el, nameOf(id).charAt(0).toUpperCase());
}

// Neutral placeholder holding the icon's slot while the icon bundle is still
// loading, so the full list can render immediately and the row doesn't reflow
// when the real icon lands.
function iconPlaceholder() {
    return ui.dom("span", {
        width: "24px", height: "24px", borderRadius: "6px", flexShrink: "0",
        background: color.surfaceLow, border: "1px solid " + color.outlineVar,
        display: "inline-block"
    });
}

// Brand icon: the server-provided data-URI icon (same-origin, no third-party call).
// While the bundle is still loading (ICONS null) a neutral placeholder holds the
// slot; once loaded, falls back to the monogram when the bundle has no icon for
// this id or the data URI fails to render.
function brandIcon(id) {
    if (ICONS === null) { return iconPlaceholder(); }
    var mono = monogram(id);
    var uri = ICONS[id];
    if (!uri) { return mono; }
    var img = ui.dom("img", {
        width: "24px", height: "24px", borderRadius: "6px", flexShrink: "0",
        objectFit: "contain", background: "#fff"
    }, { src: uri, alt: "" });
    img.addEventListener("error", function () {
        if (img.parentNode) { img.parentNode.replaceChild(mono, img); }
    });
    return img;
}

// A compact row action button; `danger` paints it red (destructive Disconnect).
function rowButton(label, danger, onClick) {
    var b = button(label, false, { click: onClick });
    b.style.padding = "6px 14px";
    b.style.fontSize = "0.8rem";
    b.style.flexShrink = "0";
    if (danger) {
        b.style.background = color.error;
        b.style.color = color.onError;
        b.style.border = "none";
    }
    return b;
}

// POST /vendors/{id}/unlink — confirm, then disconnect. `onDone` (optional) runs
// after a successful unlink (used to refresh the list).
function unlinkVendor(vendorId, displayName, onDone) {
    var modals = require("./modals");
    modals.showConfirmModal(
        t("vendorUnlinkTitle"),
        t("vendorUnlinkMsg", displayName || vendorId),
        t("vendorUnlink"), true,
        function () {
            net.post("/vendors/" + encodeURIComponent(vendorId) + "/unlink", {}, function () {
                if (typeof onDone === "function") { onDone(); }
                else {
                    modals.showAlertModal(t("vendorUnlinkedTitle"), t("vendorUnlinkedMsg"), t("ehrOk"));
                }
            }, function (msg) {
                modals.showAlertModal(t("ehrErrorTitle"), msg || t("ehrErrorMsg"), t("ehrOk"));
            });
        });
}

// Start the OAuth connect: GET /vendors/{id}/authorize -> {authorize_url}, then hand
// the browser off to the vendor. The vendor redirects back to the server callback,
// which exchanges the code + stores tokens and bounces here with ?vendor=<status>
// (see consumeVendorRedirect).
function connectVendor(vendorId, displayName) {
    net.get("/vendors/" + encodeURIComponent(vendorId) + "/authorize", function (data) {
        if (data && data.authorize_url) {
            window.location.href = data.authorize_url;
        } else {
            var modals = require("./modals");
            modals.showAlertModal(t("vendorConnectTitle"), t("ehrErrorMsg"), t("ehrOk"));
        }
    }, function (msg) {
        var modals = require("./modals");
        modals.showAlertModal(t("vendorConnectTitle"), msg || t("ehrErrorMsg"), t("ehrOk"));
    });
}

// On load: if we returned from a vendor OAuth callback (?vendor=connected|expired|
// error), strip it and surface the result. Returns true when handled.
function consumeVendorRedirect() {
    var params = new URLSearchParams(window.location.search || "");
    var status = params.get("vendor");
    if (!status) { return false; }
    if (window.history && window.history.replaceState) {
        window.history.replaceState(null, "", window.location.origin + window.location.pathname);
    }
    var modals = require("./modals");
    if (status === "connected") {
        modals.showAlertModal(t("vendorConnectedTitle"), t("vendorConnectedMsg"), t("ehrOk"));
    } else {
        modals.showAlertModal(t("ehrErrorTitle"), t("ehrErrorMsg"), t("ehrOk"));
    }
    return true;
}

// One catalog row: icon + name (+ Pending badge), and Connect / Disconnect on the
// right. `known` is whether the connected state has loaded; until it has, the
// action slot is left empty (the row height is reserved) so the full list can show
// immediately without guessing Connect vs Disconnect.
function vendorRow(id, link, reload, known) {
    var name = nameOf(id);
    var row = ui.dom("div", {
        display: "flex", alignItems: "center", justifyContent: "space-between",
        gap: "8px", border: "1px solid " + color.outlineVar, borderRadius: "8px", padding: "8px 12px"
    });
    var left = ui.dom("div", { display: "flex", alignItems: "center", gap: "10px", minWidth: "0" });
    left.appendChild(brandIcon(id));
    left.appendChild(ui.setText(ui.dom("span", {
        fontSize: "0.9rem", color: color.onSurface, overflow: "hidden", textOverflow: "ellipsis"
    }), name));
    if (known && link && !link.verified) {
        left.appendChild(ui.setText(ui.dom("span", {
            fontSize: "0.7rem", color: color.onSurfaceVar, border: "1px solid " + color.outlineVar,
            borderRadius: "6px", padding: "1px 6px", flexShrink: "0"
        }), t("vendorPending")));
    }
    row.appendChild(left);
    if (!known) {
        // Connected state not loaded yet — leave the action slot empty; the button
        // appears once /vendors lands.
    } else if (link) {
        row.appendChild(rowButton(t("vendorUnlink"), true, function () {
            unlinkVendor(id, name, reload);
        }));
    } else {
        row.appendChild(rowButton(t("vendorConnect"), false, function () {
            connectVendor(id, name);
        }));
    }
    return row;
}

// Modal: the device / platform catalog split into two tabs (Devices / Platforms),
// with per-vendor Connect / Disconnect. Defaults to Devices — the consumer
// wearables most users connect; the B2B platforms sit behind the second tab.
function showVendorsModal() {
    var backdrop = ui.dom("div", {
        position: "fixed", inset: "0", background: "rgba(0, 0, 0, 0.4)",
        display: "flex", alignItems: "center", justifyContent: "center",
        padding: "16px", zIndex: "1000"
    });
    var card = ui.dom("div", {
        background: color.background, borderRadius: "14px", padding: "20px 22px",
        width: "100%", maxWidth: "440px", maxHeight: "82vh",
        boxShadow: "0 8px 32px rgba(0, 0, 0, 0.25)",
        display: "flex", flexDirection: "column", gap: "10px", overflow: "hidden"
    });

    // Header: title + top-right close (X), shared across all dialogs.
    card.appendChild(widgets.modalHeader(t("vendorManageTitle"), function () { dismiss(); }));

    card.appendChild(ui.setText(ui.dom("div", {
        fontSize: "0.875rem", color: color.onSurfaceVar, lineHeight: "1.4"
    }), t("vendorManageSubtitle")));

    // Active tab = index into CATALOG. The connected-links map is cached across
    // tab switches (fetched once, refreshed only after a connect/disconnect), so
    // flipping tabs just re-renders the visible group without another request.
    var activeGroup = 0;
    var connected   = null;   // id -> link; null until the first fetch lands

    // Segmented Devices / Platforms control (selected tab in navy). One button
    // per CATALOG group; the tabs mirror with `dir` since they're plain flex.
    var tabBtns = [];
    function styleTabs() {
        for (var i = 0; i < tabBtns.length; i++) {
            var on = (i === activeGroup);
            tabBtns[i].style.background = on ? color.primary : "transparent";
            tabBtns[i].style.color      = on ? color.onPrimary : color.onSurfaceVar;
        }
    }
    var tabs = ui.dom("div", {
        display: "flex", gap: "4px", padding: "3px",
        background: color.surfaceLow, borderRadius: "10px",
        border: "1px solid " + color.outlineVar
    });
    for (var g = 0; g < CATALOG.length; g++) {
        (function (idx) {
            var b = ui.dom("button", {
                flex: "1 1 0", padding: "6px 10px", border: "none", borderRadius: "8px",
                cursor: "pointer", font: "inherit", fontSize: "0.85rem", fontWeight: "600",
                background: "transparent", color: color.onSurfaceVar
            }, { type: "button" });
            ui.setText(b, t(CATALOG[idx].labelKey));
            b.addEventListener("click", function () {
                if (activeGroup === idx) { return; }
                activeGroup = idx;
                styleTabs();
                renderList();
            });
            tabBtns.push(b);
            tabs.appendChild(b);
        })(g);
    }
    card.appendChild(tabs);

    // The list sizes to its content and only scrolls once it exceeds the cap, so
    // the shorter tab (Devices) shows every row without a scrollbar when there's
    // room, while the taller tab (Platforms) scrolls with the native scrollbar.
    // The cap keeps the tall tab from ballooning the dialog; it scales down on
    // short screens via the vh term.
    var listWrap = ui.dom("div", {
        maxHeight: "min(60vh, 460px)",
        overflow: "auto", display: "flex", flexDirection: "column", gap: "6px"
    });
    card.appendChild(listWrap);

    var status = ui.dom("div", { fontSize: "0.8rem", color: color.onSurfaceVar, minHeight: "1em" });
    card.appendChild(status);

    // Render the active group's rows from the static catalog immediately, marking
    // connected ones from the cached `connected` map. Renders the full list even
    // before data lands: icon slots show placeholders (ICONS null) and the action
    // buttons are withheld (connected null) until their fetches complete.
    function renderList() {
        ui.clear(listWrap);
        var known = connected !== null;
        var conn = connected || {};
        var ids = CATALOG[activeGroup].ids.slice().sort(function (a, b) {
            return nameOf(a).localeCompare(nameOf(b));
        });
        for (var j = 0; j < ids.length; j++) {
            listWrap.appendChild(vendorRow(ids[j], conn[ids[j]] || null, refresh, known));
        }
    }

    // Show the full catalog at once, then fill in the connected state and real
    // icons as their fetches land (each re-renders on completion). Independent
    // requests so a slow one doesn't hold back the list. Also the row reload after
    // a connect/disconnect.
    function refresh() {
        ui.setText(status, "");
        renderList();   // immediate: full list with placeholders
        if (ICONS === null) {
            net.get("/vendors/icons", function (icons) { ICONS = icons || {}; renderList(); },
                    function () { ICONS = {}; renderList(); });
        }
        net.get("/vendors", function (data) {
            connected = {};
            var links = data || [];
            for (var i = 0; i < links.length; i++) { connected[links[i].id] = links[i]; }
            renderList();
        }, function (msg) { ui.setText(status, msg || t("ehrErrorMsg")); });
    }

    function dismiss() {
        if (backdrop.parentNode) { backdrop.parentNode.removeChild(backdrop); }
    }

    backdrop.addEventListener("click", function (evt) {
        if (evt.target === backdrop) { dismiss(); }
    });
    backdrop.appendChild(card);
    document.body.appendChild(backdrop);

    styleTabs();
    refresh();
}

exports.unlinkVendor = unlinkVendor;
exports.showVendorsModal = showVendorsModal;
exports.consumeVendorRedirect = consumeVendorRedirect;
