"""Python bindings for the mirobody on-device server.

The heavy lifting lives in the compiled extension ``mirobody._mirobody`` (a
pybind11 module wrapping the C++ ``mirobody::Server``). This package re-exports
the public surface and adds a small convenience helper.

Example
-------
    import mirobody

    # Block-scoped: starts on enter, stops on exit.
    with mirobody.Server(listen_port=8080, openai_api_key="sk-...") as srv:
        print("listening on", srv.listen_port())
        ...  # talk to the server over http://127.0.0.1:<port>

    # Or manage the lifecycle yourself:
    srv = mirobody.start(listen_port=8080)
    ...
    srv.stop()
"""

from ._mirobody import Server

__all__ = ["Server", "start", "__version__"]

try:
    from importlib.metadata import PackageNotFoundError
    from importlib.metadata import version as _pkg_version

    __version__ = _pkg_version("mirobody")
except PackageNotFoundError:  # running from a source tree without an install
    __version__ = "0+unknown"


def start(**kwargs) -> Server:
    """Construct a :class:`Server` and start it, returning the running handle.

    Accepts the same keyword arguments as :class:`Server`: ``config_path``,
    ``data_dir``, ``sql_dir``, ``listen_addr``, ``openai_api_key``,
    ``gemini_api_key``, ``listen_port``.
    """
    server = Server(**kwargs)
    server.start()
    return server
