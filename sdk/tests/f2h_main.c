/*
 * f2h_main.c -- test harness for the library's f32->f16 converter (RNE,
 * numpy-astype(float16) semantics). Emits (f32_bits, f16_bits) pairs for a
 * deterministic corpus; test_f2h.py compares against numpy.
 * Build: gcc -O2 -I src -I include -I ../hw/femu/cylon
 */
#include "../src/cylon_api.c"

static uint32_t xs = 0x12345678u;
static uint32_t xorshift(void)
{
    xs ^= xs << 13;
    xs ^= xs >> 17;
    xs ^= xs << 5;
    return xs;
}
int main(void)
{
    /* corpus: exponent/mantissa edge classes + RNE tie cases + random */
    static const uint32_t edges[] = {
        0x00000000u, 0x00000001u, 0x007fffffu, 0x00800000u, 0x0afffffeu,
        0x33000000u, 0x32ffffffu, 0x33800000u, 0x33ffffffu, 0x387fffffu,
        0x387ffffeu, 0x38800000u, 0x3fffffffu, 0x3f000000u, 0x477fefffu,
        0x477ff000u, 0x477ff001u, 0x477fffffu, 0x477ffffeu, 0x7f7fffffu,
        0x7f800000u, 0x7f800001u, 0x7fffffffu, 0xff800000u, 0xc048f5c3u,
        0xbf800000u, 0x3f800000u, 0x000003ffu, 0x00000400u, 0x00000401u,
    };
    for (unsigned i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        union { uint32_t u; float f; } v;
        v.u = edges[i];
        uint16_t h = f32_to_f16(v.f);
        fwrite(&v.u, 4, 1, stdout);
        fwrite(&h, 2, 1, stdout);
    }
    /* 1M xorshift random bit patterns (valid floats incl. subnormals) */
    for (int r = 0; r < 1000000; r++) {
        union { uint32_t u; float f; } v;
        v.u = xorshift();
        if (isnan(v.f)) {
            v.u = 0;
        }
        uint16_t h = f32_to_f16(v.f);
        fwrite(&v.u, 4, 1, stdout);
        fwrite(&h, 2, 1, stdout);
    }
    return 0;
}
