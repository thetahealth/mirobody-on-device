using System;
using System.Runtime.InteropServices;

namespace Mirobody
{
    /// <summary>
    /// C# binding for libmirobody via P/Invoke. Talks to the C functions
    /// exported by mirobody.h; the same shared library every other FFI host
    /// loads. CoreCLR does not shadow the system C runtime the way the JDK does,
    /// so this works in-process without the CRT caveat noted for Java.
    ///
    /// The native library "mirobody" (mirobody.dll / libmirobody.so) must be on
    /// the loader path, with its transitive deps reachable on PATH /
    /// LD_LIBRARY_PATH. Run from the repo root so the default config's sql_dir
    /// ("res/sql") resolves. See bindings/README.md.
    /// </summary>
    public sealed class Server : IDisposable
    {
        private const string Lib = "mirobody";

        // On x64 there is a single calling convention, but Cdecl is the explicit,
        // correct choice for the extern "C" surface across platforms.
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr mirobody_start(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? configPath,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? dataDir);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        private static extern void mirobody_stop(IntPtr handle);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int mirobody_is_running(IntPtr handle);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int mirobody_listen_port(IntPtr handle);

        private IntPtr _handle = IntPtr.Zero;

        /// <summary>
        /// Start the server. Null/empty args fall back to config/defaults. The
        /// listen port and LLM keys come from config (HTTP_PORT / OPENAI_API_KEY /
        /// GOOGLE_API_KEY, which also read their environment variables); read the
        /// bound port back via <see cref="ListenPort"/>.
        /// </summary>
        public void Start(string? configPath, string? dataDir)
        {
            _handle = mirobody_start(configPath, dataDir);
            if (_handle == IntPtr.Zero)
                throw new InvalidOperationException("mirobody_start failed (see native log)");
        }

        public bool IsRunning => _handle != IntPtr.Zero && mirobody_is_running(_handle) != 0;

        public int ListenPort => _handle == IntPtr.Zero ? -1 : mirobody_listen_port(_handle);

        public void Stop()
        {
            if (_handle != IntPtr.Zero)
            {
                mirobody_stop(_handle);
                _handle = IntPtr.Zero;
            }
        }

        public void Dispose() => Stop();
    }
}
