/* Astral Chain additions to the vendored Maxwell translator (GLSL compute only).
 *
 * shader_build.py includes this file twice in the reviewed translate.c:
 *  - with MAXWELL_EXT_DEFS before emit_insn: helpers;
 *  - without it inside emit_insn's opcode chain, before LDL/STL: the instruction cases (each begins "} else if").
 * Encodings were checked against nvdisasm 11.4 (SM50) on words from the game's own compute shaders
 * (output/research/cs-dis). Unsupported variants fail translation explicitly instead of guessing.
 *
 * Shared memory: `shared uint smem_[SMEM_WORDS]` (48 KB, the GM20B per-block maximum and the common Vulkan
 * maxComputeSharedMemorySize); indices are clamped into it. Sub-word stores use atomics, since neighbouring
 * invocations may own the other bytes of a word. */
#ifdef MAXWELL_EXT_DEFS
#define SMEM_WORDS 12288

// old_ = atomic <op> of v on the lvalue mem (GLSL). op: 0 add, 1 min, 2 max, 5 and, 6 or, 7 xor, 8 exch.
// Signed min/max (S32) run as compare-and-swap loops: the storage is uint.
static bool ext_atomic(Ctx *c, const char *mem, int op, bool sgn, const char *v) {
    static const char *const fn[] = {"atomicAdd", "atomicMin", "atomicMax", NULL, NULL, "atomicAnd", "atomicOr", "atomicXor",
                                     "atomicExchange"};
    if (op > 8 || !fn[op]) { fail(c, "unsupported atomic operation %d", op); return false; }
    if ((op == 1 || op == 2) && sgn)
        sp(&c->body, "{ uint o_ = %s; for (;;) { uint n_ = uint(%s(int(o_), int(%s))); uint p_ = atomicCompSwap(%s, o_, n_); "
                     "if (p_ == o_) break; o_ = p_; } old_ = o_; }", mem, op == 1 ? "min" : "max", v, mem);
    else
        sp(&c->body, "old_ = %s(%s, %s);", fn[op], mem, v);
    return true;
}

