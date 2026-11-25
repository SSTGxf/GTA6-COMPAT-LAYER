// ScarlettCompat shader pipeline - link-contract smoke main.
// MIT. Built by tests\shader\build_link_smoke.bat, never shipped.
//
// Purpose: force the decode path into a link so that the one cross-object
// dependency the layer has to satisfy shows up. sc_dxil_backend.cpp calls
// sc_make_dxil_emitter, which is defined in sc_dxil_emit.cpp; this program
// itself never touches the backend, but every no-DXC object is passed to the
// linker, so the missing definition is reported as LNK2019 either way.
//
// It also does real work: decode a FLAT row from the fixture set
// (gfx10_asm_flat.s:200, flat_load_dword v5, v[1:2] = 0xDC300000 0x057D0001)
// and return 0 only if the measured fields come back right.
#include "shader/sc_isa.h"
#include "shader/sc_wave.h"

int main() {
    const unsigned int words[2] = {0xDC300000u, 0x057D0001u};
    const sc::sc_instruction_operands io = sc::sc_decode_instruction(words, 2u);
    if (io.format != sc::sc_isa_format::flat || !io.has_mem_flat) {
        return 1;
    }
    if (io.mem_flat_vdst != 5u || io.mem_flat_vaddr != 1u || !io.mem_flat_saddr_off) {
        return 2;
    }
    return 0;
}
