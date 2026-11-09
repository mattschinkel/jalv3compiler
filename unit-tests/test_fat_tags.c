/* Fat-pointer tag encoding (F0/F0b): pure header test, no library linking. */
#include <assert.h>
#include <stdio.h>
#include "pic_fat.h"

/* A LINEAR file address must never read as a tag: tag 0 is "legacy linear".
 * F0b (2026-11-08): the 14H tag moved from byte 1 bits 4..6 to bits 5..6
 * because every PIC_14H device in the library places data up to 0x196F, so
 * byte 1 reaches 0x10 from 0x1000 on and bit 4 is a real address bit. */
static void linear_range_ok(pic_fat_target_t t, unsigned long lo,
    unsigned long hi)
{
  unsigned char b[3];
  unsigned long a;

  for (a = lo; a <= hi; a++) {
    b[0] = (unsigned char) (a & 0xFFUL);
    b[1] = (unsigned char) ((a >> 8) & 0xFFUL);
    b[2] = (unsigned char) ((a >> 16) & 0xFFUL);
    assert(0 == PIC_FAT_TAG_GET(t, b));
    assert(0 == PIC_FAT_IS_ROM(t, b));
  }
}

/* ---- F2: the segment descriptor ------------------------------------------ */

/* F2 fact: the descriptor is the runtime chunk-map format, and the walk
 * reproduces the object's physical addresses for every in-object offset. */
static void f2_descriptor(void)
{
  /* the real 16F18857 chunk layout of a 200-byte object: 0x29-0x6E,
   * 0xA0-0xEF, 0x120-0x16F */
  static const unsigned long base[3] = { 0x0029UL, 0x00A0UL, 0x0120UL };
  static const unsigned      len[3]  = { 0x0046U,  0x0050U,  0x0050U };
  unsigned char desc[10];
  unsigned long ofs;

  assert(pic_fat_desc_size(3) == 10);
  pic_fat_desc_build(3, base, len, desc);
  assert(desc[0] == 3);
  assert(desc[1] == 0x29 && desc[2] == 0x00 && desc[3] == 0x46);
  assert(desc[4] == 0xA0 && desc[5] == 0x00 && desc[6] == 0x50);
  assert(desc[7] == 0x20 && desc[8] == 0x01 && desc[9] == 0x50);

  for (ofs = 0; ofs < 0x46UL + 0x50UL + 0x50UL; ofs++) {
    unsigned long want;

    if (ofs < 0x46UL) {
      want = 0x29UL + ofs;
    } else if (ofs < 0x46UL + 0x50UL) {
      want = 0xA0UL + (ofs - 0x46UL);
    } else {
      want = 0x120UL + (ofs - 0x46UL - 0x50UL);
    }
    assert(want == pic_fat_seg_addr(desc, ofs));
  }
  /* past the end clamps to the last byte of the last segment (the object
   * view's documented behaviour) */
  assert(0x120UL + 0x50UL - 1UL == pic_fat_seg_addr(desc, 0xE6UL));
  assert(0x120UL + 0x50UL - 1UL == pic_fat_seg_addr(desc, 0xFFFFUL));

  /* one segment is the contiguous case */
  {
    static const unsigned long b1[1] = { 0x0100UL };
    static const unsigned      l1[1] = { 0x0040U };
    unsigned char d1[4];

    assert(pic_fat_desc_size(1) == 4);
    pic_fat_desc_build(1, b1, l1, d1);
    assert(pic_fat_seg_addr(d1, 0) == 0x0100UL);
    assert(pic_fat_seg_addr(d1, 0x3FUL) == 0x013FUL);
  }

  /* zero segments is a caller bug, not a walk off the map */
  desc[0] = 0;
  assert(0 == pic_fat_seg_addr(desc, 0));
}

/* ---- F1: the value operations (pic_fat.c) -------------------------------- */

/* the widest object each target can hold, i.e. the largest object-relative
 * offset the compiler can form */
struct f1_case {
  pic_fat_target_t t;
  unsigned long    max_off;
  unsigned         tag;
};

/* a plain multi-byte add on the encoded value, exactly what the emitters do */
static void raw_add(pic_fat_target_t t, unsigned char *b, unsigned long k)
{
  unsigned long carry;
  unsigned long sum;
  unsigned       i;
  unsigned       n;

  n = pic_fat_ptr_size(t);
  carry = 0;
  for (i = 0; i < n; i++) {
    sum = (unsigned long) b[i] + ((k >> (8U * i)) & 0xFFUL) + carry;
    b[i] = (unsigned char) (sum & 0xFFUL);
    carry = sum >> 8;
  }
}

