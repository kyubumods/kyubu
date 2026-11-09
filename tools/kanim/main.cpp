// kanim: the command-line checker of ability clips (openspec add-ability-animation, task 3.6). Needs no game.
//   kanim check <clip> [--json]
//   kanim describe <clip> [--ability-ms N] [--charge N] [--speed N] [--rank N]
//   kanim sample <clip> [--t 0,0.25,0.5] [--ability-ms N] [--charge N] [--speed N] [--rank N]
//   kanim bake <clip> [--ability-ms N] [--charge N] [--speed N] [--rank N]
// Exit codes: 0 done, 1 the clip is invalid (the errors are printed), 2 a usage error or a file that cannot be read.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "kanim_core.h"

namespace {

const char* const kUsage =
    "usage: kanim check <clip> [--json]\n"
    "       kanim describe <clip> [--ability-ms N] [--charge N] [--speed N] [--rank N]\n"
    "       kanim sample <clip> [--t 0,0.25,0.5] [--ability-ms N] [--charge N] [--speed N] [--rank N]\n"
    "       kanim bake <clip> [--ability-ms N] [--charge N] [--speed N] [--rank N]\n";

int usage(const std::string& why) {
    if (!why.empty()) std::fprintf(stderr, "kanim: %s\n", why.c_str());
    std::fputs(kUsage, stderr);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);   // keep "\n": the output is the same on every host
#endif
    if (argc < 3) return usage("");
    const std::string cmd = argv[1];
    const std::string path = argv[2];
    kyubu::kanim::Options opt;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--json") {
            opt.json = true;
        } else if (a == "--charge" || a == "--speed" || a == "--rank") {
            if (i + 1 >= argc) return usage(a + " needs a value");
            const std::string v = argv[++i];
            char* end = nullptr;
            double x = std::strtod(v.c_str(), &end);
            if (end == v.c_str() || *end != '\0' || !(x >= 0 && x <= 1e6)) return usage(a + " takes a number from 0 up");
            (a == "--charge" ? opt.signals.charge : a == "--speed" ? opt.signals.speed : opt.signals.rank) = static_cast<float>(x);
        } else if (a == "--ability-ms" || a == "--t") {
            if (i + 1 >= argc) return usage(a + " needs a value");
            const std::string v = argv[++i];
            if (a == "--t") {
                if (!kyubu::kanim::parse_times(v, opt.times)) return usage("--t takes numbers from 0 to 1, such as 0,0.5");
            } else {
                char* end = nullptr;
                double ms = std::strtod(v.c_str(), &end);
                if (end == v.c_str() || *end != '\0' || !(ms > 0)) return usage("--ability-ms takes a number above 0");
                opt.ability_ms = static_cast<float>(ms);
            }
        } else {
            return usage("unknown option " + a);
        }
    }

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "kanim: cannot read %s\n", path.c_str());
        return 2;
    }
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    kyubu::kanim::Result r;
    if (cmd == "check") r = kyubu::kanim::check(text, path, opt);
    else if (cmd == "describe") r = kyubu::kanim::describe(text, path, opt);
    else if (cmd == "sample") r = kyubu::kanim::sample(text, path, opt);
    else if (cmd == "bake") r = kyubu::kanim::bake(text, path, opt);
    else return usage("unknown command " + cmd);
    std::fwrite(r.text.data(), 1, r.text.size(), stdout);
    return r.code;
}
