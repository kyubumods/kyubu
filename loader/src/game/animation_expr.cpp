// The bounded expression of an `expr` track: see animation_expr.h. No game.
#include "animation_expr.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace kyubu::game::anim {
namespace {

enum ExprOp : uint8_t { OpNum, OpU, OpT, OpCharge, OpSpeed, OpRank, OpNeg, OpAdd, OpSub, OpMul, OpDiv, OpCall };

enum Fn : uint8_t { FnSin, FnCos, FnAbs, FnMin, FnMax, FnClamp, FnLerp, FnSmoothstep, FnPow, FnNoise, FnCount };
struct FnInfo {
    const char* name;
    uint8_t arity;
};
const FnInfo kFns[FnCount] = {{"sin", 1}, {"cos", 1}, {"abs", 1},        {"min", 2}, {"max", 2},
                              {"clamp", 3}, {"lerp", 3}, {"smoothstep", 3}, {"pow", 2}, {"noise", 1}};

struct NameInfo {
    const char* name;
    ExprOp op;
    uint32_t use;
};
const NameInfo kNames[] = {{"u", OpU, kUsesU}, {"t", OpT, kUsesT}, {"charge", OpCharge, kUsesCharge},
                           {"speed", OpSpeed, kUsesSpeed}, {"rank", OpRank, kUsesRank}};

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_name_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool is_name_char(char c) { return is_name_start(c) || is_digit(c); }

// A value that is not finite is 0: a division by zero or a huge pow must not reach the game as a NaN.
double fin(double v) { return std::isfinite(v) ? v : 0.0; }

uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
double lattice(int32_t i) { return static_cast<double>(hash32(static_cast<uint32_t>(i))) / 4294967295.0 * 2.0 - 1.0; }
// Smooth value noise from -1 to 1: a hashed value at each whole number, a smoothstep between them.
double noise1(double x) {
    if (!(x > -1e9 && x < 1e9)) return 0;
    const double fl = std::floor(x);
    const double f = x - fl;
    const int32_t i = static_cast<int32_t>(fl);
    const double s = f * f * (3 - 2 * f);
    const double a = lattice(i);
    return a + (lattice(i + 1) - a) * s;
}

double smoothstep3(double e0, double e1, double x) {
    if (e0 == e1) return x < e0 ? 0.0 : 1.0;
    double s = (x - e0) / (e1 - e0);
    s = s < 0 ? 0 : (s > 1 ? 1 : s);
    return s * s * (3 - 2 * s);
}

double call(uint8_t fn, const double* a) {
    switch (fn) {
        case FnSin: return std::sin(a[0] * kDegToRad);
        case FnCos: return std::cos(a[0] * kDegToRad);
        case FnAbs: return std::fabs(a[0]);
        case FnMin: return a[0] < a[1] ? a[0] : a[1];
        case FnMax: return a[0] > a[1] ? a[0] : a[1];
        case FnClamp: {
            double v = a[0] < a[1] ? a[1] : a[0];
            return v > a[2] ? a[2] : v;
        }
        case FnLerp: return a[0] + (a[1] - a[0]) * a[2];
        case FnSmoothstep: return smoothstep3(a[0], a[1], a[2]);
        case FnPow: return std::pow(a[0], a[1]);
        case FnNoise: return noise1(a[0]);
    }
    return 0;
}

std::string quoted(const std::string& s) { return "`" + s + "`"; }

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    bool run(ExprProgram& out, ExprError& err) {
        if (s_.size() > static_cast<size_t>(kMaxExprBytes)) {
            fail(0, "expr_too_big", "the text has " + std::to_string(s_.size()) + " bytes; the most is " + std::to_string(kMaxExprBytes),
                 "Shorten the expression, or split the motion over two tracks.");
        } else {
            skip();
            if (pos_ >= s_.size())
                fail(pos_, "bad_expression", "the expression is empty",
                     "Write a number, such as 0, or an expression such as sin(u * 360) * 5.");
            else if (parse_sum() && !failed_) {
                skip();
                if (pos_ < s_.size()) fail(pos_, "bad_expression", "unexpected " + describe_at(pos_), "Remove it, or add an operator before it.");
            }
        }
        if (failed_) {
            err = err_;
            out = ExprProgram();
            return false;
        }
        out = prog_;
        return true;
    }

private:
    const std::string& s_;
    size_t pos_ = 0;
    int depth_ = 0;
    bool failed_ = false;
    ExprProgram prog_;
    ExprError err_;

    bool fail(size_t at, const char* code, std::string message, std::string hint) {
        if (!failed_) {
            failed_ = true;
            err_.column = static_cast<int>(at) + 1;
            err_.code = code;
            err_.message = std::move(message);
            err_.hint = std::move(hint);
        }
        return false;
    }

    void skip() {
        while (pos_ < s_.size() && is_space(s_[pos_])) ++pos_;
    }

