import ai.thetahealth.mirobody.NativeBridge;

import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;

/**
 * Runnable example for the JNI binding (the in-process path that works on
 * desktop HotSpot). Starts the server, hits its HTTP front door, then stops it.
 *
 * Usage: JniDemo [port] [dataDir]
 *
 * Run from the repo root so the default config's sql_dir ("res/sql") resolves,
 * with -Djava.library.path pointing at the directory holding mirobody_jni.dll
 * and the native deps reachable on PATH. See bindings/README.md.
 */
public final class JniDemo {
    public static void main(String[] args) throws Exception {
        int port = args.length > 0 ? Integer.parseInt(args[0]) : 18091;
        String dataDir = args.length > 1 ? args[1] : System.getProperty("java.io.tmpdir");

        try (NativeBridge m = new NativeBridge()) {
            m.start(null, dataDir, null, null, port);
            System.out.println("started: isRunning=" + m.isRunning() + " port=" + m.listenPort());

            HttpClient http = HttpClient.newBuilder().version(HttpClient.Version.HTTP_1_1).build();
            HttpResponse<String> r = http.send(
                HttpRequest.newBuilder(URI.create("http://127.0.0.1:" + m.listenPort() + "/")).build(),
                HttpResponse.BodyHandlers.ofString());
            System.out.println("GET / -> HTTP " + r.statusCode());

            m.stop();
            System.out.println("stopped: isRunning=" + m.isRunning());
        }
    }
}
