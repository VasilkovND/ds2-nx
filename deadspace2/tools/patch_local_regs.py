#!/usr/bin/env python3
"""Guest registers in host locals inside each recompiled function.

Every guest register of the generated code is a field of the context (ctx.r3.u64, ctx.f1.f64, ctx.cr6.eq ...), and
every guest memory access is a volatile or memcpy access through `base`. GCC cannot tell the two apart (a byte store
may alias anything, and the build has -fno-strict-aliasing), so after each guest store it reloads every register it
needs from ctx and writes each register back as soon as it changes. This pass gives every function its own copies of
the registers it uses (L_r3, L_f1, L_cr6 ...), which nothing can alias, and keeps ctx exact only where other code
can look at it: at calls and returns.

Which registers have to go through ctx at a call is worked out for the whole program, from the code itself (not from
the PowerPC calling convention, which the game's compiler does not always follow between its own functions):

  REF(F)   the registers F (or anything it calls) may read before writing them: they hold F's inputs.
  MOD(F)   the registers whose value in ctx may differ when F returns.
  PRES(F)  the registers F saves on its stack at entry and loads back right before every return (the non-volatiles
           of its prologue and epilogue, inline or through the __savegprlr_N/__savefpr_N helpers): F's own writes to
           them are not seen by its caller, and their save does not count as a read.

Both are fixpoints over the call graph. At a call to G the caller writes back the registers that changed since ctx
was last exact ("dirty", a dataflow over labels and gotos) and that G may read or change (REF(G) | MOD(G)), and reads
back MOD(G) after it. At a return F writes back the dirty registers its caller may read back (MOD(F)) and the saved
ones whose ctx copy it changed on the way. Calls the code cannot see into are taken at the calling convention's word:
indirect calls (bctrl), the kernel's imports and the hooked functions (native code) read and change every volatile
register, and preserve the non-volatiles, r1 and r13 (the guest compiler cannot use its own conventions for a function
whose address is taken). A jump through a table to a target outside it (the switch fallback) and a call followed by a
return (a tail call, possibly into the rest of the same function) write everything back.

The register save/restore helpers (__savegprlr_N, __restgprlr_N, __savefpr_N, __restfpr_N, __savevmx_N, __restvmx_N:
a fifth of all calls) are copied into each function first, so they work on the copies too. A function with anything the
pass does not understand (another use of ctx) is left as it is; its REF and MOD still come from its text.

Dead Space 2 (ported from gears2-nx's gow2/tools/patch_local_regs.py, see its CPU_OPTIMIZATION.md): the recompiled
code is Dead-Space-2-Recomp's checked-in src_tu2/, one .cpp per original source file (PDB names, not sub_X), compiled
through _units/. The pass reads those files and writes the rewritten ones into the build's mirror of the tree
(tools/prepare_tree.py), in place of their symlinks; files whose text did not change are not touched, so Ninja only
rebuilds what changed. Every function defined in the tree uses its computed summary; the hooked ones (a REX_EXTERN
with a body in the sources) also take the calling convention's word, and every defined name the sources mention gets
LIVEOUT = ALL (native code may read anything after calling it). REX_FUNC_PROLOGUE has no stack guard here.

Usage: patch_local_regs.py <src_tu2> <mirror> <source dir>...
(the source dirs: where the hooks and the native callers of guest functions are)
PLR_CHECK=1: the check build (calls verify the analysis at run time, ds2_regcheck_fail in src/ds2_cpu.cpp); the site
numbers go to <mirror>/patch_local_regs.check.map.
"""

import os
import re
import sys
from concurrent.futures import ProcessPoolExecutor

# ---------------------------------------------------------------------------------------------------------------------
# Registers: one bit each.
NAMES = ([f"r{i}" for i in range(32)] + [f"f{i}" for i in range(32)] + [f"v{i}" for i in range(128)] +
         [f"cr{i}" for i in range(8)] + ["xer", "ctr", "lr", "reserved"])
BIT = {n: i for i, n in enumerate(NAMES)}
ALL = (1 << len(NAMES)) - 1


def mask(names):
    m = 0
    for n in names:
        m |= 1 << BIT[n]
    return m


def nonvolatile(reg):
    if reg.startswith("cr"):
        return reg in ("cr2", "cr3", "cr4")
    kind, num = reg[0], reg[1:]
    if kind in "rf" and num.isdigit():
        return int(num) >= 14
    if kind == "v" and num.isdigit():
        n = int(num)
        return 14 <= n <= 31 or n >= 64
    return False


NONVOL = mask(n for n in NAMES if nonvolatile(n))
# The calling convention: a callee reads and changes any volatile register and preserves the rest (r1 and r13 too).
# What such a callee may read: its arguments (r3-r10, f1-f13, v1-v13), the stack and thread pointers, the return
# address, and XER (every compare copies its summary-overflow bit). What it may change: every volatile register.
ARGS = mask([f"r{i}" for i in range(3, 11)] + [f"f{i}" for i in range(1, 14)] + [f"v{i}" for i in range(1, 14)])
ABI_REF = ARGS | mask(["r1", "r2", "r13", "lr", "xer"])
ABI_MOD = ALL & ~NONVOL & ~mask(["r1", "r13"])
TRAP_REF = mask(["r3", "r4"])
CHECK = os.environ.get("PLR_CHECK") == "1"
# Imports that look at the whole register state (a context for a crash report or an exception): this function's
# registers are all written back before them (the callers' non-volatiles may be stale there).
CTX_IMPORTS = {"RtlCaptureContext", "RtlRaiseException", "RtlUnwind", "KeBugCheck", "KeBugCheckEx"}