/* encode/decode round-trip: the offset field is wide enough for the case */
static void f1_offset_field(void)
{
  static const struct f1_case cases[] = {
    { PIC_FAT_TARGET_CLASSIC, 0x01EFUL, 5 },
    { PIC_FAT_TARGET_14H,     0x196FUL, 2 },
    { PIC_FAT_TARGET_18,      0x0FFFUL, 9 }
  };
  unsigned char b[4];
  unsigned      ci;

  for (ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
    pic_fat_target_t t;
    unsigned long    off;

    t = cases[ci].t;
    for (off = 0; off <= cases[ci].max_off; off++) {
      pic_fat_value_set(t, b, cases[ci].tag, off, 0);
      assert(cases[ci].tag == PIC_FAT_TAG_GET(t, b));
      assert(off == pic_fat_offset_get(t, b));
      assert(0 == PIC_FAT_IS_ROM(t, b));
    }
  }
}

/* F1 fact 1: offset + k stays inside the field for every in-object k, so the
 * ordinary add keeps the tag -- on every target, with no guard. */
static void f1_add_keeps_tag(void)
{
  static const struct f1_case cases[] = {
    { PIC_FAT_TARGET_CLASSIC, 0x01EFUL, 5 },
    { PIC_FAT_TARGET_14H,     0x196FUL, 2 },
    { PIC_FAT_TARGET_18,      0x0FFFUL, 9 }
  };
  static const unsigned long steps[] = { 1UL, 2UL, 3UL, 255UL, 256UL, 0x100UL };
  unsigned char b[4];
  unsigned char c[4];
  unsigned      ci;
  unsigned      si;
  unsigned long off;

  for (ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
    pic_fat_target_t t;

    t = cases[ci].t;
    for (off = 0; off <= cases[ci].max_off; off++) {
      for (si = 0; si < sizeof(steps) / sizeof(steps[0]); si++) {
        unsigned long k;

        k = steps[si];
        if (off + k > cases[ci].max_off) {
          continue;
        }
        /* the raw add (the emitter's path) keeps the tag and the offset */
        pic_fat_value_set(t, b, cases[ci].tag, off, 0);
        raw_add(t, b, k);
        assert(cases[ci].tag == PIC_FAT_TAG_GET(t, b));
        assert(off + k == pic_fat_offset_get(t, b));

        /* pic_fat_offset_add agrees with it byte for byte */
        pic_fat_value_set(t, c, cases[ci].tag, off, 0);
        pic_fat_offset_add(t, c, (long) k);
        assert(0 == pic_fat_cmp(t, b, c));

        /* the ROM marker survives the add too */
        pic_fat_value_set(t, b, 0, off, 1);
        raw_add(t, b, k);
        assert(1 == PIC_FAT_IS_ROM(t, b));
        assert(off + k == pic_fat_offset_get(t, b));
      }
      /* and a bounded subtract comes back */
      if (off > 0) {
        pic_fat_value_set(t, b, cases[ci].tag, off, 0);
        pic_fat_offset_add(t, b, -1L);
        assert(cases[ci].tag == PIC_FAT_TAG_GET(t, b));
        assert(off - 1 == pic_fat_offset_get(t, b));
      }
    }
  }
}

/* F1 fact 2: the layout makes an ordinary unsigned compare "tag first, then
 * offset"; the ROM marker sorts above every RAM value. */
static void f1_compare_is_tag_first(void)
{
  static const struct {
    unsigned     tag;
    unsigned long off;
    int           rom;
  } v[] = {
    { 0, 0x0000UL, 0 },
    { 0, 0x0100UL, 0 },
    { 0, 0x0000UL, 1 },
    { 0, 0x02AAUL, 1 },
    { 1, 0x0000UL, 0 },
    { 1, 0x07FFUL, 0 },
    { 2, 0x0000UL, 0 },
    { 3, 0x0FEFUL, 0 }
  };
  static const struct f1_case cases[] = {
    { PIC_FAT_TARGET_CLASSIC, 0x01EFUL, 5 },
    { PIC_FAT_TARGET_14H,     0x196FUL, 2 },
    { PIC_FAT_TARGET_18,      0x0FFFUL, 9 }
  };
  unsigned char a[4];
  unsigned char c[4];
  unsigned      ci;
  unsigned      i;
  unsigned      j;

  for (ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
    pic_fat_target_t t;

    t = cases[ci].t;
    for (i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
      for (j = 0; j < sizeof(v) / sizeof(v[0]); j++) {
        int want;

        /* the ROM marker is only legal on a linear (tag 0) value */
        if ((v[i].rom || v[j].rom) && (v[i].tag || v[j].tag)) {
          continue;
        }
        if (v[i].tag > cases[ci].tag || v[j].tag > cases[ci].tag) {
          continue;
        }
        if (v[i].off > cases[ci].max_off || v[j].off > cases[ci].max_off) {
          continue;
        }
        pic_fat_value_set(t, a, v[i].tag, v[i].off, v[i].rom);
        pic_fat_value_set(t, c, v[j].tag, v[j].off, v[j].rom);
        /* tuple order: (rom, tag, offset) */
        want = 0;
        if (v[i].rom != v[j].rom) {
          want = (v[i].rom < v[j].rom) ? -1 : 1;
        } else if (v[i].tag != v[j].tag) {
          want = (v[i].tag < v[j].tag) ? -1 : 1;
        } else if (v[i].off != v[j].off) {
          want = (v[i].off < v[j].off) ? -1 : 1;
        }
        assert(want == pic_fat_cmp(t, a, c));
        /* same object == same tag; two linear values are always "the same
         * object" because the linear space has no objects */
        assert((v[i].tag == v[j].tag) == (1 == pic_fat_same_object(t, a, c)));
      }
    }
  }
}

