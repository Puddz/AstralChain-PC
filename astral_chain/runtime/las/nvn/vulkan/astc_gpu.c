// ASTC LDR decoding on the GPU (Astral Chain). Desktop NVIDIA and AMD GPUs cannot sample ASTC, so its textures were
// decoded on the CPU into RGBA8 (astc.c): up to ~180 ms in one frame for the textures an effect brings, and ~34 ms to
// copy one decoded 4096x4096 texture (RGBA8 is 16x the size of ASTC 8x8). Here the blocks themselves are uploaded and
// a compute shader decodes them into the RGBA8 image: one invocation per block, a line-by-line port of astc.c (bit
// for bit the same output; tools/astc_gpu_test.c checks it against astc.c). LARECOMP_GPU_ASTC=0: CPU decoding.
#ifndef ASTC_GPU_SOURCE_ONLY  // (astc_gpu_test.c: just the shader source and the tables)
#include "vk.h"
#endif
#include "../astc.h"

#include <stdlib.h>
#include <string.h>

// Buffer layout (32-bit words): [0, 270) the unquantization tables as bytes (weights 142, then colours 936), then the
// blocks of each level, 4 words per block, at the word offset in the push constants.
static const char *ASTC_GLSL =
"#version 460\n"
"layout(local_size_x = 64) in;\n"
"layout(std430, set = 0, binding = 0) readonly buffer Data { uint data[]; };\n"
"layout(set = 0, binding = 1, rgba8) uniform writeonly image2D dst;\n"
"layout(push_constant) uniform PC { uint first, bx, by, w, h, bw, bh, srgb; };\n"
"const uint q_bits[21]   = uint[](1u,0u,2u,0u,1u,3u,1u,2u,4u,2u,3u,5u,3u,4u,6u,4u,5u,7u,5u,6u,8u);\n"
"const uint q_trits[21]  = uint[](0u,1u,0u,0u,1u,0u,0u,1u,0u,0u,1u,0u,0u,1u,0u,0u,1u,0u,0u,1u,0u);\n"
"const uint q_quints[21] = uint[](0u,0u,0u,1u,0u,0u,1u,0u,0u,1u,0u,0u,1u,0u,0u,1u,0u,0u,1u,0u,0u);\n"
"const uint wunq_off[12] = uint[](0u,2u,5u,9u,14u,20u,28u,38u,50u,66u,86u,110u);\n"
"const uint cunq_off[16] = uint[](0u,6u,14u,24u,36u,52u,72u,96u,128u,168u,216u,280u,360u,456u,584u,744u);\n"
"uint tbyte(uint i) { return (data[i >> 2] >> ((i & 3u) * 8u)) & 0xFFu; }\n"
"uint wunq(uint i) { return tbyte(i); }\n"
"uint cunq(uint i) { return tbyte(142u + i); }\n"
// n (<= 32) bits at pos; bits past 127 read as zero
"uint rbits(uvec4 b, int pos, int n) {\n"
"  if (n <= 0 || pos >= 128) return 0u;\n"
"  int wi = pos >> 5, sh = pos & 31;\n"
"  uint v = b[wi] >> uint(sh);\n"
"  if (sh != 0 && wi < 3) v |= b[wi + 1] << uint(32 - sh);\n"
"  return n >= 32 ? v : v & ((1u << uint(n)) - 1u);\n"
"}\n"
"int ise_bitcount(int count, int q) {\n"
"  int n = count * int(q_bits[q]);\n"
"  if (q_trits[q] != 0u) n += (8 * count + 4) / 5;\n"
"  if (q_quints[q] != 0u) n += (7 * count + 2) / 3;\n"
"  return n;\n"
"}\n"
"uint ise_out[64];\n"
"void trits_of(uint T, inout uint t[5]) {\n"
"  uint C;\n"
"  if (((T >> 2) & 7u) == 7u) { C = ((T >> 5) & 7u) << 2 | (T & 3u); t[4] = 2u; t[3] = 2u; }\n"
"  else {\n"
"    C = T & 0x1Fu;\n"
"    if (((T >> 5) & 3u) == 3u) { t[4] = 2u; t[3] = (T >> 7) & 1u; }\n"
"    else { t[4] = (T >> 7) & 1u; t[3] = (T >> 5) & 3u; }\n"
"  }\n"
"  if ((C & 3u) == 3u) { t[2] = 2u; t[1] = (C >> 4) & 1u; t[0] = ((C >> 3) & 1u) << 1 | (((C >> 2) & 1u) & ~(C >> 3) & 1u); }\n"
"  else if (((C >> 2) & 3u) == 3u) { t[2] = 2u; t[1] = 2u; t[0] = C & 3u; }\n"
"  else { t[2] = (C >> 4) & 1u; t[1] = (C >> 2) & 3u; t[0] = ((C >> 1) & 1u) << 1 | ((C & 1u) & ~(C >> 1) & 1u); }\n"
"}\n"
"void quints_of(uint Q, inout uint q[3]) {\n"
"  if (((Q >> 1) & 3u) == 3u && ((Q >> 5) & 3u) == 0u) {\n"
"    uint q0 = Q & 1u;\n"
"    q[2] = q0 << 2 | (((Q >> 4) & 1u) & ~q0 & 1u) << 1 | (((Q >> 3) & 1u) & ~q0 & 1u);\n"
"    q[1] = 4u; q[0] = 4u;\n"
"    return;\n"
"  }\n"
"  uint C;\n"
"  if (((Q >> 1) & 3u) == 3u) { q[2] = 4u; C = ((Q >> 3) & 3u) << 3 | (~(Q >> 5) & 3u) << 1 | (Q & 1u); }\n"
"  else { q[2] = (Q >> 5) & 3u; C = Q & 0x1Fu; }\n"
"  if ((C & 7u) == 5u) { q[1] = 4u; q[0] = (C >> 3) & 3u; }\n"
"  else { q[1] = (C >> 3) & 3u; q[0] = C & 7u; }\n"
"}\n"
// count raw ISE values (tq << bits | bits) from bit pos into ise_out
"void ise_decode(uvec4 b, int pos, int count, int q) {\n"
"  int nb = int(q_bits[q]);\n"
"  if (q_trits[q] != 0u) {\n"
"    const int tb[5] = int[](2, 2, 1, 2, 1), ts[5] = int[](0, 2, 4, 5, 7);\n"
"    for (int i = 0; i < count; i += 5) {\n"
"      uint m[5] = uint[](0u,0u,0u,0u,0u), T = 0u, t[5] = uint[](0u,0u,0u,0u,0u);\n"
"      int k = 0;\n"
"      for (; k < 5 && i + k < count; k++) {\n"
"        m[k] = rbits(b, pos, nb); pos += nb;\n"
"        T |= rbits(b, pos, tb[k]) << uint(ts[k]); pos += tb[k];\n"
"      }\n"
"      trits_of(T, t);\n"
"      for (int j = 0; j < k; j++) ise_out[i + j] = (t[j] << uint(nb) | m[j]) & 0xFFu;\n"
"    }\n"
"  } else if (q_quints[q] != 0u) {\n"
"    const int qb[3] = int[](3, 2, 2), qs[3] = int[](0, 3, 5);\n"
"    for (int i = 0; i < count; i += 3) {\n"
"      uint m[3] = uint[](0u,0u,0u), Q = 0u, v[3] = uint[](0u,0u,0u);\n"
"      int k = 0;\n"
"      for (; k < 3 && i + k < count; k++) {\n"
"        m[k] = rbits(b, pos, nb); pos += nb;\n"
"        Q |= rbits(b, pos, qb[k]) << uint(qs[k]); pos += qb[k];\n"
"      }\n"
"      quints_of(Q, v);\n"
"      for (int j = 0; j < k; j++) ise_out[i + j] = (v[j] << uint(nb) | m[j]) & 0xFFu;\n"
"    }\n"
"  } else {\n"
"    for (int i = 0; i < count; i++, pos += nb) ise_out[i] = rbits(b, pos, nb);\n"
"  }\n"
"}\n"
// block mode: false if illegal. wx, wy, dual, quant, wbits out
"bool block_mode_2d(uint m, uint bw_, uint bh_, out int wx, out int wy, out int dual, out int quant, out int wbits) {\n"
"  uint r = (m >> 4) & 1u, hh = (m >> 9) & 1u, d = (m >> 10) & 1u, a = (m >> 5) & 3u;\n"
"  wx = 0; wy = 0; dual = 0; quant = 0; wbits = 0;\n"
"  if ((m & 3u) != 0u) {\n"
"    r |= (m & 3u) << 1;\n"
"    uint b = (m >> 7) & 3u;\n"
"    uint sel = (m >> 2) & 3u;\n"
"    if (sel == 0u) { wx = int(b) + 4; wy = int(a) + 2; }\n"
"    else if (sel == 1u) { wx = int(b) + 8; wy = int(a) + 2; }\n"
"    else if (sel == 2u) { wx = int(a) + 2; wy = int(b) + 8; }\n"
"    else { b &= 1u; if ((m & 0x100u) != 0u) { wx = int(b) + 2; wy = int(a) + 2; } else { wx = int(a) + 2; wy = int(b) + 6; } }\n"
"  } else {\n"
"    r |= ((m >> 2) & 3u) << 1;\n"
"    if (((m >> 2) & 3u) == 0u) return false;\n"
"    uint b = (m >> 9) & 3u;\n"
"    uint sel = (m >> 7) & 3u;\n"
"    if (sel == 0u) { wx = 12; wy = int(a) + 2; }\n"
"    else if (sel == 1u) { wx = int(a) + 2; wy = 12; }\n"
"    else if (sel == 2u) { wx = int(a) + 6; wy = int(b) + 6; d = 0u; hh = 0u; }\n"
"    else { if (a == 0u) { wx = 6; wy = 10; } else if (a == 1u) { wx = 10; wy = 6; } else return false; }\n"
"  }\n"
"  int count = wx * wy * int(d + 1u);\n"
"  dual = int(d);\n"
"  quant = int(r) - 2 + 6 * int(hh);\n"
"  if (quant < 0 || quant > 11) return false;\n"
"  wbits = ise_bitcount(count, quant);\n"
"  return count <= 64 && wbits >= 24 && wbits <= 96 && wx <= int(bw_) && wy <= int(bh_);\n"
"}\n"
"uint hash52(uint p) {\n"
"  p ^= p >> 15; p -= p << 17; p += p << 7; p += p << 4;\n"
"  p ^= p >> 5;  p += p << 16; p ^= p >> 7; p ^= p >> 3;\n"
"  p ^= p << 6;  p ^= p >> 17;\n"
"  return p;\n"
"}\n"
"int ps_s[8]; uint ps_r[4]; int ps_count;\n"
"void part_init(int seed, int count) {\n"
"  seed += (count - 1) * 1024;\n"
"  uint rnum = hash52(uint(seed));\n"
"  int s[8];\n"
"  for (int i = 0; i < 8; i++) { int v = int((rnum >> uint(4 * i)) & 0xFu); s[i] = v * v; }\n"
"  int sh1, sh2;\n"
"  if ((seed & 1) != 0) { sh1 = (seed & 2) != 0 ? 4 : 5; sh2 = count == 3 ? 6 : 5; }\n"
"  else { sh1 = count == 3 ? 6 : 5; sh2 = (seed & 2) != 0 ? 4 : 5; }\n"
"  for (int i = 0; i < 8; i++) ps_s[i] = s[i] >> ((i & 1) != 0 ? sh2 : sh1);\n"
"  ps_r[0] = rnum >> 14; ps_r[1] = rnum >> 10; ps_r[2] = rnum >> 6; ps_r[3] = rnum >> 2;\n"
"  ps_count = count;\n"
"}\n"
"int part_of(int x, int y) {\n"
"  int a = int((uint(ps_s[0] * x + ps_s[1] * y) + ps_r[0]) & 0x3Fu);\n"
"  int b = int((uint(ps_s[2] * x + ps_s[3] * y) + ps_r[1]) & 0x3Fu);\n"
"  int c = ps_count >= 3 ? int((uint(ps_s[4] * x + ps_s[5] * y) + ps_r[2]) & 0x3Fu) : 0;\n"
"  int d = ps_count >= 4 ? int((uint(ps_s[6] * x + ps_s[7] * y) + ps_r[3]) & 0x3Fu) : 0;\n"
"  if (a >= b && a >= c && a >= d) return 0;\n"
"  if (b >= c && b >= d) return 1;\n"
"  if (c >= d) return 2;\n"
"  return 3;\n"
"}\n"
"int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }\n"
"uint cv[18];\n"
// endpoints of partition p from cv[k..]: false for an HDR mode
"bool unpack_endpoints(int cem, int k, out ivec4 e0, out ivec4 e1) {\n"
"  e0 = ivec4(0); e1 = ivec4(0);\n"
"  int v0 = int(cv[k]), v1 = int(cv[k + 1]);\n"
"  if (cem == 0) { e0 = ivec4(v0, v0, v0, 255); e1 = ivec4(v1, v1, v1, 255); return true; }\n"
"  if (cem == 1) {\n"
"    int l0 = v0 >> 2 | (v1 & 0xC0);\n"
"    int l1 = min(l0 + (v1 & 0x3F), 255);\n"
"    e0 = ivec4(l0, l0, l0, 255); e1 = ivec4(l1, l1, l1, 255); return true;\n"
"  }\n"
"  if (cem == 4) { e0 = ivec4(v0, v0, v0, int(cv[k + 2])); e1 = ivec4(v1, v1, v1, int(cv[k + 3])); return true; }\n"
"  if (cem == 5) {\n"
"    int l0 = v0, l1 = v1, a0 = int(cv[k + 2]), a1 = int(cv[k + 3]);\n"
"    l0 |= (l1 & 0x80) << 1; a0 |= (a1 & 0x80) << 1;\n"
"    l1 &= 0x7F; a1 &= 0x7F;\n"
"    if ((l1 & 0x40) != 0) l1 -= 0x80;\n"
"    if ((a1 & 0x40) != 0) a1 -= 0x80;\n"
"    l0 >>= 1; a0 >>= 1; l1 >>= 1; a1 >>= 1;\n"
"    l1 = clamp255(l1 + l0); a1 = clamp255(a1 + a0);\n"
"    e0 = ivec4(l0, l0, l0, a0); e1 = ivec4(l1, l1, l1, a1); return true;\n"
"  }\n"
"  if (cem == 6 || cem == 10) {\n"
"    int s = int(cv[k + 3]);\n"
"    for (int c = 0; c < 3; c++) { e1[c] = int(cv[k + c]); e0[c] = int(cv[k + c]) * s >> 8; }\n"
"    if (cem == 6) { e0[3] = 255; e1[3] = 255; } else { e0[3] = int(cv[k + 4]); e1[3] = int(cv[k + 5]); }\n"
"    return true;\n"
"  }\n"
"  if (cem == 8 || cem == 12) {\n"
"    ivec4 a = ivec4(int(cv[k]), int(cv[k + 2]), int(cv[k + 4]), cem == 12 ? int(cv[k + 6]) : 0);\n"
"    ivec4 b = ivec4(int(cv[k + 1]), int(cv[k + 3]), int(cv[k + 5]), cem == 12 ? int(cv[k + 7]) : 0);\n"
"    if (a[0] + a[1] + a[2] > b[0] + b[1] + b[2]) {\n"
"      e0 = b; e1 = a;\n"
"      e0[0] = (e0[0] + e0[2]) >> 1; e0[1] = (e0[1] + e0[2]) >> 1;\n"
"      e1[0] = (e1[0] + e1[2]) >> 1; e1[1] = (e1[1] + e1[2]) >> 1;\n"
"    } else { e0 = a; e1 = b; }\n"
"    if (cem == 8) { e0[3] = 255; e1[3] = 255; }\n"
"    return true;\n"
"  }\n"
"  if (cem == 9 || cem == 13) {\n"
"    int n = cem == 13 ? 4 : 3;\n"
"    ivec4 a = ivec4(0), b = ivec4(0);\n"
"    for (int c = 0; c < n; c++) {\n"
"      int base = int(cv[k + 2 * c]), delta = int(cv[k + 2 * c + 1]);\n"
"      a[c] = base >> 1 | (delta & 0x80);\n"
"      b[c] = (delta >> 1) & 0x3F;\n"
"      if ((b[c] & 0x20) != 0) b[c] -= 0x40;\n"
"    }\n"
"    int sum = b[0] + b[1] + b[2];\n"
"    b += a;\n"
"    if (sum < 0) {\n"
"      e0 = b; e1 = a;\n"
"      e0[0] = (e0[0] + e0[2]) >> 1; e0[1] = (e0[1] + e0[2]) >> 1;\n"
"      e1[0] = (e1[0] + e1[2]) >> 1; e1[1] = (e1[1] + e1[2]) >> 1;\n"
"    } else { e0 = a; e1 = b; }\n"
"    for (int c = 0; c < 4; c++) { e0[c] = clamp255(e0[c]); e1[c] = clamp255(e1[c]); }\n"
"    if (cem == 9) { e0[3] = 255; e1[3] = 255; }\n"
"    return true;\n"
"  }\n"
"  return false;\n"
"}\n"
"void put(uint x0, uint y0, uint x, uint y, uvec4 c) {\n"
"  if (x0 + x < w && y0 + y < h) imageStore(dst, ivec2(int(x0 + x), int(y0 + y)), vec4(c) / 255.0);\n"
"}\n"
"void fill(uint x0, uint y0, uvec4 c) {\n"
"  for (uint y = 0u; y < bh; y++) for (uint x = 0u; x < bw; x++) put(x0, y0, x, y, c);\n"
"}\n"
"uint grid0[80], grid1[80];\n"
"void main() {\n"
"  uint id = gl_GlobalInvocationID.x;\n"
"  if (id >= bx * by || bw < 4u || bh < 4u || bw > 12u || bh > 12u) return;\n"
"  uint x0 = (id % bx) * bw, y0 = (id / bx) * bh;\n"
"  uint base = first + id * 4u;\n"
"  uvec4 b = uvec4(data[base], data[base + 1u], data[base + 2u], data[base + 3u]);\n"
"  const uvec4 MAGENTA = uvec4(255u, 0u, 255u, 255u);\n"
"  uint mode = b[0] & 0x7FFu;\n"
"  if ((mode & 0x1FFu) == 0x1FCu) {\n"
"    if ((mode & 0x200u) != 0u || ((b[0] >> 10) & 3u) != 3u) { fill(x0, y0, MAGENTA); return; }\n"
"    uint s0 = rbits(b, 12, 13), s1 = rbits(b, 25, 13), t0 = rbits(b, 38, 13), t1 = rbits(b, 51, 13);\n"
"    bool all_ones = s0 == 0x1FFFu && s1 == 0x1FFFu && t0 == 0x1FFFu && t1 == 0x1FFFu;\n"
"    if ((s0 >= s1 || t0 >= t1) && !all_ones) { fill(x0, y0, MAGENTA); return; }\n"
"    fill(x0, y0, uvec4((b[2] >> 8) & 0xFFu, b[2] >> 24, (b[3] >> 8) & 0xFFu, b[3] >> 24));\n"
"    return;\n"
"  }\n"
"  int wx, wy, dual, quant, wbits;\n"
"  if (!block_mode_2d(mode, bw, bh, wx, wy, dual, quant, wbits)) { fill(x0, y0, MAGENTA); return; }\n"
"  int pcount = int((b[0] >> 11) & 3u) + 1;\n"
"  int wcount = wx * wy;\n"
"  uvec4 rev = uvec4(bitfieldReverse(b[3]), bitfieldReverse(b[2]), bitfieldReverse(b[1]), bitfieldReverse(b[0]));\n"
"  ise_decode(rev, 0, wcount << dual, quant);\n"
"  if (dual != 0 && pcount == 4) { fill(x0, y0, MAGENTA); return; }\n"
"  uint wo = wunq_off[quant];\n"
"  for (int i = 0; i < 80; i++) { grid0[i] = 0u; grid1[i] = 0u; }\n"
"  if (dual != 0) { for (int i = 0; i < wcount; i++) { grid0[i] = wunq(wo + ise_out[2 * i]); grid1[i] = wunq(wo + ise_out[2 * i + 1]); } }\n"
"  else { for (int i = 0; i < wcount; i++) grid0[i] = wunq(wo + ise_out[i]); }\n"
"  int cem[4] = int[](0, 0, 0, 0);\n"
"  int below = 128 - wbits, high = 0;\n"
"  if (pcount == 1) cem[0] = int((b[0] >> 13) & 0xFu);\n"
"  else {\n"
"    high = 3 * pcount - 4;\n"
"    below -= high;\n"
"    uint enc = ((b[0] >> 23) & 0x3Fu) | rbits(b, below, high) << 6;\n"
"    uint cbase = enc & 3u;\n"
"    if (cbase == 0u) {\n"
"      for (int i = 0; i < pcount; i++) cem[i] = int((enc >> 2) & 0xFu);\n"
"      below += high; high = 0;\n"
"    } else {\n"
"      cbase--;\n"
"      for (int i = 0; i < pcount; i++) cem[i] = int((((enc >> uint(2 + i)) & 1u) + cbase) << 2);\n"
"      for (int i = 0; i < pcount; i++) cem[i] |= int((enc >> uint(2 + pcount + 2 * i)) & 3u);\n"
"    }\n"
"  }\n"
"  int ccount = 0;\n"
"  for (int i = 0; i < pcount; i++) ccount += ((cem[i] >> 2) + 1) * 2;\n"
"  if (ccount > 18) { fill(x0, y0, MAGENTA); return; }\n"
"  int cbits = (pcount == 1 ? 111 : 99) - wbits - high - (dual != 0 ? 2 : 0);\n"
"  if (cbits < 0) cbits = 0;\n"
"  int cq = -1;\n"
"  for (int q = 20; q >= 0; q--) { if (ise_bitcount(ccount, q) <= cbits) { cq = q; break; } }\n"
"  if (cq < 4) { fill(x0, y0, MAGENTA); return; }\n"
"  ise_decode(b, pcount == 1 ? 17 : 29, ccount, cq);\n"
"  for (int i = 0; i < ccount; i++) cv[i] = cq != 20 ? cunq(cunq_off[cq - 4] + ise_out[i]) : ise_out[i];\n"
"  int ccs = dual != 0 ? int(rbits(b, below - 2, 2)) : -1;\n"
"  ivec4 ep0[4], ep1[4];\n"
"  for (int p = 0, k = 0; p < pcount; p++) {\n"
"    ivec4 e0, e1;\n"
"    if (!unpack_endpoints(cem[p], k, e0, e1)) { e0 = ivec4(255, 0, 255, 255); e1 = e0; }\n"
"    k += ((cem[p] >> 2) + 1) * 2;\n"
"    ep0[p] = srgb != 0u ? (e0 << 8 | 0x80) : e0 * 257;\n"
"    ep1[p] = srgb != 0u ? (e1 << 8 | 0x80) : e1 * 257;\n"
"  }\n"
"  if (pcount > 1) part_init(int((b[0] >> 13) & 0x3FFu), pcount);\n"
"  bool small_ = bw * bh < 32u;\n"
"  for (uint y = 0u; y < bh; y++) {\n"
"    int vy = int(((1024u + bh / 2u) / (bh - 1u)) * y * uint(wy - 1) + 32u) >> 6;\n"
"    int yi = (vy >> 4) * wx, ft = vy & 0xF;\n"
"    for (uint x = 0u; x < bw; x++) {\n"
"      int vx = int(((1024u + bw / 2u) / (bw - 1u)) * x * uint(wx - 1) + 32u) >> 6;\n"
"      int fs = vx & 0xF;\n"
"      int w11 = (fs * ft + 8) >> 4, w10 = fs - w11, w01 = ft - w11, w00 = 16 - fs - ft + w11;\n"
"      int i0 = yi + (vx >> 4);\n"
"      int wt0 = (int(grid0[i0]) * w00 + int(grid0[i0 + 1]) * w10 + int(grid0[i0 + wx]) * w01 + int(grid0[i0 + wx + 1]) * w11 + 8) >> 4;\n"
"      int wt1 = dual != 0 ? (int(grid1[i0]) * w00 + int(grid1[i0 + 1]) * w10 + int(grid1[i0 + wx]) * w01 + int(grid1[i0 + wx + 1]) * w11 + 8) >> 4 : 0;\n"
"      int p = pcount > 1 ? part_of(small_ ? int(x) << 1 : int(x), small_ ? int(y) << 1 : int(y)) : 0;\n"
"      uvec4 o;\n"
"      for (int c = 0; c < 4; c++) {\n"
"        int wc = c == ccs ? wt1 : wt0;\n"
"        o[c] = uint((ep0[p][c] * (64 - wc) + ep1[p][c] * wc + 32) >> 14);\n"
"      }\n"
"      put(x0, y0, x, y, o);\n"
"    }\n"
"  }\n"
"}\n";