REG = r"(?:r\d+|f\d+|v\d+|cr\d|xer|ctr|lr|reserved)"
OCC = re.compile(r"\bctx\.(" + REG + r")\b")
CALL = re.compile(r"\b([A-Za-z_]\w*)\(ctx, base\);")
TRAP = re.compile(r"\bppc_trap\(ctx, base, \d+\);")
INDIRECT = "REX_CALL_INDIRECT_FUNC("
GOTO = re.compile(r"\bgoto (loc_[0-9A-F]+);")
LABEL = re.compile(r"^(loc_[0-9A-F]+):\s*$")
OTHER_CTX = re.compile(r"\bctx\b")
# The codegen's setjmp: the context is copied before it and copied back when longjmp returns to it.
SETJMP_LINES = {"env = ctx;", "temp.s64 = ppc_setjmp(ctx.r3.u32);", "if (temp.s64 != 0) ctx = env;"}
KEEP = re.compile(r"\bctx\.(?:fpscr|msr)\b")  # stays in ctx: FPSCR and MSR emulation
# One assignment to a register, with no other assignment on the line (a ?: on its right writes nothing itself).
PLAIN_ASSIGN = re.compile(r"^ctx\.(" + REG + r")(?:\.\w+)?(?:\[\d+\])? = (.*);$")


def plain_assign(code):
    m = PLAIN_ASSIGN.match(code.strip())
    return m is not None and ";" not in m.group(2) and "=" not in re.sub(r"==|!=|<=|>=", "", m.group(2))


# subfe rD,rA,rA: ~rA + rA (+ CA) is all ones (+ CA) whatever rA holds; both temp (carry) and rD lines.
SUBFE_SAME = re.compile(r"~ctx\.(r\d+)\.(u\d+) \+ ctx\.\1\.\2\b")
CR_FIELD = re.compile(r"^ctx\.(cr\d)\.(lt|gt|eq|so) = (.*);$")
COMPOUND = re.compile(r"^(?:\+|-|\*|/|%|&|\||\^|<<|>>)=")
HELPER = r"__(?:save|rest)(?:gpr|gprlr|fpr|vmx)_\d+"
HELPER_DEF = re.compile(r"^DEFINE_REX_FUNC\((" + HELPER + r")\) \{\n(.*?)^\}\n", re.M | re.S)
HELPER_CALL = re.compile(r"^\t(" + HELPER + r")\(ctx, base\);\n", re.M)
# The hooks: REX_EXTERN(name) with a body (src/ds2_audio.cpp also makes them through a macro, REX_EXTERN(prefix##arg)).
HOOK = re.compile(r"\bREX_EXTERN\(\s*(\w+)\s*\)\s*\{")
HOOK_MACRO = re.compile(r"#define (\w+)\((\w+)[^)]*\)[^\n]*(?:\\\n[^\n]*)*?REX_EXTERN\((\w+)##(\w+)\)\s*\{")

# Statements the frame analysis understands (on the code with ctx. names).
R_ADDI = re.compile(r"^ctx\.(r\d+)\.s64 = ctx\.(r\d+)\.s64 \+ (-?\d+);$")
R_MR = re.compile(r"^ctx\.(r\d+)\.u64 = ctx\.(r\d+)\.u64;$")
R_LI = re.compile(r"^ctx\.(r\d+)\.s64 = (-?\d+);$")
EA_D = re.compile(r"^ea = (-?\d+) \+ ctx\.(r\d+)\.u32;$")
EA_X = re.compile(r"^ea = ctx\.(r\d+)\.u32 \+ ctx\.(r\d+)\.u32;$")
STWU = re.compile(r"^REX_STORE_U32\(ea, ctx\.(r\d+)\.u32\);$")
R1_EA = re.compile(r"^ctx\.r1\.u32 = ea;$")
BACKCHAIN = re.compile(r"^ctx\.r1\.u64 = REX_S?LOAD_U32\(ctx\.r1\.u32 \+ 0\);$")
SAVE = re.compile(r"^REX_S?STORE_U64\(ctx\.(r\d+)\.u32 \+ (-?\d+), ctx\.((?:r|f)\d+)\.u64\);$")
RESTORE = re.compile(r"^ctx\.((?:r|f)\d+)\.u64 = REX_S?LOAD_U64\(ctx\.(r\d+)\.u32 \+ (-?\d+)\);$")


def code_part(line):
    i = line.find("//")
    return line if i < 0 else line[:i]


def access(code, m):
    """(writes, full) for the occurrence m: whether it writes the register and whether it replaces all of it."""
    reg = m.group(1)
    rest = code[m.end():]
    f = re.match(r"(\.(\w+))?(\[[^\]]*\])?\s*", rest)
    field, index = f.group(2), f.group(3)
    after = rest[f.end():]
    if field in ("compare", "setFromMask"):
        return True, True
    if field == "set_raw":
        return True, True
    if after.startswith("=") and not after.startswith("=="):
        if reg == "reserved":
            return True, True  # lwarx: only its stwcx. reads it
        if index:
            return True, False
        if field is None:
            return True, True
        if reg[0] in "rf" or reg in ("ctr", "reserved"):
            return True, field in ("u64", "s64", "f64")
        return True, False  # a field of a CR, XER, vector
    if COMPOUND.match(after) or after.startswith("++") or after.startswith("--"):
        return True, False
    before = code[:m.start()]
    if re.search(r"store\w*\(\s*(?:\([^()]*\*\)\s*)?&?$", before):
        # A 128-bit store into a vector register replaces it; anything else is partial.
        full = reg[0] == "v" and re.search(r"simde_mm_store_(?:si128|ps)\(\s*(?:\([^()]*\*\)\s*)?$", before) is not None
        return True, full
    if re.search(r"load\w*\(\s*(?:\([^()]*\*\)\s*)?&?$", before):
        return False, False
    if before.endswith("&") or re.search(r"\*\)\s*$", before):
        return True, False
    return False, False


def find_indirect(code):
    spans = []
    i = code.find(INDIRECT)
    while i >= 0:
        j = i + len(INDIRECT)
        depth = 1
        while depth and j < len(code):
            if code[j] == "(":
                depth += 1
            elif code[j] == ")":
                depth -= 1
            j += 1
        if code[j:j + 1] == ";":
            j += 1
        spans.append((i, j))
        i = code.find(INDIRECT, j)
    return spans


class Skip(Exception):
    pass


# ---------------------------------------------------------------------------------------------------------------------
# Parsing one function.

