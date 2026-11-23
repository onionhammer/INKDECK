// HOLD BOOT, the navigator's second BOOT gesture (ui::nav::onTalkKey()):
//   - on the dialer's ringing sheet, in a build with comms (INKDECK_COMMS, features.h): a
//     message for the ring nobody answered (its talkTo() says so, apps/comms_ui.h). It records
//     from the input timer as the hold registers, and letting go sends it. Elsewhere on the
//     Comms screens HOLD BOOT is the agent's, as anywhere (their mic has its own switch)
//   - while music or a podcast plays (not paused): the player sheet (apps::openPlayer()), and
//     HOLD BOOT on the sheet closes it
//   - otherwise, in a build with the agent (INKDECK_AGENT, features.h): talk. The mic
//     opens from the input timer as the hold registers, and the agent sheet rises (apps/agent_sheet.h)
//   - otherwise nothing
#pragma once

namespace apps {

// From setup(): starts the agent client's task and the board's tools for it (with the agent), and
// installs the handlers.
void beginHoldBoot();

}  // namespace apps
