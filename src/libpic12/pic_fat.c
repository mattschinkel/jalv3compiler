/************************************************************
 **
 ** pic_fat.c : fat-pointer value operations (F1)
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ** The representation lives in pic_fat.h.  This file implements the
 ** operations on an encoded pointer value: decode, build, offset
 ** arithmetic and ordering.  It is deliberately free of compiler state so
 ** the encoding, the arithmetic and the comparison rule can be unit-tested
 ** (unit-tests/test_fat_tags.c).
 **
 ** Two facts about the layout carry the whole F1 design, and both are
 ** asserted by unit-tests/test_fat_tags.c:
 **
 **  1. NO TARGET NEEDS AN ADD GUARD.  A pointer's offset is object-relative
 **     and an object is at most one data space, so every offset the compiler
 **     can produce fits the offset field with room to spare:
 **
 **       target    offset field        widest object in the library
 **       PIC18     16 bits (2 bytes)   0x1000  (4 KiB)
 **       classic   9 bits (bit 8 top)  0x1EF   (16F887, 495 B)
 **       14H       13 bits (bits 8-12) 0x196F  (16F18857, 6503 B)
 **
 **     `offset + k` with `offset + k` still inside the object therefore
 **     cannot carry out of the field, and an ordinary multi-byte add on the
 **     encoded value -- which is what the emitters already do -- keeps the
 **     tag intact.  (The addend must be bounded by the object; the language
 **     only forms pointer offsets from subscripts and element binds, which
 **     are.)  The tests sweep every offset/complement over the real object
 **     range per target and assert the tag survives.
 **
 **  2. NO TARGET NEEDS A COMPARISON CHANGE.  The tag occupies the most
 **     significant free bits of the top byte, so an ordinary unsigned
 **     compare of the encoded value is already "compare the tag first, then
 **     the offset" -- on every target.  The compiler therefore needs no new
 **     comparison code for fat pointers; this file exposes the rule as
 **     pic_fat_cmp()/pic_fat_same_object() so it can be tested and reused by
 **     `whereis`, constant folding and the debugger.
 **
 ************************************************************/
#include "pic_fat.h"

/* pointer value width in bytes (the tag never grows the value in F1) */
unsigned pic_fat_ptr_size(pic_fat_target_t t)
{
  if ((PIC_FAT_TARGET_18 == t) || (PIC_FAT_TARGET_14H3 == t)) {
    return 3;
  }
  if (PIC_FAT_TARGET_FLAT == t) {
    return 4;
  }
  return 2;
}

/* the offset bits carried in the TOP byte; the low bytes carry 8 bits each.
 * PIC18 keeps the tag in the whole top byte, so it has no offset bits there;
 * classic 14 has one (the address bit 8), 14H has five (address bits 8..12). */
unsigned pic_fat_off_mask(pic_fat_target_t t)
{
  if (PIC_FAT_TARGET_14H == t) {
    return PIC_FAT_OFF_MASK_14H;
  }
  if (PIC_FAT_TARGET_CLASSIC == t) {
    return PIC_FAT_OFF_MASK_CLASSIC;
  }
  return 0;
}

unsigned long pic_fat_offset_get(pic_fat_target_t t, const unsigned char *b)
{
  unsigned long v;
  unsigned      i;
  unsigned      n;

  n = pic_fat_ptr_size(t);
  v = 0;
  for (i = 0; i < n; i++) {
    unsigned long byte;

    byte = b[i];
    if (i + 1 == n) {
      byte &= pic_fat_off_mask(t);
    }
    v |= byte << (8U * i);
  }
  return v;
}

/* tag, offset and program-memory marker of an encoded value */
void pic_fat_value_decode(pic_fat_target_t t, const unsigned char *b,
    unsigned *tag, unsigned long *offset, int *rom)
{
  if (0 != tag) {
    *tag = PIC_FAT_TAG_GET(t, b);
  }
  if (0 != offset) {
    *offset = pic_fat_offset_get(t, b);
  }
  if (0 != rom) {
    *rom = PIC_FAT_IS_ROM(t, b);
  }
}

/* rebuild an encoded value from its parts; every other bit is zero, so the
 * result is canonical (two equal (tag, offset, rom) triples always encode
 * to the same bytes) */
