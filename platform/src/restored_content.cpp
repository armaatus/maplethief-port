// Restored content (#389); see restored_content.h.
#include "restored_content.h"

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

} // namespace maplethief::frontend