class Func:
    """What the analyses need from one function body."""

    def __init__(self, name, body):
        self.name = name
        self.body = body
        self.start = 0
        while self.start < len(body) and body[self.start].strip().startswith("REX_FUNC_PROLOGUE"):
            self.start += 1
        self.ok = True  # can be rewritten (no unknown ctx use)
        self.setjmp = False  # uses setjmp: left as it is, but analysed
        self.unknown = False  # another use of ctx: left as it is, REF = MOD = ALL
        self.reason = ""
        self.mentioned = 0
        n = len(body)
        self.n = n
        self.use = [0] * n      # read (a partial write reads the rest)
        self.kill = [0] * n     # replaced
        self.wr = [0] * n       # written
        self.calls = [None] * n  # list of (kind, target) on the line; kind: 'd' direct, 'i' indirect, 's' switch
        #                          fallback, 't' trap
        self.plain = [False] * n
        self.tail = [False] * n
        self.returns = [False] * n
        self.succ = [None] * n
        self.depth = [1] * n
        labels = {}
        depth = 1
        gotos = []
        # For each open brace: whether what is inside may not run (opened by an if/else/switch line).
        cond_stack = []
        skips = []  # (line opening a conditional block, line closing it): the path that does not run the block
        # A CR field written field by field (stwcx., mtcrf: lt, gt, eq, so on four lines) is replaced in full.
        cr_full = set()
        for i, line in enumerate(body):
            m = CR_FIELD.match(code_part(line).strip())
            if not m or m.group(2) != "lt" or "ctx." + m.group(1) + "." in m.group(3):
                continue
            fields, j = [], i + 1
            while j < len(body) and len(fields) < 3:
                s = code_part(body[j]).strip()
                j += 1
                if not s:
                    continue
                k = CR_FIELD.match(s)
                if not k or k.group(1) != m.group(1) or "ctx." + m.group(1) + "." in k.group(3):
                    break
                fields.append(k.group(2))
            if sorted(fields) == ["eq", "gt", "so"]:
                cr_full.add(i)
        for i, line in enumerate(body):
            lm = LABEL.match(line)
            if lm:
                labels[lm.group(1)] = i
            code = code_part(line)
            self.depth[i] = depth
            if i >= self.start and code.strip():
                use = kill = wr = 0
                # subfe rD,rA,rA (ca - 1, the compiler's idiom) does not depend on rA: not a read of it.
                same = {m.group(1) for m in SUBFE_SAME.finditer(code)}
                if same:
                    rest = re.sub(r"< ~ctx\.(r\d+)\.u\d+\)", ")", SUBFE_SAME.sub("", code))
                    same = {r for r in same if not re.search(r"\bctx\." + r + r"\b", rest)}
                for m in OCC.finditer(code):
                    b = 1 << BIT[m.group(1)]
                    self.mentioned |= b
                    w, full = access(code, m)
                    if w:
                        wr |= b
                        if full:
                            kill |= b
                        else:
                            use |= b
                    elif m.group(1) not in same:
                        use |= b
                if i in cr_full:
                    b = 1 << BIT[CR_FIELD.match(code.strip()).group(1)]
                    use &= ~b
                    kill |= b
                # x = x + 1: the read comes first. A write that may not happen (in an if, in a conditional block)
                # replaces nothing for certain. A ?: is conditional only when it is not the right-hand side of one
                # plain assignment (ctx.f11.f64 = c ? a : b, the codegen's fsel, clz, divw ...: always written).
                if any(c for c, _ in cond_stack) or re.search(r"\b(?:if|else|switch|case|default)\b", code) or (
                        "?" in code and not plain_assign(code)):
                    kill = 0
                self.use[i], self.kill[i], self.wr[i] = use, kill, wr
                stripped = OCC.sub("", code)
                stripped = KEEP.sub("", stripped)
                stripped = CALL.sub("", stripped)
                stripped = TRAP.sub("", stripped)
                stripped = stripped.replace(INDIRECT, "")
                if code.strip() in SETJMP_LINES:
                    # setjmp (Lua's protected calls, libpng/JPEG errors): the function stays as it is, with ctx
                    # always exact, but its text still gives its REF, MOD and what it reads after its calls.
                    self.ok = False
                    self.setjmp = True
                    self.reason = "setjmp"
                elif OTHER_CTX.search(stripped) or "ppc_setjmp" in code or "SEH_" in code:
                    self.ok = False
                    self.unknown = True
                    self.reason = "ctx use: " + line.strip()[:60]
                calls = [("i", None) for _ in find_indirect(code)]
                for m in CALL.finditer(code):
                    calls.append(("d", m.group(1)))
                if TRAP.search(code):
                    calls.append(("t", None))
                if calls:
                    self.calls[i] = calls
                    s = code.strip()
                    self.plain[i] = len(calls) == 1 and calls[0][0] != "t" and (
                        bool(re.fullmatch(r"[A-Za-z_]\w*\(ctx, base\);", s)) or
                        (s.startswith(INDIRECT) and find_indirect(s) == [(0, len(s))]))
                self.returns[i] = "return;" in code
                gotos.append((i, GOTO.findall(code)))
            conditional = re.search(r"\b(?:if|else|switch|case|default|for|while|do)\b", code) is not None
            for ch in code:
                if ch == "{":
                    cond_stack.append((conditional, i))
                elif ch == "}" and cond_stack:
                    c, opened = cond_stack.pop()
                    if c and opened != i:
                        skips.append((opened, i))
            depth += code.count("{") - code.count("}")
        if depth != 1:
            raise Skip("unbalanced braces")
        # Switch fallback: REX_CALL_INDIRECT_FUNC inside a switch's default.
        for i in range(n):
            if self.calls[i] and any(k == "i" for k, _ in self.calls[i]) and self.depth[i] > 1:
                j = i - 1
                while j >= 0 and not code_part(body[j]).strip():
                    j -= 1
                if j >= 0 and code_part(body[j]).strip() == "default:":
                    self.calls[i] = [("s", None) if k == "i" else (k, t) for k, t in self.calls[i]]
        # Tail calls: a call statement followed by return.
        for i in range(n):
            if self.calls[i] and self.plain[i]:
                for j in range(i + 1, n):
                    s = code_part(body[j]).strip()
                    if s:
                        self.tail[i] = s == "return;"
                        break
                else:
                    self.tail[i] = True
        # Control flow: lines fall through unless they are an unconditional goto/return at the top level. A conditional
        # block may also be skipped: an edge from its opening line to its closing one (found in the Dante's Inferno
        # port: `if (c) { X(ctx, base); return; }` clears the dirty registers at the call, and without the edge the
        # code after the block looked clean on the path where c is false). A goto/return inside a block does not end
        # the path through it (the block's fall-through stays, as in gears2-nx).
        gmap = dict(gotos)
        skipmap = {}
        for a, b in skips:
            skipmap.setdefault(a, []).append(b)
        for i in range(n):
            s = code_part(body[i]).strip()
            term = self.depth[i] == 1 and (s == "return;" or (s.startswith("goto ") and s.endswith(";") and
                                                               s.count(";") == 1))
            succ = [] if term else [i + 1]
            succ.extend(b for b in skipmap.get(i, ()) if b not in succ)
            for g in gmap.get(i, ()):
                if g not in labels:
                    raise Skip("goto to unknown label " + g)
                succ.append(labels[g])
            self.succ[i] = succ
        self.used = self.mentioned  # every register named gets a local (subfe rD,rA,rA names rA without reading it)
        self.written = 0
        for i in range(n):
            self.used |= self.use[i] | self.wr[i]
            self.written |= self.wr[i]
        self.reach = self._reach()
        self.pres, self.save_lines, self.restore_lines = self._preserved()

    def _reach(self):
        reach = [False] * (self.n + 1)
        work = [0]
        reach[0] = True
        while work:
            i = work.pop()
            if i >= self.n:
                continue
            for s in self.succ[i]:
                if not reach[s]:
                    reach[s] = True
                    work.append(s)
        return reach

    def _preserved(self):
        """PRES: registers saved at entry (straight-line prefix) into a frame slot and loaded back from it right
        before every return (no write and no call in between, but a tail call). Returns (mask, {line: reg})."""
        body, n = self.body, self.n
        # Symbolic values: register -> offset from the entry r1, for the registers that hold one; plus 'ea' and the
        # back chain (the r1 the last stwu/stwux saved).
        def step(state, code):
            s = code.strip()
            if not s:
                return state
            m = R_ADDI.match(s)
            if m:
                d, a, k = m.groups()
                st = dict(state)
                if a in st:
                    st[d] = st[a] + int(k)
                else:
                    st.pop(d, None)
                return st
            m = R_MR.match(s)
            if m:
                d, a = m.groups()
                st = dict(state)
                if a in st:
                    st[d] = st[a]
                else:
                    st.pop(d, None)
                return st
            m = EA_D.match(s)
            if m:
                k, a = m.groups()
                st = dict(state)
                if a in st:
                    st["ea"] = st[a] + int(k)
                else:
                    st.pop("ea", None)
                return st
            m = EA_X.match(s)
            if m:
                st = dict(state)
                st.pop("ea", None)
                return st
            m = STWU.match(s)
            if m:
                st = dict(state)
                if m.group(1) in st:
                    st["bc"] = st[m.group(1)]
                else:
                    st.pop("bc", None)
                return st
            if R1_EA.match(s):
                st = dict(state)
                if "ea" in st:
                    st["r1"] = st["ea"]
                else:
                    st.pop("r1", None)
                return st
            if BACKCHAIN.match(s):
                st = dict(state)
                if "bc" in st:
                    st["r1"] = st["bc"]
                else:
                    st.pop("r1", None)
                st.pop("bc", None)
                return st
            # Anything else that writes a tracked register forgets it.
            st = state
            for m in OCC.finditer(s):
                r = m.group(1)
                if r in state and access(s, m)[0]:
                    if st is state:
                        st = dict(state)
                    st.pop(r, None)
            if "ea =" in s and "ea" in st:
                if st is state:
                    st = dict(state)
                st.pop("ea", None)
            return st

        # Forward must-analysis of the symbolic values (intersection at joins).
        TOP = None
        states = [TOP] * (n + 1)
        states[self.start] = {"r1": 0}
        order = list(range(self.start, n))
        changed = True
        out = [TOP] * n
        while changed:
            changed = False
            for i in order:
                if states[i] is TOP:
                    continue
                o = step(states[i], code_part(body[i]))
                if self.calls[i]:
                    # A call leaves r1 alone (MOD never has r1, see ABI_MOD); everything volatile is forgotten.
                    o = {k: v for k, v in o.items() if k in ("r1", "bc") or (k.startswith("r") and k != "ea" and
                                                                              nonvolatile(k))}
                if o != out[i]:
                    out[i] = o
                    for s in self.succ[i]:
                        cur = states[s]
                        new = dict(o) if cur is TOP else {k: v for k, v in cur.items() if o.get(k) == v}
                        if new != cur:
                            states[s] = new
                            changed = True
        # Saves in the straight-line prefix (before any label, branch or call).
        saves = {}       # slot -> reg
        save_lines = {}  # line -> reg
        written = 0
        for i in range(self.start, n):
            line = body[i]
            code = code_part(line).strip()
            if LABEL.match(line) or self.calls[i] or GOTO.search(code) or "return;" in code or "if (" in code or \
                    "switch" in code:
                break
            m = SAVE.match(code)
            if m and states[i] is not TOP:
                base_reg, k, reg = m.groups()
                if base_reg in states[i] and not (written >> BIT[reg] & 1) and nonvolatile(reg):
                    slot = states[i][base_reg] + int(k)
                    if slot in saves:
                        saves[slot] = None
                    else:
                        saves[slot] = reg
                        save_lines[i] = reg
            written |= self.wr[i]
        cand = {r for r in saves.values() if r}
        if not cand:
            return 0, {}, {}
        # Any other store into a save slot through a known frame address spoils it.
        for i in range(self.start, n):
            if i in save_lines or states[i] is TOP:
                continue
            code = code_part(body[i]).strip()
            m = re.match(r"^REX_S?STORE_U(\d+)\((?:ctx\.(r\d+)\.u32 \+ (-?\d+)|ea),", code)
            if m:
                size = int(m.group(1)) // 8
                if m.group(2):
                    if m.group(2) not in states[i]:
                        continue
                    addr = states[i][m.group(2)] + int(m.group(3))
                elif "ea" in states[i]:
                    addr = states[i]["ea"]
                else:
                    continue
                for slot, reg in list(saves.items()):
                    if reg and addr < slot + 8 and slot < addr + size:
                        cand.discard(reg)
        # Must-analysis: the registers restored from their slot with nothing after (but a tail call).
        cmask = mask(cand)
        rest_in = [None] * (n + 1)
        rest_in[self.start] = 0
        work = [self.start]
        ret_ok = cmask
        reached_ret = False
        restore_lines = {}
        while work:
            i = work.pop()
            if i >= n:
                continue
            cur = rest_in[i]
            code = code_part(body[i]).strip()
            m = RESTORE.match(code)
            o = cur & ~self.wr[i]
            if m:
                reg, base_reg, k = m.groups()
                st = states[i]
                if reg in cand and st is not TOP and base_reg in st:
                    slot = st[base_reg] + int(k)
                    if saves.get(slot) == reg:
                        o |= 1 << BIT[reg]
                        restore_lines[i] = reg
            if self.calls[i] and not self.tail[i]:
                o = 0
            if self.returns[i]:
                reached_ret = True
                ret_ok &= o
            for s in self.succ[i]:
                if s >= n:
                    # Falling off the end is a return too.
                    reached_ret = True
                    ret_ok &= o
                    continue
                if rest_in[s] is None:
                    rest_in[s] = o
                    work.append(s)
                elif rest_in[s] & o != rest_in[s]:
                    rest_in[s] &= o
                    work.append(s)
        if not reached_ret:
            return 0, {}, {}
        pres = ret_ok & cmask
        return (pres, {i: r for i, r in save_lines.items() if pres >> BIT[r] & 1},
                {i: r for i, r in restore_lines.items() if pres >> BIT[r] & 1})


