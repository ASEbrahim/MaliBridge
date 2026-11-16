/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Symbolizable crash frames (ledger row 340, a player log: the server jumped to address 0
 * right after the self-test on a Note 8, and the only frames were logcat's "#01 pc 0x3b912c" of whichever build the
 * player had, with no build id in the upload). The crash handler (server_main.c on_crash) now writes, after its own
 * account, one line per frame in the tombstone's form, so ndk-stack / llvm-symbolizer / addr2line read them against
 * the unstripped build with that id:
 *
 *   vkbridge-server E crash-frame #00 pc 0x00000000003b912c libvkbridge_jni.so (BuildId: 5c1e...) via=unwind
 *
 * pc = the address in the module's own (link-time) address space, the module by file name only (no path: the
 * scrubber's rule, no user directory). Frames come from the unwinder (_Unwind_Backtrace through the signal frame),
 * from the faulting pc on. When the unwinder cannot pass the faulting frame (a jump to 0 or to unmapped memory: no
 * unwind info there) the frames are the pc, the return address (AArch64 lr, x86-64 the word at sp) and on AArch64
 * the frame-pointer chain from x29 (Android builds keep frame pointers), every word checked readable first.
 * Not async-signal-safe (dl_iterate_phdr, the unwinder); the process is dying, as with the dladdr before it.
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <elf.h>
#include <link.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>

#define CS_FRAMES 32
#define CS_MODS 12

typedef struct {
    uintptr_t want, bias;
    const char *name;
    const ElfW(Phdr) * phdr;
    int nphdr, found;
} cs_find;

static int find_cb(struct dl_phdr_info *info, size_t size, void *arg)
{
    (void)size;
    cs_find *f = arg;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD) continue;
        uintptr_t lo = info->dlpi_addr + ph->p_vaddr;
        if (f->want >= lo && f->want < lo + ph->p_memsz) {
            f->bias = info->dlpi_addr;
            f->name = info->dlpi_name;
            f->phdr = info->dlpi_phdr;
            f->nphdr = info->dlpi_phnum;
            f->found = 1;
            return 1;
        }
    }
    return 0;
}

/* the module's GNU build id as hex into out (empty: none) */
static void build_id(const cs_find *f, char *out, size_t cap)
{
    out[0] = 0;
    for (int i = 0; i < f->nphdr; i++) {
        const ElfW(Phdr) *ph = &f->phdr[i];
        if (ph->p_type != PT_NOTE) continue;
        const uint8_t *p = (const uint8_t *)(f->bias + ph->p_vaddr), *end = p + ph->p_memsz;
        while (p + sizeof(ElfW(Nhdr)) <= end) {
            const ElfW(Nhdr) *nh = (const ElfW(Nhdr) *)p;
            const uint8_t *name = p + sizeof(*nh), *desc = name + ((nh->n_namesz + 3) & ~3u);
            if (desc + nh->n_descsz > end) break;
            if (nh->n_type == NT_GNU_BUILD_ID && nh->n_namesz == 4 && !memcmp(name, "GNU", 4)) {
                size_t o = 0;
                for (uint32_t k = 0; k < nh->n_descsz && o + 3 < cap; k++) o += (size_t)snprintf(out + o, cap - o, "%02x", desc[k]);
                return;
            }
            p = desc + ((nh->n_descsz + 3) & ~3u);
        }
    }
}

typedef struct {
    uintptr_t ip[CS_FRAMES];
    int n;
} cs_trace;

static _Unwind_Reason_Code trace_cb(struct _Unwind_Context *ctx, void *arg)
{
    cs_trace *t = arg;
    if (t->n >= CS_FRAMES) return _URC_END_OF_STACK;
    t->ip[t->n++] = (uintptr_t)_Unwind_GetIP(ctx);
    return _URC_NO_REASON;
}

/* 1 when the 16 bytes at a can be read (a write of them into a pipe fails with EFAULT otherwise) */
static int readable(int pfd[2], uintptr_t a)
{
    if (!a || (a & 7)) return 0;
    ssize_t k = write(pfd[1], (const void *)a, 16);
    if (k <= 0) return 0;
    char sink[16];
    if (read(pfd[0], sink, (size_t)k) < 0) {}
    return 1;
}

