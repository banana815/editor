#!/usr/bin/env python3
"""Build-tool regression: byte identity, deterministic output and fail-closed pins."""
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "embed_citation_styles.py"
RESOURCES = ROOT / "csl" / "resources"


def run(resources, output, succeeds):
    process = subprocess.run([sys.executable, str(TOOL), "--resources", str(resources),
                              "--output", str(output)], capture_output=True, text=True)
    if (process.returncode == 0) != succeeds:
        raise AssertionError(process.stdout + process.stderr)
    if not succeeds:
        assert "CSL resource generation failed:" in process.stderr


def main():
    with tempfile.TemporaryDirectory(prefix="notelab-csl-embedding-") as temporary:
        directory = Path(temporary)
        resources, output = directory / "resources", directory / "data.inc"
        shutil.copytree(RESOURCES, resources)
        run(resources, output, True)
        generated = output.read_bytes()
        names = (("apa", "apa.csl"), ("ieee", "ieee.csl"),
                 ("gbt", "china-national-standard-gb-t-7714-2015-numeric.csl"),
                 ("en_us", "locales-en-US.xml"), ("zh_cn", "locales-zh-CN.xml"))
        for symbol, filename in names:
            match = re.search(rb"citation_data_" + symbol.encode() + rb"\[\] = \{(.*?)\};",
                              generated, flags=re.DOTALL)
            assert match
            data = bytes(int(value, 16) for value in re.findall(rb"0x([0-9a-f]{2})", match[1]))
            assert data == (resources / filename).read_bytes() + b"\0"
        modified = output.stat().st_mtime_ns
        run(resources, output, True)
        assert output.read_bytes() == generated and output.stat().st_mtime_ns == modified
        for filename in ("apa.csl", "locales-zh-CN.xml", "LICENSE.txt"):
            path = resources / filename
            original = path.read_bytes()
            path.write_bytes(bytes([original[0] ^ 1]) + original[1:])
            run(resources, output, False)
            assert output.read_bytes() == generated
            path.write_bytes(original)
        path = resources / "SOURCES.json"
        original = path.read_bytes()
        manifest = json.loads(original)
        manifest["resources"].append(manifest["resources"][0])
        path.write_text(json.dumps(manifest), encoding="utf-8")
        run(resources, output, False)
        assert output.read_bytes() == generated
        path.write_bytes(original.replace(b'"format": 1', b'"format": 1, "format": 1', 1))
        run(resources, output, False)
        assert output.read_bytes() == generated
        # XML bytes and hashes remain unchanged: descriptive metadata must not
        # silently drift from the resource it claims to describe.
        for index, key, value in ((0, "id", "https://example.invalid/changed"),
                                  (0, "title", "Different edition"),
                                  (0, "csl_version", "9.9"),
                                  (0, "updated", "1900-01-01T00:00:00+00:00"),
                                  (3, "csl_version", "9.9"),
                                  (3, "updated", "1900-01-01T00:00:00+00:00"),
                                  (3, "language", "fr-FR")):
            manifest = json.loads(original)
            manifest["resources"][index][key] = value
            path.write_text(json.dumps(manifest), encoding="utf-8")
            run(resources, output, False)
            assert output.read_bytes() == generated
        manifest = json.loads(original)
        manifest["resources"][0] = None
        path.write_text(json.dumps(manifest), encoding="utf-8")
        run(resources, output, False)
        assert output.read_bytes() == generated
        path.write_bytes(original)
        run(resources, output, True)
        assert output.read_bytes() == generated
    print("CSL embedding verified: five byte-identical arrays, stable output, rejected resource/license/metadata corruption")


if __name__ == "__main__":
    main()
