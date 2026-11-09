// The bounded expression of an `expr` track (openspec add-ability-animation, design D11 "Procedural tracks").
// Text in, a program out; a program and its inputs in, a number out. No game, no hooks, no Lua, no windows.h: the validator
// (animation_load.cpp), the evaluator (animation_eval.cpp), `kanim` and the Animator all link this one source, so they agree.
//
// What an expression is (the rules the design leaves open; recorded in the notes of 8.2):
//  - Numbers, the names below, `+ - * /`, unary `-`, parentheses and the calls of the whitelist. Nothing else: there is no loop, no
//    assignment, no user function, so no recursion either. A `for`, `while` or `if` is an unknown name or function.
//  - Names: `u` (the clip's time as 0..1), `t` (the clip's time in ms, from the start of the lap), and the three signals `charge`
//    (the creature's charged_mp), `speed` (the creature's ground speed, voxels per second) and `rank` (the local player's rank in the
//    ability, 1 for anyone else and for the Animator's preview).
//  - Functions: sin(deg) cos(deg) abs(x) min(a,b) max(a,b) clamp(x,lo,hi) lerp(a,b,f) smoothstep(e0,e1,x) pow(a,b) noise(x).
//    Angles are in DEGREES, like every rotation of a clip. noise is smooth value noise from -1 to 1, one lattice point per unit.
//  - Bounded: at most kMaxExprNodes nodes (a number, a name, a minus, an operator and a call are one node each), at most
//    kMaxExprBytes of text and kMaxExprDepth of nesting. Parsed once at load. A result that is not finite (a division by zero, a huge
//    pow) is 0, so an expression never gives the game a NaN. The same text, the same inputs and the same build give the same number.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace kyubu::game::anim {

constexpr int kMaxExprNodes = 64;
constexpr int kMaxExprBytes = 256;
constexpr int kMaxExprDepth = 32;   // parser levels: a parenthesis or a call argument costs two

// What the game tells a clip at draw time besides the time (design D11): the signals whose fields are verified in the mapping
// (`cube::CreatureData.charged_mp`, `.velocity`; the rank is the ranks file of add-ability-api).
struct Signals {
    float charge = 0;   // the creature's charged_mp
    float speed = 0;    // the length of the creature's velocity on the ground plane, voxels per second
    float rank = 1;     // the local player's rank in the bound ability; 1 for another creature and for a preview
};

// A name an expression can use, in the order of `kExprNames`.
enum ExprUse : uint32_t {
    kUsesU = 1u << 0, kUsesT = 1u << 1, kUsesCharge = 1u << 2, kUsesSpeed = 1u << 3, kUsesRank = 1u << 4,
};
// The names, for messages: "u, t, charge, speed, rank".
const char* expr_name_list();
// The functions, for messages: "sin, cos, abs, ...".
const char* expr_function_list();

// One node of the program, in postfix order. The program is a flat array, so a copy of a clip copies it whole.
struct ExprNode {
    uint8_t op = 0;       // an ExprOp (animation_expr.cpp)
    uint8_t arity = 0;    // the operands a call or an operator takes
    uint8_t fn = 0;       // the function of a call
    float value = 0;      // the number of a literal
};

struct ExprProgram {
    std::vector<ExprNode> code;   // empty: no program (the value is 0)
    uint32_t uses = 0;            // ExprUse bits: which names the text mentions
    bool empty() const { return code.empty(); }
};

struct ExprError {
    int column = 0;       // 1-based byte column of the text where the problem starts
    std::string code;     // bad_expression, unknown_name, unknown_function, bad_arg_count, expr_too_big
    std::string message;  // one sentence, without the column
    std::string hint;     // how to fix it
};

// Parses and checks one expression. False, with `err` set and `out` left empty, for any text outside the rules above.
bool compile_expr(const std::string& text, ExprProgram& out, ExprError& err);

// Runs a program. `u` is the clip's time in 0..1 and `t_ms` its time in ms. An empty program gives 0. Never throws, never loops.
float eval_expr(const ExprProgram& p, float u, float t_ms, const Signals& s);

}  // namespace kyubu::game::anim
