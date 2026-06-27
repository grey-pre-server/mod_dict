"""
Копирует собранный mod_dict.pyd в папку пакета для локальной разработки.
Запускать после сборки в CLion:  python install_dev.py
"""
import shutil
import sys
from pathlib import Path

root = Path(__file__).parent
candidates = [
    root / "cmake-build-release" / "mod_dict.pyd",
    root / "cmake-build-debug"   / "mod_dict.pyd",
    root / "cmake-build-release" / "mod_dict.so",
    root / "cmake-build-debug"   / "mod_dict.so",
]

src = next((p for p in candidates if p.exists()), None)
if not src:
    print("mod_dict.pyd not found. Build the project first.", file=sys.stderr)
    sys.exit(1)

dst = root / "mod_dict" / src.name
shutil.copy2(src, dst)
print(f"Copied {src.name}  →  {dst}")
