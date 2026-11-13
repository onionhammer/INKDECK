// Stand-in for esp-dsp's dsps_fft2r_platform.h, which PNGdec's ESP32-S3 SIMD file
// (s3_simd_rgb565.S) includes only to learn that the S3's vector extension is there. The real
// header sits in the Arduino core but pulls in Xtensa headers the assembler step can't find.
// It defines this as 1 on the ESP32-S3; so do we (platformio.ini adds this folder).
#pragma once
#define dsps_fft2r_sc16_aes3_enabled 1
