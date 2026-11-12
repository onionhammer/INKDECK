// The only translation unit that includes font data (the headers define const arrays, so
// including them elsewhere would duplicate the bitmaps).
#include "fonts.h"

#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSerif12pt7b.h>
#include <Fonts/FreeSerif18pt7b.h>
#include <Fonts/FreeSerif24pt7b.h>
#include <Fonts/FreeSerifBold12pt7b.h>
#include <Fonts/FreeSerifBold18pt7b.h>
#include <Fonts/FreeSerifBold24pt7b.h>
#include <Fonts/FreeSerifBoldItalic12pt7b.h>
#include <Fonts/FreeSerifBoldItalic18pt7b.h>
#include <Fonts/FreeSerifBoldItalic24pt7b.h>
#include <Fonts/FreeSerifItalic12pt7b.h>
#include <Fonts/FreeSerifItalic18pt7b.h>
#include <Fonts/FreeSerifItalic24pt7b.h>

namespace fonts {

const FontFamily kReader[kReaderSizeCount] = {
    {"Serif 12", {&FreeSerif12pt7b, &FreeSerifBold12pt7b, &FreeSerifItalic12pt7b,
                  &FreeSerifBoldItalic12pt7b}},
    {"Serif 18", {&FreeSerif18pt7b, &FreeSerifBold18pt7b, &FreeSerifItalic18pt7b,
                  &FreeSerifBoldItalic18pt7b}},
    {"Serif 24", {&FreeSerif24pt7b, &FreeSerifBold24pt7b, &FreeSerifItalic24pt7b,
                  &FreeSerifBoldItalic24pt7b}},
};

const GFXfont *const kUi = &FreeSans12pt7b;
const GFXfont *const kUiBold = &FreeSansBold12pt7b;
const GFXfont *const kUiSmall = &FreeSans9pt7b;
const GFXfont *const kUiLarge = &FreeSansBold18pt7b;

}  // namespace fonts
