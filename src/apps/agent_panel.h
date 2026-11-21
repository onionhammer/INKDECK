// A screen a session drew (show_panel; issue #52, "Screens the agent draws"; docs/voice-
// protocol.md, "Panels"): its blocks top to bottom in the board's own type and 1-bit rules,
// under the status bar and hints. A kind the board doesn't know is skipped. Opened from the
// session (●), and on its own the first time a session draws one while its screen is up.
//   no choice:   ▲▼ scroll by block   HOLD BOOT talk into the session   BOOT back
//   a choice:    ▲▼ choose (past the first or last, scroll)   ● send it   BOOT back
// A `choice` block stays pinned at the foot; its answer goes to the session as words. The same
// panel drawn again updates in place. Only in a build with INKDECK_AGENT (features.h).
#pragma once
#include <memory>
#include <string>

#include "ui/nav.h"

namespace apps {

std::unique_ptr<ui::Screen> makeAgentPanel(const std::string &session);

}  // namespace apps