    // "`)`" or "the end of the text", for a message.
    std::string describe_at(size_t at) const {
        if (at >= s_.size()) return "the end of the text";
        const unsigned char c = static_cast<unsigned char>(s_[at]);
        if (c < 0x20 || c >= 0x7F) return "a character that an expression does not use";
        return quoted(std::string(1, static_cast<char>(c)));
    }

    bool emit(size_t at, ExprOp op, uint8_t arity = 0, uint8_t fn = 0, float value = 0) {
        if (prog_.code.size() >= static_cast<size_t>(kMaxExprNodes))
            return fail(at, "expr_too_big", "the expression has more than " + std::to_string(kMaxExprNodes) + " nodes",
                        "Shorten it. A number, a name, a minus, an operator and a call are one node each.");
        ExprNode n;
        n.op = op;
        n.arity = arity;
        n.fn = fn;
        n.value = value;
        prog_.code.push_back(n);
        return true;
    }

    struct Depth {
        Parser& p;
        bool ok;
        Depth(Parser& parser, size_t at) : p(parser), ok(true) {
            if (++p.depth_ > kMaxExprDepth) {
                ok = false;
                p.fail(at, "expr_too_big", "the expression nests deeper than " + std::to_string(kMaxExprDepth) + " levels",
                       "Take parentheses or calls out of each other.");
            }
        }
        ~Depth() { --p.depth_; }
    };

    // sum := product (('+' | '-') product)*
    bool parse_sum() {
        Depth d(*this, pos_);
        if (!d.ok || !parse_product()) return false;
        for (;;) {
            skip();
            if (pos_ >= s_.size() || (s_[pos_] != '+' && s_[pos_] != '-')) return true;
            const size_t at = pos_;
            const ExprOp op = s_[pos_++] == '+' ? OpAdd : OpSub;
            if (!parse_product() || !emit(at, op, 2)) return false;
        }
    }

    // product := unary (('*' | '/') unary)*
    bool parse_product() {
        if (!parse_unary()) return false;
        for (;;) {
            skip();
            if (pos_ >= s_.size() || (s_[pos_] != '*' && s_[pos_] != '/')) return true;
            const size_t at = pos_;
            const ExprOp op = s_[pos_++] == '*' ? OpMul : OpDiv;
            if (!parse_unary() || !emit(at, op, 2)) return false;
        }
    }

    // unary := '-' unary | primary
    bool parse_unary() {
        Depth d(*this, pos_);
        if (!d.ok) return false;
        skip();
        if (pos_ < s_.size() && s_[pos_] == '-') {
            const size_t at = pos_++;
            return parse_unary() && emit(at, OpNeg, 1);
        }
        return parse_primary();
    }

