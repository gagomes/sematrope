/* sematrope - superoptimizer using the z3 SMT solver
   Copyright (C) 2018  Falk Hüffner

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>. */

#include <string>
#include <vector>

#include <stdint.h>

#include <z3++.h>

constexpr int REGISTER_WIDTH = 32;
using uint64 = __uint64; // needs to match z3's definition

enum class Opcode {
    SUB,
    AND,
    CMPEQ,
    LAST = CMPEQ
};

template<typename T>
std::string hex(T x) {
    std::stringstream stream;
    stream << std::hex << uint64_t(x);
    return stream.str();
}

struct Insn {
    Opcode opcode;
    int r1, r2;
    bool isImm;
    uint64 imm;

    std::string toString(int dest) const {
	std::string s;
	switch (opcode) {
	case Opcode::SUB: s = "sub"; break;
	case Opcode::AND: s = "and"; break;
	case Opcode::CMPEQ: s = "cmpeq"; break;
	default: abort();
	}
	s += " r" + std::to_string(r1) + ", ";
	if (isImm)
	    s += "0x" + hex(imm);
	else
	    s += "r" + std::to_string(r2);
	s += ", r" + std::to_string(dest);
	return s;
    }
};

// The z3 variables that define an instruction.
struct SymbolicInsn {
    SymbolicInsn(z3::context& c, const std::string& prefix)
	: opcode(c.int_const((prefix + "_op").c_str())),
	  r1(c.int_const((prefix + "_r1").c_str())),
	  r2(c.int_const((prefix + "_r2").c_str())),
	  imm(c.bv_const((prefix + "_imm").c_str(), REGISTER_WIDTH)) {}
    z3::expr opcode;
    // r1 is the number of a register; r2 is the number of a register
    // or implies use of the immediate if the number is out of the
    // valid range.
    z3::expr r1, r2;
    z3::expr imm;
    // We use a SSA representation where the output register is always
    // implicitly a new register, thus it doesn't need to be specified
    // here.
};

z3::expr bvConst(uint64 x, z3::context& c) {
    return c.bv_val(x, REGISTER_WIDTH);
}

z3::expr boolToBv(const z3::expr& b, z3::context& c) {
    return z3::ite(b, bvConst(1, c), bvConst(0, c));
}

// Returns an expression representing the result of running the
// program in insns on the input value x.
z3::expr eval(const z3::expr& x, const std::vector<SymbolicInsn>& insns, z3::context& c) {
    std::vector<z3::expr> regs;
    regs.push_back(x);
    for (std::size_t i = 0; i < insns.size(); ++i) {
	z3::expr in1 = regs[i];
	for (int j = int(i) - 1; j >= 0; --j)
	    in1 = z3::ite(insns[i].r1 == j, regs[j], in1);
	z3::expr in2 = insns[i].imm;
	for (int j = int(i); j >= 0; --j)
	    in2 = z3::ite(insns[i].r2 == j, regs[j], in2);

	z3::expr result = in1 - in2; // SUB is the default
	result = z3::ite(insns[i].opcode == int(Opcode::AND), in1 & in2, result);
	result = z3::ite(insns[i].opcode == int(Opcode::CMPEQ), boolToBv(in1 == in2, c), result);
	regs.push_back(result);
    }
    return regs[insns.size()];
}

std::pair<
    std::vector<SymbolicInsn>,
    std::vector<z3::expr>
    >
makeInsns(int numInsns, z3::context& c) {
    std::vector<SymbolicInsn> insns;
    std::vector<z3::expr> constraints;
    for (int i = 0; i < numInsns; ++i) {
	insns.push_back(SymbolicInsn(c, std::string("op") + std::to_string(i)));
	constraints.push_back(z3::ult(insns.back().imm, bvConst(0xff, c)));
    }
    return {insns, constraints};
}

int getIntDefault(z3::expr e, int d = 0) {
    return e.is_numeral() ? e.get_numeral_int() : d;
}
uint64 getUint64Default(z3::expr e, uint64 d = 0) {
    return e.is_numeral() ? e.get_numeral_uint64() : d;
}