def parse_file(path, helpers):
    text = open(path, encoding="utf-8", errors="surrogateescape").read()

    def inline(m):
        body = helpers.get(m.group(1))
        if body is None:
            return m.group(0)
        return "\t{\n" + "".join("\t" + l + "\n" for l in body) + "\t}\n"

    text = HELPER_CALL.sub(inline, text)
    lines = text.split("\n")
    funcs = []  # (name, start line index of DEFINE, end index of }, Func or None)
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith("DEFINE_REX_FUNC(") and line.endswith("{"):
            j = i + 1
            while lines[j] != "}":
                j += 1
            name = line[len("DEFINE_REX_FUNC("):line.index(")")]
            try:
                f = Func(name, lines[i + 1:j])
            except Skip as e:
                f = None
                print(f"patch_local_regs: {name} left as it is ({e})", file=sys.stderr)
            funcs.append((name, i, j, f))
            i = j
        i += 1
    return lines, funcs


def load_helpers(paths, hooked):
    """The bodies of the save/restore helpers (straight-line code; each is defined once, in XDK/.../crt*.asm.cpp)."""
    helpers = {}
    for path in paths:
        text = open(path, encoding="utf-8", errors="surrogateescape").read()
        if "DEFINE_REX_FUNC(__" not in text:
            continue
        for name, body in HELPER_DEF.findall(text):
            if name in hooked or "goto " in body or "loc_" in body:
                continue
            helpers[name] = [l for l in body.split("\n") if l.strip() and
                             not l.strip().startswith("REX_FUNC_PROLOGUE") and l.strip() != "return;"]
    return helpers