    bool parse_number() {
        const size_t start = pos_;
        while (pos_ < s_.size() && is_digit(s_[pos_])) ++pos_;
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            while (pos_ < s_.size() && is_digit(s_[pos_])) ++pos_;
        }
        // An exponent only when digits follow it; otherwise the `e` is left for the "unexpected" message.
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            size_t q = pos_ + 1;
            if (q < s_.size() && (s_[q] == '+' || s_[q] == '-')) ++q;
            if (q < s_.size() && is_digit(s_[q])) {
                while (q < s_.size() && is_digit(s_[q])) ++q;
                pos_ = q;
            }
        }
        const std::string lit = s_.substr(start, pos_ - start);
        if (lit == ".")
            return fail(start, "bad_expression", "a `.` with no digits is not a number", "Write the number with digits, such as 0.5.");
        const double v = std::strtod(lit.c_str(), nullptr);
        if (!std::isfinite(v) || std::fabs(v) > 1e9)
            return fail(start, "bad_expression", "the number " + quoted(lit) + " is too large", "Keep numbers below 1000000000.");
        return emit(start, OpNum, 0, 0, static_cast<float>(v));
    }

    bool parse_name() {
        const size_t start = pos_;
        while (pos_ < s_.size() && is_name_char(s_[pos_])) ++pos_;
        const std::string name = s_.substr(start, pos_ - start);
        skip();
        const bool is_call = pos_ < s_.size() && s_[pos_] == '(';
        int fn = -1;
        for (int i = 0; i < FnCount; ++i)
            if (name == kFns[i].name) fn = i;
        if (is_call) {
            if (fn < 0)
                return fail(start, "unknown_function", "unknown function " + quoted(name) + "; the functions are " + expr_function_list(),
                            "Use one of: " + std::string(expr_function_list()) + ".");
            return parse_call(start, static_cast<uint8_t>(fn));
        }
        if (fn >= 0)
            return fail(start, "bad_expression", quoted(name) + " is a function; write " + name + "(...) with its arguments",
                        "Call it with " + std::to_string(kFns[fn].arity) + " argument" + (kFns[fn].arity > 1 ? "s" : "") + ".");
        for (const NameInfo& n : kNames)
            if (name == n.name) {
                prog_.uses |= n.use;
                return emit(start, n.op);
            }
        return fail(start, "unknown_name", "unknown name " + quoted(name) + "; the names are " + expr_name_list(),
                    "Use one of: " + std::string(expr_name_list()) + ".");
    }

    // The '(' is at pos_.
    bool parse_call(size_t name_at, uint8_t fn) {
        ++pos_;   // '('
        int args = 0;
        skip();
        if (pos_ < s_.size() && s_[pos_] == ')') {
            ++pos_;
        } else {
            for (;;) {
                if (!parse_sum()) return false;
                ++args;
                skip();
                if (pos_ < s_.size() && s_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < s_.size() && s_[pos_] == ')') {
                    ++pos_;
                    break;
                }
                return fail(pos_, "bad_expression", "expected `,` or `)`, found " + describe_at(pos_),
                            "Separate the arguments with commas and close the call with `)`.");
            }
        }
        if (args != kFns[fn].arity)
            return fail(name_at, "bad_arg_count",
                        quoted(kFns[fn].name) + " takes " + std::to_string(kFns[fn].arity) + " argument" + (kFns[fn].arity > 1 ? "s" : "") +
                            ", not " + std::to_string(args),
                        "Write " + std::string(kFns[fn].name) + " with " + std::to_string(kFns[fn].arity) + " argument" +
                            (kFns[fn].arity > 1 ? "s" : "") + ".");
        return emit(name_at, OpCall, kFns[fn].arity, fn);
    }

    bool parse_primary() {
        skip();
        if (pos_ >= s_.size())
            return fail(pos_, "bad_expression", "expected a number, a name or `(`, found the end of the text",
                        "Finish the expression: an operator needs a value after it.");
        const char c = s_[pos_];
        if (is_digit(c) || c == '.') return parse_number();
        if (is_name_start(c)) return parse_name();
        if (c == '(') {
            ++pos_;
            if (!parse_sum()) return false;
            skip();
            if (pos_ >= s_.size() || s_[pos_] != ')')
                return fail(pos_, "bad_expression", "expected `)`, found " + describe_at(pos_), "Close the parenthesis.");
            ++pos_;
            return true;
        }
        return fail(pos_, "bad_expression", "expected a number, a name or `(`, found " + describe_at(pos_),
                    "An expression has numbers, the names " + std::string(expr_name_list()) + ", + - * /, and calls.");
    }
};

}  // namespace

const char* expr_name_list() { return "u, t, charge, speed, rank"; }
const char* expr_function_list() { return "sin, cos, abs, min, max, clamp, lerp, smoothstep, pow, noise"; }

bool compile_expr(const std::string& text, ExprProgram& out, ExprError& err) {
    Parser p(text);
    return p.run(out, err);
}

float eval_expr(const ExprProgram& p, float u, float t_ms, const Signals& sig) {
    double st[kMaxExprNodes];
    int sp = 0;
    for (const ExprNode& n : p.code) {
        switch (n.op) {
            case OpNum: if (sp >= kMaxExprNodes) return 0; st[sp++] = n.value; break;
            case OpU: if (sp >= kMaxExprNodes) return 0; st[sp++] = u; break;
            case OpT: if (sp >= kMaxExprNodes) return 0; st[sp++] = t_ms; break;
            case OpCharge: if (sp >= kMaxExprNodes) return 0; st[sp++] = sig.charge; break;
            case OpSpeed: if (sp >= kMaxExprNodes) return 0; st[sp++] = sig.speed; break;
            case OpRank: if (sp >= kMaxExprNodes) return 0; st[sp++] = sig.rank; break;
            case OpNeg:
                if (sp < 1) return 0;
                st[sp - 1] = -st[sp - 1];
                break;
            case OpAdd: case OpSub: case OpMul: case OpDiv: {
                if (sp < 2) return 0;
                const double b = st[--sp];
                const double a = st[sp - 1];
                double r = 0;
                if (n.op == OpAdd) r = a + b;
                else if (n.op == OpSub) r = a - b;
                else if (n.op == OpMul) r = a * b;
                else r = b == 0 ? 0.0 : a / b;
                st[sp - 1] = fin(r);
                break;
            }
            case OpCall: {
                if (n.arity < 1 || n.arity > 3 || sp < n.arity || n.fn >= FnCount) return 0;
                sp -= n.arity;
                st[sp] = fin(call(n.fn, st + sp));
                ++sp;
                break;
            }
            default: return 0;
        }
    }
    if (sp != 1) return 0;
    const float r = static_cast<float>(fin(st[0]));   // a double beyond a float is inf here
    return std::isfinite(r) ? r : 0.0f;
}

}  // namespace kyubu::game::anim
