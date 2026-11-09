// my_mod: native Kyubu mod template. See docs/native-api.md.
#include "kyubu.hpp"

struct MyMod {
    // Called once per logical side listed in KYUBU_MOD below (twice in single-player).
    explicit MyMod(kyubu::Mod& m) : mod(m) {
        mod.info("my_mod loaded on the %s side", m.side() == KY_SIDE_CLIENT ? "client" : "server");

        // Registries are shared by both sides of a process; adding the same entry on each side is fine.
        mod.on("lifecycle.register", [this](KyubuEvent&) {
            mod.add_model("my_mod:cube", "my_mod-cube.cub");   // assets/models/my_mod-cube.vox
        });
        mod.on("lifecycle.game_ready", [this](KyubuEvent&) {
            mod.info("my_mod:cube is model slot %u", mod.model_slot("my_mod:cube"));
        });

        if (m.side() == KY_SIDE_CLIENT) {
            mod.on("input.key", [this](KyubuEvent& ev) {
                auto* k = kyubu::Mod::data<KyubuKeyData>(ev);
                // an option from config.toml; players change it in kyubu/config/my_mod.toml
                if (k->vk == 0x77 /* F8 */ && k->down && mod.config_bool("log_f8", true)) mod.info("F8 pressed");
            });
        }
    }
    // Called on shutdown (once per side).
    ~MyMod() { mod.info("my_mod shutting down"); }

    kyubu::Mod& mod;
};

KYUBU_MOD("my_mod", KY_SIDE_CLIENT | KY_SIDE_SERVER, MyMod)