int main(void)
{
  unsigned char p18[3];
  unsigned char p14[2];
  unsigned char p14h[2];
  unsigned char pflat[4];
  unsigned      i;

  /* PIC18: 3 bytes, tag in byte 2 bits 0..6 (127 ids), ROM = bit 7 */
  assert(PIC_FAT_TAG_MAX(PIC_FAT_TARGET_18) == 127);
  p18[0] = 0x34;
  p18[1] = 0x12;
  p18[2] = 0x80;
  for (i = 0; i <= 127; i++) {
    PIC_FAT_TAG_SET(PIC_FAT_TARGET_18, p18, i);
    assert(PIC_FAT_TAG_GET(PIC_FAT_TARGET_18, p18) == i);
    assert(PIC_FAT_IS_ROM(PIC_FAT_TARGET_18, p18) == 1);
    assert(p18[0] == 0x34);
    assert(p18[1] == 0x12);
  }
  PIC_FAT_TAG_SET(PIC_FAT_TARGET_18, p18, 0);
  assert(p18[2] == 0x80);

  /* classic 14: 2 bytes, tag in byte 1 bits 1..6 (63 ids), bit 0 + bit 7 kept */
  assert(PIC_FAT_TAG_MAX(PIC_FAT_TARGET_CLASSIC) == 63);
  p14[0] = 0xAA;
  p14[1] = 0x81;
  for (i = 0; i <= 63; i++) {
    PIC_FAT_TAG_SET(PIC_FAT_TARGET_CLASSIC, p14, i);
    assert(PIC_FAT_TAG_GET(PIC_FAT_TARGET_CLASSIC, p14) == i);
    assert((p14[1] & 0x01) == 0x01);
    assert(PIC_FAT_IS_ROM(PIC_FAT_TARGET_CLASSIC, p14) == 1);
    assert(p14[0] == 0xAA);
  }

  /* 14H: 2 bytes, tag in byte 1 bits 5..6 (3 ids), bits 0..4 + bit 7 kept */
  assert(PIC_FAT_TAG_MAX(PIC_FAT_TARGET_14H) == 3);
  assert(PIC_FAT_TAG_SHIFT(PIC_FAT_TARGET_14H) == 5);
  assert(PIC_FAT_TAG_BITS(PIC_FAT_TARGET_14H) == 2);
  p14h[0] = 0x55;
  p14h[1] = 0x9F;
  for (i = 0; i <= 3; i++) {
    PIC_FAT_TAG_SET(PIC_FAT_TARGET_14H, p14h, i);
    assert(PIC_FAT_TAG_GET(PIC_FAT_TARGET_14H, p14h) == i);
    assert((p14h[1] & 0x1F) == 0x1F);
    assert(PIC_FAT_IS_ROM(PIC_FAT_TARGET_14H, p14h) == 1);
    assert(p14h[0] == 0x55);
  }

  /* the largest 14H object-relative offset (the whole data space) still fits
   * under the tag field: 0x0FEF is bits 0..11, the tag starts at bit 13 */
  p14h[0] = 0xEF;
  p14h[1] = 0x0F;
  PIC_FAT_TAG_SET(PIC_FAT_TARGET_14H, p14h, 3);
  assert(PIC_FAT_TAG_GET(PIC_FAT_TARGET_14H, p14h) == 3);
  assert(p14h[0] == 0xEF);
  assert((p14h[1] & 0x1F) == 0x0F);

  /* F0b witness: no real linear address on any core reads as a fat id.
   * Ranges are the full data space of the library's device files. */
  linear_range_ok(PIC_FAT_TARGET_CLASSIC, 0x0020UL, 0x01EFUL);
  linear_range_ok(PIC_FAT_TARGET_14H,     0x0020UL, 0x196FUL);
  linear_range_ok(PIC_FAT_TARGET_18,      0x0020UL, 0x36FFUL);

  /* flat: no tag bits -- a set is a no-op */
  assert(PIC_FAT_TAG_MAX(PIC_FAT_TARGET_FLAT) == 0);
  pflat[0] = 1;
  pflat[1] = 2;
  pflat[2] = 3;
  pflat[3] = 4;
  PIC_FAT_TAG_SET(PIC_FAT_TARGET_FLAT, pflat, 5);
  assert(pflat[0] == 1 && pflat[1] == 2 && pflat[2] == 3 && pflat[3] == 4);

  f1_offset_field();
  f1_add_keeps_tag();
  f1_compare_is_tag_first();
  f2_descriptor();

  printf("fat tag encoding: ok\n");
  return 0;
}
