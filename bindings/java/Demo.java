import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;

/**
 * Runnable example for the Mirobody Java binding. Starts the server via
 * libmirobody, hits its HTTP front door to prove it is serving, then stops it.
 *
 * Usage: Demo <path-to-mirobody.dll-or-.so> [dataDir]
 *
 * The listen port comes from config (HTTP_PORT env / config.yml, default 8080);
 * the demo reads the bound port back via listenPort(). Run from the repo root so
 * the default config's sql_dir ("res/sql") resolves, and put the native
 * dependency directory on PATH (Windows) / LD_LIBRARY_PATH (Linux) so the
 * DLL/.so's transitive deps load. See bindings/README.md.
 */
public final class Demo {
    public static void main(String[] args) throws Exception {
        if (args.length < 1) {
            System.err.println("usage: Demo <library-path> [dataDir]");
            System.exit(2);
        }
        String lib = args[0];
        String dataDir = args.length > 1 ? args[1] : System.getProperty("java.io.tmpdir");

        try (Mirobody m = new Mirobody(lib)) {
            m.start(null, dataDir);
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
