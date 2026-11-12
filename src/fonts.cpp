#include "fonts.h"

#include "ui/fonts/jbmono.h"
#include "ui/fonts/literata.h"

namespace fonts {

using namespace ui::fonts;

const FontFamily kReader[kReaderSizeCount] = {
    {"Literata 16", {&kLiterata16, &kLiterata16Bold, &kLiterata16Italic, &kLiterata16BoldItalic},
     26},
    {"Literata 19", {&kLiterata19, &kLiterata19Bold, &kLiterata19Italic, &kLiterata19BoldItalic},
     30},
    {"Literata 23", {&kLiterata23, &kLiterata23Bold, &kLiterata23Italic, &kLiterata23BoldItalic},
     36},
};

const GFXfont *const kDropCap = &kMono68ExtraBold;

}  // namespace fonts
