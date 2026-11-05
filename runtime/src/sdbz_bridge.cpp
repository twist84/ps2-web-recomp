#include "ps2/generated.hpp"
#include "ps2/system.hpp"
#include "ps2/memory.hpp"

#include "sdbz/game.h"

#include <cstdint>
#include <cstring>

namespace ps2 {

static sdbz::c_memory s_sdbz_memory(g_ram, kRamSize);

static void bridge_start_face_program(Cpu &c) {
    sdbz::start_face_program(s_sdbz_memory, c.r[4].w[0], c.r[5].w[0], (std::int32_t)c.r[6].w[0]);
    c.pc = c.r[31].w[0];
}

static void bridge_set_face_form(Cpu &c) {
    sdbz::set_face_form(s_sdbz_memory, c.r[4].w[0], (std::int32_t)c.r[5].w[0]);
    c.pc = c.r[31].w[0];
}

static void bridge_apply_model_face(Cpu &c) {
    float blend;
    std::memcpy(&blend, &c.f[12], 4);
    sdbz::apply_model_face(s_sdbz_memory, c.r[4].w[0], (std::int32_t)c.r[5].w[0], c.r[6].w[0], blend);
    c.pc = c.r[31].w[0];
}

static void bridge_select_hands(Cpu &c) {
    sdbz::select_hands(s_sdbz_memory, c.r[4].w[0], (std::int32_t)c.r[5].w[0], (std::int32_t)c.r[6].w[0]);
    c.pc = c.r[31].w[0];
}

static void bridge_bind_player_assets(Cpu &c) {
    sdbz::bind_player_assets(s_sdbz_memory, c.r[4].w[0], c.r[5].w[0], c.r[6].w[0]);
    c.pc = c.r[31].w[0];
}

static void bridge_tick_voice_slot_0(Cpu &c) {
    sdbz::tick_voice_slot_0(s_sdbz_memory, c.r[4].w[0]);
    c.pc = c.r[31].w[0];
}

static void bridge_tick_voice_slot_1(Cpu &c) {
    sdbz::tick_voice_slot_1(s_sdbz_memory, c.r[4].w[0]);
    c.pc = c.r[31].w[0];
}

static void bridge_tick_voice_slot_2(Cpu &c) {
    sdbz::tick_voice_slot_2(s_sdbz_memory, c.r[4].w[0]);
    c.pc = c.r[31].w[0];
}

static void bridge_script_set_form(Cpu &c) {
    sdbz::script_set_form(s_sdbz_memory, c.r[4].w[0], c.r[5].w[0], (std::int32_t)c.r[6].w[0]);
    c.pc = c.r[31].w[0];
}

static void bridge_script_start_face(Cpu &c) {
    sdbz::script_start_face(s_sdbz_memory, c.r[4].w[0], c.r[5].w[0], c.r[6].w[0]);
    c.pc = c.r[31].w[0];
}

static void bridge_select_voice_slot_1_motion(Cpu &c) {
    std::uint16_t r = sdbz::select_voice_slot_1_motion(s_sdbz_memory, c.r[4].w[0]);
    c.r[2].w[0] = r;
    c.pc = c.r[31].w[0];
}

void register_sdbz_reconstructed() {
    register_reconstructed(0x001E60A0u, bridge_start_face_program);
    register_reconstructed(0x001E62B0u, bridge_set_face_form);
    register_reconstructed(0x001D5A50u, bridge_apply_model_face);
    register_reconstructed(0x001D5BB0u, bridge_select_hands);
    register_reconstructed(0x001EB150u, bridge_bind_player_assets);
    register_reconstructed(0x001ECAD0u, bridge_tick_voice_slot_0);
    register_reconstructed(0x001EC0E0u, bridge_tick_voice_slot_1);
    register_reconstructed(0x001EC580u, bridge_tick_voice_slot_2);
    register_reconstructed(0x002E19F0u, bridge_script_set_form);
    register_reconstructed(0x002E1BF0u, bridge_script_start_face);
}

} // namespace ps2
