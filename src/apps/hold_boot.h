// HOLD BOOT, the navigator's second BOOT gesture (ui::nav::onTalkKey()):
//   - on the Comms screens and the call sheet, in a build with comms (INKDECK_COMMS,
//     features.h): push-to-talk to the boards, the group or the board selected (their
//     talkTo() says which, apps/comms_ui.h). The mic opens from the input timer as the hold
//     registers, and letting go sends
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