const char *vk_astc_glsl(void) { return ASTC_GLSL; }

// The tables at the start of each upload's buffer (1080 bytes, padded to 1088).
uint32_t vk_astc_tables(uint8_t *out) {
    const uint8_t *w, *c;
    astc_unquant_tables(&w, &c);
    memcpy(out, w, 142);
    memcpy(out + 142, c, 936);
    memset(out + 1078, 0, 10);
    return 1088;
}

#ifndef ASTC_GPU_SOURCE_ONLY
static VkPipeline pipe;
static VkPipelineLayout layout;
static VkDescriptorSetLayout set_layout;
static int state;  // 0 not tried, 1 ready, 2 unavailable

bool vk_astc_gpu_ready(void) {
    if (state) return state == 1;
    state = 2;
    const char *e = getenv("LARECOMP_GPU_ASTC");
    if (e && !atoi(e)) { LOG("[vk] ASTC: CPU decoding (LARECOMP_GPU_ASTC=0)\n"); return false; }
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, VK_FORMAT_R8G8B8A8_UNORM, &p);
    if (!(p.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) return false;
    VkShaderModule m = vk_compile_glsl(ASTC_GLSL, STAGE_COMPUTE, "astc");
    if (!m) { LOG("[vk] ASTC: decoder shader failed, CPU decoding\n"); return false; }
    VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b};
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &set_layout));
    VkPushConstantRange r = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &pi, NULL, &layout));
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = layout};
    VkResult res = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &pipe);
    vkDestroyShaderModule(g_vk.dev, m, NULL);
    if (res != VK_SUCCESS) { LOG("[vk] ASTC: decoder pipeline failed (%d), CPU decoding\n", res); return false; }
    LOG("[vk] ASTC: decoded on the GPU\n");
    state = 1;
    return true;
}

// Decode one level/layer: the bx x by blocks at word `first` of buf (bound at off, size bytes) into view (an RGBA8
// UNORM storage view of that level and layer), texels outside w x h dropped.
void vk_astc_gpu_decode(VkCommandBuffer cb, VkBuffer buf, uint64_t off, uint64_t size, uint32_t first, VkImageView view,
                        uint32_t bx, uint32_t by, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, bool srgb) {
    VkDescriptorSet ds = vk_alloc_set(set_layout);
    VkDescriptorBufferInfo bi = {buf, off, size};
    VkDescriptorImageInfo ii = {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ii}};
    vkUpdateDescriptorSets(g_vk.dev, 2, wr, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &ds, 0, NULL);
    uint32_t pc[8] = {first, bx, by, w, h, bw, bh, srgb ? 1u : 0u};
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (bx * by + 63) / 64, 1, 1);
}
#endif
