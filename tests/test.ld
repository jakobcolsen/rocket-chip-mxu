/*======================================================================*/
/* Proxy kernel linker script                                           */
/*======================================================================*/
/* This is the linker script used when building the proxy kernel. */

/*----------------------------------------------------------------------*/
/* Setup                                                                */
/*----------------------------------------------------------------------*/

/* The OUTPUT_ARCH command specifies the machine architecture where the
   argument is one of the names used in the BFD library. More
   specifically one of the entires in bfd/cpu-mips.c */

OUTPUT_ARCH( "riscv" )
ENTRY(_start)

/*----------------------------------------------------------------------*/
/* Sections                                                             */
/*----------------------------------------------------------------------*/

SECTIONS
{

  /* text: test code section */
  . = 0x80000000;
  .text.init : { *(.text.init); . = ALIGN(16); }

  . = ALIGN(0x1000);
  .tohost : { *(.tohost); . = ALIGN(16); }

  . = ALIGN(0x1000);
  .text : { *(.text); . = ALIGN(16); }

  /* data segment */
  .data : { *(.data); . = ALIGN(16); }

  .sdata : {
    __global_pointer$ = . + 0x800;
    *(.srodata.cst16) *(.srodata.cst8) *(.srodata.cst4) *(.srodata.cst2) *(.srodata*)
    *(.sdata .sdata.* .gnu.linkonce.s.*)
    . = ALIGN(16);
  }

  /* bss segment */
  .sbss : {
    *(.sbss .sbss.* .gnu.linkonce.sb.*)
    *(.scommon)
    . = ALIGN(16);
  }
  .bss : { *(.bss); . = ALIGN(16); }

  /* Dedicated shared memory area for SIMD results */
  . = ALIGN(0x1000);
  .shared_mem : {
    _shared_results = .;
    . = . + 0x1000; /* 4KB of space */
  }

  /* thread-local data segment */
  .tdata :
  {
    _tdata_begin = .;
    *(.tdata)
    _tdata_end = .;
    . = ALIGN(16);
  }
  .tbss :
  {
    *(.tbss)
    _tbss_end = .;
    . = ALIGN(16);
  }

  /* End of uninitalized data segement */
  _end = .;
}

