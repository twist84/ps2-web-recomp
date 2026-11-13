#!/usr/bin/env python3
"""Filesystem resource pack resolution and safety regressions."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest
import zipfile

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
    if (argc != 3 && argc != 4) return 1;
    mount(argv[1], argv[2] == std::string("exe")
        ? std::filesystem::path(argv[3]) : std::filesystem::path{});
    const auto &found = packs();
    if (argv[2] == std::string("exe"))
    {
        if (found.size() != 1 || found[0].m_id != "Executable Pack") return 11;
        std::vector<std::uint8_t> bytes;
        if (!read_resource("textures/dis/main_menu.pix.001.png", bytes)
            || std::string(bytes.begin(), bytes.end()) != "EXEC") return 12;
        return 0;
    }
    if (argv[2] == std::string("zip")
        || argv[2] == std::string("zipdisabled")
        || argv[2] == std::string("zipordered"))
    {
        const auto &all = packs();
        const bool off = argv[2] == std::string("zipdisabled");
        if (all.size() != (off ? 1u : 2u)) return 30;
        std::vector<std::uint8_t> bytes;
        if (!read_resource("textures/dis/main_menu.pix.001.png", bytes))
            return 31;
        const std::string wanted = off || argv[2] == std::string("zipordered")
            ? "FOLDER" : "ZIP-RED";
        if (std::string(bytes.begin(), bytes.end()) != wanted)
            return 32;
        if (!off)
        {
            const auto name = "textures/dis/main_menu.pix.002.png";
            if (!read_resource(name, bytes)
                || std::string(bytes.begin(), bytes.end()) != "ZIP-SECOND")
                return 33;
        }
        return 0;
    }
    if (argv[2] == std::string("disabled"))
    {
        if (found.size() != 1 || found[0].m_id != "lower") return 20;
        std::vector<std::uint8_t> bytes;
        if (!read_resource("textures/dis/main_menu.pix.001.png", bytes)
            || std::string(bytes.begin(), bytes.end()) != "LOW-OLD") return 21;
        return 0;
    }
    if (argv[2] == std::string("ordered"))
    {
        if (found.size() != 2 || found[0].m_id != "lower"
            || found[1].m_id != "Higher Pack") return 22;
        std::vector<std::uint8_t> bytes;
        if (!read_resource("textures/dis/main_menu.pix.001.png", bytes)
            || std::string(bytes.begin(), bytes.end()) != "LOW-OLD") return 23;
        return 0;
    }
    if (found.size() != 2 || found[0].m_id != "Higher Pack"
        || found[1].m_id != "lower"
        || found[0].m_priority != 50
        || found[1].m_priority != -5) return 2;

    const std::string key = "textures/dis/main_menu.pix.001.png";
    s_resource resource;
    if (!find_resource(key, resource) || resource.m_pack_id != "Higher Pack")
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
            str(ROOT / "runtime/src/resource_pack_zip.cpp"),
            "-lz", "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "work"):
            cls.work.cleanup()

    def test_zip_packs_priority_disable_and_order(self):
        root = self.base / "zip_case" / "disc"
        root.mkdir(parents=True)
        folder = root / "resourcepacks"
        folder.mkdir()
        installed = folder / "Fallback Folder"
        (installed / "assets/sdbz/textures/dis").mkdir(parents=True)
        (installed / "pack.json").write_text(
            '{"pack":{"format":1,"game":"sdbz","priority":2}}',
            encoding="utf-8")
        (installed / "assets/sdbz/textures/dis/main_menu.pix.001.png").write_bytes(b"FOLDER")

        archive = folder / "Red Menu.zip"
        with zipfile.ZipFile(archive, "w") as out:
            out.writestr("pack.json",
                '{"pack":{"format":1,"game":"sdbz","region":"eu-beta","priority":100}}',
                compress_type=zipfile.ZIP_STORED)
            out.writestr("assets/sdbz/textures/dis/main_menu.pix.001.png",
                b"ZIP-RED", compress_type=zipfile.ZIP_DEFLATED)
            out.writestr("assets/sdbz/textures/dis/main_menu.pix.002.png",
                b"ZIP-SECOND", compress_type=zipfile.ZIP_STORED)
        subprocess.run([str(self.binary), str(root), "zip"], check=True)

        order = folder / "load_order.txt"
        order.write_text("-Red Menu\\n+Fallback Folder\\n", encoding="utf-8")
        subprocess.run([str(self.binary), str(root), "zipdisabled"], check=True)
        order.write_text("+Fallback Folder\\n+Red Menu\\n", encoding="utf-8")
        subprocess.run([str(self.binary), str(root), "zipordered"], check=True)

        # Fail closed for malformed archives and path traversal. They should
        # not displace the known-good pack.
        order.unlink()
        with zipfile.ZipFile(folder / "Untrusted.zip", "w") as out:
            out.writestr("pack.json",
                '{"pack":{"format":1,"game":"sdbz","priority":200}}')
            out.writestr("../assets/sdbz/textures/dis/main_menu.pix.001.png",
                b"BAD")
        subprocess.run([str(self.binary), str(root), "zip"], check=True)

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
        high = add_pack("Higher Pack", 50)
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

        # Disable a higher-priority pack without touching its manifest.
        order_file = packs / "load_order.txt"
        order_file.write_text("-Higher Pack\n+lower\n", encoding="utf-8")
        subprocess.run([str(self.binary), str(root), "disabled"], check=True)

        # Explicit order overrides numeric manifest priorities.
        order_file.write_text("# first wins\n+lower\n+Higher Pack\n",
                              encoding="utf-8")
        subprocess.run([str(self.binary), str(root), "ordered"], check=True)
        order_file.unlink()

        # Pack discovery is relative to SDBZ.exe, not the process CWD.
        executable_directory = self.base / "standalone"
        executable_pack = executable_directory / "resourcepacks" / "Executable Pack"
        executable_pack.mkdir(parents=True)
        (executable_pack / "pack.json").write_text(
            '{"pack":{"format":1,"game":"sdbz","region":"eu-beta","priority":5}}',
            encoding="utf-8")
        target = executable_pack / sub / "main_menu.pix.001.png"
        target.parent.mkdir(parents=True)
        target.write_bytes(b"EXEC")
        subprocess.run([str(self.binary), str(root), "exe",
                        str(executable_directory)], check=True)



if __name__ == "__main__":
    unittest.main()
