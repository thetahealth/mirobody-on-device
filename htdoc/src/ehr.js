// EHR connect (SMART on FHIR) — the one health source a web page can collect
// (Apple/Samsung/Xiaomi are on-device-only; see src/health/README.md). Flow:
//   1. showEhrModal(): search the provider directory, pick a tenant.
//   2. POST /health/ehr/authorize {fhir_base_url} -> {authorize_url}; redirect.
//   3. The EHR sends the browser to the server's /health/ehr/callback, which
//      exchanges the code and 302s back here with ?ehr=connected.
//   4. consumeEhrRedirect() (on load) surfaces that and offers to sync.
//   5. syncEhr(): POST /health/ehr/sync -> the server pulls FHIR and stores it.
// All the OAuth/token work is server-side; the browser only redirects + triggers.

const ui = require("./ui");
const net = require("./net");
const i18n = require("./i18n");
const config = require("./config");
const color = config.color;
const widgets = require("./widgets");
const button = widgets.button;
const field = widgets.field;

const t = i18n.t;

function stripQuery() {
    if (window.history && window.history.replaceState) {
        window.history.replaceState(null, "", window.location.origin + window.location.pathname);
    }
}

// On load: if we returned from the EHR callback (?ehr=connected|expired|error|
// token_error), strip it and surface the result. Returns true when handled.
function consumeEhrRedirect() {
    var params = new URLSearchParams(window.location.search || "");
    var status = params.get("ehr");
    if (!status) { return false; }
    stripQuery();
    var modals = require("./modals");
    if (status === "connected") {
        modals.showConfirmModal(t("ehrConnectedTitle"), t("ehrConnectedMsg"),
            t("ehrSyncNow"), false, function () { syncEhr(); });
    } else {
        modals.showConfirmModal(t("ehrErrorTitle"), t("ehrErrorMsg"), t("ehrOk"), false, function () {});
    }
    return true;
}

// POST /health/ehr/sync — the server pulls Observations from the connected EHR
// and stores them; report the count.
function syncEhr() {
    net.post("/health/ehr/sync", {}, function (data) {
        var modals = require("./modals");
        modals.showConfirmModal(t("ehrSyncDoneTitle"),
            t("ehrSyncDoneMsg", (data && data.posted) || 0), t("ehrOk"), false, function () {});
    }, function (msg) {
        var modals = require("./modals");
        modals.showConfirmModal(t("ehrErrorTitle"), msg || t("ehrErrorMsg"), t("ehrOk"), false, function () {});
    });
}

