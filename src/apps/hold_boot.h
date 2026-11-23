// HOLD BOOT, the navigator's second BOOT gesture (ui::nav::onTalkKey()):
//   - in a build with comms (INKDECK_COMMS, features.h), where a screen's talkTo() says comms
//     (apps/comms_ui.h): on the dialer's ringing sheet, a message for the ring nobody answered;
//     with the card's mesh_mic = push, this board's mic while held (the talk screen and the sheets
//     over it, the call sheet, a board's mic screen, the ring sheet: answered first). Either
//     starts from the input timer as the hold registers, and letting go sends the message or
//     switches the mic off. Sticky (the default), HOLD BOOT on the Comms screens is the agent's,
//     as anywhere (● switches the mic)
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