def scan_sources(dirs, defined):
    """(hooked, native): the guest functions a REX_EXTERN with a body replaces, and every defined name the sources
    mention (native code calling a guest function, a hook calling the original through __imp__)."""
    hooked, native = set(), set()
    for d in dirs:
        for root, _, fs in os.walk(d):
            for fn in fs:
                if not fn.endswith((".cpp", ".h", ".hpp", ".cc", ".inc")):
                    continue
                text = open(os.path.join(root, fn), encoding="utf-8", errors="replace").read()
                hooked.update(HOOK.findall(text))
                for macro, arg, prefix, _ in HOOK_MACRO.findall(text):
                    hooked.update(prefix + a for a in re.findall(r"^" + macro + r"\(\s*(\w+)", text, re.M))
                for w in set(re.findall(r"\b\w+\b", text)):
                    name = w[len("__imp__"):] if w.startswith("__imp__") else w
                    if name in defined:
                        native.add(name)
    hooked = {h[len("__imp__"):] if h.startswith("__imp__") else h for h in hooked}
    return hooked, native


# ---------------------------------------------------------------------------------------------------------------------
# Phase 1 (per file): the facts the whole-program fixpoint needs.

def summarize(f):
    """Compact form of a function for the REF/MOD fixpoint: basic blocks (use, kill) and call nodes."""
    n = f.n
    nodes = []  # (use, kill, call) with call = None or (kind, target)
    node_of = {}
    # One node per line (lines without anything are kept as empty nodes so successors stay simple), but only
    # reachable ones matter.
    for i in range(n):
        use, kill = f.use[i], f.kill[i]
        if i in f.save_lines:
            use &= ~(1 << BIT[f.save_lines[i]])
        call = None
        if f.calls[i]:
            call = tuple(f.calls[i])
        nodes.append((use, kill, call, tuple(f.succ[i]), f.tail[i], f.returns[i]))
    return {
        "nodes": nodes,
        "start": f.start,
        "written": f.written,
        "pres": f.pres,
    }


def phase1(args):
    path, helpers = args
    _, funcs = parse_file(path, helpers)
    out = {}
    for name, _, _, f in funcs:
        # A function the pass cannot follow (another use of ctx) may read and change anything.
        out[name] = summarize(f) if f is not None and not f.unknown else None
    return out


# ---------------------------------------------------------------------------------------------------------------------
# Phase 2: REF / MOD fixpoints.

def call_ref_mod(kind, target, ref, mod, hooked, defined):
    if kind == "t":
        return TRAP_REF, 0
    if kind == "s":
        # A jump table's out-of-range path: dispatched like a tail call through a pointer.
        return ABI_REF, ABI_MOD
    if kind == "i":
        return ABI_REF, ABI_MOD
    # direct
    if target.startswith("__imp__"):
        inner = target[len("__imp__"):]
        if inner in defined:
            return ref.get(inner, ALL), mod.get(inner, ALL)
        return ABI_REF, ABI_MOD  # a kernel import
    r, m = ref.get(target, ALL), mod.get(target, ALL)
    if target in hooked or target not in defined:
        r |= ABI_REF
        m |= ABI_MOD
    return r, m


