/* ASTC 4x4 blocks (LDR) to RGBA, for the game's item icons (gameimage.c shrinks them as they're read). A
 * port of the LDR part of texture2ddecoder's ASTC decoder (src/Texture2DDecoder/astc.cpp), its tables of
 * trits and quints worked out as the ASTC specification gives them instead:
 *
 *   MIT License
 *
 *   Copyright (c) 2020 K0lb3
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
 *   associated documentation files (the "Software"), to deal in the Software without restriction,
 *   including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *   and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so,
 *   subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all copies or substantial
 *   portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT
 *   LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
 *   NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 *   WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 *   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * HDR endpoints, which the icons don't use, come out as the specification's error colour (magenta). */
#include "astc.h"

/* the weights' and the colour endpoints' ranges: trits (3) or quints (5) or neither (0), and bits */
static const uint8_t weight_tq[16] = { 0, 0, 0, 3, 0, 5, 3, 0, 0, 0, 5, 3, 0, 5, 3, 0 };
static const uint8_t weight_bits[16] = { 0, 0, 1, 0, 2, 0, 1, 3, 0, 0, 1, 2, 4, 2, 3, 5 };
static const uint8_t cem_tq[19] = { 0, 3, 5, 0, 3, 5, 0, 3, 5, 0, 3, 5, 0, 3, 5, 0, 3, 0, 0 };
static const uint8_t cem_bits[19] = { 8, 6, 5, 7, 5, 4, 6, 4, 3, 5, 3, 2, 4, 2, 1, 3, 1, 2, 1 };

typedef struct {
  int width, height, parts, dual, plane, weight_range, weight_num, cem[4], cem_range, values;
  int endpoints[4][8];
  int weights[16][2];
  int partition[16];
} block_t;

typedef struct { int bits, nonbits; } seq_t;

/* the block's bits [bit, bit + len), len up to 64 (below bit 0: zeros shifted in) */
static uint64_t bits_at(const uint64_t v[2], int bit, int len)
{
  if (len <= 0)
    return 0;
  uint64_t r;
  if (bit >= 64)
    r = v[1] >> (bit - 64);
  else if (bit <= 0)
    r = v[0] << -bit;
  else if (bit + len <= 64)
    r = v[0] >> bit;
  else
    r = v[0] >> bit | v[1] << (64 - bit);
  return len >= 64 ? r : r & ((1ull << len) - 1);
}

static uint64_t reverse(uint64_t d, int len)
{
  uint64_t r = 0;
  for (int i = 0; i < len; i++)
    r = r << 1 | (d >> i & 1);
  return r;
}

static int clamp255(int n) { return n < 0 ? 0 : n > 255 ? 255 : n; }

/* the five trits packed in T, and the three quints in Q (ASTC specification, C.2.12) */
static void trits_of(unsigned T, int t[5])
{
  unsigned C;
  if ((T >> 2 & 7) == 7) {
    C = (T >> 5 & 7) << 2 | (T & 3);
    t[4] = 2, t[3] = 2;
  } else {
    C = T & 0x1f;
    if ((T >> 5 & 3) == 3)
      t[4] = 2, t[3] = T >> 7 & 1;
    else
      t[4] = T >> 7 & 1, t[3] = T >> 5 & 3;
  }
  if ((C & 3) == 3)
    t[2] = 2, t[1] = C >> 4 & 1, t[0] = (int)((C >> 3 & 1) << 1 | ((C >> 2 & 1) & ~(C >> 3) & 1));
  else if ((C >> 2 & 3) == 3)
    t[2] = 2, t[1] = 2, t[0] = C & 3;
  else
    t[2] = C >> 4 & 1, t[1] = C >> 2 & 3, t[0] = (int)((C >> 1 & 1) << 1 | ((C & 1) & ~(C >> 1) & 1));
}

