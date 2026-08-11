import QtQuick
import QtWebEngine
import Mirobody

// The offscreen render host's one visible-to-QML part: a hidden WebEngineView running
// render/render.html (MathJax + ECharts). It draws nothing and is never interacted
// with — C++ (renderhost.cpp) owns the queue, the cache and the files; this only runs
// the scripts it is handed and reports the answers back.
//
// It exists as a QML view rather than a QWebEnginePage in C++ because that is the
// difference between working and not: a standalone QWebEnginePage in this app killed
// its renderer process outright (0xC0000409 inside Qt6WebEngineCore.dll) as soon as a
// page needed V8 — a plain HTML file loaded, anything with a <script> did not — while
// this view runs the identical page and the identical scripts without complaint. It is
// also the shape harmony's port has (a hidden Web component in the UI tree), so the two
// clients now differ in nothing but language.
//
// Loaded through a Loader in Main.qml, so a build WITHOUT Qt WebEngine never reaches
// the `import QtWebEngine` above.
WebEngineView {
    id: host

    // Present but not drawn. Sized 1x1 rather than 0x0: a zero-sized view is not
    // guaranteed to get a renderer at all, and one pixel costs nothing.
    width: 1
    height: 1
    visible: false

    url: render.pageUrl

    onLoadingChanged: function (info) {
        if (info.status === WebEngineView.LoadSucceededStatus) {
            render.hostLoaded();
        } else if (info.status === WebEngineView.LoadFailedStatus) {
            render.hostFailed(info.errorString);
        }
    }

    // One job at a time (the page's document state is shared), so there is no need to
    // guard against overlapping calls here -- the C++ queue already serialises them.
    Connections {
        target: render
        function onRunScript(id, script) {
            host.runJavaScript(script, function (result) {
                render.scriptResult(id, result);
            });
        }
    }
}