def liveness(s, ref, mod, hooked, defined, at_return, calls_read=True):
    """Backward liveness over the lines: a call reads REF of its callee (and changes nothing for certain), a return
    (and the end of the function) reads at_return. Returns (live_in, live_out) per line."""
    nodes = s["nodes"]
    n = len(nodes)
    live = [0] * (n + 1)
    live[n] = at_return
    outs = [0] * n
    callref = [0] * n
    for i in range(n):
        call = nodes[i][2]
        if call and calls_read:
            for kind, target in call:
                callref[i] |= call_ref_mod(kind, target, ref, mod, hooked, defined)[0]
    changed = True
    while changed:
        changed = False
        for i in range(n - 1, -1, -1):
            use, kill, call, succ, tail, ret = nodes[i]
            out = 0
            for t in succ:
                out |= live[t]
            outs[i] = out
            inn = (out & ~kill) | use | callref[i]
            if ret:
                inn |= at_return
            if inn != live[i]:
                live[i] = inn
                changed = True
    return live, outs


def compute_ref(s, ref, mod, hooked, defined):
    live, _ = liveness(s, ref, mod, hooked, defined, 0)
    n = len(s["nodes"])
    return live[s["start"]] if s["start"] < n else 0


def compute_mod(s, ref, mod, hooked, defined):
    m = s["written"]
    tailm = 0
    for use, kill, call, succ, tail, ret in s["nodes"]:
        if call:
            for kind, target in call:
                _, cm = call_ref_mod(kind, target, ref, mod, hooked, defined)
                if tail:
                    tailm |= cm
                else:
                    m |= cm
    return (m & ~s["pres"]) | tailm


def fixpoint(summ, hooked):
    defined = set(summ)
    callers = {}
    for name, s in summ.items():
        if s is None:
            continue
        for use, kill, call, succ, tail, ret in s["nodes"]:
            if call:
                for kind, target in call:
                    if kind == "d":
                        t = target[len("__imp__"):] if target.startswith("__imp__") else target
                        callers.setdefault(t, set()).add(name)
    ref = {name: 0 for name in summ}
    mod = {name: 0 for name in summ}
    for name, s in summ.items():
        if s is None:
            ref[name] = ALL
            mod[name] = ALL
    # MOD first (it does not depend on REF), then REF.
    for which in ("mod", "ref"):
        work = [name for name, s in summ.items() if s is not None]
        inwork = set(work)
        rounds = 0
        while work:
            name = work.pop()
            inwork.discard(name)
            rounds += 1
            s = summ[name]
            if which == "mod":
                new = compute_mod(s, ref, mod, hooked, defined)
                old = mod[name]
                if new != old:
                    mod[name] = new | old
            else:
                new = compute_ref(s, ref, mod, hooked, defined)
                old = ref[name]
                if new != old:
                    ref[name] = new | old
            if new != old:
                for c in callers.get(name, ()):
                    if c not in inwork and summ.get(c) is not None:
                        inwork.add(c)
                        work.append(c)
    return ref, mod


# What native code may read in ctx after calling a guest function (thread entries, callbacks, the stack cut), and what
# every function's return keeps exact on top of what its callers read after the call.
RESULT_FLOOR = mask(["r1", "r3", "r4", "r13", "f1", "f2", "v2", "xer"])


def fixpoint_liveout(summ, ref, mod, hooked, defined, native):
    """LIVEOUT(F): the registers a caller may read after F returns (before writing them): from the liveness after
    every call site of F, the callers' own LIVEOUT through their returns, and after every indirect call (any
    function may be the target of one)."""
    if any(s is None for s in summ.values()):
        return {name: ALL for name in summ}
    floor = RESULT_FLOOR
    liveout = {name: floor for name in summ}
    for name in native | hooked:
        if name in liveout:
            liveout[name] = ALL
    work = list(summ)
    inwork = set(work)
    while work:
        name = work.pop()
        inwork.discard(name)
        s = summ[name]
        live, outs = liveness(s, ref, mod, hooked, defined, liveout[name])
        own = None
        grew_floor = 0
        for i, (use, kill, call, succ, tail, ret) in enumerate(s["nodes"]):
            if not call:
                continue
            after = outs[i] | (liveout[name] if ret else 0)
            for kind, target in call:
                if kind == "d":
                    t = target[len("__imp__"):] if target.startswith("__imp__") else target
                    if t in liveout and after & ~liveout[t]:
                        liveout[t] |= after
                        if t not in inwork:
                            inwork.add(t)
                            work.append(t)
                elif kind == "i":
                    # What the code after an indirect call reads of the volatile registers itself (the target
                    # preserves the others): the registers later callees read (REF) are their arguments, set
                    # after this call, not something an unknown target hands over. (The switch fallback, kind "s",
                    # is a jump table's out-of-range path.)
                    if own is None:
                        own = liveness(s, ref, mod, hooked, defined, liveout[name] & RESULT_FLOOR, False)[1]
                    # The non-volatiles are another matter: the target must leave their ctx copy as it found it
                    # when anything after the call may read it (a later callee's REF included), so those count
                    # in full.
                    after = ((own[i] | (liveout[name] & RESULT_FLOOR if ret else 0)) & ABI_MOD) | (
                        (outs[i] | (liveout[name] if ret else 0)) & ~ABI_MOD)
                    if tail:
                        # A tail call (`if (c) {` / call / `return;` / `}`): the lines after the block are not
                        # reached from it, only this function's caller reads what it leaves (its volatile part
                        # capped as above).
                        after = (liveout[name] & RESULT_FLOOR & ABI_MOD) | (liveout[name] & ~ABI_MOD)
                    grew_floor |= after
        if grew_floor & ~floor:
            floor |= grew_floor
            for t in liveout:
                if floor & ~liveout[t]:
                    liveout[t] |= floor
                    if t not in inwork:
                        inwork.add(t)
                        work.append(t)
    return liveout


# ---------------------------------------------------------------------------------------------------------------------
# Phase 3 (per file): the rewrite.

