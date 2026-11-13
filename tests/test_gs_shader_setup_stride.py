#!/usr/bin/env python3
"""Keep the GPU host setup record stride aligned with WGSL.

A mismatched stride corrupts all setup records after the first and can make
textures, UVs, alpha, and framebuffer state appear unrelated to the draw.
"""
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class GsShaderSetupTests(unittest.TestCase):
    def test_cpp_wgsl_record_stride_and_native_slot(self):
        cpp = (ROOT / "runtime/src/gs/gpu/gpu.cpp").read_text(encoding="utf-8")
        wgsl = (ROOT / "runtime/src/gs/gpu/gs.wgsl").read_text(encoding="utf-8")
        cpp_match = re.search(r"\bkSetupWords\s*=\s*(\d+)\s*;", cpp)
        wgsl_match = re.search(r"\bSETUP_WORDS\s*:\s*u32\s*=\s*(\d+)u\s*;", wgsl)
        self.assertIsNotNone(cpp_match)
        self.assertIsNotNone(wgsl_match)
        self.assertEqual(int(cpp_match[1]), int(wgsl_match[1]))
        self.assertEqual(int(cpp_match[1]), 26)
        self.assertIn("w[25] = 0xFFFFFFFFu;", cpp)
        self.assertIn("s.native = data[at + 25u];", wgsl)
        self.assertIn("setups + si * SETUP_WORDS", wgsl)

    def test_native_gpu_atlas_scaling_is_synchronized(self):
        cpp = (ROOT / "runtime/src/gs/gpu/gpu.cpp").read_text(encoding="utf-8")
        wgsl = (ROOT / "runtime/src/gs/gpu/gs.wgsl").read_text(encoding="utf-8")
        raster = (ROOT / "runtime/src/gs/raster.cpp").read_text(encoding="utf-8")
        self.assertIn("kNativeAtlasSide = 1024u", cpp)
        self.assertIn("kNativeAtlasPixels = kNativeAtlasSide * kNativeAtlasSide", cpp)
        self.assertIn("atlas * 1048576u + y * 1024u + x", wgsl)
        self.assertIn("w[25] = (scale << 16)", cpp)
        self.assertIn("let scale = f32(s.native >> 16u)", wgsl)
        self.assertIn("u = su - 0.5f", raster)
        self.assertIn("let u = su - 0.5", wgsl)

    def test_gpu_binding_layout_includes_native_atlases(self):
        cpp = (ROOT / "runtime/src/gs/gpu/gpu.cpp").read_text(encoding="utf-8")
        wgsl = (ROOT / "runtime/src/gs/gpu/gs.wgsl").read_text(encoding="utf-8")
        self.assertIn("WGPUBuffer buffers[9]", cpp)
        self.assertIn("WGPUBindGroupLayoutEntry le[9]", cpp)
        self.assertIn("@binding(8) var<storage, read> native_atlases", wgsl)


if __name__ == "__main__":
    unittest.main()
