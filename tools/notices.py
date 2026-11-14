"""THIRD-PARTY-NOTICES.txt for dist/: every component, model and font a release .bin carries, with its license.

The components come from managed_components/, which a build downloads and the repo does not keep, so their
licenses would not travel with a published .bin without this file. Run by build.sh after each build (all trees
present are read); by hand: python tools/notices.py dist/
"""
import hashlib
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
TREES = ("s3sat", "cyd", "tdongle", "cardputer")
NAME = re.compile(r"^(licen[cs]e|copying|copyrights?|notice)", re.I)   # LICENSE, license.txt, LVGL's LICENCE.txt, LICENSE-OpenMV, NOTICE

HEAD = """Third-party notices for the Sapphire satellite firmware images (dist/*.bin)

The Sapphire firmware itself is under the GNU AGPL v3 (LICENSE in the source repository), with the
additional permission in its README for Espressif's binary-only components. Each image also contains:

- ESP-IDF, Espressif's framework, Apache License 2.0 (https://github.com/espressif/esp-idf), including
  its binary-only Wi-Fi, PHY and coexistence libraries.
- The components and models listed below, each with its license text. A license text that several
  components share is printed once, where it first appears, and named by the others.
"""


def sources():
    for tree in TREES:
        root = HERE / tree
        for base in (root / "managed_components", root / "components", root / "models"):
            if not base.is_dir():
                continue
            dirs = [base] if base.name == "models" else sorted(p for p in base.iterdir() if p.is_dir())
            for d in dirs:
                files = sorted(f for f in d.iterdir() if f.is_file() and NAME.match(f.name))
                yml = d / "idf_component.yml"
                said = re.search(r"^license:\s*(\S+)", yml.read_text(), re.M) if yml.is_file() else None
                if files or said:
                    yield tree, d.relative_to(root), files, said and said.group(1)


def main(out_dir):
    seen, parts = {}, [HEAD]
    for tree, rel, files, said in sources():
        key = f"{tree}/{rel}"
        texts = [] if files else [f"[no license file in the component; its idf_component.yml declares {said}]"]
        for f in files:
            body = f.read_text(errors="replace").strip()
            h = hashlib.sha256(body.encode()).hexdigest()
            if h in seen:
                texts.append(f"[{f.name}: same text as {seen[h]}]")
            else:
                seen[h] = f"{key}/{f.name}"
                texts.append(f"--- {f.name} ---\n{body}")
        parts.append("=" * 78 + f"\n{key}\n" + "=" * 78 + "\n" + "\n\n".join(texts))
    out = Path(out_dir) / "THIRD-PARTY-NOTICES.txt"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n\n".join(parts) + "\n")
    print(f"notices: {out} ({len(parts) - 1} components)")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else HERE / "dist")
