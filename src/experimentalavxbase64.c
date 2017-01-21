#include "experimentalavxbase64.h"

#include <x86intrin.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>

/**
* This code borrows from Wojciech Mula's library at
* https://github.com/WojciechMula/base64simd (published under BSD)
* as well as code from Alfred Klomp's library https://github.com/aklomp/base64 (published under BSD)
*
*/




/**
* Note : Hardware such as Knights Landing might do poorly with this AVX2 code since it relies on shuffles. Alternatives might be faster.
*/


static inline __m256i _mm256_bswap_epi32(const __m256i in) {
  // _mm256_shuffle_epi8() works on two 128-bit lanes separately:
  return _mm256_shuffle_epi8(in, _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11,
                                                  10, 9, 8, 15, 14, 13, 12, 3,
                                                  2, 1, 0, 7, 6, 5, 4, 11, 10,
                                                  9, 8, 15, 14, 13, 12));
}

static inline __m256i enc_reshuffle(const __m256i input) {

    // translation from SSE into AVX2 of procedure
    // https://github.com/WojciechMula/base64simd/blob/master/encode/unpack_bigendian.cpp
    const __m256i in = _mm256_shuffle_epi8(input, _mm256_set_epi8(
        10, 11,  9, 10,
         7,  8,  6,  7,
         4,  5,  3,  4,
         1,  2,  0,  1,

        14, 15, 13, 14,
        11, 12, 10, 11,
         8,  9,  7,  8,
         5,  6,  4,  5
    ));

    const __m256i t0 = _mm256_and_si256(in, _mm256_set1_epi32(0x0fc0fc00));
    const __m256i t1 = _mm256_mulhi_epu16(t0, _mm256_set1_epi32(0x04000040));

    const __m256i t2 = _mm256_and_si256(in, _mm256_set1_epi32(0x003f03f0));
    const __m256i t3 = _mm256_mullo_epi16(t2, _mm256_set1_epi32(0x01000010));

    return _mm256_or_si256(t1, t3);
}

static inline __m256i enc_translate(const __m256i in) {
  const __m256i lut = _mm256_setr_epi8(
      65, 71, -4, -4, -4, -4, -4, -4, -4, -4, -4, -4, -19, -16, 0, 0, 65, 71,
      -4, -4, -4, -4, -4, -4, -4, -4, -4, -4, -19, -16, 0, 0);
  __m256i indices = _mm256_subs_epu8(in, _mm256_set1_epi8(51));
  __m256i mask = _mm256_cmpgt_epi8((in), _mm256_set1_epi8(25));
  indices = _mm256_sub_epi8(indices, mask);
  __m256i out = _mm256_add_epi8(in, _mm256_shuffle_epi8(lut, indices));
  return out;
}

static inline __m256i dec_reshuffle(__m256i in) {

  // inlined procedure pack_madd from https://github.com/WojciechMula/base64simd/blob/master/decode/pack.avx2.cpp
  // The only difference is that elements are reversed,
  // only the multiplication constants were changed.

  const __m256i merge_ab_and_bc = _mm256_maddubs_epi16(in, _mm256_set1_epi32(0x01400140)); //_mm256_maddubs_epi16 is likely expensive
  __m256i out = _mm256_madd_epi16(merge_ab_and_bc, _mm256_set1_epi32(0x00011000));
  // end of inlined

  // Pack bytes together within 32-bit words, discarding words 3 and 7:
  out = _mm256_shuffle_epi8(out, _mm256_setr_epi8(
        2, 1, 0, 6, 5, 4, 10, 9, 8, 14, 13, 12, -1, -1, -1, -1,
        2, 1, 0, 6, 5, 4, 10, 9, 8, 14, 13, 12, -1, -1, -1, -1
  ));
  // the call to _mm256_permutevar8x32_epi32 could be replaced by a call to _mm256_storeu2_m128i but it is doubtful that it would help
  return _mm256_permutevar8x32_epi32(
      out, _mm256_setr_epi32(0, 1, 2, 4, 5, 6, -1, -1));
}


size_t expavx2_base64_encode(char* dest, const char* str, size_t len) {
      const char* const dest_orig = dest;
      if(len >= 32 - 4) {
        // first load is masked
        __m256i inputvector = _mm256_maskload_epi32((int const*)(str - 4),  _mm256_set_epi32(
            0x80000000,
            0x80000000,
            0x80000000,
            0x80000000,

            0x80000000,
            0x80000000,
            0x80000000,
            0x00000000 // we do not load the first 4 bytes
        ));
        //////////
        // Intel docs: Faults occur only due to mask-bit required memory accesses that caused the faults.
        // Faults will not occur due to referencing any memory location if the corresponding mask bit for
        //that memory location is 0. For example, no faults will be detected if the mask bits are all zero.
        ////////////
        while(true) {
          inputvector = enc_reshuffle(inputvector);
          inputvector = enc_translate(inputvector);
          _mm256_storeu_si256((__m256i *)dest, inputvector);
          str += 24;
          dest += 32;
          len -= 24;
          if(len >= 32) {
            inputvector = _mm256_loadu_si256((__m256i *)(str - 4)); // no need for a mask here
            // we could do a mask load as long as len >= 24
          } else {
            break;
          }
        }
      }
      size_t scalarret = chromium_base64_encode(dest, str, len);
      if(scalarret == MODP_B64_ERROR) return MODP_B64_ERROR;
      return (dest - dest_orig) + scalarret;
}