// The connect modal: search the directory, pick a tenant, redirect to its SMART
// authorize URL. Also offers "Sync now" for an already-connected EHR.
function showEhrModal() {
    var backdrop = ui.dom("div", {
        position: "fixed", inset: "0", background: "rgba(0, 0, 0, 0.4)",
        display: "flex", alignItems: "center", justifyContent: "center",
        padding: "16px", zIndex: "1000"
    });
    var card = ui.dom("div", {
        background: color.background, borderRadius: "14px", padding: "20px 22px",
        width: "100%", maxWidth: "460px", maxHeight: "80vh",
        boxShadow: "0 8px 32px rgba(0, 0, 0, 0.25)",
        display: "flex", flexDirection: "column", gap: "12px", overflow: "hidden"
    });

    card.appendChild(widgets.modalHeader(t("ehrConnectTitle"), function () { dismiss(); }));
    card.appendChild(ui.setText(ui.dom("div", {
        fontSize: "0.875rem", color: color.onSurfaceVar, lineHeight: "1.4"
    }), t("ehrConnectSubtitle")));

    // Manual entry — for a FHIR base URL not in the directory (e.g. a SMART
    // sandbox like https://launch.smarthealthit.org/v/r4/fhir).
    card.appendChild(ui.setText(ui.dom("div", {
        fontSize: "0.8rem", color: color.onSurfaceVar
    }), t("ehrManualLabel")));
    var manual = field({ type: "text", placeholder: "https://launch.smarthealthit.org/v/r4/fhir", autocomplete: "off" });
    manual.style.flex = "1";
    var manualBtn = button(t("ehrConnectBtn"), true, { click: function () {
        var url = manual.value.trim().replace(/\/+$/, "");
        if (url) { pick({ fhir_base_url: url }); }
    } });
    manualBtn.style.width = "104px";
    manualBtn.style.flexShrink = "0";
    var manualRow = ui.dom("div", { display: "flex", gap: "8px", alignItems: "center" });
    manualRow.appendChild(manual);
    manualRow.appendChild(manualBtn);
    card.appendChild(manualRow);

    card.appendChild(ui.dom("div", {
        height: "1px", background: color.outlineVar, opacity: "0.6", margin: "2px 0"
    }));

    var input = field({ type: "text", placeholder: t("ehrSearchPlaceholder"), autocomplete: "off" });
    input.style.flex = "1";
    var searchBtn = button(t("ehrSearch"), true, { click: search });
    searchBtn.style.width = "104px";
    searchBtn.style.flexShrink = "0";
    var searchRow = ui.dom("div", { display: "flex", gap: "8px", alignItems: "center" });
    searchRow.appendChild(input);
    searchRow.appendChild(searchBtn);
    card.appendChild(searchRow);

    var listWrap = ui.dom("div", {
        overflow: "auto", display: "flex", flexDirection: "column", gap: "4px", minHeight: "48px"
    });
    card.appendChild(listWrap);

    var status = ui.dom("div", { fontSize: "0.8rem", color: color.onSurfaceVar, minHeight: "1em" });
    card.appendChild(status);

    function dismiss() {
        if (backdrop.parentNode) { backdrop.parentNode.removeChild(backdrop); }
    }

    function pick(ep) {
        ui.setText(status, t("ehrConnecting"));
        net.post("/health/ehr/authorize", { fhir_base_url: ep.fhir_base_url }, function (data) {
            if (data && data.authorize_url) {
                window.location.href = data.authorize_url;   // hand off to the EHR
            } else {
                ui.setText(status, t("ehrErrorMsg"));
            }
        }, function (msg) { ui.setText(status, msg || t("ehrErrorMsg")); });
    }

    function search() {
        ui.setText(status, t("ehrSearching"));
        ui.clear(listWrap);
        net.get("/health/ehr/providers?q=" + encodeURIComponent(input.value.trim()), function (data) {
            var list = (data && data.providers) || [];
            ui.clear(listWrap);
            if (!list.length) { ui.setText(status, t("ehrNoResults")); return; }
            ui.setText(status, "");
            for (var i = 0; i < list.length; i++) {
                (function (ep) {
                    var row = ui.dom("button", {
                        textAlign: "start", width: "100%", border: "1px solid " + color.outlineVar,
                        background: "transparent", padding: "10px 12px", borderRadius: "8px",
                        color: color.onSurface, display: "flex", flexDirection: "column", gap: "2px"
                    }, { type: "button" }, { click: function () { pick(ep); } });
                    row.appendChild(ui.setText(ui.dom("span", { fontSize: "0.9rem" }),
                        ep.name || ep.fhir_base_url));
                    row.appendChild(ui.setText(ui.dom("span", {
                        fontSize: "0.72rem", color: color.onSurfaceVar, wordBreak: "break-all"
                    }), ep.fhir_base_url));
                    listWrap.appendChild(row);
                })(list[i]);
            }
        }, function (msg) { ui.setText(status, msg || t("ehrErrorMsg")); });
    }

    input.addEventListener("keydown", function (e) {
        if (e.key === "Enter") { e.preventDefault(); search(); }
    });

    var syncNow = button(t("ehrSyncNow"), false, { click: function () { dismiss(); syncEhr(); } });
    card.appendChild(ui.dom("div", {
        height: "1px", background: color.outlineVar, opacity: "0.6", margin: "4px 0"
    }));
    var actions = ui.dom("div", { display: "flex", justifyContent: "flex-end", gap: "8px" });
    actions.appendChild(syncNow);
    card.appendChild(actions);

    backdrop.addEventListener("click", function (evt) {
        if (evt.target === backdrop) { dismiss(); }
    });
    backdrop.appendChild(card);
    document.body.appendChild(backdrop);
}

exports.showEhrModal = showEhrModal;
exports.consumeEhrRedirect = consumeEhrRedirect;
exports.syncEhr = syncEhr;
