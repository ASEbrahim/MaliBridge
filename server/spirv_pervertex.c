/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Mali (r44p1, Tab S10+) crashes inside its compiler (cmpbe_v2_compile_multiple_shaders, a null
 * read at 0x40) on any Vulkan pipeline whose geometry or tessellation shader declares the
 * per-vertex built-ins (Position, PointSize, ClipDistance, CullDistance) as loose variables
 * instead of members of a gl_PerVertex block. glslang always emits the block; Zink emits loose
 * variables, and gives Steam's UI a geometry shader (quads). Proven on the device with
 * tests/gauntlet.c: the same geometry shader crashes loose and builds as a block. Arm's errata
 * list a related bug for loose gl_Position in these stages (3787459, fixed r47p0).
 *
 * This pass folds a stage's loose per-vertex built-ins of each interface (input, output) into one
 * Block struct and redirects their uses to its members. Only geometry and tessellation stages;
 * the vertex shader may stay loose (a loose vertex output feeds a block input fine). Anything
 * unexpected - a use the pass does not know how to redirect - leaves the module untouched.
 *
 * DXVK's geometry shaders (Deep Rock Galactic, tests/corpus/drg_dxvk_gs.spv) decorate the output
 * SV_POSITION Invariant (D3D's precise position); that moves to the block member. Whole loads and
 * stores of an arrayed (per-vertex) variable become one access per vertex.
 */
#include "spirv_pervertex.h"

#include "spirv_edit.h"

#include <stdlib.h>
#include <string.h>

#define MAXV 8
#define MAXA 32  /* array length of an arrayed interface (patch vertices); larger stops the pass */
#define MAXMEM 4 /* memory operand words of a whole-array load/store */

enum { OpAccessChain = 65, OpInBoundsAccessChain = 66, OpCompositeExtract = 81, OpCompositeInsert = 82,
       OpVectorShuffle = 79, OpSwitch = 251, OpLoopMerge = 246, OpCopyMemory = 63, OpPtrAccessChain = 67,
       OpCompositeConstruct = 80 };
enum { SpvDecorationInvariant = 18 };

typedef struct {
    uint32_t var, ptr_type, pointee, member_type, builtin;
    int invariant;
} loose_var;

static int is_annotation_or_debug(uint16_t op)
{
    return op == SpvOpName || op == SpvOpMemberName || op == SpvOpDecorate || op == SpvOpMemberDecorate ||
           op == SpvOpDecorateId || op == SpvOpDecorateString || op == SpvOpMemberDecorateString || op == SpvOpString ||
           op == SpvOpSource || op == SpvOpSourceExtension || op == SpvOpSourceContinued || op == SpvOpLine ||
           op == SpvOpNoLine || op == SpvOpModuleProcessed || op == SpvOpExecutionMode || op == SpvOpExecutionModeId ||
           op == SpvOpCapability || op == SpvOpExtension || op == SpvOpExtInstImport || op == SpvOpMemoryModel ||
           op == 411 /* DecorationGroup... */;
}

static int is_type_or_const(uint16_t op) { return (op >= 19 && op <= 39) || (op >= 41 && op <= 52) || op == 4472; }

/* Number of leading operand words that may hold ids for an instruction in a function body (the
 * rest are literals); UINT16_MAX = all. Conservative: an id in a literal position would only
 * stop the pass, never break a shader. */
static uint32_t id_words(const spv_inst *in)
{
    switch (in->op) {
    case OpCompositeExtract: return 3;
    case OpCompositeInsert: return 4;
    case OpVectorShuffle: return 4;
    case SpvOpExtInst: return 3; /* type, result, set; then the instruction number, then ids */
    case SpvOpLoad: return 3;
    case SpvOpStore: return 2;
    case OpCopyMemory: return 2;
    case OpSwitch: return 2;
    case OpLoopMerge: return 2;
    case SpvOpSelectionMerge: return 1;
    case SpvOpBranchConditional: return 3;
    default: return UINT16_MAX;
    }
}

static int find_var(const loose_var *v, int n, uint32_t id)
{
    for (int i = 0; i < n; i++)
        if (v[i].var == id) return i;
    return -1;
}

/* Collects the loose built-ins of one storage class; 0 none, -1 something the pass cannot take. */
static int collect(const spv_mod *m, uint32_t storage, int arrayed, loose_var *out)
{
    int n = 0;
    for (uint32_t i = 0; i < m->count; i++) {
        const spv_inst *in = &m->ins[i];
        if (in->op != SpvOpVariable || in->n < 4 || in->w[2] != storage) continue;
        int64_t b = spv_get_decoration(m, in->w[1], SpvDecorationBuiltIn);
        if (b != SpvBuiltInPosition && b != SpvBuiltInPointSize && b != SpvBuiltInClipDistance && b != SpvBuiltInCullDistance)
            continue;
        if (n == MAXV) return -1;
        int p = spv_find_def(m, in->w[0]);
        if (p < 0 || m->ins[p].op != SpvOpTypePointer) return -1;
        uint32_t pointee = m->ins[p].w[2], member = pointee;
        if (arrayed) {
            int a = spv_find_def(m, pointee);
            if (a < 0 || m->ins[a].op != SpvOpTypeArray) return -1;
            member = m->ins[a].w[1];
        }
        int inv = spv_get_decoration(m, in->w[1], SpvDecorationInvariant) >= 0;
        out[n++] = (loose_var){in->w[1], in->w[0], pointee, member, (uint32_t)b, inv};
    }
    if (arrayed && n > 1) /* one array length for the block array */
        for (int i = 1; i < n; i++) {
            int a = spv_find_def(m, out[i].pointee), a0 = spv_find_def(m, out[0].pointee);
            if (m->ins[a].w[2] != m->ins[a0].w[2]) return -1;
        }
    if (arrayed && n > 0) {
        uint32_t len = spv_array_length(m, out[0].pointee);
        if (len == 0 || len > MAXA) return -1;
    }
    return n;
}

/* Every use of the variables is one the rewrite knows; their other decorations are only BuiltIn
 * and Invariant (which the block member takes). */
static int uses_ok(const spv_mod *m, const loose_var *v, int n, int arrayed)
{
    int in_body = 0;
    for (uint32_t i = 0; i < m->count; i++) {
        const spv_inst *in = &m->ins[i];
        if (in->op == SpvOpFunction) in_body = 1;
        if (in->op == SpvOpDecorate) {
            if (find_var(v, n, in->w[0]) >= 0 && in->w[1] != SpvDecorationBuiltIn && in->w[1] != SpvDecorationInvariant)
                return 0;
            continue;
        }
        if (in->op == SpvOpEntryPoint || is_annotation_or_debug(in->op) || is_type_or_const(in->op)) continue;
        if (in->op == SpvOpVariable && find_var(v, n, in->w[1]) >= 0) continue;
        if (!in_body) {
            /* A global (another variable's initializer, say) naming one of ours. */
            if (in->op == SpvOpVariable && in->n > 4 && find_var(v, n, in->w[3]) >= 0) return 0;
            continue;
        }
        uint32_t lim = id_words(in), words = (uint32_t)in->n - 1;
        if (lim > words) lim = words;
        for (uint32_t k = 0; k < lim; k++) {
            if (find_var(v, n, in->w[k]) < 0) continue;
            if ((in->op == OpAccessChain || in->op == OpInBoundsAccessChain) && k == 2 && (!arrayed || words >= 4)) continue;
            /* whole loads and stores; arrayed ones are split per vertex, memory operands copied */
            if (in->op == SpvOpLoad && k == 2 && (!arrayed || words <= 3 + MAXMEM)) continue;
            if (in->op == SpvOpStore && k == 0 && (!arrayed || words <= 2 + MAXMEM)) continue;
            return 0;
        }
    }
    return 1;
}

/* A whole load or store of an arrayed variable at `at`: one access chain [vertex, member] per
 * vertex, the load's elements gathered by an OpCompositeConstruct that keeps its result id, the
 * store's object split by OpCompositeExtract. Memory operands go to every access. Returns the
 * index of the last instruction it leaves. */
static uint32_t whole_array(spv_mod *m, uint32_t at, uint32_t nv, uint32_t mptr, uint32_t mtype, uint32_t member,
                            const uint32_t *vert, uint32_t len)
{
    int load = m->ins[at].op == SpvOpLoad;
    uint32_t head = load ? 3 : 2, words = (uint32_t)m->ins[at].n - 1;
    /* load [type, result, pointer, memory...]; store [pointer, object, memory...] */
    uint32_t type = m->ins[at].w[0], id = m->ins[at].w[1];
    uint32_t tail[MAXMEM], nt = words - head;
    memcpy(tail, &m->ins[at].w[head], nt * 4);
    uint32_t elem[MAXA], ops[3 + MAXMEM];
    for (uint32_t j = 0; j < len; j++) {
        uint32_t p = spv_new_id(m);
        elem[j] = spv_new_id(m);
        if (!load) {
            uint32_t ex[4] = {mtype, elem[j], id, j};
            spv_insert(m, at++, OpCompositeExtract, ex, 4);
        }
        uint32_t ac[5] = {mptr, p, nv, vert[j], member};
        spv_insert(m, at++, OpAccessChain, ac, 5);
        uint32_t o = 0;
        if (load) ops[o++] = mtype;
        ops[o++] = load ? elem[j] : p;
        ops[o++] = load ? p : elem[j];
        memcpy(&ops[o], tail, nt * 4);
        spv_insert(m, at++, load ? SpvOpLoad : SpvOpStore, ops, (uint16_t)(o + nt));
    }
    if (!load) {
        spv_remove(m, at);
        return at - 1;
    }
    uint32_t cc[2 + MAXA] = {type, id};
    memcpy(&cc[2], elem, len * 4);
    spv_inst *in = &m->ins[at];
    free(in->w);
    in->w = malloc((size_t)(len + 2) * 4);
    if (!in->w) {
        m->err = 1;
        return at;
    }
    memcpy(in->w, cc, (size_t)(len + 2) * 4);
    in->op = OpCompositeConstruct;
    in->n = (uint16_t)(len + 3);
    return at;
}

static void wrap(spv_mod *m, uint32_t storage, int arrayed, const loose_var *v, int n)
{
    /* Types first: the block, its variable type, pointers - all appended to the globals. */
    uint32_t sops[MAXV + 1];
    uint32_t block = spv_new_id(m);
    sops[0] = block;
    for (int i = 0; i < n; i++) sops[1 + i] = v[i].member_type;
    spv_insert(m, spv_types_end(m), SpvOpTypeStruct, sops, (uint16_t)(n + 1));
    uint32_t var_type = block;
    if (arrayed) {
        int a = spv_find_def(m, v[0].pointee);
        var_type = spv_type_array(m, block, m->ins[a].w[2]);
    }
    uint32_t ptr = spv_type_pointer(m, storage, var_type);
    uint32_t member_ptr[MAXV], index[MAXV];
    for (int i = 0; i < n; i++) {
        member_ptr[i] = spv_type_pointer(m, storage, v[i].member_type);
        index[i] = spv_const_u32(m, (uint32_t)i);
    }
    /* Vertex indices for whole-array loads and stores (made now: constants shift the body). */
    uint32_t len = arrayed ? spv_array_length(m, v[0].pointee) : 0, vert[MAXA];
    for (uint32_t j = 0; j < len; j++) vert[j] = spv_const_u32(m, j);
    uint32_t nv = spv_new_id(m), vops[3] = {ptr, nv, storage};
    spv_insert(m, spv_types_end(m), SpvOpVariable, vops, 3);

    /* Decorations: members carry the built-ins, the struct is a Block. */
    for (uint32_t i = 0; i < m->count;) {
        spv_inst *in = &m->ins[i];
        if ((in->op == SpvOpDecorate || in->op == SpvOpName) && find_var(v, n, in->w[0]) >= 0) spv_remove(m, i);
        else i++;
    }
    uint32_t at = spv_annotations_end(m);
    for (int i = 0; i < n; i++) {
        uint32_t d[4] = {block, (uint32_t)i, SpvDecorationBuiltIn, v[i].builtin};
        spv_insert(m, at++, SpvOpMemberDecorate, d, 4);
        if (!v[i].invariant) continue;
        d[2] = SpvDecorationInvariant;
        spv_insert(m, at++, SpvOpMemberDecorate, d, 3);
    }
    uint32_t bd[2] = {block, SpvDecorationBlock};
    spv_insert(m, at, SpvOpDecorate, bd, 2);

    /* Entry points: the old variables out, the block in (once). */
    for (uint32_t i = 0; i < m->count; i++) {
        spv_inst *in = &m->ins[i];
        if (in->op != SpvOpEntryPoint) continue;
        /* operands: model, id, name (literal string words), interface ids */
        uint32_t k = 2;
        while (k < (uint32_t)in->n - 1) {
            uint32_t word = in->w[k++];
            if (!(word & 0xff000000u) || !(word & 0x00ff0000u) || !(word & 0x0000ff00u) || !(word & 0xffu)) break;
        }
        uint32_t o = k;
        int had = 0;
        for (; k < (uint32_t)in->n - 1; k++) {
            if (find_var(v, n, in->w[k]) >= 0) {
                had = 1;
                continue;
            }
            in->w[o++] = in->w[k];
        }
        if (had) in->w[o++] = nv;
        in->n = (uint16_t)(o + 1);
    }

    /* The old variables go. */
    for (uint32_t i = 0; i < m->count;) {
        if (m->ins[i].op == SpvOpVariable && find_var(v, n, m->ins[i].w[1]) >= 0) spv_remove(m, i);
        else i++;
    }

    /* Uses. */
    int in_body = 0;
    for (uint32_t i = 0; i < m->count; i++) {
        spv_inst *in = &m->ins[i];
        if (in->op == SpvOpFunction) in_body = 1;
        if (!in_body) continue;
        int k;
        if ((in->op == OpAccessChain || in->op == OpInBoundsAccessChain) && (k = find_var(v, n, in->w[2])) >= 0) {
            /* [type, result, base, idx...] -> base = block; member index after the array index */
            uint32_t words = (uint32_t)in->n - 1;
            uint32_t *nw = malloc((size_t)(words + 1) * 4);
            if (!nw) {
                m->err = 1;
                return;
            }
            uint32_t pos = 0;
            nw[pos++] = in->w[0];
            nw[pos++] = in->w[1];
            nw[pos++] = nv;
            uint32_t src = 3;
            if (arrayed) nw[pos++] = in->w[src++];
            nw[pos++] = index[k];
            while (src < words) nw[pos++] = in->w[src++];
            free(in->w);
            in->w = nw;
            in->n = (uint16_t)(pos + 1);
            continue;
        }
        uint32_t slot = in->op == SpvOpLoad ? 2 : in->op == SpvOpStore ? 0 : UINT32_MAX;
        if (slot == UINT32_MAX || (k = find_var(v, n, in->w[slot])) < 0) continue;
        if (arrayed) {
            i = whole_array(m, i, nv, member_ptr[k], v[k].member_type, index[k], vert, len);
            if (m->err) return;
            continue;
        }
        uint32_t p = spv_new_id(m), ac[4] = {member_ptr[k], p, nv, index[k]};
        spv_insert(m, i, OpAccessChain, ac, 4);
        in = &m->ins[++i];
        in->w[slot] = p;
    }
}

int vkb_spv_wrap_pervertex(const uint32_t *code, size_t size, uint32_t **out, size_t *out_size, const char **why)
{
    *why = NULL;
    spv_mod m;
    if (spv_parse(&m, code, size) != 0) return 0;
    int model = spv_execution_model(&m);
    if (model != SpvExecutionModelTessellationControl && model != SpvExecutionModelTessellationEvaluation &&
        model != SpvExecutionModelGeometry) {
        spv_free(&m);
        return 0;
    }
    /* One entry point only: the interfaces belong to it. */
    if (spv_find_op(&m, SpvOpEntryPoint, (uint32_t)spv_find_op(&m, SpvOpEntryPoint, 0) + 1) >= 0) {
        *why = "more than one entry point";
        spv_free(&m);
        return 0;
    }
    int changed = 0;
    const uint32_t storages[2] = {SpvStorageClassInput, SpvStorageClassOutput};
    for (int s = 0; s < 2; s++) {
        int arrayed = storages[s] == SpvStorageClassInput || model == SpvExecutionModelTessellationControl;
        loose_var v[MAXV];
        int n = collect(&m, storages[s], arrayed, v);
        if (n == 0) continue;
        if (n < 0 || !uses_ok(&m, v, n, arrayed)) {
            *why = n < 0 ? "built-in variables of an unexpected shape" : "a use of a built-in the rewrite does not handle";
            continue;
        }
        wrap(&m, storages[s], arrayed, v, n);
        changed = 1;
    }
    if (!changed || m.err) {
        spv_free(&m);
        return 0;
    }
    *out_size = spv_write(&m, out);
    spv_free(&m);
    return *out_size != 0;
}

#ifdef VKB_PERVERTEX_MAIN
/* Host check: spirv_pervertex in.spv out.spv */
#include <stdio.h>
int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    uint32_t *code = malloc((size_t)sz);
    if (fread(code, 1, (size_t)sz, f) != (size_t)sz) return 2;
    fclose(f);
    uint32_t *out;
    size_t osz;
    const char *why;
    int r = vkb_spv_wrap_pervertex(code, (size_t)sz, &out, &osz, &why);
    printf("%s: %s%s%s\n", argv[1], r ? "rewritten" : "unchanged", why ? " - " : "", why ? why : "");
    if (!r) return 1;
    FILE *o = fopen(argv[2], "wb");
    fwrite(out, 1, osz, o);
    fclose(o);
    return 0;
}
#endif
