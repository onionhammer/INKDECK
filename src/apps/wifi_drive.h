// WI-FI DRIVE (System's actions): the microSD card as a network drive on the computer
// (webdav.h). Under System's tag, context WI-FI DRIVE, laid out like the Updater: a // STATE panel
// with the phase in an inverted pill (JOINING, READY, CONNECTED, READING, WRITING, FAILED,
// STOPPED), the address to connect to as a 38 px headline (inkdeck.local) and the IP and network
// under it, then // SIGN IN (user and password; DIGEST on the right), // TRANSFER (written, read,
// the last change; the rate on the right while data moves, else when the idle stop comes) and
// // ON THE COMPUTER (how Finder and Windows connect).
//
// Opening it starts the server and leaving it stops it; the board's own use of the card carries
// on meanwhile. BOOT leaves at once, unless data moved in the last few seconds: then a sheet asks
// first (// STOP SHARING?, CANCEL or STOP, `COPY UNDER WAY`), since the copy on the computer
// would fail. Stopped or failed: ● starts it again. If the computer changed SD:/inkdeck.ini or
// SD:/Podcasts, leaving reads them again, as a boot would.
#pragma once
#include <memory>

#include "ui/nav.h"

namespace apps {

std::unique_ptr<ui::Screen> makeWifiDrive();

}  // namespace apps
