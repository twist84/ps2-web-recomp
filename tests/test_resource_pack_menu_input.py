#!/usr/bin/env python3
"""Regression checks for controller-only resource pack manager inputs."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class ResourcePackMenuInputTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.host = (ROOT / "host/window.cpp").read_text(encoding="utf-8")
        cls.gpu = (ROOT / "runtime/src/gs/gpu/gpu.cpp").read_text(encoding="utf-8")

    def test_controller_open_close_and_modal_navigation(self):
        self.assertIn("SDL_CONTROLLER_BUTTON_BACK", self.host)
        self.assertIn("SDL_CONTROLLER_BUTTON_START", self.host)
        self.assertIn("if (select_down && start_down)", self.host)
        self.assertIn("if (!g_pack_shortcut_down)", self.host)
        self.assertIn("if (g_pack_manager_open) close_pack_manager(false);", self.host)
        self.assertIn("else open_pack_manager();", self.host)
        self.assertIn("if (g_pack_manager_open || g_pack_shortcut_down)", self.host)

    def test_gamepad_a_toggles_start_saves_b_cancels(self):
        self.assertIn("case SDL_CONTROLLER_BUTTON_A:\n"
                      "                    handle_pack_action(_pack_action_toggle);",
                      self.host)
        self.assertIn("case SDL_CONTROLLER_BUTTON_START:\n"
                      "                    handle_pack_action(_pack_action_save);",
                      self.host)
        self.assertIn("case SDL_CONTROLLER_BUTTON_B:\n"
                      "                    handle_pack_action(_pack_action_cancel);",
                      self.host)
        self.assertIn("case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:", self.host)
        self.assertIn("case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:", self.host)

    def test_no_mouse_or_keyboard_in_pack_menu(self):
        self.assertNotIn("handle_pack_key", self.host)
        self.assertIn("e.key.keysym.scancode == SDL_SCANCODE_F6", self.host)
        self.assertNotIn("e.type == SDL_MOUSEBUTTONDOWN", self.host)
        self.assertNotIn("e.type == SDL_MOUSEWHEEL", self.host)
        self.assertNotIn("SDL_SCANCODE_SPACE", self.host)

    def test_modal_hides_stats(self):
        self.assertIn("if (overlay_lines.empty())", self.gpu)


if __name__ == "__main__":
    unittest.main()
