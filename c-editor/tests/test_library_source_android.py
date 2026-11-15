"""Compile the exact Android transport and run local fixtures in a temporary JVM."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parent.parent
javac = shutil.which('javac')
java = shutil.which('java')
if not javac or not java:
    raise SystemExit('Java 17 javac/java are required for the Android transport fixture')
with tempfile.TemporaryDirectory(prefix='notelab-library-java-') as classes:
    subprocess.run([javac, '-Xlint:all', '-Werror', '-d', classes,
                    str(root / 'platform/android/app/src/main/java/org/notelab/editor/LibraryHttpSource.java'),
                    str(root / 'tests/TestLibraryHttpSource.java')], check=True, timeout=30)
    subprocess.run([sys.executable, str(root / 'tests/test_library_source_native_http.py'),
                    java, '-cp', classes, 'org.notelab.editor.TestLibraryHttpSource'],
                   check=True, timeout=60)
