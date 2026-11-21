"""nix/deps.nix pins every FetchContent source the CMake files declare, at the same version.

nix/deps.nix's trees win over the CMake files' GIT_TAG / URL in every Nix build and in `nix develop`
(FETCHCONTENT_SOURCE_DIR_<NAME>, KAMITE_CMAKE_INIT): a tag bumped in CMake but not there would build the old tree
silently. Per declaration: a URL must be the same URL; a commit GIT_TAG the same rev; a named tag or branch must be the
name in the comment above the deps.nix entry ("# tag 5.5", "# branch Raylib_5_5", "# v1.1.1-SDL").

    python -I -m unittest discover -s tools/linux -p "test_*.py"
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CMAKE_FILES = ["runtime/ps2xRuntime/CMakeLists.txt", "runtime/ps2xRecomp/CMakeLists.txt", "src/CMakeLists.txt",
               "thirdparty/pcsx2-gs/shaderc.cmake"]
# Declared only when tests/unit is configured on its own (the full build reuses raylib's tree): not a pin.
NOT_PINNED = {"RAYLIB_UNIT_TESTS"}


def declarations(text):
    """name (upper case) -> {"tag": ..., "url": ...} for every FetchContent_Declare in a CMake file."""
    out = {}
    for m in re.finditer(r"FetchContent_Declare\(\s*([A-Za-z0-9_]+)(.*?)\)", text, re.S):
        body = re.sub(r"#[^\n]*", "", m.group(2))
        tag = re.search(r'GIT_TAG\s+"?([^\s")]+)"?', body)
        url = re.search(r'\bURL\s+"?([^\s")]+)"?', body)
        out[m.group(1).upper()] = {"tag": tag and tag.group(1), "url": url and url.group(1)}
    return out


def pins(text):
    """NAME -> {"comment", "rev", "url"} for each entry of nix/deps.nix's fetchContent / recompFetchContent."""
    out = {}
    comment = ""
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("#"):
            comment = s.lstrip("# ").strip()
            continue
        m = re.match(r"([A-Z0-9_]+)\s*=\s*(gh|zip|pkgs\.fetchzip)\b(.*)", s)
        if not m:
            if s and not s.startswith('"') and not s.startswith("url") and not s.startswith("sha256"):
                comment = ""
            continue
        name, kind, rest = m.groups()
        entry = {"comment": comment, "rev": None, "url": None}
        if kind == "gh":
            entry["rev"] = re.findall(r'"([^"]*)"', rest)[2]
        elif kind == "zip":
            entry["url"] = re.findall(r'"([^"]*)"', rest)[0]
        else:  # pkgs.fetchzip { url = "..."; ... } over the next lines
            entry["url"] = "pending"
        out[name] = entry
        comment = ""
    # pkgs.fetchzip's url is on a following line.
    for name, url in re.findall(r"([A-Z0-9_]+)\s*=\s*pkgs\.fetchzip\s*\{\s*url\s*=\s*\"([^\"]+)\"", text):
        out[name]["url"] = url
    return out


def mismatches(decls, pinned):
    problems = []
    for name, d in sorted(decls.items()):
        if name in NOT_PINNED:
            continue
        p = pinned.get(name)
        if p is None:
            problems.append(f"{name}: declared in CMake, not pinned in nix/deps.nix")
        elif d["url"]:
            if p["url"] != d["url"]:
                problems.append(f"{name}: CMake URL {d['url']}, nix/deps.nix {p['url']}")
        elif d["tag"] and re.fullmatch(r"[0-9a-f]{40}", d["tag"]):
            if p["rev"] != d["tag"]:
                problems.append(f"{name}: CMake commit {d['tag']}, nix/deps.nix {p['rev']}")
        elif d["tag"]:
            words = re.split(r"[\s(),;]+", p["comment"])
            if d["tag"] not in words:
                problems.append(f"{name}: CMake tag {d['tag']}, nix/deps.nix's comment says '{p['comment']}'")
    for name in sorted(set(pinned) - set(decls)):
        problems.append(f"{name}: pinned in nix/deps.nix, declared nowhere")
    return problems


class DepsPins(unittest.TestCase):
    def test_every_declaration_pinned_at_its_version(self):
        decls = {}
        for f in CMAKE_FILES:
            decls.update(declarations((ROOT / f).read_text(encoding="utf-8")))
        pinned = pins((ROOT / "nix" / "deps.nix").read_text(encoding="utf-8"))
        self.assertGreaterEqual(len(decls), 16, "the CMake files' declarations were not found")
        self.assertEqual(mismatches(decls, pinned), [])

    def test_a_bumped_tag_is_caught(self):
        decls = {"FMT": {"tag": "12.2.0", "url": None}}
        pinned = {"FMT": {"comment": "tag 12.1.0", "rev": "407c905e45ad75fc29bf0f9bb7c5c2fd3475976f", "url": None}}
        self.assertEqual(len(mismatches(decls, pinned)), 1)

    def test_a_changed_commit_or_url_is_caught(self):
        sha_a, sha_b = "a" * 40, "b" * 40
        self.assertEqual(len(mismatches({"X": {"tag": sha_a, "url": None}},
                                        {"X": {"comment": "", "rev": sha_b, "url": None}})), 1)
        self.assertEqual(len(mismatches({"Y": {"tag": None, "url": "https://e/1.zip"}},
                                        {"Y": {"comment": "", "rev": None, "url": "https://e/2.zip"}})), 1)

    def test_an_unpinned_declaration_is_caught(self):
        self.assertEqual(mismatches({"NEW": {"tag": "v1", "url": None}}, {}),
                         ["NEW: declared in CMake, not pinned in nix/deps.nix"])

    def test_a_tag_is_matched_as_a_word(self):
        # "5.5" must not pass against "tag 5.50" (a prefix) or "Raylib_5_5".
        self.assertEqual(len(mismatches({"R": {"tag": "5.5", "url": None}},
                                        {"R": {"comment": "tag 5.50", "rev": "c" * 40, "url": None}})), 1)


if __name__ == "__main__":
    unittest.main()
