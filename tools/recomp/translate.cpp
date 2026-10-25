// 68000 -> C, one instruction at a time. The output uses the helpers in
// runtime/recomp_rt.h and follows Musashi's behaviour so the two can be
// checked against each other.
#include "translate.h"

#include <cstdarg>
#include <cstdio>

namespace {

std::string fmt(const char *f, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

const char *cc_expr(int cc)
{
    static const char *t[16] = {
        "1", "0", "(!cpu.c && !cpu.z)", "(cpu.c || cpu.z)", "!cpu.c", "cpu.c", "!cpu.z", "cpu.z",
        "!cpu.v", "cpu.v", "!cpu.n", "cpu.n", "(cpu.n == cpu.v)", "(cpu.n != cpu.v)",
        "(!cpu.z && cpu.n == cpu.v)", "(cpu.z || cpu.n != cpu.v)",
    };
    return t[cc];
}

int bits(int size) { return size * 8; }
uint32_t mask_of(int size) { return size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu; }

struct Ea {
    enum Kind { DREG, AREG, MEM, IMM, BAD } kind = BAD;
    int reg = 0, size = 0;
    uint32_t imm = 0;
    std::string addr;   // C expression for the address (a temp, or a constant)
    bool const_addr = false;
    uint32_t caddr = 0;
};

class Tx {
public:
    Tx(uint32_t pc, const Fetch16 &f) : pc_(pc), pos_(pc + 2), fetch_(f) { op_ = f(pc); }
    Insn run();

private:
    uint32_t pc_, pos_;
    const Fetch16 &fetch_;
    uint16_t op_;
    std::string b_;
    int tmp_ = 0;
    bool bad_ = false;
    Insn in_;

    uint16_t next16()
    {
        uint16_t w = fetch_(pos_);
        pos_ += 2;
        return w;
    }
    uint32_t next32()
    {
        uint32_t hi = next16();
        return hi << 16 | next16();
    }
    std::string tmp(const char *p) { return fmt("%s%d", p, tmp_++); }
    void emit(const std::string &s) { b_ += s; }
    void emitf(const char *f, ...)
    {
        char buf[2048];
        va_list ap;
        va_start(ap, f);
        std::vsnprintf(buf, sizeof buf, f, ap);
        va_end(ap);
        b_ += buf;
    }

    uint32_t next_pc() const { return pos_; }   // valid once all extension words are read

    // Decode an effective address and emit its address calculation, including
    // any (An)+ / -(An) register update.
    Ea ea(int mode, int reg, int size);
    std::string index_expr(uint16_t ext);
    std::string read(const Ea &e);                       // expression, zero-extended
    std::string load(const Ea &e);                       // read into a temp, return its name
    void write(const Ea &e, const std::string &v);
    std::string dreg_write(int r, int size, const std::string &v);

    void raise(int vector, uint32_t ret)
    {
        emitf("exception(%d, 0x%06Xu); return;\n", vector, ret);
    }
    bool privileged()
    {
        emitf("if (!cpu.s) { exception(8, 0x%06Xu); return; }\n", pc_);
        return true;
    }

    void group0();
    void move(int size);
    void group4();
    void group5();
    void group6();
    void moveq();
    void group8();
    void addsub(bool add);
    void groupB();
    void groupC();
    void shifts();
    void bitop(int type, bool dynamic);
    void immop(int kind, int size);
    void movem();
    void jumpto(const Ea &e, bool call);
};

std::string Tx::index_expr(uint16_t ext)
{
    int r = (ext >> 12) & 7;
    const char *bank = (ext & 0x8000) ? "a" : "d";
    if (ext & 0x800)
        return fmt("cpu.%s[%d]", bank, r);
    return fmt("sx16(cpu.%s[%d])", bank, r);
}

Ea Tx::ea(int mode, int reg, int size)
{
    Ea e;
    e.size = size;
    e.reg = reg;
    int step = (size == 1 && reg == 7) ? 2 : size;
    switch (mode) {
    case 0:
        e.kind = Ea::DREG;
        return e;
    case 1:
        e.kind = Ea::AREG;
        return e;
    case 2:
        e.kind = Ea::MEM;
        e.addr = tmp("ea");
        emitf("uint32_t %s = cpu.a[%d];\n", e.addr.c_str(), reg);
        return e;
    case 3:
        e.kind = Ea::MEM;
        e.addr = tmp("ea");
        emitf("uint32_t %s = cpu.a[%d]; cpu.a[%d] += %d;\n", e.addr.c_str(), reg, reg, step);
        return e;
    case 4:
        e.kind = Ea::MEM;
        e.addr = tmp("ea");
        emitf("cpu.a[%d] -= %d; uint32_t %s = cpu.a[%d];\n", reg, step, e.addr.c_str(), reg);
        return e;
    case 5: {
        int16_t d = int16_t(next16());
        e.kind = Ea::MEM;
        e.addr = tmp("ea");
        emitf("uint32_t %s = cpu.a[%d] + 0x%08Xu;\n", e.addr.c_str(), reg, uint32_t(int32_t(d)));
        return e;
    }
    case 6: {
        uint16_t ext = next16();
        e.kind = Ea::MEM;
        e.addr = tmp("ea");
        emitf("uint32_t %s = cpu.a[%d] + 0x%08Xu + %s;\n", e.addr.c_str(), reg, uint32_t(int32_t(int8_t(ext))),
              index_expr(ext).c_str());
        return e;
    }
    case 7:
        switch (reg) {
        case 0: {
            uint32_t a = uint32_t(int32_t(int16_t(next16())));
            e.kind = Ea::MEM;
            e.const_addr = true;
            e.caddr = a;
            e.addr = fmt("0x%08Xu", a);
            return e;
        }
        case 1: {
            uint32_t a = next32();
            e.kind = Ea::MEM;
            e.const_addr = true;
            e.caddr = a;
            e.addr = fmt("0x%08Xu", a);
            return e;
        }
        case 2: {
            uint32_t base = pos_;
            uint32_t a = base + uint32_t(int32_t(int16_t(next16())));
            e.kind = Ea::MEM;
            e.const_addr = true;
            e.caddr = a;
            e.addr = fmt("0x%08Xu", a);
            return e;
        }
        case 3: {
            uint32_t base = pos_;
            uint16_t ext = next16();
            uint32_t a = base + uint32_t(int32_t(int8_t(ext)));
            e.kind = Ea::MEM;
            e.addr = tmp("ea");
            emitf("uint32_t %s = 0x%08Xu + %s;\n", e.addr.c_str(), a, index_expr(ext).c_str());
            return e;
        }
        case 4:
            e.kind = Ea::IMM;
            if (size == 1)
                e.imm = next16() & 0xFF;
            else if (size == 2)
                e.imm = next16();
            else
                e.imm = next32();
            return e;
        }
        break;
    }
    bad_ = true;
    return e;
}

std::string Tx::read(const Ea &e)
{
    switch (e.kind) {
    case Ea::DREG:
        if (e.size == 4)
            return fmt("cpu.d[%d]", e.reg);
        return fmt("(cpu.d[%d] & 0x%Xu)", e.reg, mask_of(e.size));
    case Ea::AREG:
        if (e.size == 4)
            return fmt("cpu.a[%d]", e.reg);
        return fmt("(cpu.a[%d] & 0x%Xu)", e.reg, mask_of(e.size));
    case Ea::MEM:
        return fmt("rd%d(%s)", bits(e.size), e.addr.c_str());
    case Ea::IMM:
        return fmt("0x%Xu", e.imm & mask_of(e.size));
    default:
        bad_ = true;
        return "0";
    }
}

std::string Tx::load(const Ea &e)
{
    std::string v = tmp("v");
    emitf("uint32_t %s = %s;\n", v.c_str(), read(e).c_str());
    return v;
}

std::string Tx::dreg_write(int r, int size, const std::string &v)
{
    if (size == 4)
        return fmt("cpu.d[%d] = %s;\n", r, v.c_str());
    uint32_t m = mask_of(size);
    return fmt("cpu.d[%d] = (cpu.d[%d] & 0x%08Xu) | ((%s) & 0x%Xu);\n", r, r, ~m, v.c_str(), m);
}

void Tx::write(const Ea &e, const std::string &v)
{
    switch (e.kind) {
    case Ea::DREG:
        emit(dreg_write(e.reg, e.size, v));
        return;
    case Ea::AREG:
        emitf("cpu.a[%d] = %s;\n", e.reg, v.c_str());
        return;
    case Ea::MEM:
        emitf("wr%d(%s, %s);\n", bits(e.size), e.addr.c_str(), v.c_str());
        return;
    default:
        bad_ = true;
    }
}

// ---- group 0: immediates, bit operations, MOVEP -------------------------------------
void Tx::bitop(int type, bool dynamic)
{
    static const char *op[4] = {"", " ^ ", " & ~", " | "};
    std::string bit;
    if (dynamic)
        bit = fmt("cpu.d[%d]", (op_ >> 9) & 7);
    else
        bit = fmt("0x%Xu", next16() & 0xFF);
    int mode = (op_ >> 3) & 7, reg = op_ & 7;
    if (mode == 0) {
        std::string n = tmp("b");
        emitf("uint32_t %s = (%s) & 31;\n", n.c_str(), bit.c_str());
        emitf("cpu.z = !((cpu.d[%d] >> %s) & 1);\n", reg, n.c_str());
        if (type)
            emitf("cpu.d[%d] = cpu.d[%d]%s(1u << %s);\n", reg, reg, op[type], n.c_str());
        return;
    }
    Ea e = ea(mode, reg, 1);
    if (e.kind == Ea::AREG || (type && e.kind == Ea::IMM)) {
        bad_ = true;
        return;
    }
    std::string n = tmp("b");
    emitf("uint32_t %s = (%s) & 7;\n", n.c_str(), bit.c_str());
    std::string v = load(e);
    emitf("cpu.z = !((%s >> %s) & 1);\n", v.c_str(), n.c_str());
    if (type)
        write(e, fmt("(%s%s(1u << %s))", v.c_str(), op[type], n.c_str()));
}

void Tx::immop(int kind, int size)
{
    // kind: 0 ori, 1 andi, 2 subi, 3 addi, 5 eori, 6 cmpi
    uint32_t imm = size == 4 ? next32() : size == 2 ? next16() : (next16() & 0xFF);
    int mode = (op_ >> 3) & 7, reg = op_ & 7;
    Ea e = ea(mode, reg, size);
    if (e.kind == Ea::AREG || e.kind == Ea::IMM) {
        bad_ = true;
        return;
    }
    std::string d = load(e);
    std::string s = fmt("0x%Xu", imm);
    int bt = bits(size);
    std::string r = tmp("r");
    switch (kind) {
    case 0:
        emitf("uint32_t %s = %s | %s; logic%d(%s);\n", r.c_str(), d.c_str(), s.c_str(), bt, r.c_str());
        break;
    case 1:
        emitf("uint32_t %s = %s & %s; logic%d(%s);\n", r.c_str(), d.c_str(), s.c_str(), bt, r.c_str());
        break;
    case 5:
        emitf("uint32_t %s = %s ^ %s; logic%d(%s);\n", r.c_str(), d.c_str(), s.c_str(), bt, r.c_str());
        break;
    case 2:
        emitf("uint32_t %s = sub%d(%s, %s);\n", r.c_str(), bt, s.c_str(), d.c_str());
        break;
    case 3:
        emitf("uint32_t %s = add%d(%s, %s);\n", r.c_str(), bt, s.c_str(), d.c_str());
        break;
    case 6:
        emitf("cmp%d(%s, %s);\n", bt, s.c_str(), d.c_str());
        return;
    }
    write(e, r);
}

void Tx::group0()
{
    if ((op_ & 0x0138) == 0x0108) {   // MOVEP
        int dx = (op_ >> 9) & 7, ay = op_ & 7;
        int16_t d = int16_t(next16());
        std::string a = tmp("ea");
        emitf("uint32_t %s = cpu.a[%d] + 0x%08Xu;\n", a.c_str(), ay, uint32_t(int32_t(d)));
        switch ((op_ >> 6) & 3) {
        case 0:
            emitf("cpu.d[%d] = (cpu.d[%d] & 0xFFFF0000u) | rd8(%s) << 8 | rd8(%s + 2);\n", dx, dx, a.c_str(), a.c_str());
            break;
        case 1:
            emitf("cpu.d[%d] = rd8(%s) << 24 | rd8(%s + 2) << 16 | rd8(%s + 4) << 8 | rd8(%s + 6);\n", dx, a.c_str(),
                  a.c_str(), a.c_str(), a.c_str());
            break;
        case 2:
            emitf("wr8(%s, cpu.d[%d] >> 8); wr8(%s + 2, cpu.d[%d]);\n", a.c_str(), dx, a.c_str(), dx);
            break;
        case 3:
            emitf("wr8(%s, cpu.d[%d] >> 24); wr8(%s + 2, cpu.d[%d] >> 16); wr8(%s + 4, cpu.d[%d] >> 8); "
                  "wr8(%s + 6, cpu.d[%d]);\n",
                  a.c_str(), dx, a.c_str(), dx, a.c_str(), dx, a.c_str(), dx);
            break;
        }
        return;
    }
    if (op_ & 0x0100) {
        bitop((op_ >> 6) & 3, true);
        return;
    }
    int kind = (op_ >> 9) & 7;
    if (kind == 4) {
        bitop((op_ >> 6) & 3, false);
        return;
    }
    if (kind == 7) {
        bad_ = true;
        return;
    }
    // to CCR / SR
    if ((op_ & 0x00FF) == 0x003C || (op_ & 0x00FF) == 0x007C) {
        bool sr = (op_ & 0x40) != 0;
        if (kind != 0 && kind != 1 && kind != 5) {
            bad_ = true;
            return;
        }
        const char *o = kind == 0 ? "|" : kind == 1 ? "&" : "^";
        if (sr) {
            uint32_t imm = next16();
            privileged();
            emitf("set_sr(get_sr() %s 0x%Xu);\n", o, imm);
            in_.flow = Insn::SYNC;
        } else {
            uint32_t imm = next16() & 0xFF;
            emitf("set_ccr(get_ccr() %s 0x%Xu);\n", o, imm);
        }
        return;
    }
    int size = (op_ >> 6) & 3;
    if (size == 3) {
        bad_ = true;
        return;
    }
    immop(kind, 1 << size);
}

// ---- MOVE / MOVEA --------------------------------------------------------------------
void Tx::move(int size)
{
    Ea s = ea((op_ >> 3) & 7, op_ & 7, size);
    if (size == 1 && s.kind == Ea::AREG) {
        bad_ = true;
        return;
    }
    std::string v = load(s);
    int dmode = (op_ >> 6) & 7, dreg = (op_ >> 9) & 7;
    if (dmode == 1) {
        if (size == 1) {
            bad_ = true;
            return;
        }
        emitf("cpu.a[%d] = %s;\n", dreg, size == 2 ? fmt("sx16(%s)", v.c_str()).c_str() : v.c_str());
        return;
    }
    Ea d = ea(dmode, dreg, size);
    if (d.kind == Ea::IMM || d.kind == Ea::AREG) {
        bad_ = true;
        return;
    }
    emitf("logic%d(%s);\n", bits(size), v.c_str());
    write(d, v);
}

// ---- group 4: miscellaneous ------------------------------------------------------------
void Tx::jumpto(const Ea &e, bool call)
{
    if (e.kind != Ea::MEM) {
        bad_ = true;
        return;
    }
    if (call)
        emitf("push32(0x%06Xu);\n", next_pc());
    in_.call = call;
    if (e.const_addr) {
        in_.flow = Insn::JUMP;
        in_.target = e.caddr & 0xFFFFFF;
    } else {
        in_.flow = Insn::DYNAMIC;
        in_.dyn = e.addr;
    }
}

void Tx::movem()
{
    bool to_regs = op_ & 0x0400;
    int size = (op_ & 0x40) ? 4 : 2;
    uint16_t list = next16();
    int mode = (op_ >> 3) & 7, reg = op_ & 7;
    if (!to_regs && mode == 4) {
        std::string a = tmp("ea");
        emitf("uint32_t %s = cpu.a[%d];\n", a.c_str(), reg);
        for (int i = 0; i < 16; i++) {
            if (!(list & (1 << i)))
                continue;
            int r = 15 - i;
            emitf("%s -= %d; wr%d(%s, cpu.%s[%d]);\n", a.c_str(), size, bits(size), a.c_str(), r < 8 ? "d" : "a", r & 7);
        }
        emitf("cpu.a[%d] = %s;\n", reg, a.c_str());
        return;
    }
    if (to_regs && mode == 3) {
        std::string a = tmp("ea");
        emitf("uint32_t %s = cpu.a[%d];\n", a.c_str(), reg);
        for (int r = 0; r < 16; r++) {
            if (!(list & (1 << r)))
                continue;
            const char *bank = r < 8 ? "d" : "a";
            if (size == 2)
                emitf("cpu.%s[%d] = sx16(rd16(%s)); %s += 2;\n", bank, r & 7, a.c_str(), a.c_str());
            else
                emitf("cpu.%s[%d] = rd32(%s); %s += 4;\n", bank, r & 7, a.c_str(), a.c_str());
        }
        emitf("cpu.a[%d] = %s;\n", reg, a.c_str());
        return;
    }
    if (mode == 0 || mode == 1 || mode == 3 || mode == 4 || (mode == 7 && reg == 4)) {
        bad_ = true;
        return;
    }
    if (!to_regs && mode == 7 && (reg == 2 || reg == 3)) {
        bad_ = true;   // no PC-relative destination
        return;
    }
    Ea e = ea(mode, reg, size);
    std::string a = tmp("ea");
    emitf("uint32_t %s = %s;\n", a.c_str(), e.addr.c_str());
    for (int r = 0; r < 16; r++) {
        if (!(list & (1 << r)))
            continue;
        const char *bank = r < 8 ? "d" : "a";
        if (to_regs) {
            if (size == 2)
                emitf("cpu.%s[%d] = sx16(rd16(%s)); %s += 2;\n", bank, r & 7, a.c_str(), a.c_str());
            else
                emitf("cpu.%s[%d] = rd32(%s); %s += 4;\n", bank, r & 7, a.c_str(), a.c_str());
        } else {
            emitf("wr%d(%s, cpu.%s[%d]); %s += %d;\n", bits(size), a.c_str(), bank, r & 7, a.c_str(), size);
        }
    }
}

void Tx::group4()
{
    int mode = (op_ >> 3) & 7, reg = op_ & 7;
    switch (op_) {
    case 0x4AFC:
        raise(4, pc_);
        in_.flow = Insn::EXCEPT;
        return;
    case 0x4E70:   // RESET
        privileged();
        return;
    case 0x4E71:   // NOP
        return;
    case 0x4E72: { // STOP
        uint32_t imm = next16();
        privileged();
        emitf("set_sr(0x%Xu); cpu.stopped = 1;\n", imm);
        in_.flow = Insn::SYNC;
        return;
    }
    case 0x4E73:   // RTE
        privileged();
        emit("uint32_t sr = rd16(cpu.a[7]); cpu.a[7] += 2; uint32_t nt = pop32(); set_sr(sr);\n");
        in_.flow = Insn::DYNAMIC;
        in_.dyn = "nt";
        return;
    case 0x4E75:   // RTS
        emit("uint32_t nt = pop32();\n");
        in_.flow = Insn::DYNAMIC;
        in_.dyn = "nt";
        return;
    case 0x4E76:   // TRAPV
        emitf("if (cpu.v) { exception(7, 0x%06Xu); return; }\n", next_pc());
        return;
    case 0x4E77:   // RTR
        emit("set_ccr(rd16(cpu.a[7])); cpu.a[7] += 2; uint32_t nt = pop32();\n");
        in_.flow = Insn::DYNAMIC;
        in_.dyn = "nt";
        return;
    }
    if ((op_ & 0xFFF0) == 0x4E40) {   // TRAP
        raise(32 + (op_ & 15), next_pc());
        in_.flow = Insn::EXCEPT;
        return;
    }
    if ((op_ & 0xFFF8) == 0x4E50) {   // LINK
        int16_t d = int16_t(next16());
        if (reg == 7)
            emitf("cpu.a[7] -= 4; wr32(cpu.a[7], cpu.a[7]); cpu.a[7] += 0x%08Xu;\n", uint32_t(int32_t(d)));
        else
            emitf("push32(cpu.a[%d]); cpu.a[%d] = cpu.a[7]; cpu.a[7] += 0x%08Xu;\n", reg, reg, uint32_t(int32_t(d)));
        return;
    }
    if ((op_ & 0xFFF8) == 0x4E58) {   // UNLK
        if (reg == 7)
            emit("cpu.a[7] = rd32(cpu.a[7]);\n");
        else
            emitf("cpu.a[7] = cpu.a[%d]; cpu.a[%d] = pop32();\n", reg, reg);
        return;
    }
    if ((op_ & 0xFFF8) == 0x4E60) {   // MOVE An,USP
        privileged();
        emitf("cpu.other_sp = cpu.a[%d];\n", reg);
        return;
    }
    if ((op_ & 0xFFF8) == 0x4E68) {   // MOVE USP,An
        privileged();
        emitf("cpu.a[%d] = cpu.other_sp;\n", reg);
        return;
    }
    if ((op_ & 0xFFC0) == 0x4E80 || (op_ & 0xFFC0) == 0x4EC0) {   // JSR / JMP
        Ea e = ea(mode, reg, 4);
        if (mode == 3 || mode == 4 || e.kind != Ea::MEM) {
            bad_ = true;
            return;
        }
        jumpto(e, (op_ & 0xFFC0) == 0x4E80);
        return;
    }
    if ((op_ & 0xFFB8) == 0x4880) {   // EXT
        int r = reg;
        if (op_ & 0x40)
            emitf("cpu.d[%d] = sx16(cpu.d[%d]); logic32(cpu.d[%d]);\n", r, r, r);
        else
            emitf("cpu.d[%d] = (cpu.d[%d] & 0xFFFF0000u) | (sx8(cpu.d[%d]) & 0xFFFF); logic16(cpu.d[%d]);\n", r, r, r, r);
        return;
    }
    if ((op_ & 0xFB80) == 0x4880) {
        movem();
        return;
    }
    if ((op_ & 0xF1C0) == 0x41C0) {   // LEA
        Ea e = ea(mode, reg, 4);
        if (e.kind != Ea::MEM || mode == 3 || mode == 4) {
            bad_ = true;
            return;
        }
        emitf("cpu.a[%d] = %s;\n", (op_ >> 9) & 7, e.addr.c_str());
        return;
    }
    if ((op_ & 0xF1C0) == 0x4180) {   // CHK.W
        Ea e = ea(mode, reg, 2);
        if (e.kind == Ea::AREG) {
            bad_ = true;
            return;
        }
        std::string bound = load(e);
        int dn = (op_ >> 9) & 7;
        emitf("{ int32_t s = int16_t(cpu.d[%d]); int32_t b = int16_t(%s);\n", dn, bound.c_str());
        emit("cpu.z = (s & 0xFFFF) == 0; cpu.v = cpu.c = 0;\n");
        emitf("if (!(s >= 0 && s <= b)) { cpu.n = s < 0; exception(6, 0x%06Xu); return; } }\n", next_pc());
        return;
    }
    if ((op_ & 0xFFC0) == 0x40C0) {   // MOVE from SR
        Ea e = ea(mode, reg, 2);
        if (e.kind == Ea::AREG || e.kind == Ea::IMM) {
            bad_ = true;
            return;
        }
        write(e, "get_sr()");
        return;
    }
    if ((op_ & 0xFFC0) == 0x44C0) {   // MOVE to CCR
        Ea e = ea(mode, reg, 2);
        if (e.kind == Ea::AREG) {
            bad_ = true;
            return;
        }
        emitf("set_ccr(%s);\n", read(e).c_str());
        return;
    }
    if ((op_ & 0xFFC0) == 0x46C0) {   // MOVE to SR
        privileged();   // before the operand: no (An)+ side effect when it traps
        Ea e = ea(mode, reg, 2);
        if (e.kind == Ea::AREG) {
            bad_ = true;
            return;
        }
        std::string v = load(e);
        emitf("set_sr(%s);\n", v.c_str());
        in_.flow = Insn::SYNC;
        return;
    }
    if ((op_ & 0xFFC0) == 0x4800) {   // NBCD
        Ea e = ea(mode, reg, 1);
        if (e.kind == Ea::AREG || e.kind == Ea::IMM) {
            bad_ = true;
            return;
        }
        std::string v = load(e);
        std::string r = tmp("r"), st = tmp("st");
        emitf("bool %s; uint32_t %s = nbcd(%s, &%s);\n", st.c_str(), r.c_str(), v.c_str(), st.c_str());
        std::string w = b_;
        b_.clear();
        write(e, r);
        std::string wr = b_;
        b_ = w;
        emitf("if (%s) { %s}\n", st.c_str(), wr.c_str());
        return;
    }
    if ((op_ & 0xFFF8) == 0x4840) {   // SWAP
        emitf("cpu.d[%d] = cpu.d[%d] >> 16 | cpu.d[%d] << 16; logic32(cpu.d[%d]);\n", reg, reg, reg, reg);
        return;
    }
    if ((op_ & 0xFFC0) == 0x4840) {   // PEA
        Ea e = ea(mode, reg, 4);
        if (e.kind != Ea::MEM || mode == 3 || mode == 4) {
            bad_ = true;
            return;
        }
        emitf("push32(%s);\n", e.addr.c_str());
        return;
    }
    if ((op_ & 0xFFC0) == 0x4AC0) {   // TAS
        Ea e = ea(mode, reg, 1);
        if (e.kind == Ea::AREG || e.kind == Ea::IMM) {
            bad_ = true;
            return;
        }
        std::string v = load(e);
        emitf("logic8(%s);\n", v.c_str());
        write(e, fmt("(%s | 0x80u)", v.c_str()));
        return;
    }
    int size = (op_ >> 6) & 3;
    if (size == 3) {
        bad_ = true;
        return;
    }
    int sz = 1 << size, bt = bits(sz);
    switch (op_ & 0xFF00) {
    case 0x4000:   // NEGX
    case 0x4400:   // NEG
    case 0x4600:   // NOT
    case 0x4200: { // CLR
        Ea e = ea(mode, reg, sz);
        if (e.kind == Ea::AREG || e.kind == Ea::IMM) {
            bad_ = true;
            return;
        }
        if ((op_ & 0xFF00) == 0x4200) {
            emit("cpu.n = 0; cpu.z = 1; cpu.v = cpu.c = 0;\n");
            write(e, "0");
            return;
        }
        std::string v = load(e);
        std::string r = tmp("r");
        if ((op_ & 0xFF00) == 0x4000)
            emitf("uint32_t %s = negx%d(%s);\n", r.c_str(), bt, v.c_str());
        else if ((op_ & 0xFF00) == 0x4400)
            emitf("uint32_t %s = neg%d(%s);\n", r.c_str(), bt, v.c_str());
        else
            emitf("uint32_t %s = ~%s & 0x%Xu; logic%d(%s);\n", r.c_str(), v.c_str(), mask_of(sz), bt, r.c_str());
        write(e, r);
        return;
    }
    case 0x4A00: { // TST
        Ea e = ea(mode, reg, sz);
        if (e.kind == Ea::AREG && sz == 1) {
            bad_ = true;
            return;
        }
        emitf("logic%d(%s);\n", bt, read(e).c_str());
        return;
    }
    }
    bad_ = true;
}

// ---- group 5: ADDQ/SUBQ, Scc, DBcc -------------------------------------------------------
void Tx::group5()
{
    int mode = (op_ >> 3) & 7, reg = op_ & 7;
    if ((op_ & 0xC0) == 0xC0) {
        int cc = (op_ >> 8) & 15;
        if (mode == 1) {   // DBcc
            uint32_t base = pos_;
            int16_t d = int16_t(next16());
            in_.flow = Insn::BRANCH;
            in_.target = (base + uint32_t(int32_t(d))) & 0xFFFFFF;
            emitf("bool tk = false;\nif (!%s) { uint32_t w = (cpu.d[%d] - 1) & 0xFFFF; "
                  "cpu.d[%d] = (cpu.d[%d] & 0xFFFF0000u) | w; tk = w != 0xFFFF; }\n",
                  cc_expr(cc), reg, reg, reg);
            in_.cond = "tk";
            return;
        }
        Ea e = ea(mode, reg, 1);   // Scc
        if (e.kind == Ea::AREG || e.kind == Ea::IMM) {
            bad_ = true;
            return;
        }
        write(e, fmt("(%s ? 0xFFu : 0u)", cc_expr(cc)));
        return;
    }
    int size = 1 << ((op_ >> 6) & 3);
    uint32_t q = (op_ >> 9) & 7;
    if (!q)
        q = 8;
    bool sub = op_ & 0x100;
    Ea e = ea(mode, reg, size);
    if (e.kind == Ea::IMM || (e.kind == Ea::AREG && size == 1)) {
        bad_ = true;
        return;
    }
    if (e.kind == Ea::AREG) {
        emitf("cpu.a[%d] %s= %u;\n", reg, sub ? "-" : "+", q);
        return;
    }
    std::string v = load(e);
    std::string r = tmp("r");
    emitf("uint32_t %s = %s%d(%u, %s);\n", r.c_str(), sub ? "sub" : "add", bits(size), q, v.c_str());
    write(e, r);
}

// ---- group 6: branches ------------------------------------------------------------------
void Tx::group6()
{
    int cc = (op_ >> 8) & 15;
    uint32_t base = pc_ + 2;
    int32_t d = int8_t(op_ & 0xFF);
    if (d == 0)
        d = int16_t(next16());
    uint32_t target = (base + uint32_t(d)) & 0xFFFFFF;
    in_.target = target;
    if (cc == 1) {   // BSR
        emitf("push32(0x%06Xu);\n", next_pc());
        in_.flow = Insn::JUMP;
        in_.call = true;
    } else if (cc == 0) {
        in_.flow = Insn::JUMP;
    } else {
        in_.flow = Insn::BRANCH;
        in_.cond = cc_expr(cc);
    }
}

void Tx::moveq()
{
    if (op_ & 0x100) {
        bad_ = true;
        return;
    }
    int r = (op_ >> 9) & 7;
    emitf("cpu.d[%d] = 0x%08Xu; logic32(cpu.d[%d]);\n", r, uint32_t(int32_t(int8_t(op_ & 0xFF))), r);
}

// ---- groups 8 / 9 / B / C / D ---------------------------------------------------------------
void Tx::group8()
{
    int mode = (op_ >> 3) & 7, reg = op_ & 7, dn = (op_ >> 9) & 7;
    if ((op_ & 0x1C0) == 0x0C0 || (op_ & 0x1C0) == 0x1C0) {   // DIVU / DIVS
        bool sgn = op_ & 0x100;
        Ea e = ea(mode, reg, 2);
        if (e.kind == Ea::AREG) {
            bad_ = true;
            return;
        }
        std::string s = load(e);
        emitf("if ((%s & 0xFFFF) == 0) { exception(5, 0x%06Xu); return; }\n", s.c_str(), next_pc());
        emitf("cpu.d[%d] = %s(%s, cpu.d[%d]);\n", dn, sgn ? "divs" : "divu", s.c_str(), dn);
        return;
    }
    if ((op_ & 0x1F0) == 0x100) {   // SBCD
        if (op_ & 8) {
            int ax = dn, ay = reg;
            int sy = ay == 7 ? 2 : 1, sx = ax == 7 ? 2 : 1;
            emitf("cpu.a[%d] -= %d; uint32_t s = rd8(cpu.a[%d]); cpu.a[%d] -= %d; uint32_t d = rd8(cpu.a[%d]);\n", ay, sy,
                  ay, ax, sx, ax);
            emitf("wr8(cpu.a[%d], sbcd(s, d));\n", ax);
        } else {
            emitf("uint32_t r = sbcd(cpu.d[%d] & 0xFF, cpu.d[%d] & 0xFF);\n", reg, dn);
            emit(dreg_write(dn, 1, "r"));
        }
        return;
    }
    int size = 1 << ((op_ >> 6) & 3);
    Ea e = ea(mode, reg, size);
    if (e.kind == Ea::AREG) {
        bad_ = true;
        return;
    }
    if (op_ & 0x100) {   // OR Dn,<ea>
        if (e.kind == Ea::DREG || e.kind == Ea::IMM) {
            bad_ = true;
            return;
        }
        std::string v = load(e);
        emitf("uint32_t r = (%s | cpu.d[%d]) & 0x%Xu; logic%d(r);\n", v.c_str(), dn, mask_of(size), bits(size));
        write(e, "r");
    } else {
        std::string v = load(e);
        emitf("uint32_t r = (%s | cpu.d[%d]) & 0x%Xu; logic%d(r);\n", v.c_str(), dn, mask_of(size), bits(size));
        emit(dreg_write(dn, size, "r"));
    }
}

void Tx::addsub(bool add)
{
    int mode = (op_ >> 3) & 7, reg = op_ & 7, rx = (op_ >> 9) & 7;
    const char *op = add ? "add" : "sub";
    if ((op_ & 0xC0) == 0xC0) {   // ADDA / SUBA
        int size = (op_ & 0x100) ? 4 : 2;
        Ea e = ea(mode, reg, size);
        std::string v = load(e);
        if (size == 2)
            emitf("cpu.a[%d] %s= sx16(%s);\n", rx, add ? "+" : "-", v.c_str());
        else
            emitf("cpu.a[%d] %s= %s;\n", rx, add ? "+" : "-", v.c_str());
        return;
    }
    int size = 1 << ((op_ >> 6) & 3);
    int bt = bits(size);
    if ((op_ & 0x130) == 0x100) {   // ADDX / SUBX
        if (op_ & 8) {
            int step_y = (size == 1 && reg == 7) ? 2 : size, step_x = (size == 1 && rx == 7) ? 2 : size;
            emitf("cpu.a[%d] -= %d; uint32_t s = rd%d(cpu.a[%d]); cpu.a[%d] -= %d; uint32_t d = rd%d(cpu.a[%d]);\n", reg,
                  step_y, bt, reg, rx, step_x, bt, rx);
            emitf("wr%d(cpu.a[%d], %sx%d(s, d));\n", bt, rx, op, bt);
        } else {
            emitf("uint32_t r = %sx%d(cpu.d[%d] & 0x%Xu, cpu.d[%d] & 0x%Xu);\n", op, bt, reg, mask_of(size), rx,
                  mask_of(size));
            emit(dreg_write(rx, size, "r"));
        }
        return;
    }
    Ea e = ea(mode, reg, size);
    if (op_ & 0x100) {   // Dn,<ea>
        if (e.kind != Ea::MEM) {
            bad_ = true;
            return;
        }
        std::string v = load(e);
        emitf("uint32_t r = %s%d(cpu.d[%d] & 0x%Xu, %s);\n", op, bt, rx, mask_of(size), v.c_str());
        write(e, "r");
    } else {
        if (e.kind == Ea::AREG && size == 1) {
            bad_ = true;
            return;
        }
        std::string v = load(e);
        emitf("uint32_t r = %s%d(%s, cpu.d[%d] & 0x%Xu);\n", op, bt, v.c_str(), rx, mask_of(size));
        emit(dreg_write(rx, size, "r"));
    }
}

void Tx::groupB()
{
    int mode = (op_ >> 3) & 7, reg = op_ & 7, rx = (op_ >> 9) & 7;
    if ((op_ & 0xC0) == 0xC0) {   // CMPA
        int size = (op_ & 0x100) ? 4 : 2;
        Ea e = ea(mode, reg, size);
        std::string v = load(e);
        emitf("cmp32(%s, cpu.a[%d]);\n", size == 2 ? fmt("sx16(%s)", v.c_str()).c_str() : v.c_str(), rx);
        return;
    }
    int size = 1 << ((op_ >> 6) & 3);
    int bt = bits(size);
    if (op_ & 0x100) {
        if (mode == 1) {   // CMPM (Ay)+,(Ax)+
            int sy = (size == 1 && reg == 7) ? 2 : size, sx = (size == 1 && rx == 7) ? 2 : size;
            emitf("uint32_t s = rd%d(cpu.a[%d]); cpu.a[%d] += %d; uint32_t d = rd%d(cpu.a[%d]); cpu.a[%d] += %d;\n", bt,
                  reg, reg, sy, bt, rx, rx, sx);
            emitf("cmp%d(s, d);\n", bt);
            return;
        }
        Ea e = ea(mode, reg, size);   // EOR Dn,<ea>
        if (e.kind == Ea::AREG || e.kind == Ea::IMM) {
            bad_ = true;
            return;
        }
        std::string v = load(e);
        emitf("uint32_t r = (%s ^ cpu.d[%d]) & 0x%Xu; logic%d(r);\n", v.c_str(), rx, mask_of(size), bt);
        write(e, "r");
        return;
    }
    Ea e = ea(mode, reg, size);   // CMP <ea>,Dn
    if (e.kind == Ea::AREG && size == 1) {
        bad_ = true;
        return;
    }
    std::string v = load(e);
    emitf("cmp%d(%s, cpu.d[%d] & 0x%Xu);\n", bt, v.c_str(), rx, mask_of(size));
}

void Tx::groupC()
{
    int mode = (op_ >> 3) & 7, reg = op_ & 7, rx = (op_ >> 9) & 7;
    if ((op_ & 0x1C0) == 0x0C0 || (op_ & 0x1C0) == 0x1C0) {   // MULU / MULS
        Ea e = ea(mode, reg, 2);
        if (e.kind == Ea::AREG) {
            bad_ = true;
            return;
        }
        std::string v = load(e);
        emitf("cpu.d[%d] = %s(%s, cpu.d[%d]);\n", rx, (op_ & 0x100) ? "muls" : "mulu", v.c_str(), rx);
        return;
    }
    if ((op_ & 0x1F0) == 0x100) {   // ABCD
        if (op_ & 8) {
            int sy = reg == 7 ? 2 : 1, sx = rx == 7 ? 2 : 1;
            emitf("cpu.a[%d] -= %d; uint32_t s = rd8(cpu.a[%d]); cpu.a[%d] -= %d; uint32_t d = rd8(cpu.a[%d]);\n", reg, sy,
                  reg, rx, sx, rx);
            emitf("wr8(cpu.a[%d], abcd(s, d));\n", rx);
        } else {
            emitf("uint32_t r = abcd(cpu.d[%d] & 0xFF, cpu.d[%d] & 0xFF);\n", reg, rx);
            emit(dreg_write(rx, 1, "r"));
        }
        return;
    }
    if ((op_ & 0x1F8) == 0x140) {   // EXG Dx,Dy
        emitf("uint32_t t = cpu.d[%d]; cpu.d[%d] = cpu.d[%d]; cpu.d[%d] = t;\n", rx, rx, reg, reg);
        return;
    }
    if ((op_ & 0x1F8) == 0x148) {   // EXG Ax,Ay
        emitf("uint32_t t = cpu.a[%d]; cpu.a[%d] = cpu.a[%d]; cpu.a[%d] = t;\n", rx, rx, reg, reg);
        return;
    }
    if ((op_ & 0x1F8) == 0x188) {   // EXG Dx,Ay
        emitf("uint32_t t = cpu.d[%d]; cpu.d[%d] = cpu.a[%d]; cpu.a[%d] = t;\n", rx, rx, reg, reg);
        return;
    }
    int size = 1 << ((op_ >> 6) & 3);
    Ea e = ea(mode, reg, size);
    if (e.kind == Ea::AREG) {
        bad_ = true;
        return;
    }
    if (op_ & 0x100) {   // AND Dn,<ea>
        if (e.kind != Ea::MEM) {
            bad_ = true;
            return;
        }
        std::string v = load(e);
        emitf("uint32_t r = (%s & cpu.d[%d]) & 0x%Xu; logic%d(r);\n", v.c_str(), rx, mask_of(size), bits(size));
        write(e, "r");
    } else {
        std::string v = load(e);
        emitf("uint32_t r = (%s & cpu.d[%d]) & 0x%Xu; logic%d(r);\n", v.c_str(), rx, mask_of(size), bits(size));
        emit(dreg_write(rx, size, "r"));
    }
}

// ---- group E: shifts and rotates -----------------------------------------------------------------
void Tx::shifts()
{
    static const char *names[2][4] = {{"asr", "lsr", "roxr", "ror"}, {"asl", "lsl", "roxl", "rol"}};
    int left = (op_ >> 8) & 1;
    if ((op_ & 0xC0) == 0xC0) {   // memory, word, by one
        if (op_ & 0x800) {
            bad_ = true;
            return;
        }
        int type = (op_ >> 9) & 3;
        Ea e = ea((op_ >> 3) & 7, op_ & 7, 2);
        if (e.kind != Ea::MEM) {
            bad_ = true;
            return;
        }
        std::string v = load(e);
        emitf("uint32_t r = %s16(%s, 1);\n", names[left][type], v.c_str());
        write(e, "r");
        return;
    }
    int size = 1 << ((op_ >> 6) & 3);
    int type = (op_ >> 3) & 3;
    int reg = op_ & 7, cnt = (op_ >> 9) & 7;
    std::string n = (op_ & 0x20) ? fmt("(cpu.d[%d] & 63)", cnt) : fmt("%d", cnt ? cnt : 8);
    emitf("uint32_t r = %s%d(cpu.d[%d], %s);\n", names[left][type], bits(size), reg, n.c_str());
    emit(dreg_write(reg, size, "r"));
}

Insn Tx::run()
{
    in_.pc = pc_;
    switch (op_ >> 12) {
    case 0x0: group0(); break;
    case 0x1: move(1); break;
    case 0x2: move(4); break;
    case 0x3: move(2); break;
    case 0x4: group4(); break;
    case 0x5: group5(); break;
    case 0x6: group6(); break;
    case 0x7: moveq(); break;
    case 0x8: group8(); break;
    case 0x9: addsub(false); break;
    case 0xA:
        raise(10, pc_);
        in_.flow = Insn::EXCEPT;
        break;
    case 0xB: groupB(); break;
    case 0xC: groupC(); break;
    case 0xD: addsub(true); break;
    case 0xE: shifts(); break;
    case 0xF:
        raise(11, pc_);
        in_.flow = Insn::EXCEPT;
        break;
    }
    in_.len = pos_ - pc_;
    if (bad_) {
        Insn i;
        i.pc = pc_;
        i.len = 2;
        i.flow = Insn::INTERP;
        return i;
    }
    in_.body = b_;
    return in_;
}

} // namespace

Insn translate(uint32_t pc, const Fetch16 &fetch)
{
    Tx t(pc, fetch);
    return t.run();
}
