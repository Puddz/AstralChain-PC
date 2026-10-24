"""Build an offline probe with reviewed patches to the pinned MIT translator.

The reference checkout stays untouched; derived source and executable belong in
ignored output. Reject upstream drift rather than silently applying a stale patch.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess

TRANSLATOR_SHA256 = 'f1c65e86c8e3f18e79027a397c06c4a0326d10fc4b73a71c5d0bb6e986ebd412'
EXIT_ORIGINAL = '''    if (OPM(0xe300000000000000ull, 0xfff0000000000000ull)) {  // EXIT
        sp(&c->body, c->branchy ? "    exit_now = true; break;\\n" : "    break;\\n");'''
EXIT_REVIEWED = '''    if (OPM(0xe300000000000000ull, 0xfff0000000000000ull)) {  // EXIT
        unsigned condition = (unsigned)bits(w, 0, 5);
        if (condition == 15)
            sp(&c->body, c->branchy ? "    exit_now = true; break;\\n" : "    break;\\n");
        else if (condition != 0 && condition != 28)
            fail(c, "unsupported EXIT condition %#x at %#x", condition, pc);'''
# Ryujinx d1a093e5 InstEmitFlowControl.Exit evaluates FCSM_TR (28) through
# GetCondition's false default, and InstEmit.Votevtg emits nothing. Both are
# viewport-culling early outs; host primitive clipping discards the same work.
VOTE_ORIGINAL = '''    } else if (OPM(0xf0a8000000000000ull, 0xfff8000000000000ull)) {  // BAR'''
VOTE_REVIEWED = '''    } else if (OPM(0x50e0000000000000ull, 0xfff8000000000000ull)) {  // VOTE.VTG
        sp(&c->body, "    /* vote.vtg: host clipping culls */\\n");
    } else if (OPM(0xf0a8000000000000ull, 0xfff8000000000000ull)) {  // BAR'''
# LASRecomp's luminance tint is a Link's Awakening post-process, not Maxwell.
TINT_ORIGINAL = '''    bool tint = stage == STAGE_FRAGMENT && !dbg && (c->omap_target & 0x7) == 0x7 && (!key || key->rt_kind[0] == 0);'''
TINT_REVIEWED = '''    bool tint = false;  /* game-specific LASRecomp tint excluded */'''
# NVN's fixed-function alpha test (nvnColorStateSetAlphaTest + nvnCommandBufferSetAlphaRef) has no Vulkan
# equivalent: the GLSL fragment shader discards when c0.a fails it (ShaderKey.alpha_func / alpha_ref, a half float;
# the reference is emitted bit-exact). Astral Chain's chimera blood (EffectOpacityBlood) writes alpha 0 or 1 and
# blends ONE/ZERO, relying on the test to dissolve: without it the splash stayed a solid black mass.
ALPHA_ORIGINAL = '''        if (c->omap_depth) {
            if (c->reg_used[r + 1]) sp(&s, "  gl_FragDepth'''
ALPHA_REVIEWED = r'''        if (key && key->alpha_func && key->alpha_func < 8 && (c->omap_target & 0x8) && key->rt_kind[0] == 0) {
            static const char *const alpha_ops[8] = {"", "", "<", "==", "<=", ">", "!=", ">="};
            uint32_t h = key->alpha_ref, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
            float ref = e ? ldexpf((float)(m | 0x400u), (int)e - 25) : ldexpf((float)m, -24);
            if (h & 0x8000u) ref = -ref;
            uint32_t bits;
            memcpy(&bits, &ref, 4);
            if (key->alpha_func == 1) sp(&s, "  discard;\n");
            else sp(&s, "  if (!(c0.a %s uintBitsToFloat(0x%08xu))) discard;\n", alpha_ops[key->alpha_func], bits);
        }
        if (c->omap_depth) {
            if (c->reg_used[r + 1]) sp(&s, "  gl_FragDepth'''
# Maxwell keeps SSY, PBK and PCNT tokens on one CRS stack: SYNC and BRK pop down to and including the nearest
# token of their own kind (discarding tokens pushed above it), CONT returns to the nearest PCNT token and keeps it.
# LASRecomp's three independent stacks leave a stale SSY token behind whenever a BRK or CONT leaves an SSY region;
# a later SYNC then jumps back into an old region (a loop that only ends at the iteration guard) and unbounded
# pushes write past the 16-entry arrays. Tokens: target | kind << 30 (1 SSY, 2 PBK, 3 PCNT); an empty stack exits.
CRS_ORIGINAL = (
    '        sp(&c->body, "    ssy[ssy_n++] = %uu;\\n", norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));',
    '        sp(&c->body, "    pbk[pbk_n++] = %uu;\\n", norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));',
    '        sp(&c->body, "    pcnt[pcnt_n++] = %uu;\\n", norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));',
    '        sp(&c->body, "    pc = ssy[--ssy_n]; continue;\\n");',
    '        sp(&c->body, "    pc = pbk[--pbk_n]; continue;\\n");',
    '        sp(&c->body, "    pc = pcnt[pcnt_n - 1]; continue;\\n");',
    '        sp(&s, "  uint pc = 8u; uint ssy[16]; uint ssy_n = 0u; uint pbk[16]; uint pbk_n = 0u; uint pcnt[16]; uint pcnt_n = 0u;\\n");',
    '        sp(&s, "  uint pc = 8u; uint ssy[16]; uint ssy_n = 0; uint pbk[16]; uint pbk_n = 0; uint pcnt[16]; uint pcnt_n = 0;\\n");',
)
CRS_PUSH = '        sp(&c->body, "    if (crs_n < 32u) { crs[crs_n] = %uu | {kind}u; crs_n++; }\\n", norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));'
CRS_POP = ('        sp(&c->body, "    {{ uint e_ = 0u; while (crs_n > 0u) {{ crs_n--; e_ = crs[crs_n]; if ((e_ >> 30) == {kind}u) break; }} '
           'if ((e_ >> 30) != {kind}u) {{ exit_now = true; break; }} pc = e_ & 0x3FFFFFFFu; continue; }}\\n");')
CRS_REVIEWED = (
    CRS_PUSH.replace('{kind}', '0x40000000'),
    CRS_PUSH.replace('{kind}', '0x80000000'),
    CRS_PUSH.replace('{kind}', '0xC0000000'),
    CRS_POP.format(kind=1),
    CRS_POP.format(kind=2),
    '        sp(&c->body, "    { while (crs_n > 0u && (crs[crs_n - 1u] >> 30) != 3u) crs_n--; '
    'if (crs_n == 0u) { exit_now = true; break; } pc = crs[crs_n - 1u] & 0x3FFFFFFFu; continue; }\\n");',
    '        sp(&s, "  uint pc = 8u; uint crs[32]; uint crs_n = 0u;\\n");',
    '        sp(&s, "  uint pc = 8u; uint crs[32]; uint crs_n = 0u;\\n");',
)
# State-machine iteration cap per invocation (block transitions, not loop iterations). LASRecomp's 100000 lets one
# runaway pixel cost seconds of GPU time across a full-screen pass (driver timeouts); LARECOMP_SHADER_GUARD at
# translation time overrides the default. Changing it changes the source and so the SPIR-V cache key.
GUARD_ORIGINAL = ('        sp(&s, "  for (uint guard = 0u; guard < 100000u; guard++) {\\n  switch (pc) {\\n");',
                  '        sp(&s, "  for (uint guard = 0; guard < 100000u; guard++) {\\n  switch (pc) {\\n");')
GUARD_REVIEWED = tuple(
    '        sp(&s, "%s  for (uint guard = 0u; guard < %uu; guard++) {\\n%s  switch (pc) {\\n", '
    'c->bar_wait ? "  bool bar_wait = false;\\n" : "", '
    'getenv("LARECOMP_SHADER_GUARD") ? (unsigned)strtoul(getenv("LARECOMP_SHADER_GUARD"), NULL, 0) : 16384u, '
    'c->bar_wait ? "  if (subgroupAll(bar_wait)) { barrier(); bar_wait = false; }\\n  if (!bar_wait) {\\n" : "");'
    for _ in range(2))
# Compute barriers (BAR.SYNC). Maxwell reconverges a warp at SYNC before it reaches a barrier; in the translated state
# machine a thread that took a branch (a bounds check, say) can reach the barrier one loop iteration later than the
# rest of its warp, and a host barrier() in a divergent warp is undefined: on NVIDIA it can release before those
# threads have stored to shared memory, so the others read stale shared memory (Astral Chain's average brightness,
# 72c18fb800, summed garbage for the 16 threads past its 240 values: 1e30, inf or NaN, and the exposure flickered).
# Here a thread at a barrier parks (bar_wait, its pc set past the barrier) and does nothing more until every live
# thread of its subgroup is parked; then they run barrier() together, converged, at the top of the loop: every live
# thread passes there each iteration (a branch's continue would skip a check at the bottom).
BAR_ORIGINAL = (
    '        (void)is_branch_target_op;',
    '        else if (c->stage == STAGE_COMPUTE) sp(&c->body, "    barrier();\\n");',
    '        sp(&s, "    exit_now = true; break;\\n  default: exit_now = true; break;\\n  }\\n  if (exit_now) break;\\n  }\\n");',
)
BAR_REVIEWED = (
    '        if (c->glsl && stage == STAGE_COMPUTE && (w & 0xfff8000000000000ull) == 0xf0a8000000000000ull) {  // BAR\n'
    '            size_t nx = i + 1;\n'
    '            if (nx % 4 == 0) nx++;\n'
    '            if (nx < end) label[nx] = 1;\n'
    '            any_branch = true;\n'
    '            c->bar_wait = true;\n'
    '        }\n'
    '        (void)is_branch_target_op;',
    '        else if (c->stage == STAGE_COMPUTE) {\n'
    '            uint32_t nx = pc + 8;\n'
    '            if ((nx / 8) % 4 == 0) nx += 8;\n'
    '            sp(&c->body, "    bar_wait = true; pc = %uu; break;\\n", nx);\n'
    '        }',
    '        sp(&s, c->bar_wait ? "    exit_now = true; break;\\n  default: exit_now = true; break;\\n  }\\n  if (exit_now) break;\\n  }\\n'
    '  }\\n" '
    ': "    exit_now = true; break;\\n  default: exit_now = true; break;\\n  }\\n  if (exit_now) break;\\n  }\\n");',
)
# Compute instructions the game needs that the translator lacks (shared memory, shared/global atomics, LEA.HI):
# runtime/las/maxwell_ext.h, included before emit_insn (helpers) and in its opcode chain (cases).
EXT_ORIGINAL = (
    '    uint32_t cbuf_need[18];  // highest constant vec4 index + 1 per cbuf (array size of the GLSL block)\n} Ctx;',
    '// ------------------------------------------------------------------ instruction translation\n',
    '    } else if (OPM(0xef40000000000000ull, 0xfff8000000000000ull) || OPM(0xef50000000000000ull, 0xfff8000000000000ull)) {  // LDL / STL',
    '    sp(&s, "void main() {\\n");',
    '    "#extension GL_KHR_shader_subgroup_shuffle_relative : require\\n"',
    '        sp(&s, "layout(set = %d, binding = %d) uniform %s t%d;\\n", set, 32 + i, glsl_sampler_type(t), i);',
)
EXT_REVIEWED = (
    '    uint32_t cbuf_need[18];  // highest constant vec4 index + 1 per cbuf (array size of the GLSL block)\n'
    '    bool uses_shared;        // (Astral Chain) LDS/STS/ATOMS: shared uint smem_[]\n'
    '    bool bar_wait;           // (Astral Chain) GLSL compute with BAR: threads park at barriers (BAR_REVIEWED)\n} Ctx;',
    '#define MAXWELL_EXT_DEFS\n#include "maxwell_ext.h"\n#undef MAXWELL_EXT_DEFS\n\n'
    '// ------------------------------------------------------------------ instruction translation\n',
    '#include "maxwell_ext.h"\n'
    '    } else if (OPM(0xef40000000000000ull, 0xfff8000000000000ull) || OPM(0xef50000000000000ull, 0xfff8000000000000ull)) {  // LDL / STL',
    '    if (c->uses_shared) sp(&s, "shared uint smem_[%d];\\n", SMEM_WORDS);\n    sp(&s, "void main() {\\n");',
    '    "#extension GL_KHR_shader_subgroup_shuffle_relative : require\\n"\n'
    '    "#extension GL_KHR_shader_subgroup_vote : require\\n"\n'
    '    "#extension GL_KHR_shader_subgroup_ballot : require\\n"',
    # SUST targets: storage images, written without a format qualifier (shaderStorageImageWriteWithoutFormat).
    '        if (t->is_image) {\n'
    '            static const char *const img[] = {"image1D", "image2D", "image3D", "imageCube", "image2DArray", "imageCubeArray", "image1DArray"};\n'
    '            sp(&s, "layout(set = %d, binding = %d) uniform writeonly %s t%d;\\n", set, 32 + i, img[t->type <= TEXTYPE_1D_ARRAY ? t->type : 1], i);\n'
    '            continue;\n'
    '        }\n'
    '        sp(&s, "layout(set = %d, binding = %d) uniform %s t%d;\\n", set, 32 + i, glsl_sampler_type(t), i);',
)
S2R_ORIGINAL = '        default: sprintf(t, "0u /* sr %#x */", sr); set_reg(c, d, t);'
# WScaleFactorXY/Z (Ryujinx SReg 30/31): viewport W scaling factors. Astral Chain
# never calls an NVN W-scaling or viewport-swizzle API (runs 294-296), so the
# hardware factors are zero; in its shaders they only feed the culling vote above.
# SR_EQMASK..SR_GEMASK (0x38-0x3c, nvdisasm): lane masks; 32-lane warps = the low word of the subgroup masks.
S2R_REVIEWED = ('        case 0x1e: case 0x1f: set_reg(c, d, "0u"); break;  // W scaling disabled\n'
                '        case 0x38: set_reg(c, d, "gl_SubgroupEqMask.x"); break;\n'
                '        case 0x39: set_reg(c, d, "gl_SubgroupLtMask.x"); break;\n'
                '        case 0x3a: set_reg(c, d, "gl_SubgroupLeMask.x"); break;\n'
                '        case 0x3b: set_reg(c, d, "gl_SubgroupGtMask.x"); break;\n'
                '        case 0x3c: set_reg(c, d, "gl_SubgroupGeMask.x"); break;\n'
                '        default: fail(c, "unsupported special register %#x at %#x", sr, pc); break;')


def prepare_translator(reference: Path, output: Path) -> Path:
    source = reference/'runtime/nvn/maxwell/translate.c'
    with source.open('rb') as stream:
        data = stream.read(2 * 1024 * 1024 + 1)
    data = data.replace(b'\r\n', b'\n')  # (a git checkout may have CRLF line endings, a zip download LF)
    if hashlib.sha256(data).hexdigest() != TRANSLATOR_SHA256:
        raise ValueError('unreviewed Maxwell translator identity')
    text = data.decode('utf-8')
    # The tint declaration occurs once each in the GLSL and MSL emitters.
    patches = [(EXIT_ORIGINAL, EXIT_REVIEWED, 1), (S2R_ORIGINAL, S2R_REVIEWED, 1),
               (VOTE_ORIGINAL, VOTE_REVIEWED, 1), (TINT_ORIGINAL, TINT_REVIEWED, 2), (ALPHA_ORIGINAL, ALPHA_REVIEWED, 1)]
    patches += [(o, r, 1) for o, r in zip(CRS_ORIGINAL + GUARD_ORIGINAL + EXT_ORIGINAL,
                                          CRS_REVIEWED + GUARD_REVIEWED + EXT_REVIEWED)]
    patches += [(BAR_ORIGINAL[0], BAR_REVIEWED[0], 1), (BAR_ORIGINAL[1], BAR_REVIEWED[1], 1),
                (BAR_ORIGINAL[2], BAR_REVIEWED[2], 2)]
    for original, replacement, expected in patches:
        if text.count(original) != expected:
            raise ValueError('expected exactly one reviewed translator patch location')
        text = text.replace(original, replacement)
    output.mkdir(parents=True, exist_ok=True)
    target = output/'translate-reviewed.c'
    target.write_text(text, encoding='utf-8', newline='\n')
    return target


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    reference, output = args.reference.resolve(), args.output.resolve()
    source = prepare_translator(reference, output)
    subprocess.run([str(args.compiler.resolve()), 'cc', '-O1', '-I',
                    str(reference/'runtime/nvn/maxwell'), '-I', str(Path(__file__).parent/'runtime/las'),
                    str(Path(__file__).parent/'runtime/shader_probe.c'), str(source),
                    '-o', str(output/'shader-probe.exe')], check=True)


if __name__ == '__main__':
    main()