// TODO: the lookup is huge, we need to work out smaller solution
#include "compress.inl"


__m256i check_ws(const __m256i v) {
    
    const __m256i e0 = _mm256_cmpeq_epi8(v, _mm256_set1_epi8(' '));
    const __m256i e1 = _mm256_cmpeq_epi8(v, _mm256_set1_epi8('\r'));
    const __m256i e2 = _mm256_cmpeq_epi8(v, _mm256_set1_epi8('\n'));

    return _mm256_or_si256(e0, _mm256_or_si256(e1, e2));
}


#if 0
static
void dump(const __m256i v) {
    
    char tmp[32];

    _mm256_storeu_si256((__m256i*)tmp, v);

    putchar('\'');
    for (int i=0; i < 32; i++) {
        putchar(tmp[i]);
    }
    putchar('\'');
}


static
void dump16(const __m128i v) {

    char tmp[16] __attribute__((aligned(16)));

    _mm_storeu_si128((__m128i*)tmp, v);

    putchar('\'');
    for (int i=0; i < 16; i++) {
        putchar(tmp[i]);
    }
    putchar('\'');
}


static
void dump16hex(const __m128i v) {

    char tmp[16] __attribute__((aligned(16)));

    _mm_storeu_si128((__m128i*)tmp, v);

    putchar('\'');
    for (int i=0; i < 16; i++) {
        printf("%02x ", (uint8_t)tmp[i]);
    }
    putchar('\'');
}


static
void dumphex(const __m256i v) {
    
    char tmp[32];

    _mm256_storeu_si256((__m256i*)tmp, v);

    putchar('\'');
    for (int i=0; i < 32; i++) {
        putchar(tmp[i] ? '1' : '0');
        //printf("%02x", tmp[i]);
    }
    putchar('\'');
}
#endif


static char* encode_LUT = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";


