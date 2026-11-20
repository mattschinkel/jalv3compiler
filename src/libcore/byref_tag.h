/************************************************************
 **
 ** byref_tag.h : byref hidden tag encoding
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#ifndef byref_tag_h__
#define byref_tag_h__

/* One hidden tag byte per byref formal.
 *   bits 0..2  size/type code
 *   bit  3     string
 *   bit  4     array
 *   bit  5     pointer
 *   bit  6     signed
 *   bit  7     reserved
 *
 * An array carries its ELEMENT code + ARRAY; a pointer carries its
 * POINTEE code + POINTER.  The unsigned scalar predicates ignore SIGNED,
 * so `_is_byte()` stays true for an sbyte; `_is_sbyte()` is the signed
 * test.  A pointer-to-string is marshalled as a string actual, so it
 * carries STRING (no POINTER bit). */
#define BYREF_TAG_CODE_MASK     0x07U
#define BYREF_TAG_BYTE          0x00U
#define BYREF_TAG_WORD          0x01U
#define BYREF_TAG_DWORD         0x02U
#define BYREF_TAG_QWORD         0x03U
#define BYREF_TAG_OWORD         0x04U
#define BYREF_TAG_FLOAT         0x05U

#define BYREF_TAG_STRING        0x08U
#define BYREF_TAG_ARRAY         0x10U
#define BYREF_TAG_POINTER       0x20U
#define BYREF_TAG_SIGNED        0x40U

/* scalar (not string/array/pointer), signed ignored */
#define BYREF_TAG_SCALAR_MASK   (BYREF_TAG_CODE_MASK \
                                 | BYREF_TAG_STRING \
                                 | BYREF_TAG_ARRAY \
                                 | BYREF_TAG_POINTER)
/* signed scalar / signed array / signed pointer */
#define BYREF_TAG_SIGNED_MASK   (BYREF_TAG_SCALAR_MASK | BYREF_TAG_SIGNED)
/* string, or not an array/pointer */
#define BYREF_TAG_STRING_MASK   (BYREF_TAG_STRING \
                                 | BYREF_TAG_ARRAY \
                                 | BYREF_TAG_POINTER)

#endif /* byref_tag_h__ */
