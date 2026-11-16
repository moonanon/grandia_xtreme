# Third-party code

The game's executables link library code next to the game's own. The C that
matches it here was written to reproduce those binaries, but where a library
is open source, the matching C follows the upstream source and falls under
that library's license, not this project's. Every such file carries a
one-line note at the top naming the library; list them with:

```sh
grep -l "Third-party library code\|PS2 SDK library code" src/*/*.c src/*/data/*.c
```

| Library | Files | License |
| --- | ---: | --- |
| libgcc (GCC 2.95 `libgcc2.c`, `config/fp-bit.c`) | 44 | GPL v2 or later, with each file's own linking exception (below) |
| newlib libc (stdio, stdlib, `mprec`/`dtoa`/`strtod`, malloc, locale, reent, ctype) | 89 | per file: UC Berkeley (two forms), David M. Gay, or newlib's default Cygnus notice |
| newlib libm (fdlibm-derived) | 70 | Sun Microsystems fdlibm notice |
| Sony libmpeg, whose decoder follows MSSG `mpeg2decode` | 61 | Sony: proprietary; MSSG: its own notice (below) |
| Other Sony PS2 SDK libraries (libkernel, libgraph, libdma, libpad, libmc, libcdvd, libipu, crt0) | 294 | proprietary, no public source |

The counts include the data-only files under `src/*/data/` (24 of the
newlib ones). All 558 notes are in the main executable except 3 in FIELD
(`exit`, `sqrt`, `__ieee754_sqrt`) and 10 in VIEWER (single-precision
maths).

The Sony SDK code has no license to reproduce: decompiling it is the same
kind of work, in the same legal position, as decompiling the game itself.

## What was checked

On 2026-10-24 every notice below was compared word for word with the
upstream files, and is quoted here exactly as upstream has it:

- GCC 2.95.2 (the release closest to Sony's `ee-gcc 2.9-991111`):
  `gcc/libgcc2.c`, `gcc/config/fp-bit.c`, `gcc/longlong.h` and
  `gcc/gbl-ctors.h` from the GCC git repository, tag
  `releases/gcc-2.95.2`
  (`https://gcc.gnu.org/git/?p=gcc.git;a=blob_plain;f=gcc/<file>;hb=refs/tags/releases/gcc-2.95.2`).
- newlib 1.8.2, the last 1.8.x release:
  `https://sourceware.org/pub/newlib/newlib-1.8.2.tar.gz` (3,015,101 bytes,
  SHA-256 `e52667ed78595dd1919f4a303f8b658b1d57b926e085e8021851b279143cf3ef`):
  `COPYING.NEWLIB` and the header of every upstream file this project's
  newlib C follows.
- fdlibm 5.3 from `https://netlib.org/fdlibm/` (its `readme`, `e_acos.c`,
  `e_rem_pio2.c`, `k_cos.c`, `k_rem_pio2.c`, `s_floor.c`, `w_sqrt.c`).
- MSSG MPEG-2 video codec v1.2 (mpeg2decode/mpeg2encode, July 1996):
  `mpeg2vidcodec_v12.tar.gz` (259,790 bytes, SHA-256
  `e59da9988b2e964026c8370658771954a724e8319cc266f75c6aef2a85c40076`). The
  MSSG's own sites no longer serve it; it was fetched from the Internet
  Archive's copy of `http://www.mpeg.org/pub_ftp/mpeg/mssg/mpeg2vidcodec_v12.tar.gz`.

What the check changed, compared with the notices this file used to give
from memory:

- libgcc: the old text was the exception from later GCCs ("... into
  combinations with other programs ... a combine executable"). GCC 2.95.2's
  `libgcc2.c` has a different one ("As a special exception, if you link
  this library ..."); `fp-bit.c` has both its own "In addition to the
  permissions" paragraph (worded differently: "with other programs",
  "another program") and that special exception, under its own copyright
  years.
- fdlibm: newlib 1.8.2's files say "Developed at SunPro"; netlib's fdlibm
  5.3 says "Developed at SunSoft". The code here follows newlib, so the
  newlib wording is the one quoted.
- newlib libc: no newlib string or memory function is in C here (`memset`,
  `memcpy`, `strlen`, ... are R5900 assembly, which newlib 1.8.2 does not
  have), so the old "Red Hat / Cygnus string functions" entry is gone. The
  stdio files carry two different Berkeley notices, and many files carry
  none, which puts them under `COPYING.NEWLIB`'s default Cygnus notice.
  David M. Gay's notice was already exact.
- MSSG: the disclaimer reads "damages of any kind whatsoever" (the old text
  said "nature").
- 28 files' one-line notes were corrected (comments only; each still
  matches): `cos`, `cosf`, `floorf`, `sinf`, `tanf`, `scalbnf` (marked Sony,
  are newlib libm) and `_printf_r` (marked Sony, is newlib `printf.c`);
  `fstat`, `iSyncDCache`, `iInvalidDCache`, `iRotateThreadReadyQueue`,
  `_rpc_free_packet`, `_rpc_get_fpacket`, `_request_end`, `search_svdata`,
  `_request_bind`, `_request_call` (marked newlib, are Sony libkernel; newlib
  1.8.2 has no `fstat` of its own, only the `_fstat_r` that calls it);
  `atanf`, `ceilf`, `fabsf`, `rint`, `finitef`, `copysign`, `copysignf`,
  `isnan` and `s_lib_ver.c`'s data (marked plain newlib, are libm files
  under Sun's notice); and two upstream file names (`_strtol_r` is in
  `strtol.c`, `__kernel_rem_pio2f` in `kf_rem_pio2.c`).

