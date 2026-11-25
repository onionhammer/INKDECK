/* The order of src/'s code in flash (scripts/lto.py passes this to the linker as its
   --section-ordering-file).

   A call reaches 512 KB either way; one that can't costs 3 bytes more, for loading the address
   first. LTO emits src/ as six partitions of 110 to 140 KB (--param=lto-partitions=6), in the
   order of the source files, and the linker puts them first in flash, in that order. The code
   everything calls sits at the two ends of that: partition 0 has the standard library's string
   and vector code (it lands with the first file that uses it), and the last has ui/. So half of
   src/ was out of reach of each.

   Here those two go in the middle. Partitions this file doesn't name follow in their own order,
   then the libraries. Measured 2026-11-24: 12,800 calls out of reach became 7,000, and
   firmware.bin 19.9 KB smaller.

   If the partition count changes (lto.py), the last one's number here changes with it. A name
   that matches nothing is ignored, so a stale file costs size, not correctness. */
.flash.text :
{
  *.ltrans1.ltrans.o(.literal .literal.* .text .text.*)
  *.ltrans2.ltrans.o(.literal .literal.* .text .text.*)
  *.ltrans5.ltrans.o(.literal .literal.* .text .text.*)
  *.ltrans0.ltrans.o(.literal .literal.* .text .text.*)
}
