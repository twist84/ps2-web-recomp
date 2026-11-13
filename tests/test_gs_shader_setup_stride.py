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

    def test_gpu_binding_layout_includes_native_atlases(self):
        cpp = (ROOT / "runtime/src/gs/gpu/gpu.cpp").read_text(encoding="utf-8")
        wgsl = (ROOT / "runtime/src/gs/gpu/gs.wgsl").read_text(encoding="utf-8")
        self.assertIn("WGPUBuffer buffers[9]", cpp)
        self.assertIn("WGPUBindGroupLayoutEntry le[9]", cpp)
        self.assertIn("@binding(8) var<storage, read> native_atlases", wgsl)


if __name__ == "__main__":
    unittest.main()
