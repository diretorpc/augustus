#ifndef CORE_STRING_H
#define CORE_STRING_H

#include <stdint.h>

/**
 * @file
 * String conversion functions.
 */

 /**
  * Checks if the two strings are equal
  * @param a String A
  * @param b String B
  * @return Boolean true if the strings are equal, false if they differ
  */
int string_equals(const uint8_t *a, const uint8_t *b);

/**
 * Checks if the two strings are equal up until a limit of chars
 * @param a String A
 * @param b String B
 * @param limit Limit of chars to check
 * @return Boolean true if the strings are equal until the limit, false if they differ
 */
int string_equals_until(const uint8_t *a, const uint8_t *b, unsigned int limit);

/**
 * Tries to find a value in a string
 * @param text String to search
 * @param value Value to find
 * @return A pointer to the first occurence of the found value if found, 0 otherwise.
 */
const uint8_t *string_find(const uint8_t *text, uint8_t value);

/**
 * Copies a string
 * @param src Source string
 * @param dst Destination string
 * @param maxlength Maximum length of the destination string
 * @return Position of the last copied character (null-terminator in dst)
 */
uint8_t *string_copy(const uint8_t *src, uint8_t *dst, int maxlength);

/**
 * Determines the length of the string
 * @param str String
 * @return Length of the string
 */
int string_length(const uint8_t *str);

/**
 * Convert (cast) C-string to internal string.
 * Only use this for known ASCII-only strings!
 * @param str C string
 * @return Game string, or NULL if non-ascii values are found in str
 */
const uint8_t *string_from_ascii(const char *str);

/**
 * Converts the string to integer
 * @return integer
 */
int string_to_int(const uint8_t *str);

/**
 * Converts integer to string
 * @param dst Output string
 * @param value Value to write
 * @param force_plus_sign Force plus sign in front of positive value
 * @return Total number of characters written to dst
 */
int string_from_int(uint8_t *dst, int value, int force_plus_sign);

 /**
  * Compares two strings similar to how strcmp does.
  * Compares the lowercase of the two strings, so uppercase is treated equal to lowercase.
  * @param a String A
  * @param b String B
  * @return int < 0 if a is before b alphabetically, or > 0 if b is before a.
  */
int string_compare(const uint8_t *a, const uint8_t *b);

#endif // CORE_STRING_H
