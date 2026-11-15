#!/usr/bin/env python3
"""Regenerate the C stop-word list from the verified, vendored CSL schema data."""
from pathlib import Path
import hashlib
import json

root = Path(__file__).resolve().parents[1]
manifest = json.loads((root / 'data/csl-stop-words-source.json').read_text())
data = (root / 'data/csl-stop-words.json').read_bytes()
license_text = (root / 'licenses/CSL-Schema.txt').read_bytes()
for blob, key in ((data, 'sha256'), (license_text, 'license_sha256')):
    if hashlib.sha256(blob).hexdigest() != manifest[key]:
        raise SystemExit(f'CSL source verification failed: {key}')
words = json.loads(data)['stop-words']
if not all(isinstance(word, str) and word and '\0' not in word for word in words):
    raise SystemExit('Invalid stop-word table')
if len(set(words)) != len(words):
    raise SystemExit('Duplicate stop words')
output = f'/* CSL schema {manifest["commit"]}; MIT, see licenses/CSL-Schema.txt. */\n'
output += 'static const char *const stop_words[] = {\n'
output += ''.join('    ' + json.dumps(word, ensure_ascii=False) + ',\n' for word in words)
output += '};\n'
(root / 'data/csl-stop-words.inc').write_text(output, encoding='utf-8')
print(f'Generated {len(words)} verified CSL stop words')
