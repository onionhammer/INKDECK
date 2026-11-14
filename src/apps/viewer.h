// Picture viewer: a JPEG or PNG from Files, full screen. Not a module (no Home row, it wears
// Files' tag); Files opens it on a picture, and the dial steps through the folder's others.
//   - fitted to the whole panel and centred on paper, never grown more than 2x;
//   - portrait or landscape (the wheel on top, as for the Clock) as the board is held, read from
//     the IMU, and turning the board turns the picture within a couple of seconds, on battery
//     too (the IMU's wake on motion wakes the nap). Lying flat, by the picture's shape:
//     landscape when it's wider than tall;
//   - the chrome (status bar with "03/12 name.jpg", key hints) sits over the picture, and ●
//     hides or shows it;
//   - ▲ previous   ▼ next   ● hide / show the chrome   BOOT back to Files, on the last picture.
// A JPEG's EXIF orientation is applied (picture::renderFile). Counts as reading (Screen::
// reading()): on battery the board naps under it rather than powering off after 10 minutes.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/nav.h"

namespace apps {

// paths: the folder's pictures, in list order; index: the one to show first. shown, if given, is
// called with each picture's path as it comes up (Files puts its cursor there).
std::unique_ptr<ui::Screen> makeViewer(std::vector<std::string> paths, int index,
                                       std::function<void(const std::string &)> shown = {});

}  // namespace apps