std::vector<Insn> reconstructProgram(const std::vector<SymbolicInsn>& insns, const z3::model& model) {
    std::vector<Insn> result;
    for (int i = 0; i < static_cast<int>(insns.size()); ++i) {
	Insn insn;
	int opcode = getIntDefault(model.eval(insns[i].opcode), 0);
	insn.opcode = (opcode < 0 || opcode > int(Opcode::LAST)) ? Opcode(0) : Opcode(opcode);
	int r1 = getIntDefault(model.eval(insns[i].r1), 0);
	insn.r1 = (r1 < 0 || r1 > i) ? i : r1;
	int r2 = getIntDefault(model.eval(insns[i].r2), 0);
	if (r2 < 0 || r2 > i) {
	    insn.isImm = true;
	    insn.r2 = 0;
	    insn.imm = getUint64Default(model.eval(insns[i].imm), 0);
	} else {
	    insn.isImm = false;
	    insn.r2 = r2;
	    insn.imm = 0;
	}
	result.push_back(insn);
    }
    return result;
}

z3::expr isPowerOfTwoOrZero(const z3::expr& x, z3::context& c) {
    auto r = x == 0;
    uint64 p = 1;
    for (int i = 0; i < REGISTER_WIDTH; ++i) {
	r = r || (x == bvConst(p, c));
	p <<= 1;
    }
    return boolToBv(r, c);
}

int main() {
    const auto targetProgram = isPowerOfTwoOrZero;

    try {
	std::vector<uint64> testCases;
	for (int numInsns = 1; ; ++numInsns) {
	    std::cerr << "\n=== Trying with " << numInsns << " instructions ===\n\n";
	    while (true) {
		z3::context c;
		std::cerr << "Finding program with " << numInsns
			  << " instructions that is correct for all " << testCases.size() << " test cases...\n";
		auto solver = z3::solver(c);
		const auto [insns, constraints] = makeInsns(numInsns, c);
		for (const auto& c : constraints)
		    solver.add(c);
		for (const auto t : testCases) {
		    const uint64 correctResult = targetProgram(bvConst(t, c), c).simplify().get_numeral_uint64();
		    const auto programResult = eval(bvConst(t, c), insns, c);
		    solver.add(programResult == bvConst(correctResult, c));
		}
		const auto result = solver.check();
		if (result != z3::sat) {
		    if (result != z3::unsat)
			throw z3::exception("unexpected check value");
		    std::cerr << "Not possible to find program that is correct for all " << testCases.size() << " test cases.\n";
		    break;
		}

		std::cerr << "Found program:\n";
		const auto& model = solver.get_model();
		const auto x = c.bv_const("x", REGISTER_WIDTH);
		const auto solutionProgram = model.eval(eval(x, insns, c));
		const auto program = reconstructProgram(insns, model);
		for (std::size_t i = 0; i < program.size(); ++i)
		    std::cerr << program[i].toString(i + 1) << std::endl;

		std::cerr << "\nFinding counterexample...\n";
		auto cesolver = z3::solver(c);
		cesolver.add(solutionProgram != targetProgram(x, c));
		const auto ceResult = cesolver.check();
		if (ceResult != z3::sat) {
		    if (ceResult != z3::unsat)
			throw z3::exception("unexpected check value");
		    std::cerr << "No counterexample found. Correct program is:\n";
		    for (std::size_t i = 0; i < program.size(); ++i)
			std::cout << program[i].toString(i + 1) << std::endl;
		    return 0;
		}
		const auto& cemodel = cesolver.get_model();
		auto t = cemodel.eval(x).get_numeral_uint64();
		std::cerr << "Found counterexample: " << t
			  << " evals to " << cemodel.eval(solutionProgram).get_numeral_uint64()
			  << " but should be " << targetProgram(bvConst(t, c), c).simplify().get_numeral_uint64()
			  << std::endl;
		testCases.push_back(t);
	    }
	}
    } catch (const z3::exception& e) {
	std::cerr << e.msg() << std::endl;
    }

    return 0;
}
