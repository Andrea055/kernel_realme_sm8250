/*
 * LZ4 - Fast LZ compression algorithm
 * Copyright (C) 2011 - 2016, Yann Collet.
 * BSD 2 - Clause License (http://www.opensource.org/licenses/bsd - license.php)
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *	* Redistributions of source code must retain the above copyright
 *	  notice, this list of conditions and the following disclaimer.
 *	* Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * You can contact the author at :
 *	- LZ4 homepage : http://www.lz4.org
 *	- LZ4 source repository : https://github.com/lz4/lz4
 *
 *	Changed for kernel usage by:
 *	Sven Schmidt <4sschmid@informatik.uni-hamburg.de>
 */

/*-************************************
 *	Dependencies
 **************************************/
#include <linux/lz4.h>
#include "lz4defs.h"
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <asm/unaligned.h>

#include "lz4armv8/lz4accel.h"

/*-*****************************
 *	Decompression functions
 *******************************/
/* LZ4_decompress_generic() :
 * This generic decompression function cover all use cases.
 * It shall be instantiated several times, using different sets of directives
 * Note that it is important this generic function is really inlined,
 * in order to remove useless branches during compilation optimization.
 */
static FORCE_INLINE int __LZ4_decompress_generic(
	 const char * const src,
	 char * const dst,
	 const BYTE * ip,
	 BYTE * op,
	 int srcSize,
		/*
		 * If endOnInput == endOnInputSize,
		 * this value is the max size of Output Buffer.
		 */
	 int outputSize,
	 /* endOnOutputSize, endOnInputSize */
	 int endOnInput,
	 /* full, partial */
	 earlyEnd_directive partialDecoding,
	 /* noDict, withPrefix64k */
	 dict_directive dict,
	 /* always <= dst, == dst when no prefix */
	 const BYTE * const lowPrefix,
	 /* only if dict == usingExtDict */
	 const BYTE * const dictStart,
	 /* note : = 0 if noDict */
	 const size_t dictSize
	 )
{
	const BYTE * const iend = src + srcSize;

	BYTE * const oend = dst + outputSize;
	BYTE *cpy;
	BYTE *oexit = op + targetOutputSize;
	const BYTE * const lowLimit = lowPrefix - dictSize;

	static const unsigned int inc32table[8] = {0, 1, 2, 1, 0, 4, 4, 4};
	static const int dec64table[8] = {0, 0, 0, -1, -4, 1, 2, 3};

	const int safeDecode = (endOnInput == endOnInputSize);
	const int checkOffset = ((safeDecode) && (dictSize < (int)(64 * KB)));

	/* Special cases */
	/* targetOutputSize too high => decode everything */
	if ((partialDecoding) && (oexit > oend - MFLIMIT))
		oexit = oend - MFLIMIT;

	/* Empty output buffer */
	if ((endOnInput) && (unlikely(outputSize == 0)))
		return ((inputSize == 1) && (*ip == 0)) ? 0 : -1;

	if ((!endOnInput) && (unlikely(outputSize == 0)))
		return (*ip == 0 ? 1 : -1);

	/* Main Loop : decode sequences */
	while (1) {
		size_t length;
		const BYTE *match;
		size_t offset;

		/* get literal length */
		unsigned int const token = *ip++;

		length = token>>ML_BITS;

		/* ip < iend before the increment */
		assert(!endOnInput || ip <= iend);

		/*
		 * A two-stage shortcut for the most common case:
		 * 1) If the literal length is 0..14, and there is enough
		 * space, enter the shortcut and copy 16 bytes on behalf
		 * of the literals (in the fast mode, only 8 bytes can be
		 * safely copied this way).
		 * 2) Further if the match length is 4..18, copy 18 bytes
		 * in a similar manner; but we ensure that there's enough
		 * space in the output for those 18 bytes earlier, upon
		 * entering the shortcut (in other words, there is a
		 * combined check for both stages).
		 */
		if ((endOnInput ? length != RUN_MASK : length <= 8)
		   /*
		    * strictly "less than" on input, to re-enter
		    * the loop with at least one byte
		    */
		   && likely((endOnInput ? ip < shortiend : 1) &
			     (op <= shortoend))) {
			/* Copy the literals */
			LZ4_memcpy(op, ip, endOnInput ? 16 : 8);
			op += length; ip += length;

			/*
			 * The second stage:
			 * prepare for match copying, decode full info.
			 * If it doesn't work out, the info won't be wasted.
			 */
			length = token & ML_MASK; /* match length */
			offset = LZ4_readLE16(ip);
			ip += 2;
			match = op - offset;
			assert(match <= op); /* check overflow */

			/* Do not deal with overlapping matches. */
			if ((length != ML_MASK) &&
			    (offset >= 8) &&
			    (dict == withPrefix64k || match >= lowPrefix)) {
				/* Copy the match. */
				LZ4_memcpy(op + 0, match + 0, 8);
				LZ4_memcpy(op + 8, match + 8, 8);
				LZ4_memcpy(op + 16, match + 16, 2);
				op += length + MINMATCH;
				/* Both stages worked, load the next token. */
				continue;
			}

			/*
			 * The second stage didn't work out, but the info
			 * is ready. Propel it right to the point of match
			 * copying.
			 */
			goto _copy_match;
		}

		/* decode literal length */
		if (length == RUN_MASK) {
			unsigned int s;

			do {
				s = *ip++;
				length += s;
			} while (likely(endOnInput
				? ip < iend - RUN_MASK
				: 1) & (s == 255));

			if ((safeDecode)
				&& unlikely(
					(size_t)(op + length) < (size_t)(op))) {
				/* overflow detection */
				goto _output_error;
			}
			if ((safeDecode)
				&& unlikely(
					(size_t)(ip + length) < (size_t)(ip))) {
				/* overflow detection */
				goto _output_error;
			}
		}

		/* copy literals */
		cpy = op + length;
		if (((endOnInput) && ((cpy > (partialDecoding ? oexit : oend - MFLIMIT))
			|| (ip + length > iend - (2 + 1 + LASTLITERALS))))
			|| ((!endOnInput) && (cpy > oend - WILDCOPYLENGTH))) {
			if (partialDecoding) {
				if (cpy > oend) {
					/*
					 * Error :
					 * write attempt beyond end of output buffer
					 */
					goto _output_error;
				}
				if ((endOnInput)
					&& (ip + length > iend)) {
					/*
					 * Error :
					 * read attempt beyond
					 * end of input buffer
					 */
					goto _output_error;
				}
			} else {
				if ((!endOnInput)
					&& (cpy != oend)) {
					/*
					 * Error :
					 * block decoding must
					 * stop exactly there
					 */
					goto _output_error;
				}
				if ((endOnInput)
					&& ((ip + length != iend)
					|| (cpy > oend))) {
					/*
					 * Error :
					 * input must be consumed
					 */
					goto _output_error;
				}
			}

			/*
			 * supports overlapping memory regions; only matters
			 * for in-place decompression scenarios
			 */
			LZ4_memmove(op, ip, length);

			ip += length;
			op += length;
			/* Necessarily EOF, due to parsing restrictions */
			break;
		}

		LZ4_wildCopy(op, ip, cpy);
		ip += length;
		op = cpy;

		/* get offset */
		offset = LZ4_readLE16(ip);
		ip += 2;
		match = op - offset;

		if ((checkOffset) && (unlikely(match < lowLimit))) {
			/* Error : offset outside buffers */
			goto _output_error;
		}

		/* costs ~1%; silence an msan warning when offset == 0 */
		LZ4_write32(op, (U32)offset);

		/* get matchlength */
		length = token & ML_MASK;
		if (length == ML_MASK) {
			unsigned int s;

			do {
				s = *ip++;

				if ((endOnInput) && (ip > iend - LASTLITERALS))
					goto _output_error;

				length += s;
			} while (s == 255);

			if ((safeDecode)
				&& unlikely(
					(size_t)(op + length) < (size_t)op)) {
				/* overflow detection */
				goto _output_error;
			}
		}

		length += MINMATCH;

		/* copy match within block */
		cpy = op + length;

		if (unlikely(offset < 8)) {
			const int dec64 = dec64table[offset];

			op[0] = match[0];
			op[1] = match[1];
			op[2] = match[2];
			op[3] = match[3];
			match += dec32table[offset];
			memcpy(op + 4, match, 4);
			match -= dec64;
		} else {
			LZ4_copy8(op, match);
			match += 8;
		}

		op += 8;

		if (unlikely(cpy > oend - 12)) {
			BYTE * const oCopyLimit = oend - (WILDCOPYLENGTH - 1);

			if (cpy > oend - LASTLITERALS) {
				/*
				 * Error : last LASTLITERALS bytes
				 * must be literals (uncompressed)
				 */
				goto _output_error;
			}

			if (op < oCopyLimit) {
				LZ4_wildCopy(op, match, oCopyLimit);
				match += oCopyLimit - op;
				op = oCopyLimit;
			}

			while (op < cpy)
				*op++ = *match++;
		} else {
			LZ4_copy8(op, match);

			if (length > 16)
				LZ4_wildCopy(op + 8, match + 8, cpy);
		}

		op = cpy; /* correction */
	}

	/* end of decoding */
	if (endOnInput) {
		/* Nb of output bytes decoded */
		return (int) (((char *)op) - dest);
	} else {
		/* Nb of input bytes read */
		return (int) (((const char *)ip) - source);
	}

	/* Overflow error detected */
_output_error:
	return (int) (-(((const char *)ip) - src)) - 1;
}