static void quints_of(unsigned Q, int q[3])
{
  if ((Q >> 1 & 3) == 3 && (Q >> 5 & 3) == 0) {
    unsigned q0 = Q & 1;
    q[2] = (int)(q0 << 2 | ((Q >> 4 & 1) & ~q0 & 1) << 1 | ((Q >> 3 & 1) & ~q0 & 1));
    q[1] = 4, q[0] = 4;
    return;
  }
  unsigned C;
  if ((Q >> 1 & 3) == 3)
    q[2] = 4, C = (Q >> 3 & 3) << 3 | (~(Q >> 5) & 3) << 1 | (Q & 1);
  else
    q[2] = Q >> 5 & 3, C = Q & 0x1f;
  if ((C & 7) == 5)
    q[1] = 4, q[0] = C >> 3 & 3;
  else
    q[1] = C >> 3 & 3, q[0] = C & 7;
}

/* an integer sequence of count values (tq trits or quints or neither, b bits each) from bit `at`, read
 * upwards, or (reversed) downwards from it */
static void read_seq(const uint64_t v[2], int at, int tq, int b, int count, bool reversed, seq_t *out)
{
  int n = 0, mask = (1 << b) - 1;
  if (tq == 3 || tq == 5) {
    int per = tq == 3 ? 5 : 3, size = tq == 3 ? 8 + 5 * b : 7 + 3 * b;
    int blocks = (count + per - 1) / per, last = (count + per - 1) % per + 1;
    int last_size = (size * last + per - 1) / per;
    static const int shifts_t[5] = { 0, 2, 4, 5, 7 }, shifts_q[3] = { 0, 3, 5 };
    for (int i = 0, p = at; i < blocks; i++) {
      int now = i < blocks - 1 ? size : last_size;
      uint64_t d = reversed ? reverse(bits_at(v, p - now, now), now) : bits_at(v, p, now);
      p += reversed ? -size : size;
      int tq_v[5];
      if (tq == 3)
        trits_of((unsigned)((d >> b & 3) | (d >> b * 2 & 0xc) | (d >> b * 3 & 0x10) | (d >> b * 4 & 0x60) |
                            (d >> b * 5 & 0x80)), tq_v);
      else
        quints_of((unsigned)((d >> b & 7) | (d >> b * 2 & 0x18) | (d >> b * 3 & 0x60)), tq_v);
      for (int j = 0; j < per && n < count; j++, n++)
        out[n] = (seq_t){ (int)(d >> ((tq == 3 ? shifts_t[j] : shifts_q[j]) + b * j) & (uint64_t)mask), tq_v[j] };
    }
  } else if (reversed) {
    for (int p = at - b; n < count; n++, p -= b)
      out[n] = (seq_t){ (int)reverse(bits_at(v, p, b), b), 0 };
  } else {
    for (int p = at; n < count; n++, p += b)
      out[n] = (seq_t){ (int)bits_at(v, p, b), 0 };
  }
}

static int seq_bits(int tq, int b, int n)
{
  return n * b + (tq == 3 ? (n * 8 + 4) / 5 : tq == 5 ? (n * 7 + 2) / 3 : 0);
}