def flush(m, names):
    return "".join(f"ctx.{r} = L_{r}; " for r in names if m >> BIT[r] & 1)


def reload(m, names):
    return "".join(f" L_{r} = ctx.{r};" for r in names if m >> BIT[r] & 1)


def ctype(r):
    if r == "lr":
        return "uint64_t"
    if r == "xer":
        return "PPCXERRegister"
    if r.startswith("cr"):
        return "PPCCRRegister"
    if r[0] == "v":
        return "PPCVRegister"
    return "PPCRegister"


def rewrite(f, ref, mod, liveout, hooked, defined, sites=None):
    body, n = f.body, f.n
    if not f.used:
        return None
    names = [r for r in NAMES if f.used >> BIT[r] & 1]

    def site(kind, target):
        return call_ref_mod(kind, target, ref, mod, hooked, defined)

    # The registers whose ctx copy this function may change: written ones it writes back at a call, and what its
    # callees change. MOD(F) | CH: what a return writes back (when dirty).
    ch = 0
    for i in range(n):
        if f.calls[i]:
            for kind, target in f.calls[i]:
                r, m = site(kind, target)
                ch |= m | (f.written & (r | m))
    # ... of which the caller may read back: LIVEOUT(F).
    at_return = (mod[f.name] | ch) & liveout[f.name]
    _, live_out = liveness(summarize(f), ref, mod, hooked, defined, liveout[f.name])

    # Saved registers nobody gets back: not written back at a return, not read by a tail call, not read between
    # their restore and the return. Their save and restore go (the frame slot is left as it was).
    strip = f.pres & ~at_return
    for i in range(n):
        if f.calls[i] and f.tail[i]:
            for kind, target in f.calls[i]:
                strip &= ~site(kind, target)[0]
                if kind == "s" or (target or "").replace("__imp__", "", 1) in CTX_IMPORTS:
                    strip = 0
    for i, reg in f.restore_lines.items():
        b = 1 << BIT[reg]
        if not strip & b:
            continue
        j = i + 1
        while j < n:
            if f.use[j] & b or f.wr[j] & b or GOTO.search(code_part(body[j])) or LABEL.match(body[j]):
                strip &= ~b
                break
            if f.returns[j] or (f.calls[j] and f.tail[j]):
                break
            if f.calls[j]:
                strip &= ~b
                break
            j += 1
        else:
            strip &= ~b
    dropped = {i for i, r in f.save_lines.items() if strip >> BIT[r] & 1}
    dropped |= {i for i, r in f.restore_lines.items() if strip >> BIT[r] & 1}

    # Forward may-dirty dataflow.
    din = [0] * (n + 1)
    seen = [False] * (n + 1)
    seen[f.start] = True
    work = [f.start]
    while work:
        i = work.pop()
        if i >= n:
            continue
        cur = din[i] | f.wr[i]
        if f.calls[i] and f.plain[i]:
            kind, target = f.calls[i][0]
            r, m = site(kind, target)
            if f.tail[i]:
                cur = 0
            else:
                cur &= ~(r | m)
        for s in f.succ[i]:
            nd = din[s] | cur
            if nd != din[s] or not seen[s]:
                din[s] = nd
                seen[s] = True
                work.append(s)

    out = body[:f.start]
    out.extend(f"\t{ctype(r)} L_{r} = ctx.{r};" for r in names)
    sub = lambda m: "L_" + m.group(1)
    for i in range(f.start, n):
        line = body[i]
        if i in dropped:
            out.append("\t// (save/restore of a register nobody gets back: dropped)")
            continue
        if not (f.use[i] or f.wr[i] or f.calls[i] or f.returns[i] or OCC.search(code_part(line))):
            out.append(line)
            continue
        code = code_part(line)
        comment = line[len(code):]
        new = OCC.sub(sub, code)
        if not seen[i]:
            if f.calls[i]:
                # A call the dataflow never reaches gets no write-back: it must really be dead code.
                sites["unreached"].append(f"{f.name} line {i}: {code.strip()[:80]}")
            out.append(new + comment)
            continue
        here = din[i] | f.wr[i]
        if f.calls[i]:
            tail = f.tail[i]

            def wrap(kind, target, text):
                r, m = site(kind, target)
                if kind == "t":
                    return "{ " + flush(here, names) + text + " }"
                if kind == "s" or (target or "").replace("__imp__", "", 1) in CTX_IMPORTS:
                    return "{ " + flush(here, names) + text + reload(m & f.used, names) + " }"
                if tail:
                    # What the callee reads, and what this function's return has to leave in ctx (the return
                    # after the call writes nothing back: everything it changes is read back here).
                    return "{ " + flush(here & (r | at_return), names) + text + reload(m & f.used, names) + " }"
                fm = here & (r | (m & live_out[i]))
                if not CHECK:
                    return "{ " + flush(fm, names) + text + reload(m & f.used, names) + " }"
                # PLR_CHECK=1: the registers not read back must come back unchanged (MOD), and the dirty ones not
                # written back get a poison value the callee must not use (REF).
                # Each site has a number (the file's base, plus a count): the poison is 0x5EAD5EAD:<number>, so a
                # fault on a poisoned address names the site (patch_local_regs.check.map lists them). With 4,000
                # files the number needs the whole low word (gears2-nx's 0x5E prefix does not fit).
                sid = sites["base"] + len(sites["list"])
                sites["list"].append(f"{sid:06X} {f.name} line {i} calls {target or 'bctrl'}")
                gp = [x for x in names if x[0] in "rf" and x[1:].isdigit()]
                poison = [x for x in gp if (here & ~fm) >> BIT[x] & 1]
                keep = [x for x in gp if not (m >> BIT[x] & 1) and live_out[i] >> BIT[x] & 1]
                pv = 0x5EAD5EAD00000000 | sid
                # (and put back after the call: the poison is a change of ctx this function's analysis does not
                # know about, and a caller trusting ctx to be unchanged there would read it)
                pre = ""
                if poison:
                    pre = "const uint64_t pz_[] = {" + ", ".join(f"ctx.{x}.u64" for x in poison) + "}; "
                pre += "".join(f"ctx.{x}.u64 = 0x{pv:X}ull; " for x in poison)
                if keep:
                    pre += "const uint64_t chk_[] = {" + ", ".join(f"ctx.{x}.u64" for x in keep) + "}; "
                cond = " || ".join(f"ctx.{x}.u64 != chk_[{k}]" for k, x in enumerate(keep))
                post = (f" if (__builtin_expect({cond}, 0)) ds2_regcheck_fail(__func__, \"{target or 'bctrl'}\", "
                        f"0x{sid:X}, \"{' '.join(keep)}\", chk_, &ctx);" if keep else "")
                unpoison = "".join(f" ctx.{x}.u64 = pz_[{k}];" for k, x in enumerate(poison))
                return "{ " + flush(fm, names) + pre + text + post + unpoison + reload(m & f.used, names) + " }"

            spans = find_indirect(new)
            kinds = [k for k, _ in f.calls[i] if k in ("i", "s")]
            for (a, b), k in reversed(list(zip(spans, kinds))):
                new = new[:a] + wrap(k, None, new[a:b]) + new[b:]
            new = CALL.sub(lambda m: wrap("d", m.group(1), m.group(0)), new)
            new = TRAP.sub(lambda m: wrap("t", None, m.group(0)), new)
        if f.returns[i]:
            fm = here & at_return
            if fm:
                new = new.replace("return;", "{ " + flush(fm, names) + "return; }")
        out.append(new + comment)
    if seen[n] and din[n] & at_return:
        out.append("\t" + flush(din[n] & at_return, names).rstrip())
    return out