static bool ext_compute_only(Ctx *c, const char *what, uint32_t pc) {
    if (c->glsl && c->stage == STAGE_COMPUTE) return true;
    fail(c, "%s outside GLSL compute at %#x", what, pc);
    return false;
}
#else
    } else if (OPM(0xef48000000000000ull, 0xfff8000000000000ull) || OPM(0xef58000000000000ull, 0xfff8000000000000ull)) {  // LDS / STS
        bool store = bits(w, 52, 1);
        int size = (int)bits(w, 48, 3);  // 0 u8, 1 s8, 2 u16, 3 s16, 4 32, 5 64, 6 128
        uint32_t off = (uint32_t)sbits(w, 20, 24);
        if (ext_compute_only(c, store ? "STS" : "LDS", pc)) {
            if (size > 6) fail(c, "LDS/STS size %d at %#x", size, pc);
            c->uses_shared = true;
            if (a == RZ) sprintf(t, "%uu", off);
            else sprintf(t, "(%s + %uu)", R(a), off);
            sp(&c->body, "    { uint sa = %s; uint si = min(sa >> 2, %du);\n", t, SMEM_WORDS - 1);
            int nw = size == 5 ? 2 : size == 6 ? 4 : 1;
            if (!store) {
                static const char *const ext[] = {"(v >> sh) & 0xFFu", "uint(int(v << (24u - sh)) >> 24)", "(v >> sh) & 0xFFFFu",
                                                  "uint(int(v << (16u - sh)) >> 16)"};
                if (size < 4) {
                    if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      uint v = smem_[si]; uint sh = (sa & %du) * 8u; r%d = %s;\n", size < 2 ? 3 : 2, d, ext[size]); }
                } else {
                    for (int k = 0; k < nw; k++)
                        if (d != RZ && d + k < RZ) { c->reg_used[d + k] = 1; sp(&c->body, "      r%d = smem_[min(si + %du, %du)];\n", d + k, k, SMEM_WORDS - 1); }
                }
            } else {
                if (size < 4) {
                    uint32_t m = size < 2 ? 0xFFu : 0xFFFFu;
                    sp(&c->body, "      uint sh = (sa & %du) * 8u; atomicAnd(smem_[si], ~(0x%xu << sh)); atomicOr(smem_[si], (%s & 0x%xu) << sh);\n",
                       size < 2 ? 3 : 2, m, R(d), m);
                } else {
                    for (int k = 0; k < nw; k++)
                        sp(&c->body, "      smem_[min(si + %du, %du)] = %s;\n", k, SMEM_WORDS - 1, R(d == RZ ? RZ : d + k));
                }
            }
            sp(&c->body, "    }\n");
        }
    } else if (OPM(0xec00000000000000ull, 0xff00000000000000ull)) {  // ATOMS (shared memory atomic)
        int op = (int)bits(w, 52, 4), type = (int)bits(w, 28, 2), b = (int)bits(w, 20, 8);
        uint32_t off = (uint32_t)bits(w, 30, 22) * 4u;
        if (ext_compute_only(c, "ATOMS", pc)) {
            if (type > 1) fail(c, "ATOMS type %d at %#x", type, pc);
            c->uses_shared = true;
            if (a == RZ) sprintf(t, "%uu", off);
            else sprintf(t, "(%s + %uu)", R(a), off);
            sp(&c->body, "    { uint si = min(%s >> 2, %du); uint old_ = 0u; ", t, SMEM_WORDS - 1);
            ext_atomic(c, "smem_[si]", op, type == 1, R(b));
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, " r%d = old_;", d); }
            sp(&c->body, " }\n");
        }
    } else if (OPM(0xebf8000000000000ull, 0xfff8000000000000ull) || OPM(0xed00000000000000ull, 0xff00000000000000ull)) {  // RED / ATOM (global)
        bool atom = (w >> 56) == 0xed;
        int op = atom ? (int)bits(w, 52, 4) : (int)bits(w, 23, 4);
        int type = atom ? (int)bits(w, 49, 3) : (int)bits(w, 20, 3);
        int v = atom ? (int)bits(w, 20, 8) : d;
        bool wide = bits(w, 48, 1);
        int64_t off = sbits(w, 28, 20);
        if (ext_compute_only(c, atom ? "ATOM" : "RED", pc)) {
            if (type > 1) fail(c, "%s type %d at %#x", atom ? "ATOM" : "RED", type, pc);
            c->info->uses_gmem = true;
            c->info->uses_gmem_map = true;
            c->uses_gmem_store = true;
            c->info->uses_gmem_store = true;
            if (a == RZ) sprintf(t, "ulong(%lld)", (long long)off);
            else if (wide) sprintf(t, "((ulong(%s) << 32 | ulong(%s)) + ulong(%lld))", R(a + 1), R(a), (long long)off);
            else sprintf(t, "(ulong(%s) + ulong(%lld))", R(a), (long long)off);
            sp(&c->body, "    { ulong ga = %s - 0x%llxul; bool ok = ga < 0x%llxul; uint fw = ok ? gfind(uint(ga)) : 0xFFFFFFFFu; uint old_ = 0u;\n",
               t, (unsigned long long)maxwell_gmem_base, (unsigned long long)maxwell_gmem_size);
            const char *val = R(v);
            sp(&c->body, "      if (fw != 0xFFFFFFFFu) ");
            ext_atomic(c, "gfast_.w[fw]", op, type == 1, val);
            sp(&c->body, "\n      else if (ok) ");
            ext_atomic(c, "gmem_.w[uint(ga >> 2)]", op, type == 1, val);
            sp(&c->body, "\n");
            if (atom && d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = old_;\n", d); }
            sp(&c->body, "    }\n");
        }
    } else if (OPM(0x50d8000000000000ull, 0xfff8000000000000ull)) {  // VOTE.ALL / ANY / EQ
        // Maxwell warps are 32 lanes, as NVIDIA subgroups on the host; d receives the ballot of the source predicate.
        int mode = (int)bits(w, 48, 2), pd = (int)bits(w, 45, 3), ps = (int)bits(w, 39, 3);
        bool pn = bits(w, 42, 1);
        if (!c->glsl || mode == 3) fail(c, "VOTE mode %d at %#x", mode, pc);
        sp(&c->body, "    { bool vp_ = %s;\n", P(ps, pn));
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = subgroupBallot(vp_).x;\n", d); }
        if (pd != PT) {
            c->pred_used[pd] = 1;
            sp(&c->body, "      p%d = %s;\n", pd, mode == 0 ? "subgroupAll(vp_)" : mode == 1 ? "subgroupAny(vp_)" : "subgroupAllEqual(vp_)");
        }
        sp(&c->body, "    }\n");
    } else if (OPM(0xdc38000000000000ull, 0xff38000000000000ull)) {  // TLD (full form: texel fetch)
        // a: [layer], x, y, z; b: [lod (LL)], [offsets (AOFFI): 4-bit signed x, y]. Bits: 28 array, 29-30 dim,
        // 31-34 mask, 35 AOFFI, 36-48 index, 50 MS, 54 CL, 55 LL (nvdisasm 11.4).
        int target = (int)bits(w, 29, 2), b = (int)bits(w, 20, 8);
        bool array = bits(w, 28, 1), aoffi = bits(w, 35, 1), ll = bits(w, 55, 1);
        uint32_t index = (uint32_t)bits(w, 36, 13), mask = (uint32_t)bits(w, 31, 4);
        if (!c->glsl || bits(w, 50, 1) || bits(w, 54, 1) || target == 3 || (target == 2 && array))
            fail(c, "TLD variant %016llx at %#x", (unsigned long long)w, pc);
        int type = target == 0 ? (array ? TEXTYPE_1D_ARRAY : TEXTYPE_1D) : target == 1 ? (array ? TEXTYPE_2D_ARRAY : TEXTYPE_2D) : TEXTYPE_3D;
        int slot = tex_slot(c, index, type, false, array);
        int ra = a, rb = b;
        char layer[48] = "", lod[48] = "0", off[96] = "", coords[256];
        if (array) { sprintf(layer, "int(%s & 0xFFFFu)", R(ra)); ra++; }
        if (ll) { sprintf(lod, "int(%s)", R(rb)); rb++; }
        if (aoffi) sprintf(off, " + ivec2(int(%s << 28) >> 28, int(%s << 24) >> 28)", R(rb), R(rb));
        if (target == 0) {
            if (aoffi) fail(c, "TLD 1D offset at %#x", pc);
            if (array) sprintf(coords, "ivec2(int(%s), %s)", R(ra), layer); else sprintf(coords, "int(%s)", R(ra));
        }
        if (target == 1) {
            char xy[192];
            snprintf(xy, sizeof xy, "ivec2(%s)", RSC2(c, slot, "uint2(int2(int(%s), int(%s))%s)", R(ra), R(ra + 1), off));
            if (array) sprintf(coords, "ivec3(%s, %s)", xy, layer); else strcpy(coords, xy);
        }
        if (target == 2) sprintf(coords, "ivec3(int(%s), int(%s), int(%s))", R(ra), R(ra + 1), R(ra + 2));
        sprintf(u, "texelFetch(t%d, %s, %s)", slot, coords, lod);
        write_tex_result(c, slot, u, mask, d, RZ, false, false);
    } else if (OPM(0xeb28000000000000ull, 0xffb8000000000000ull)) {  // SUST.P (formatted surface store, immediate handle)
        // d: first data register (one per stored component, float bits), a: x [, y] [, z / layer]; 20-23 component
        // mask, 33-35 dimension (0 1D, 2 1D array, 3 2D, 4 2D array, 5 3D), 36-48 handle index, 51 immediate.
        int dim = (int)bits(w, 33, 3);
        uint32_t index = (uint32_t)bits(w, 36, 13), mask = (uint32_t)bits(w, 20, 4);
        if (!ext_compute_only(c, "SUST", pc)) {
        } else if (!bits(w, 51, 1) || mask != 0xf || dim == 1 || dim > 5) {
            fail(c, "SUST variant %016llx at %#x", (unsigned long long)w, pc);
        } else {
            int type = dim == 0 ? TEXTYPE_1D : dim == 2 ? TEXTYPE_1D_ARRAY : dim == 3 ? TEXTYPE_2D : dim == 4 ? TEXTYPE_2D_ARRAY : TEXTYPE_3D;
            int slot = tex_slot(c, index, type, false, dim == 2 || dim == 4);
            c->info->tex[slot].is_image = true;
            const char *coord = dim == 0 ? "int(%s)" : dim == 2 || dim == 3 ? "ivec2(int(%s), int(%s))" : "ivec3(int(%s), int(%s), int(%s))";
            char xyz[192];
            snprintf(xyz, sizeof xyz, coord, R(a), R(a + 1), R(a + 2));
            sp(&c->body, "    imageStore(t%d, %s, vec4(as_type<float>(%s), as_type<float>(%s), as_type<float>(%s), as_type<float>(%s)));\n",
               slot, xyz, R(d), R(d + 1), R(d + 2), R(d + 3));
        }
    } else if (OPM(0x5bd8000000000000ull, 0xfff8000000000000ull)) {  // LEA.HI (register form)
        // d = b + high word of ({c:a} << shift); .X / .CC / predicate output / negated a not seen in this game yet
        int b = (int)bits(w, 20, 8), hi = (int)bits(w, 39, 8), sh = (int)bits(w, 28, 5);
        if (bits(w, 37, 1) || bits(w, 38, 1) || bits(w, 47, 1) || bits(w, 48, 3) != 7)
            fail(c, "LEA.HI variant %016llx at %#x", (unsigned long long)w, pc);
        sprintf(u, "(%s + uint((ulong(%s) << 32 | ulong(%s)) >> %du))", R(b), R(hi), R(a), 32 - sh);
        set_reg(c, d, u);
#endif