/* the block mode's weight grid, partitions and endpoint modes; false: not one a 4x4 block can have */
static bool block_params(const uint8_t *buf, const uint64_t v[2], block_t *bd)
{
  unsigned mode = buf[0] | (unsigned)buf[1] << 8;
  bd->dual = (buf[1] & 4) != 0;
  bd->weight_range = (buf[0] >> 4 & 1) | (buf[1] << 2 & 8);
  if (buf[0] & 3) {
    bd->weight_range |= buf[0] << 1 & 6;
    switch (buf[0] & 0xc) {
      case 0: bd->width = (int)(mode >> 7 & 3) + 4, bd->height = (buf[0] >> 5 & 3) + 2; break;
      case 4: bd->width = (int)(mode >> 7 & 3) + 8, bd->height = (buf[0] >> 5 & 3) + 2; break;
      case 8: bd->width = (buf[0] >> 5 & 3) + 2, bd->height = (int)(mode >> 7 & 3) + 8; break;
      default:
        if (buf[1] & 1)
          bd->width = (buf[0] >> 7 & 1) + 2, bd->height = (buf[0] >> 5 & 3) + 2;
        else
          bd->width = (buf[0] >> 5 & 3) + 2, bd->height = (buf[0] >> 7 & 1) + 6;
    }
  } else {
    bd->weight_range |= buf[0] >> 1 & 6;
    switch (mode & 0x180) {
      case 0: bd->width = 12, bd->height = (buf[0] >> 5 & 3) + 2; break;
      case 0x80: bd->width = (buf[0] >> 5 & 3) + 2, bd->height = 12; break;
      case 0x100:
        bd->width = (buf[0] >> 5 & 3) + 6, bd->height = (buf[1] >> 1 & 3) + 6;
        bd->dual = 0, bd->weight_range &= 7;
        break;
      default: bd->width = (buf[0] & 0x20) ? 10 : 6, bd->height = (buf[0] & 0x20) ? 6 : 10;
    }
  }
  if (bd->width > 4 || bd->height > 4) /* more weights than texels: not for a 4x4 block */
    return false;
  bd->parts = (buf[1] >> 3 & 3) + 1;
  bd->weight_num = bd->width * bd->height * (bd->dual ? 2 : 1);
  int wbits = seq_bits(weight_tq[bd->weight_range], weight_bits[bd->weight_range], bd->weight_num);
  int config, cem_base = 0;
  if (bd->parts == 1) {
    bd->cem[0] = (int)((buf[1] | (unsigned)buf[2] << 8) >> 5 & 0xf);
    config = 17;
  } else {
    cem_base = (int)((buf[2] | (unsigned)buf[3] << 8) >> 7 & 3);
    if (cem_base == 0) {
      for (int i = 0; i < bd->parts; i++)
        bd->cem[i] = buf[3] >> 1 & 0xf;
      config = 29;
    } else {
      for (int i = 0; i < bd->parts; i++)
        bd->cem[i] = ((buf[3] >> (i + 1) & 1) + cem_base - 1) << 2;
      switch (bd->parts) {
        case 2:
          bd->cem[0] |= buf[3] >> 3 & 3;
          bd->cem[1] |= (int)bits_at(v, 126 - wbits, 2);
          break;
        case 3:
          bd->cem[0] |= buf[3] >> 4 & 1;
          bd->cem[0] |= (int)bits_at(v, 122 - wbits, 2) & 2;
          bd->cem[1] |= (int)bits_at(v, 124 - wbits, 2);
          bd->cem[2] |= (int)bits_at(v, 126 - wbits, 2);
          break;
        default:
          for (int i = 0; i < 4; i++)
            bd->cem[i] |= (int)bits_at(v, 120 + i * 2 - wbits, 2);
      }
      config = 25 + bd->parts * 3;
    }
  }
  if (bd->dual) {
    config += 2;
    bd->plane = (int)bits_at(v, cem_base ? 130 - wbits - bd->parts * 3 : 126 - wbits, 2);
  }
  int remain = 128 - config - wbits;
  bd->values = 0;
  for (int i = 0; i < bd->parts; i++)
    bd->values += (bd->cem[i] >> 1 & 6) + 2;
  if (remain < 0 || bd->values > 32)
    return false;
  bd->cem_range = -1;
  for (int i = 0; i < 19; i++) {
    if (seq_bits(cem_tq[i], cem_bits[i], bd->values) <= remain) {
      bd->cem_range = i;
      break;
    }
  }
  return bd->cem_range >= 0;
}

static void transfer(int *a, int *b) /* bit_transfer_signed */
{
  *b = (*b >> 1) | (*a & 0x80);
  *a = (*a >> 1) & 0x3f;
  if (*a & 0x20)
    *a -= 0x40;
}