static int frame_line(char *out, size_t cap, int no, uintptr_t addr, const char *via, int is_ret)
{
    /* a return address points after its call: the call is the instruction before (as tombstones print it, the
     * address itself; tools subtract one). The address is printed as is. */
    (void)is_ret;
    cs_find f;
    memset(&f, 0, sizeof(f));
    f.want = addr;
    if (addr) dl_iterate_phdr(find_cb, &f);
    if (!f.found)
        return snprintf(out, cap, "vkbridge-server E crash-frame #%02d pc 0x%016lx (no module) via=%s\n", no, (unsigned long)addr, via);
    const char *base = f.name && *f.name ? strrchr(f.name, '/') : NULL;
    char exe[256] = "";
    if (!f.name || !*f.name) { /* the main program */
        ssize_t k = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        exe[k > 0 ? k : 0] = 0;
        base = strrchr(exe, '/');
    }
    const char *mod = base ? base + 1 : f.name && *f.name ? f.name : exe[0] ? exe : "?";
    char id[80];
    build_id(&f, id, sizeof(id));
    return snprintf(out, cap, "vkbridge-server E crash-frame #%02d pc 0x%016lx %s (BuildId: %s) via=%s\n", no, (unsigned long)(addr - f.bias), mod,
                    id[0] ? id : "none", via);
}

int vkb_crash_frames(char *out, size_t cap, void *ucv)
{
    uintptr_t pc = 0, ret = 0, fp = 0, sp = 0;
#if defined(__aarch64__)
    if (ucv) {
        const ucontext_t *uc = ucv;
        pc = (uintptr_t)uc->uc_mcontext.pc;
        ret = (uintptr_t)uc->uc_mcontext.regs[30];
        fp = (uintptr_t)uc->uc_mcontext.regs[29];
        sp = (uintptr_t)uc->uc_mcontext.sp;
    }
#elif defined(__x86_64__) && defined(REG_RIP)
    if (ucv) {
        const ucontext_t *uc = ucv;
        pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
        sp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
        fp = (uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
    }
#endif
    int pfd[2] = {-1, -1};
    if (pipe(pfd) < 0) pfd[0] = pfd[1] = -1;
    int n = 0, no = 0;
    cs_trace t;
    t.n = 0;
    _Unwind_Backtrace(trace_cb, &t);
    int at = -1;
    for (int i = 0; i < t.n && pc; i++)
        if (t.ip[i] == pc || t.ip[i] == pc + 1) { /* +1: an unwinder that marks a signal frame's ip */
            at = i;
            break;
        }
    if (at >= 0 && t.n - at >= 2) {
        for (int i = at; i < t.n && n < (int)cap - 200; i++) n += frame_line(out + n, cap - (size_t)n, no++, t.ip[i], "unwind", i > at);
    } else {
        /* the unwinder stopped at the faulting frame (pc 0, unmapped, no unwind info): pc, the return address, fp chain */
        n += frame_line(out + n, cap - (size_t)n, no++, pc, "pc", 0);
#if defined(__x86_64__)
        if (pfd[1] >= 0 && readable(pfd, sp)) ret = *(const uintptr_t *)sp; /* a call through a bad pointer pushed it */
#endif
        if (ret && n < (int)cap - 200) n += frame_line(out + n, cap - (size_t)n, no++, ret, "lr", 1);
#if defined(__aarch64__)
        for (int k = 0; k < CS_FRAMES - 2 && fp && pfd[1] >= 0 && readable(pfd, fp) && n < (int)cap - 200; k++) {
            uintptr_t next = ((const uintptr_t *)fp)[0], ra = ((const uintptr_t *)fp)[1];
            if (!ra) break;
            n += frame_line(out + n, cap - (size_t)n, no++, ra, "fp", 1);
            if (next <= fp || next - fp > (8u << 20)) break;
            fp = next;
        }
#else
        (void)fp;
#endif
        (void)sp;
    }
    if (pfd[0] >= 0) close(pfd[0]), close(pfd[1]);
    if (n > (int)cap - 1) n = (int)cap - 1;
    return n;
}
