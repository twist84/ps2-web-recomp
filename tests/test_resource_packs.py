#!/usr/bin/env python3
"""Filesystem resource pack resolution and safety regressions."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "ps2/resource_pack.hpp"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
    using namespace ps2::resource_pack;
    if (argc != 3) return 1;
    mount(argv[1]);
    const auto &found = packs();
    if (found.size() != 2 || found[0].m_id != "higher"
        || found[1].m_id != "lower"
        || found[0].m_priority != 50
        || found[1].m_priority != -5) return 2;

    const std::string key = "textures/dis/main_menu.pix.001.png";
    s_resource resource;
    if (!find_resource(key, resource) || resource.m_pack_id != "higher")
        return 3;
    std::vector<std::uint8_t> data;
    if (!read_resource(key, data)
        || std::string(data.begin(), data.end()) != "HIGH") return 4;

    const std::string second = "textures/dis/main_menu.pix.002.png";
    if (!find_resource(second, resource) || resource.m_pack_id != "lower")
        return 5;
    if (!read_resource(second, data)
        || std::string(data.begin(), data.end()) != "LOW") return 6;

    for (const auto &bad : {"../other", "/etc/passwd", "C:/windows",
         "textures\\bad", "textures//oops", "textures/./oops",
         "textures/../oops", "textures/"})
    {
        if (find_resource(bad, resource) || read_resource(bad, data))
            return 7;
    }
    if (find_resource("textures/missing.png", resource)) return 8;
    if (resource.m_size != 0 || !resource.m_pack_id.empty()) return 9;
    clear();
    if (!packs().empty() || find_resource(key, resource)) return 10;
    return 0;
}
'''


class ResourcePackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix="sdbz-pack-")
        cls.base = Path(cls.work.name)
        cls.source = cls.base / "pack_harness.cpp"
        cls.binary = cls.base / ("pack_harness.exe" if os.name == "nt" else "pack_harness")
        cls.source.write_text(HARNESS, encoding="utf-8")
        setting = os.environ.get("CXX")
        compiler = shlex.split(setting, posix=(os.name != "nt")) if setting else [
            shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")]
        if not compiler or not compiler[0]:
            raise unittest.SkipTest("C++ compiler unavailable")
        subprocess.run(compiler + ["-std=c++20", "-Wall", "-Wextra",
            "-I", str(ROOT / "runtime/include"),
            str(cls.source), str(ROOT / "runtime/src/resource_pack.cpp"),
            "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "work"):
            cls.work.cleanup()

    def test_priority_sparse_override_and_manifest_validation(self):
        root = self.base / "test" / "disc"
        root.mkdir(parents=True)
        packs = root.parent / "resourcepacks"

        def add_pack(name, priority, extra=""):
            folder = packs / name
            folder.mkdir(parents=True)
            manifest = ('{"pack":{"format":1,"name":"' + name
                + '","description":"Test pack","game":"sdbz","region":"eu-beta",'
                + '"priority":' + str(priority) + extra + '}}')
            (folder / "pack.json").write_text(manifest, encoding="utf-8")
            return folder

        low = add_pack("lower", -5)
        high = add_pack("higher", 50)
        add_pack("us", 100, ',"disabled":0')  # invalidated below by region
        us = packs / "us" / "pack.json"
        us.write_text('{"pack":{"format":1,"game":"sdbz","region":"us","priority":100}}',
                      encoding="utf-8")
        escaped = packs / "escaped"
        escaped.mkdir()
        (escaped / "pack.json").write_text(
            '{"pack":{"format":1,"game":"sdbz","name":"hello\\\\world"}}',
            encoding="utf-8")
        bad = packs / "broken"
        bad.mkdir()
        (bad / "pack.json").write_text('{"pack":{garbled}}', encoding="utf-8")

        sub = Path("assets/sdbz/textures/dis")
        for folder, name, payload in [
            (low, "main_menu.pix.001.png", b"LOW-OLD"),
            (low, "main_menu.pix.002.png", b"LOW"),
            (high, "main_menu.pix.001.png", b"HIGH"),
        ]:
            path = folder / sub / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(payload)

        subprocess.run([str(self.binary), str(root), "test"], check=True)



if __name__ == "__main__":
    unittest.main()
