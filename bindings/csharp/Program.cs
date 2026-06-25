using System;
using System.IO;
using System.Net.Http;

// Runnable example for the Mirobody C# binding. Starts the server, hits its HTTP
// front door to prove it is serving, then stops it.
//
// Usage: mirobody-demo [dataDir]
//
// The listen port comes from config (HTTP_PORT env / config.yml, default 8080);
// the demo reads the bound port back via ListenPort. Run from the repo root so
// the default config's sql_dir ("res/sql") resolves, with mirobody.dll and its
// native deps on PATH. See bindings/README.md.

string dataDir = args.Length > 0 ? args[0] : Path.GetTempPath();

using var m = new Mirobody.Server();
m.Start(null, dataDir);
Console.WriteLine($"started: running={m.IsRunning} port={m.ListenPort}");

// HttpClient uses HTTP/1.1 for cleartext http:// by default, matching the server.
using var http = new HttpClient();
var resp = http.GetAsync($"http://127.0.0.1:{m.ListenPort}/").GetAwaiter().GetResult();
Console.WriteLine($"GET / -> HTTP {(int)resp.StatusCode}");

m.Stop();
Console.WriteLine($"stopped: running={m.IsRunning}");