static void set_ends(int e[8], int r1, int g1, int b1, int a1, int r2, int g2, int b2, int a2, bool blue, bool clamp)
{
  int v[8] = { r1, g1, b1, a1, r2, g2, b2, a2 };
  if (blue) /* blue contraction */
    v[0] = (r1 + b1) >> 1, v[1] = (g1 + b1) >> 1, v[4] = (r2 + b2) >> 1, v[5] = (g2 + b2) >> 1;
  for (int i = 0; i < 8; i++)
    e[i] = clamp ? clamp255(v[i]) : v[i];
}

static void endpoints(const uint64_t v64[2], block_t *bd)
{
  static const int trit_c[7] = { 0, 204, 93, 44, 22, 11, 5 }, quint_c[6] = { 0, 113, 54, 26, 13, 6 };
  seq_t seq[32];
  int ev[32];
  int tq = cem_tq[bd->cem_range], b = cem_bits[bd->cem_range];
  read_seq(v64, bd->parts == 1 ? 17 : 29, tq, b, bd->values, false, seq);
  for (int i = 0; i < bd->values; i++) {
    int bits = seq[i].bits;
    if (tq == 3 || tq == 5) { /* unquantised with the specification's C and B */
      int a = (bits & 1) * 0x1ff, x = bits >> 1, bb = 0;
      if (tq == 3) {
        switch (b) {
          case 2: bb = 0x116 * x; break;
          case 3: bb = x << 7 | x << 2 | x; break;
          case 4: bb = x << 6 | x; break;
          case 5: bb = x << 5 | x >> 2; break;
          case 6: bb = x << 4 | x >> 4; break;
        }
      } else {
        switch (b) {
          case 2: bb = 0x10c * x; break;
          case 3: bb = x << 7 | x << 1 | x >> 1; break;
          case 4: bb = x << 6 | x >> 1; break;
          case 5: bb = x << 5 | x >> 3; break;
        }
      }
      int c = tq == 3 ? trit_c[b] : quint_c[b];
      ev[i] = (a & 0x80) | ((seq[i].nonbits * c + bb) ^ a) >> 2;
    } else { /* bits replicated up to 8 */
      int r = 0;
      for (int s = 8 - b; s > -b; s -= b)
        r |= s >= 0 ? bits << s : bits >> -s;
      ev[i] = b == 1 ? bits * 0xff : r & 0xff;
    }
  }
  int *v = ev;
  for (int p = 0; p < bd->parts; v += (bd->cem[p] / 4 + 1) * 2, p++) {
    int *e = bd->endpoints[p];
    switch (bd->cem[p]) {
      case 0: set_ends(e, v[0], v[0], v[0], 255, v[1], v[1], v[1], 255, false, false); break;
      case 1: {
        int l0 = (v[0] >> 2) | (v[1] & 0xc0), l1 = clamp255(l0 + (v[1] & 0x3f));
        set_ends(e, l0, l0, l0, 255, l1, l1, l1, 255, false, false);
      } break;
      case 4: set_ends(e, v[0], v[0], v[0], v[2], v[1], v[1], v[1], v[3], false, false); break;
      case 5:
        transfer(&v[1], &v[0]);
        transfer(&v[3], &v[2]);
        v[1] += v[0];
        set_ends(e, v[0], v[0], v[0], v[2], v[1], v[1], v[1], v[2] + v[3], false, true);
        break;
      case 6:
        set_ends(e, v[0] * v[3] >> 8, v[1] * v[3] >> 8, v[2] * v[3] >> 8, 255, v[0], v[1], v[2], 255, false, false);
        break;
      case 8:
        if (v[0] + v[2] + v[4] <= v[1] + v[3] + v[5])
          set_ends(e, v[0], v[2], v[4], 255, v[1], v[3], v[5], 255, false, false);
        else
          set_ends(e, v[1], v[3], v[5], 255, v[0], v[2], v[4], 255, true, false);
        break;
      case 9:
        transfer(&v[1], &v[0]);
        transfer(&v[3], &v[2]);
        transfer(&v[5], &v[4]);
        if (v[1] + v[3] + v[5] >= 0)
          set_ends(e, v[0], v[2], v[4], 255, v[0] + v[1], v[2] + v[3], v[4] + v[5], 255, false, true);
        else
          set_ends(e, v[0] + v[1], v[2] + v[3], v[4] + v[5], 255, v[0], v[2], v[4], 255, true, true);
        break;
      case 10:
        set_ends(e, v[0] * v[3] >> 8, v[1] * v[3] >> 8, v[2] * v[3] >> 8, v[4], v[0], v[1], v[2], v[5], false, false);
        break;
      case 12:
        if (v[0] + v[2] + v[4] <= v[1] + v[3] + v[5])
          set_ends(e, v[0], v[2], v[4], v[6], v[1], v[3], v[5], v[7], false, false);
        else
          set_ends(e, v[1], v[3], v[5], v[7], v[0], v[2], v[4], v[6], true, false);
        break;
      case 13:
        transfer(&v[1], &v[0]);
        transfer(&v[3], &v[2]);
        transfer(&v[5], &v[4]);
        transfer(&v[7], &v[6]);
        if (v[1] + v[3] + v[5] >= 0)
          set_ends(e, v[0], v[2], v[4], v[6], v[0] + v[1], v[2] + v[3], v[4] + v[5], v[6] + v[7], false, true);
        else
          set_ends(e, v[0] + v[1], v[2] + v[3], v[4] + v[5], v[6] + v[7], v[0], v[2], v[4], v[6], true, true);
        break;
      default: /* HDR: the error colour */
        set_ends(e, 255, 0, 255, 255, 255, 0, 255, 255, false, false);
    }
  }
}