void pic_fat_value_set(pic_fat_target_t t, unsigned char *b, unsigned tag,
    unsigned long offset, int rom)
{
  unsigned i;
  unsigned n;

  n = pic_fat_ptr_size(t);
  for (i = 0; i < n; i++) {
    b[i] = (unsigned char) ((offset >> (8U * i)) & 0xFFUL);
  }
  if (PIC_FAT_TARGET_FLAT != t) {
    b[n - 1] = (unsigned char) (b[n - 1] & pic_fat_off_mask(t));
    PIC_FAT_TAG_SET(t, b, tag);
    if (rom) {
      b[n - 1] = (unsigned char) (b[n - 1] | PIC_FAT_ROM_BIT);
    }
  }
}

/* offset += k, tag and ROM marker preserved (see the header comment for why
 * the add itself stays inside the offset field) */
void pic_fat_offset_add(pic_fat_target_t t, unsigned char *b, long k)
{
  unsigned      tag;
  unsigned long offset;
  int           rom;

  pic_fat_value_decode(t, b, &tag, &offset, &rom);
  if (k < 0) {
    offset -= (unsigned long) (-k);
  } else {
    offset += (unsigned long) k;
  }
  pic_fat_value_set(t, b, tag, offset, rom);
}

/* unsigned compare of the encoded value, MSB first.  On every target the tag
 * occupies the most significant free bits of the top byte, so this is
 * "tag first, then offset" by construction; the ROM marker (bit 7 of the top
 * byte) sorts above every RAM value. */
int pic_fat_cmp(pic_fat_target_t t, const unsigned char *a,
    const unsigned char *c)
{
  unsigned n;

  n = pic_fat_ptr_size(t);
  while (n > 0) {
    n--;
    if (a[n] != c[n]) {
      return (a[n] < c[n]) ? -1 : 1;
    }
  }
  return 0;
}

/* two values address the same object when their tags are equal; both linear
 * (tag 0) counts as the same object -- the linear space has no objects */
int pic_fat_same_object(pic_fat_target_t t, const unsigned char *a,
    const unsigned char *c)
{
  return (PIC_FAT_TAG_GET(t, a) == PIC_FAT_TAG_GET(t, c)) ? 1 : 0;
}

/* ---- F2: the segment descriptor ----------------------------------------
 *
 * A fat object needs its segment list at run time, because the deref only
 * has the object id.  The descriptor uses the RUNTIME CHUNK-MAP format the
 * walks already read (pic_op_fsr.c, pic_var.c -- the open-array formal maps
 * and the string-view maps):
 *
 *     [count][base0 lo, base0 hi, span0][base1 lo, base1 hi, span1]...
 *
 * so `_seg_addr(desc, ofs)` is the same loop with the map address taken from
 * the pointer instead of from a formal's map variable.  Sizes are the one
 * constraint the format imposes: at most 255 segments and at most 255 bytes
 * per segment, both of which hold for every RAM run the allocator produces.
 */
unsigned pic_fat_desc_size(unsigned seg_ct)
{
  return 1U + 3U * seg_ct;
}

void pic_fat_desc_build(unsigned seg_ct, const unsigned long *base,
    const unsigned *len, unsigned char *desc)
{
  unsigned ii;

  desc[0] = (unsigned char) seg_ct;
  for (ii = 0; ii < seg_ct; ii++) {
    desc[1U + 3U * ii] = (unsigned char) (base[ii] & 0xFFUL);
    desc[2U + 3U * ii] = (unsigned char) ((base[ii] >> 8) & 0xFFUL);
    desc[3U + 3U * ii] = (unsigned char) len[ii];
  }
}

/* Physical address of byte `ofs` inside the described object.  Offsets past
 * the end clamp to the last byte (the object view's documented behaviour, so
 * diagnostics keep pointing at the object).  A zero-segment descriptor is a
 * caller bug; return 0 rather than walk off the map. */
unsigned long pic_fat_seg_addr(const unsigned char *desc, unsigned long ofs)
{
  unsigned      ct;
  unsigned      ii;
  unsigned long base;
  unsigned long span;

  ct = desc[0];
  if (0 == ct) {
    return 0;
  }
  for (ii = 0; ii < ct; ii++) {
    span = desc[3U + 3U * ii];
    if (ofs < span) {
      base = (unsigned long) desc[1U + 3U * ii]
        | ((unsigned long) desc[2U + 3U * ii] << 8);
      return base + ofs;
    }
    ofs -= span;
  }
  /* past the end: clamp to the last segment's last byte */
  base = (unsigned long) desc[1U + 3U * (ct - 1U)]
    | ((unsigned long) desc[2U + 3U * (ct - 1U)] << 8);
  span = desc[3U + 3U * (ct - 1U)];
  return base + span - 1UL;
}
