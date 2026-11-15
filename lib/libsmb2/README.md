# libsmb2 (vendored)

The SMB2/3 client library behind SMB shares in Files (`src/smb_backend.cpp`), by Ronnie
Sahlberg: <https://github.com/sahlberg/libsmb2>, LGPL-2.1 or later (`COPYING`,
`LICENCE-LGPL-2.1.txt`).

Copied from commit `177a9de` (2026-11-14): the client sources its ESP-IDF build compiles
(`SMB2_CORE_SOURCES` in upstream `lib/CMakeLists.txt`, without the Kerberos wrapper) into
`src/`, the public headers into `include/smb2/`, the shared private headers into `include/`, and
upstream's ESP32 configuration as `include/esp/config.h`. Nothing in them is changed.
`library.json` builds them as a PlatformIO library, with warnings off (`-w`), since they're
upstream's to fix.

One file is built differently: `library.json` leaves out `src/errors.c` and builds
`port/errors_min.c` instead. That file includes `errors.c` unchanged but swaps its 25 KB table of
NT status names (`nterror_to_str()`) for the status code in hex. The names only appeared in
libsmb2's own error messages. Files works from the errno and the NT code, so its screens are
the same.

Authentication is NTLMSSP (user name and password; libsmb2's own MD4/MD5/HMAC), with SMB2/3
signing and optional SMB3 encryption. Kerberos is left out.

To update: clone upstream, then copy the same set of files again (the source list is in
`lib/CMakeLists.txt`), change the version in `library.json` and the commit above, build, and test
a share on the board.

The LGPL lets you link this into the firmware. If you give the firmware to someone else, they're
entitled to the library's source (this folder) and to rebuild the firmware with a changed copy of
it.
