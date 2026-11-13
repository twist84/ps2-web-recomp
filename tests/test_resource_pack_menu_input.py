#!/usr/bin/env python3
"""Guard the resource pack overlay's input behavior against regressions."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class ResourcePackMenuInputTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.host = (ROOT / "host/window.cpp").read_text(encoding="utf-8")
        cls.gpu = (ROOT / "runtime/src/gs/gpu/gpu.cpp").read_text(encoding="utf-8")

    def test_toggle_actions_are_not_save_actions(self):
        self.assertIn("key == SDL_SCANCODE_SPACE", self.host)
        self.assertIn("1 - g_pack_column", self.host)
        self.assertIn("case SDL_CONTROLLER_BUTTON_A:\n"
                      "                handle_pack_key(SDL_SCANCODE_SPACE);",
                      self.host)
        self.assertIn("case SDL_CONTROLLER_BUTTON_START:\n"
                      "                handle_pack_key(SDL_SCANCODE_RETURN);",
                      self.host)

    def test_mouse_click_toggles_only_a_valid_pack_row(self):
        self.assertIn("const int enabled_start = 6;", self.host)
        self.assertIn("const int available_start = 8", self.host)
        self.assertIn("if (clicked_pack", self.host)
        self.assertIn("handle_pack_key(SDL_SCANCODE_SPACE);", self.host)
        self.assertIn("if (e.button.clicks > 1) continue;", self.host)

    def test_modal_hides_stats(self):
        self.assertIn("if (overlay_lines.empty())", self.gpu)
        self.assertIn("g_stats_text, 10.f, 10.f", self.gpu)


if __name__ == "__main__":
    unittest.main()
