LZMA SDK (C) - XZ decoder subset, vendored
===========================================

Files in this directory are from the LZMA SDK 26.06 (standalone C decoder),
copyright Igor Pavlov. Every file carries the header:

  /* <name> -- <description>
   <date> : Igor Pavlov : Public domain */

i.e. the code is **public domain** and may be used, modified, and linked
without restriction. This is compatible with the repository's GPL-3 license
and the PS5 payload's license.

Upstream: https://www.7-zip.org/sdk.html
          https://github.com/ip7z/7zip/releases/download/26.06/lzma2606.7z

Vendored on 2026-11-17 to support in-payload XZ decompression of the
official ps5-linux OS images (ps5-<distro>-os.img.xz, which the release
serves split into <= 2 GB parts: ps5-<distro>-os.img.xz, .part2, ...).

The PS5 payload wires these sources up through source/xzimg.c, which
streams the concatenated parts through XzUnpacker (Interface-1,
partial output buffers) and writes the raw .img to disk.

Unchanged, verbatim except for a minimal Precomp.h shim (see below).

Files:
  7zTypes.h  7zAlloc.h  7zAlloc.c  7zCrc.h  7zCrc.c
  XzCrc64.h  XzCrc64.c  LzmaDec.h  LzmaDec.c  Lzma2Dec.h  Lzma2Dec.c
  Xz.h       XzDec.c    Bra.h      Bra.c      Delta.h     Delta.c
  Sha256.h   Sha256.c   Alloc.h    CpuArch.h  Compiler.h  Precomp.h
