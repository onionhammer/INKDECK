/* Ours, not upstream's. libsmb2's sources start with #include "config.h" (HAVE_CONFIG_H), and
 * port/ comes first on their include path (library.json), so this is the one they get. It adds
 * what upstream's CMake build defines on the compiler command line, which library.json can't
 * pass through a shell portably (the parentheses), then reads upstream's ESP32 configuration. */
#ifndef LIBSMB2_PORT_CONFIG_H
#define LIBSMB2_PORT_CONFIG_H
#define _U_ __attribute__((unused))
#include_next "config.h" /* include/esp/config.h */
#endif