static void weights(const uint64_t v[2], block_t *bd)
{
  seq_t seq[64];
  int wv[64 + 8] = { 0 };
  int tq = weight_tq[bd->weight_range], b = weight_bits[bd->weight_range], n = bd->weight_num;
  read_seq(v, 128, tq, b, n, true, seq);
  for (int i = 0; i < n; i++) {
    int bits = seq[i].bits, w;
    if (tq == 0) {
      switch (b) {
        case 1: w = bits ? 63 : 0; break;
        case 2: w = bits << 4 | bits << 2 | bits; break;
        case 3: w = bits << 3 | bits; break;
        case 4: w = bits << 2 | bits >> 2; break;
        default: w = bits << 1 | bits >> 4;
      }
    } else if (b == 0) {
      w = seq[i].nonbits * (tq == 3 ? 32 : 16);
    } else {
      if (tq == 3)
        w = b == 1 ? seq[i].nonbits * 50
          : b == 2 ? seq[i].nonbits * 23 + ((bits & 2) ? 0x45 : 0)
                   : seq[i].nonbits * 11 + ((bits << 4 | bits >> 1) & 0x63);
      else
        w = b == 1 ? seq[i].nonbits * 28 : seq[i].nonbits * 13 + ((bits & 2) ? 0x42 : 0);
      int a = (bits & 1) * 0x7f;
      w = (a & 0x20) | ((w ^ a) >> 2);
    }
    wv[i] = w > 32 && (tq == 0 || b > 0) ? w + 1 : w; /* (the ranges of trits or quints alone are exact) */
  }
  /* the weight grid onto the 4x4 texels, bilinearly */
  const int ds = (1024 + 2) / 3, pn = bd->dual ? 2 : 1;
  for (int t = 0, i = 0; t < 4; t++) {
    for (int s = 0; s < 4; s++, i++) {
      int gs = (ds * s * (bd->width - 1) + 32) >> 6, gt = (ds * t * (bd->height - 1) + 32) >> 6;
      int fs = gs & 0xf, ft = gt & 0xf, at = (gs >> 4) + (gt >> 4) * bd->width;
      int w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
      for (int p = 0; p < pn; p++) {
        int p00 = wv[at * pn + p], p01 = wv[(at + 1) * pn + p];
        int p10 = wv[(at + bd->width) * pn + p], p11 = wv[(at + bd->width + 1) * pn + p];
        bd->weights[i][p] = (p00 * w00 + p01 * w01 + p10 * w10 + p11 * w11 + 8) >> 4;
      }
    }
  }
}

