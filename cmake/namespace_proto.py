"""Avoid platform name collisions in generated C++, preserving wire names."""
import pathlib
import re
import sys

SERVICE_MACRO_GUARD = """// The Windows SDK aliases StartService to StartServiceA/W. Clear it after
// platform includes, in both declarations and the generated implementation.
#ifdef StartService
#undef StartService
#endif

"""

for path in pathlib.Path(sys.argv[1]).rglob("*.pb.*"):
    source = path.read_text()
    source = re.sub(r"\bnamespace daemon\b", "namespace rostra_rpc", source)
    source = re.sub(r"\bdaemon::", "rostra_rpc::", source)
    if path.name in ("desktop_service.grpc.pb.h", "desktop_service.grpc.pb.cc"):
        namespace = "\nnamespace desktop {"
        if namespace not in source:
            raise ValueError(f"Cannot locate desktop RPC namespace in {path}")
        if SERVICE_MACRO_GUARD not in source:
            source = source.replace(namespace, "\n" + SERVICE_MACRO_GUARD + namespace, 1)
    path.write_text(source)
