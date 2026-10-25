// Translates one 68000 instruction into C statements for recomp_rt.h.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

struct Insn {
    uint32_t pc = 0, len = 2;
    enum Flow {
        FALL,      // carries on to pc + len
        JUMP,      // always goes to target
        BRANCH,    // goes to target when cond holds, else falls through
        DYNAMIC,   // next pc is the value of `dyn`, computed by the body
        SYNC,      // falls through, but the dispatcher must look at interrupts first
        INTERP,    // the translator can't handle it: run it in the interpreter
        EXCEPT,    // body always raises an exception (cpu.pc already set)
    } flow = FALL;
    bool call = false;      // JUMP/DYNAMIC that pushed pc + len as a return address
    uint32_t target = 0;
    std::string body;       // C statements (may `return` after raising an exception)
    std::string cond;       // BRANCH condition, evaluated after the body
    std::string dyn;        // DYNAMIC: expression for the next pc
};

using Fetch16 = std::function<uint16_t(uint32_t)>;

Insn translate(uint32_t pc, const Fetch16 &fetch);
