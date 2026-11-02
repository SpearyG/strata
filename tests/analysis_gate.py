"""Verify that a real analyzer finding makes the shared analysis runner fail."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--analyzer', default='clang++')
settings = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='strata-analysis-gate-') as temporary:
    root = Path(temporary)
    (root / 'tools').mkdir()
    (root / 'src').mkdir()
    shutil.copyfile(Path(__file__).resolve().parents[1] / 'tools/analyze.sh', root / 'tools/analyze.sh')
    (root / 'src/deliberate_finding.cpp').write_text('int example() { int* p = nullptr; return *p; }\n')
    environment = dict(os.environ, STRATA_ANALYZER=settings.analyzer)
    result = subprocess.run(['sh', str(root / 'tools/analyze.sh')], env=environment,
                            text=True, capture_output=True, timeout=20)
    if result.returncode == 0 or 'Dereference of null pointer' not in result.stderr:
        raise RuntimeError(f'analyzer gate did not reject the sentinel: {result.returncode}\n{result.stderr}')
print('Analyzer sentinel rejected as required')
