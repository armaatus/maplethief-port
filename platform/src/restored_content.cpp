// Restored content (#389); see restored_content.h.
#include "restored_content.h"

#include "maplethief/cf.h"
#include "maplethief/hooks.h"
#include "port_text.h"

namespace maplethief::frontend {

void install_restored_shops(bool on) {
    // tr()'s texts stay valid until reload_port_text(); the runtime copies them.
    const mt_restored_shop_texts texts = {tr(Txt::restored_shop_line), tr(Txt::restored_shop_usual),
                                          tr(Txt::restored_shop_0), tr(Txt::restored_shop_1),
                                          tr(Txt::restored_shop_16)};
    mt_hooks_install_restored_shops(on ? 1 : 0, &texts);
}

void install_restored_carnival(bool on) {
    mt_hooks_install_restored_carnival(on ? 1 : 0);
    // The Spiegelman scripts' literal (Korean) texts in the game's language, from the keys whose
    // Korean value they are: each script's own chapter is the strings' ChapterN (#427).
    static const char *const prefixes[] = {"Chapter1_MonsterCarnival_Spiegelman.",
                                           "Chapter2_MonsterCarnival_Spiegelman.",
                                           "Chapter3_MonsterCarnival_Spiegelman.", nullptr};
    mt_cf_set_literal_key_prefixes(on ? prefixes : nullptr);
}

void install_restored_maps(bool on) { mt_hooks_install_restored_maps(on ? 1 : 0); }

} // namespace maplethief::frontend