Still open: Sony's newlib is a modified 1.8-era tree, not exactly 1.8.2
(`strtof`, 484280.c, is not in 1.8.2; later newlibs have it in `strtod.c`,
under David M. Gay's notice), and Sony's compiler is a GCC 2.95
snapshot, not 2.95.2 itself, so a notice could differ in the exact
sources Sony used; none of those sources is public.

## libgcc

`libgcc2.c` (`exit`, `__main`, `__do_global_ctors`/`__do_global_dtors`,
`__muldi3`, `__divdi3`, `__moddi3`, `__udivdi3`, `__umoddi3`, `__fixdfdi`,
`__fixunsdfdi`, `__fixsfdi`, `__fixunssfdi`, `__floatdidf`: 14 files),
GCC 2.95.2:

```
/* Copyright (C) 1989, 92-98, 1999 Free Software Foundation, Inc.

This file is part of GNU CC.

GNU CC is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

GNU CC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU CC; see the file COPYING.  If not, write to
the Free Software Foundation, 59 Temple Place - Suite 330,
Boston, MA 02111-1307, USA.  */

/* As a special exception, if you link this library with other files,
   some of which are compiled with GCC, to produce an executable,
   this library does not by itself cause the resulting executable
   to be covered by the GNU General Public License.
   This exception does not however invalidate any other reasons why
   the executable file might be covered by the GNU General Public License.  */
```

`config/fp-bit.c` (the soft-float routines, which Sony's ee-gcc names
`dpadd`, `fpmul`, `litodp`, ...: 30 files), GCC 2.95.2:

```
/* This is a software floating point library which can be used instead of
   the floating point routines in libgcc1.c for targets without hardware
   floating point. 
 Copyright (C) 1994, 1995, 1996, 1997, 1998 Free Software Foundation, Inc.

This file is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 2, or (at your option) any
later version.

In addition to the permissions in the GNU General Public License, the
Free Software Foundation gives you unlimited permission to link the
compiled version of this file with other programs, and to distribute
those programs without any restriction coming from the use of this
file.  (The General Public License restrictions do apply in other
respects; for example, they cover modification of the file, and
distribution when not linked into another program.)

This file is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING.  If not, write to
the Free Software Foundation, 59 Temple Place - Suite 330,
Boston, MA 02111-1307, USA.  */

/* As a special exception, if you link this library with other files,
   some of which are compiled with GCC, to produce an executable,
   this library does not by itself cause the resulting executable
   to be covered by the GNU General Public License.
   This exception does not however invalidate any other reasons why
   the executable file might be covered by the GNU General Public License.  */
```

Five of the `libgcc2.c` files (`__muldi3`, `__divdi3`, `__moddi3`,
`__udivdi3`, `__umoddi3`) also use `longlong.h`'s `umul_ppmm`, and
`__do_global_ctors` follows `gbl-ctors.h`. Their notices, GCC 2.95.2:

```
/* longlong.h -- definitions for mixed size 32/64 bit arithmetic.
   Copyright (C) 1991, 92, 94, 95, 96, 1997, 1998 Free Software Foundation, Inc.

   This definition file is free software; you can redistribute it
   and/or modify it under the terms of the GNU General Public
   License as published by the Free Software Foundation; either
   version 2, or (at your option) any later version.

   This definition file is distributed in the hope that it will be
   useful, but WITHOUT ANY WARRANTY; without even the implied
   warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
   See the GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.  */
```

```
/* Definitions relating to the special __do_global_init function used
   for getting g++ file-scope static objects constructed.  This file
   will get included either by libgcc2.c (for systems that don't support
   a .init section) or by crtstuff.c (for those that do).
   Copyright (C) 1991, 1995, 1996, 1998 Free Software Foundation, Inc.
   Contributed by Ron Guilmette (rfg@segfault.us.com)

This file is part of GNU CC.

GNU CC is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

GNU CC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU CC; see the file COPYING.  If not, write to
the Free Software Foundation, 59 Temple Place - Suite 330,
Boston, MA 02111-1307, USA.  */
```

Distributing these 44 files means distributing them under the GPL (the
exceptions cover linking the compiled code, not the source).

## fdlibm (newlib libm)

All 70 libm files (the `e_`/`ef_`, `k_`/`kf_`, `s_`/`sf_`, `w_`/`wf_`
files, `s_matherr.c`, `s_lib_ver.c`) carry this notice, as in newlib
1.8.2's `libm/math/k_cos.c`. Some files (`e_rem_pio2.c`, `ef_rem_pio2.c`,
`ef_atan2.c`, `sf_atan.c`, `wf_asin.c`, `wf_atan2.c`) have one more empty
` *` line before the closing `*/`; the float (`f`) files add a line
"Conversion to float by Ian Lance Taylor, Cygnus Support, ian@cygnus.com."
above it.