static FORCE_INLINE int LZ4_decompress_generic(
	 const char * const src,
	 char * const dst,
	 int srcSize,
		/*
		 * If endOnInput == endOnInputSize,
		 * this value is `dstCapacity`
		 */
	 int outputSize,
	 /* endOnOutputSize, endOnInputSize */
	 endCondition_directive endOnInput,
	 /* full, partial */
	 earlyEnd_directive partialDecoding,
	 /* noDict, withPrefix64k, usingExtDict */
	 dict_directive dict,
	 /* always <= dst, == dst when no prefix */
	 const BYTE * const lowPrefix,
	 /* only if dict == usingExtDict */
	 const BYTE * const dictStart,
	 /* note : = 0 if noDict */
	 const size_t dictSize
	 )
{
	return __LZ4_decompress_generic(src, dst, (const BYTE *)src, (BYTE *)dst, srcSize, outputSize, endOnInput, partialDecoding, dict, lowPrefix, dictStart, dictSize);
}

int LZ4_decompress_safe(const char *source, char *dest,
	int compressedSize, int maxDecompressedSize)
{
	return LZ4_decompress_generic(source, dest, compressedSize,
		maxDecompressedSize, endOnInputSize, full, 0,
		noDict, (BYTE *)dest, NULL, 0);
}

int LZ4_decompress_fast(const char *source, char *dest, int originalSize)
{
	return LZ4_decompress_generic(source, dest, 0, originalSize,
				      endOnOutputSize, decode_full_block,
				      withPrefix64k,
				      (BYTE *)dest - 64 * KB, NULL, 0);
}

#ifndef STATIC
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("LZ4 decompressor");
#endif
