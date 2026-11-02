/* Host-function and observer tables.
 *
 * host.h places the table bounds in .ovr$a/.ovr$z (and .obs$a/.obs$z) and the entries in .ovr$m/.obs$m, and
 * hostfn.c walks from the first to the last. On Windows the COFF linker keeps the three input sections adjacent and
 * ordered; ELF linkers keep them as separate output sections in whatever order the objects were seen, which leaves
 * the bounds pointing at an empty range. Coalescing them here restores the contract. */
SECTIONS
{
  .ovr : { *(.ovr$a) *(.ovr$m) *(.ovr$z) }
  .obs : { *(.obs$a) *(.obs$m) *(.obs$z) }
}
INSERT AFTER .data;
