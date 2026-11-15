// invkeep - the inventory keeps its built list between opens when nothing in it changed (a FIX, no key: Jay 2026-11-14, "I'd say this is
// more a fix than a feature"). Trace docs/traces/20261114_1415_inventory_keep_built.md; facts INV-526 addendum 2026-11-14. See invkeep.cpp.
#pragma once

namespace dlvr {

// Install the hooks (two call-site rewrites in MenuInventory's tab rebuild, MenuInventory's show slot, MainMenuModule(DI)'s ShowScreen
// slot). Idempotent; camhook's 1 Hz thread retries until the gamedll is mapped. One line per outcome.
void InvKeepInstall();

}  // namespace dlvr