def phase3(args):
    src, dst, index, helpers, ref, mod, liveout, hooked, defined = args
    lines, funcs = parse_file(src, helpers)
    # Check build: each call site has a number, <file index> << 16 | n (the file index in the sorted list of files).
    sites = {"base": index << 16, "list": [], "unreached": []}
    out = []
    last = 0
    done = skipped = 0
    for name, a, b, f in funcs:
        out.extend(lines[last:a + 1])
        new = None
        if f is not None and f.ok:
            new = rewrite(f, ref, mod, liveout, hooked, defined, sites)
            done += 1
        elif f is not None:
            skipped += 1
        out.extend(new if new is not None else lines[a + 1:b])
        last = b
    out.extend(lines[last:])
    write_if_changed(dst, "\n".join(out))
    return done, skipped, sites["list"], sites["unreached"]


def write_if_changed(path, text):
    """Write path (replacing the mirror's symlink) unless it already holds this text: unchanged files keep their date."""
    if not os.path.islink(path) and os.path.isfile(path):
        if open(path, encoding="utf-8", errors="surrogateescape").read() == text:
            return False
    if os.path.lexists(path):
        os.unlink(path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "w", encoding="utf-8", errors="surrogateescape").write(text)
    return True


def recomp_files(tree):
    """The files of src_tu2 that define functions (relative paths, sorted): everything but _units/ and the support TUs."""
    out = []
    for root, dirs, fs in os.walk(tree):
        dirs[:] = sorted(d for d in dirs if d != "_units")
        for fn in sorted(fs):
            if fn.endswith(".cpp"):
                path = os.path.join(root, fn)
                if "DEFINE_REX_FUNC(" in open(path, encoding="utf-8", errors="surrogateescape").read():
                    out.append(os.path.relpath(path, tree))
    return sorted(out)


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    tree, mirror, source_dirs = argv[0], argv[1], argv[2:]
    rel = recomp_files(tree)
    files = [os.path.join(tree, r) for r in rel]
    defined = set()
    for p in files:
        defined.update(re.findall(r"^DEFINE_REX_FUNC\((\w+)\)", open(p, encoding="utf-8",
                                                                        errors="surrogateescape").read(), re.M))
    hooked, native = scan_sources(source_dirs, defined)
    helpers = load_helpers(files, hooked)
    with ProcessPoolExecutor(max_workers=os.cpu_count()) as ex:
        summ = {}
        for part in ex.map(phase1, [(p, helpers) for p in files], chunksize=16):
            for name in part.keys() & summ.keys():
                print(f"patch_local_regs: {name} defined twice", file=sys.stderr)
            summ.update(part)
        ref, mod = fixpoint(summ, hooked)
        defined = set(summ)
        liveout = fixpoint_liveout(summ, ref, mod, hooked, defined, native)
        if os.environ.get("PLR_DUMP"):
            import pickle
            pickle.dump({"ref": ref, "mod": mod, "liveout": liveout,
                         "pres": {k: v["pres"] for k, v in summ.items() if v}},
                        open(os.environ["PLR_DUMP"], "wb"))
        done = skipped = 0
        site_list = []
        jobs = [(os.path.join(tree, r), os.path.join(mirror, r), k, helpers, ref, mod, liveout, hooked, defined)
                for k, r in enumerate(rel)]
        unreached = []
        for d, s, sl, un in ex.map(phase3, jobs, chunksize=16):
            done += d
            skipped += s
            site_list.extend(sl)
            unreached.extend(un)
    if CHECK:
        write_if_changed(os.path.join(mirror, "patch_local_regs.check.map"), "\n".join(site_list) + "\n")
    with open(os.path.join(mirror, "patch_local_regs.files"), "w") as fo:
        fo.write("\n".join(rel) + "\n")
    nv_ref = sum(1 for k, v in ref.items() if summ.get(k) and v & NONVOL & ~mask(["r1", "r13"]))
    if os.environ.get("PLR_DEBUG"):
        odd = {k: v & ~ABI_REF for k, v in ref.items() if summ.get(k) and v & ~ABI_REF}
        with open(os.environ["PLR_DEBUG"], "w") as fo:
            for k, v in sorted(odd.items()):
                fo.write(k + " " + " ".join(r for r in NAMES if v >> BIT[r] & 1) + "\n")
    for u in unreached:
        print(f"patch_local_regs: call not reached by the dataflow: {u}", file=sys.stderr)
    print(f"patch_local_regs: {len(unreached)} calls in code the dataflow does not reach")
    print(f"patch_local_regs: {done} functions with registers in locals, {skipped} left as they were; "
          f"{nv_ref} functions read a caller's non-volatile register; {len(helpers)} helpers inlined; "
          f"hooked: {' '.join(sorted(hooked))}; {len(native)} functions named by native code")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