```
/*
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice 
 * is preserved.
 * ====================================================
 */
```

## newlib libc

newlib is a collection of files under several notices, listed in its
`COPYING.NEWLIB`; each file's own header decides. For the 89 libc files
here (newlib 1.8.2 paths under `newlib/libc/`):

- The short University of California notice, as in `stdio/fflush.c`
  (28 files): `stdio/fflush.c`, `findfp.c`, `fread.c`, `fvwrite.c`,
  `fwalk.c`, `makebuf.c`, `refill.c`, `sprintf.c`, `sscanf.c`, `stdio.c`,
  `ungetc.c`, `vfscanf.c` (with its data), `vsprintf.c`, `wsetup.c`:

```
/*
 * Copyright (c) 1990 The Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by the University of California, Berkeley.  The name of the
 * University may not be used to endorse or promote products derived
 * from this software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */
```

- The four-clause University of California notice, with its advertising
  clause (11 files): `stdio/vfprintf.c` (`vfprintf`, `_vfprintf_r`,
  `__sprint`, `__sbprintf`, `cvt`, `exponent`) as quoted;
  `stdlib/strtol.c` without the "This code is derived from software
  contributed to Berkeley by Chris Torek." sentence (and its `*` line);
  `stdlib/strtoul.c` likewise, and its first line reads "Copyright (c) 1990
  Regents of the University of California." (no "The"); `ctype/ctype_.c`
  (data) likewise, with "Copyright (c) 1989 The Regents ...":

```
/*-
 * Copyright (c) 1990 The Regents of the University of California.
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Chris Torek.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
```

- David M. Gay's notice (23 files): `stdlib/mprec.c` (with its data),
  `dtoa.c`, `strtod.c` (and `strtof`, see above):

