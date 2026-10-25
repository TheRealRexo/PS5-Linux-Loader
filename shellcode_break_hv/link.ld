ENTRY(shellcode)
SECTIONS
{
  . = 0;
  .text : {
    entry.o(.text .text.*)     /* shellcode MUST be first */
    *(.text .text.*)
  }
  .rodata : { *(.rodata .rodata.*) }
  .data   : { *(.data .data.*)   }
  ASSERT(. <= 0x800, "break_hv does not fit in AP_COPY_SIZE (0x800)")
  /DISCARD/ : {
    *(.eh_frame .eh_frame_hdr)
    *(.note .note.*)
    *(.comment)
    *(.bss .bss.*)
  }
}
