// What the assistant can do on the board itself (voice_client.h, docs/voice-protocol.md "Device
// tools"). The voice client announces these tools in its hello, and inkharness hands the model's
// calls back down the connection. A call arrives in the voice task and runs here in the UI task
// (pollVoiceTools() from loop()), then the answer goes back through the voice task.
//
//   find_books {query?}       the EPUBs in SD:/Books and the folders below: path, how far read
//   open_book {path}          a book at its saved place
//   weather {}                the board's own forecast: now, the next 12 hours, the coming days
//   board_status {}           local time, battery, the room, volume, what's on screen and
//                             playing, the alarms, the last book
//   set_volume {volume}       0-100
//   set_alarm {time, repeat, on, slot}
//   show {screen}             home, clock, weather, music, podcasts, sensors, reader
//
// Tools that change the screen (open_book, show) wait until the reply has been said. The call
// answers at once that it will happen. When the reply has finished playing, the voice sheet (or
// the Voice screen) closes, and the screen opens from Home. A turn cut short drops the change:
// talking again, closing the sheet, or a lost connection. Only in a build with INKDECK_VOICE
// (features.h).
#pragma once

namespace apps {

void beginVoiceTools();  // from beginHoldBoot(), with the voice client
void pollVoiceTools();   // from loop(): runs the calls that came in. Does nothing without voice

}  // namespace apps