```
/****************************************************************
 *
 * The author of this software is David M. Gay.
 *
 * Copyright (c) 1991 by AT&T.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose without fee is hereby granted, provided that this entire notice
 * is included in all copies of any software which is or includes a copy
 * or modification of this software and in all copies of the supporting
 * documentation for such software.
 *
 * THIS SOFTWARE IS BEING PROVIDED "AS IS", WITHOUT ANY EXPRESS OR IMPLIED
 * WARRANTY.  IN PARTICULAR, NEITHER THE AUTHOR NOR AT&T MAKES ANY
 * REPRESENTATION OR WARRANTY OF ANY KIND CONCERNING THE MERCHANTABILITY
 * OF THIS SOFTWARE OR ITS FITNESS FOR ANY PARTICULAR PURPOSE.
 *
 ***************************************************************/
```

- No notice of their own, so `COPYING.NEWLIB`'s default applies (27 files):
  `stdlib/atof.c`, `atoi.c`, `rand.c`, `mbtowc_r.c`, `mlock.c`,
  `mallocr.c`; `errno/errno.c`; `stdio/printf.c`; `locale/locale.c` (with
  its data); `reent/impure.c` (data), `sbrkr.c`, `writer.c`, `closer.c`,
  `fstatr.c`, `lseekr.c`, `readr.c`; and two Cygnus additions to libm,
  `libm/math/s_isinf.c` and `s_infconst.c` (data), which carry no Sun
  notice:

```
(9) Unless otherwise stated in each remaining newlib file, the remaining
files in the newlib subdirectory are governed by the following copyright.

Copyright (c) 1994, 1997 Cygnus Solutions.
All rights reserved.

Redistribution and use in source and binary forms are permitted
provided that the above copyright notice and this paragraph are
duplicated in all such forms and that any documentation,
advertising materials, and other materials related to such
distribution and use acknowledge that the software was developed
at Cygnus Solutions.  Cygnus Solutions may not be used to
endorse or promote products derived from this software without
specific prior written permission.
THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
```

  `mallocr.c` (`_malloc_r`, `_free_r`, `_realloc_r`, `_calloc_r`,
  `_malloc_trim_r`, `malloc_extend_top` and the malloc state: 7 files)
  is Doug Lea's malloc 2.6.4, whose header says:

```
  A version of malloc/free/realloc written by Doug Lea and released to the 
  public domain.  Send questions/comments/complaints/performance data
  to dl@cs.oswego.edu
```

## MSSG mpeg2decode

Every source file of mpeg2decode 1.2 (`src/mpeg2dec/`) starts with this
(here from `recon.c`; `getbits.c` adds a line "All modifications
(mpeg2decode -> mpeg2play) are Copyright (C) 1996, Stefan Eckart. All Rights
Reserved."):

```
/* Copyright (C) 1996, MPEG Software Simulation Group. All Rights Reserved. */

/*
 * Disclaimer of Warranty
 *
 * These software programs are available to the user without any license fee or
 * royalty on an "as is" basis.  The MPEG Software Simulation Group disclaims
 * any and all warranties, whether express, implied, or statuary, including any
 * implied warranties or merchantability or of fitness for a particular
 * purpose.  In no event shall the copyright-holder be liable for any
 * incidental, punitive, or consequential damages of any kind whatsoever
 * arising from the use of these programs.
 *
 * This disclaimer of warranty extends to the user of these programs and user's
 * customers, employees, agents, transferees, successors, and assigns.
 *
 * The MPEG Software Simulation Group does not represent or warrant that the
 * programs furnished hereunder are free of infringement of any third-party
 * patents.
 *
 * Commercial implementations of MPEG-1 and MPEG-2 video, including shareware,
 * are subject to royalty fees to patent holders.  Many of these patents are
 * general enough such that they are unavoidable regardless of implementation
 * design.
 *
 */
```

Sony's libmpeg decoder runs on the IPU and is Sony's own code; it follows
`mpeg2decode`'s structure and names (5 files follow `getpic.c`,
`recon.c` and `motion.c` directly), so how much of the MSSG notice applies
is a judgement call: keep the notice with those 61 files.

## xxHash

`port/third_party/xxhash/` is xxHash v0.8.3 by Yann Collet (BSD 2-Clause,
license in the folder's header), used for PCSX2's texture replacement names.
