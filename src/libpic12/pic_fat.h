/************************************************************
 **
 ** pic_fat.h : fat-pointer representation (F0)
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ** The tag rides in bits the pointer already has: the file address is
 ** 9/12 bits, so the unused high bits plus the existing ROM marker bit
 ** leave room for an object id without growing the pointer on PIC18 and
 ** classic 14-bit, and on 14H for up to 7 fat objects.  Tag 0 is the
 ** legacy linear RAM form (the fast path, byte-identical); 1..max is an
 ** object id with an OBJECT-RELATIVE offset in the low bytes; bit 7 of
 ** the top byte stays the program-memory marker.
 **
 ** This header is self-contained (no library includes) so the encoding
 ** can be unit-tested directly.
 **
 ************************************************************/
#ifndef pic_fat_h__
#define pic_fat_h__

typedef enum pic_fat_target_ {
  PIC_FAT_TARGET_CLASSIC = 0, /* 14-bit: 2 bytes, tag = byte 1 bits 1..6 */
  PIC_FAT_TARGET_14H     = 1, /* 14H:     2 bytes, tag = byte 1 bits 5..6 */
  PIC_FAT_TARGET_18      = 2, /* PIC18:   3 bytes, tag = byte 2 bits 0..6 */
  PIC_FAT_TARGET_FLAT    = 3, /* 32-bit:  no tag bits                      */
  PIC_FAT_TARGET_14H3    = 4  /* 14H, 3-byte: tag = byte 2 bits 0..6 (127
                               * ids); the F4 size toggle's representation.
                               * Byte 1 is the offset's high half, so the tag
                               * byte carries no offset bits (like PIC18). */
} pic_fat_target_t;

#define PIC_FAT_ROM_BIT 0x80U

/* the most ids any target's tag can encode (PIC18: 7 bits) */
#define PIC_FAT_MAX_OBJECTS 128

/* The 14H tag is bits 5..6, NOT 4..6 (F0b, 2026-11-08).  Every PIC_14H
 * device in the library fills its bank windows from 0x0020 up to 0x196F,
 * so a LINEAR file address has bit 12 set from 0x1000 on (byte 1 = 0x10).
 * A tag at bit 4 turns every linear pointer into "fat, id 1" on the upper
 * 30 banks.  Bits 5..6 are clear for every address below 0x2000, which is
 * every 14H data space in the library; the cost is 3 ids instead of 7,
 * and the F4 size toggle moves a program with more than 3 fat objects to
 * the 3-byte form.  A 14H part with data at or above 0x2000 cannot use
 * the 2-byte form at all. */
#define PIC_FAT_TAG_SHIFT(t) \
  (((PIC_FAT_TARGET_18) == (t)) || ((PIC_FAT_TARGET_14H3) == (t)) ? 0U \
   : ((PIC_FAT_TARGET_14H) == (t)) ? 5U \
   : ((PIC_FAT_TARGET_CLASSIC) == (t)) ? 1U : 0U)

#define PIC_FAT_TAG_BITS(t) \
  (((PIC_FAT_TARGET_18) == (t)) || ((PIC_FAT_TARGET_14H3) == (t)) ? 7U \
   : ((PIC_FAT_TARGET_14H) == (t)) ? 2U \
   : ((PIC_FAT_TARGET_CLASSIC) == (t)) ? 6U : 0U)

#define PIC_FAT_TAG_MAX(t) \
  ((0U == PIC_FAT_TAG_BITS(t)) ? 0U \
   : ((1U << PIC_FAT_TAG_BITS(t)) - 1U))

/* the byte carrying the tag/ROM bits: byte 2 on PIC18 (3-byte pointer),
 * byte 1 on the 2-byte 14-bit forms */
#define PIC_FAT_TOP_BYTE(t) \
  ((((PIC_FAT_TARGET_18) == (t)) || ((PIC_FAT_TARGET_14H3) == (t))) ? 2U : 1U)

#define PIC_FAT_TAG_GET(t, bytes) \
  ((unsigned) ((((unsigned) (bytes)[PIC_FAT_TOP_BYTE(t)]) \
    >> PIC_FAT_TAG_SHIFT(t)) & PIC_FAT_TAG_MAX(t)))

#define PIC_FAT_TAG_SET(t, bytes, tag) \
  do { \
    unsigned char *pic_fat_b_ = (unsigned char *) (bytes); \
    unsigned char  pic_fat_m_ = (unsigned char) \
      (PIC_FAT_TAG_MAX(t) << PIC_FAT_TAG_SHIFT(t)); \
    pic_fat_b_[PIC_FAT_TOP_BYTE(t)] = (unsigned char) \
      ((pic_fat_b_[PIC_FAT_TOP_BYTE(t)] & (unsigned char) ~pic_fat_m_) \
       | (unsigned char) (((tag) & PIC_FAT_TAG_MAX(t)) \
         << PIC_FAT_TAG_SHIFT(t))); \
  } while (0)

#define PIC_FAT_IS_ROM(t, bytes) \
  (((bytes)[PIC_FAT_TOP_BYTE(t)] & PIC_FAT_ROM_BIT) ? 1 : 0)

/* offset bits carried in the TOP byte; the low bytes carry 8 bits each. */
#define PIC_FAT_OFF_MASK_CLASSIC 0x01U  /* address bit 8 */
#define PIC_FAT_OFF_MASK_14H     0x1FU  /* address bits 8..12 */

/* F1: value operations (pic_fat.c).  Kept free of compiler state so the
 * encoding, the offset arithmetic and the comparison rule are unit-tested
 * directly (unit-tests/test_fat_tags.c, pin fat_pointer_tags). */
unsigned      pic_fat_ptr_size(pic_fat_target_t t);
unsigned      pic_fat_off_mask(pic_fat_target_t t);
unsigned long pic_fat_offset_get(pic_fat_target_t t, const unsigned char *b);
void          pic_fat_value_decode(pic_fat_target_t t, const unsigned char *b,
                  unsigned *tag, unsigned long *offset, int *rom);
void          pic_fat_value_set(pic_fat_target_t t, unsigned char *b,
                  unsigned tag, unsigned long offset, int rom);
void          pic_fat_offset_add(pic_fat_target_t t, unsigned char *b, long k);
int           pic_fat_cmp(pic_fat_target_t t, const unsigned char *a,
                  const unsigned char *c);
int           pic_fat_same_object(pic_fat_target_t t, const unsigned char *a,
                  const unsigned char *c);

/* F2: the segment descriptor, in the [count][base lo,hi,span]... format the
 * runtime chunk walks already read, so `_seg_addr(desc, ofs)` is the same
 * loop with the map address taken from the pointer instead of a formal. */
unsigned      pic_fat_desc_size(unsigned seg_ct);
void          pic_fat_desc_build(unsigned seg_ct, const unsigned long *base,
                  const unsigned *len, unsigned char *desc);
unsigned long pic_fat_seg_addr(const unsigned char *desc, unsigned long ofs);

#endif /* pic_fat_h__ */
