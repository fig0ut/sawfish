"""Rename generated C++ tokens to avoid POSIX daemon(), preserving wire names."""
import pathlib
import re
import sys

for path in pathlib.Path(sys.argv[1]).rglob("*.pb.*"):
    source = path.read_text()
    source = re.sub(r"\bnamespace daemon\b", "namespace rostra_rpc", source)
    source = re.sub(r"\bdaemon::", "rostra_rpc::", source)
    path.write_text(source)