size_t expavx2_base64_decode(char *out, const char *src, size_t srclen) {

      uint8_t buffer[2*32] __attribute__((aligned(32)));
      int ws_count = 0;

      char* out_orig = out;
      while (srclen >= 45) {

        // The input consists of six character sets in the Base64 alphabet,
        // which we need to map back to the 6-bit values they represent.
        // There are three ranges, two singles, and then there's the rest.
        //
        //  #  From       To        Add  Characters
        //  1  [43]       [62]      +19  +
        //  2  [47]       [63]      +16  /
        //  3  [48..57]   [52..61]   +4  0..9
        //  4  [65..90]   [0..25]   -65  A..Z
        //  5  [97..122]  [26..51]  -71  a..z
        // (6) Everything else => invalid input

        __m256i str = _mm256_loadu_si256((__m256i *)src);

        // code by @aqrit from
        // https://github.com/WojciechMula/base64simd/issues/3#issuecomment-271137490
        // transated into AVX2
        const __m256i lut_lo = _mm256_setr_epi8(
            0x15, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 
            0x11, 0x11, 0x13, 0x1A, 0x1B, 0x1B, 0x1B, 0x1A,
            0x15, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 
            0x11, 0x11, 0x13, 0x1A, 0x1B, 0x1B, 0x1B, 0x1A 
        );
        const __m256i lut_hi = _mm256_setr_epi8(
            0x10, 0x10, 0x01, 0x02, 0x04, 0x08, 0x04, 0x08, 
            0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
            0x10, 0x10, 0x01, 0x02, 0x04, 0x08, 0x04, 0x08, 
            0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10
        );
        const __m256i lut_roll = _mm256_setr_epi8(
            0,   16,  19,   4, -65, -65, -71, -71,
            0,   0,   0,   0,   0,   0,   0,   0,
            0,   16,  19,   4, -65, -65, -71, -71,
            0,   0,   0,   0,   0,   0,   0,   0
        );

#define DUMP(var) printf("%-10s = ", #var); dump(var); putchar('\n')
#define DUMPHEX(var) printf("%-10s = ", #var); dumphex(var); putchar('\n')
#define DUMP16(var) printf("%-10s = ", #var); dump16(var); putchar('\n')
#define DUMP16HEX(var) printf("%-10s = ", #var); dump16hex(var); putchar('\n')

        const __m256i mask_2F = _mm256_set1_epi8(0x2f);

        // lookup
        __m256i hi_nibbles  = _mm256_srli_epi32(str, 4);
        __m256i lo_nibbles  = _mm256_and_si256(str, mask_2F);

        const __m256i lo    = _mm256_shuffle_epi8(lut_lo, lo_nibbles);
        const __m256i eq_2F = _mm256_cmpeq_epi8(str, mask_2F);

        hi_nibbles = _mm256_and_si256(hi_nibbles, mask_2F);
        const __m256i hi    = _mm256_shuffle_epi8(lut_hi, hi_nibbles);
        const __m256i roll  = _mm256_shuffle_epi8(lut_roll, _mm256_add_epi8(eq_2F, hi_nibbles));
        const __m256i decoded = _mm256_add_epi8(str, roll);

        if (!_mm256_testz_si256(lo, hi)) {
            // check if all invalid chars are WS
            const __m256i ws  = check_ws(str);
            const __m256i err = _mm256_cmpeq_epi8(_mm256_and_si256(lo, hi), _mm256_setzero_si256());
            
            if (!_mm256_testz_si256(ws, err)) {
                // there are non-base64 chars that are not white spaces
                break;
            }

            // compress all valid characters
            const uint32_t mask   = ~_mm256_movemask_epi8(ws);
            const uint16_t lo_msk = mask & 0xffff;
            const uint16_t hi_msk = mask >> 16;
            
            const size_t   lo_cnt = _mm_popcnt_u32(lo_msk);
            const size_t   hi_cnt = _mm_popcnt_u32(hi_msk);

            assert(ws_count <= 32);
            if (true || lo_cnt) {
                const __m128i v = _mm256_extractf128_si256(decoded, 0);
                const __m128i L = _mm_loadu_si128((__m128i*)&compress_LUT[lo_msk*16]);
                const __m128i c = _mm_shuffle_epi8(v, L);

                _mm_storeu_si128((__m128i*)(buffer + ws_count), c);
                ws_count += lo_cnt;
            }

            if (true || hi_cnt) {
                const __m128i v = _mm256_extractf128_si256(decoded, 1);
                const __m128i L = _mm_loadu_si128((__m128i*)&compress_LUT[hi_msk*16]);
                const __m128i c = _mm_shuffle_epi8(v, L);

                _mm_storeu_si128((__m128i*)(buffer + ws_count), c);
                ws_count += hi_cnt;
            }

            srclen -= 32;
            src += 32;

            // lower 32 bytes of the buffer are full, pack and save
            if (ws_count >= 32) {
                str = dec_reshuffle(_mm256_load_si256((__m256i*)buffer));
                _mm256_storeu_si256((__m256i*)out, str);

                out += 24;
                ws_count -= 32;

                // move the high 32 bytes of buffer into the lower part
                const __m256i tmp = _mm256_load_si256((__m256i*)(buffer + 32));
                _mm256_store_si256((__m256i*)(buffer), tmp);
            }

            continue; // !!!
        }

        // no error, but we have leftovers in the buffer
        if (ws_count > 0) {
            
            assert(ws_count <= 32);

            // store decoded data at the end of buffer
            _mm256_storeu_si256((__m256i*)(buffer + ws_count), decoded);

            // pack 32 lower bytes from the buffer
            str = dec_reshuffle(_mm256_load_si256((__m256i*)buffer));
            _mm256_storeu_si256((__m256i*)out, str);

            srclen -= 32;
            src += 32;
            out += 24;

            // move the high 32 bytes of buffer into the lower part
            const __m256i tmp = _mm256_load_si256((__m256i*)(buffer + 32));
            _mm256_store_si256((__m256i*)(buffer), tmp);

            continue; // !!!
        }

        srclen -= 32;
        src += 32;

        // Reshuffle the input to packed 12-byte output format:
        str = dec_reshuffle(decoded);
        _mm256_storeu_si256((__m256i *)out, str);
        out += 24;
      }

      if (ws_count) {
          // XXX: this is still broken

          char tmp[45 + 32 + 1];
          for (int i=0; i < ws_count; i++) {
            // Buffer is already translated, so we're translating it back to ASCII
            tmp[i] = encode_LUT[buffer[i]];
          }
          memcpy(tmp + ws_count, src, srclen);

          size_t scalarret = chromium_base64_decode(out, tmp, srclen + ws_count);
          if(scalarret == MODP_B64_ERROR) return MODP_B64_ERROR;
          return (out - out_orig) + scalarret;
      } else {
          size_t scalarret = chromium_base64_decode(out, src, srclen);
          if(scalarret == MODP_B64_ERROR) return MODP_B64_ERROR;
          return (out - out_orig) + scalarret;
      }
}