static void partitions(const uint8_t *buf, block_t *bd)
{
  uint32_t seed = (uint32_t)((buf[1] | (unsigned)buf[2] << 8) >> 5 & 0x3ff) | (uint32_t)(bd->parts - 1) << 10;
  uint32_t r = seed;
  r ^= r >> 15;
  r -= r << 17;
  r += r << 7;
  r += r << 4;
  r ^= r >> 5;
  r += r << 16;
  r ^= r >> 7;
  r ^= r >> 3;
  r ^= r << 6;
  r ^= r >> 17;
  int seeds[8];
  for (int i = 0; i < 8; i++) {
    seeds[i] = (int)(r >> (i * 4) & 0xf);
    seeds[i] *= seeds[i];
  }
  int sh[2] = { (seed & 2) ? 4 : 5, bd->parts == 3 ? 6 : 5 };
  for (int i = 0; i < 8; i++)
    seeds[i] >>= (seed & 1) ? sh[i % 2] : sh[1 - i % 2];
  for (int t = 0, i = 0; t < 4; t++) {
    for (int s = 0; s < 4; s++, i++) { /* a small block: its coordinates doubled */
      int x = s << 1, y = t << 1;
      int a = (int)((uint32_t)(seeds[0] * x + seeds[1] * y) + (r >> 14)) & 0x3f;
      int b = (int)((uint32_t)(seeds[2] * x + seeds[3] * y) + (r >> 10)) & 0x3f;
      int c = bd->parts < 3 ? 0 : (int)((uint32_t)(seeds[4] * x + seeds[5] * y) + (r >> 6)) & 0x3f;
      int d = bd->parts < 4 ? 0 : (int)((uint32_t)(seeds[6] * x + seeds[7] * y) + (r >> 2)) & 0x3f;
      bd->partition[i] = (a >= b && a >= c && a >= d) ? 0 : (b >= c && b >= d) ? 1 : (c >= d) ? 2 : 3;
    }
  }
}

static uint8_t lerp(int v0, int v1, int w)
{
  return (uint8_t)(((((v0 << 8 | v0) * (64 - w) + (v1 << 8 | v1) * w + 32) >> 6) * 255 + 32768) / 65536);
}

void astc_block(const uint8_t buf[16], uint8_t out[16][4])
{
  uint64_t v[2] = { 0, 0 };
  for (int i = 0; i < 8; i++) {
    v[0] |= (uint64_t)buf[i] << (8 * i);
    v[1] |= (uint64_t)buf[8 + i] << (8 * i);
  }
  if (buf[0] == 0xfc && (buf[1] & 1)) { /* one colour (void extent): its 16-bit channels' high bytes */
    for (int i = 0; i < 16; i++) {
      bool hdr = (buf[1] & 2) != 0;
      out[i][0] = hdr ? 255 : buf[9], out[i][1] = hdr ? 0 : buf[11];
      out[i][2] = hdr ? 255 : buf[13], out[i][3] = hdr ? 255 : buf[15];
    }
    return;
  }
  block_t bd;
  if (((buf[0] & 0xc3) == 0xc0 && (buf[1] & 1)) || (buf[0] & 0xf) == 0 || !block_params(buf, v, &bd)) {
    for (int i = 0; i < 16; i++) /* reserved or not for this size: the error colour */
      out[i][0] = 255, out[i][1] = 0, out[i][2] = 255, out[i][3] = 255;
    return;
  }
  endpoints(v, &bd);
  weights(v, &bd);
  if (bd.parts > 1)
    partitions(buf, &bd);
  int ps[4] = { 0, 0, 0, 0 };
  if (bd.dual)
    ps[bd.plane] = 1;
  for (int i = 0; i < 16; i++) {
    const int *e = bd.endpoints[bd.parts > 1 ? bd.partition[i] : 0];
    for (int c = 0; c < 4; c++)
      out[i][c] = lerp(e[c], e[c + 4], bd.weights[i][ps[c]]);
  }
}
