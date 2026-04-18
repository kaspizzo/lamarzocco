#pragma once

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT_TRUE(expr) \
  do { \
    if (!(expr)) { \
      fprintf(stderr, "Assertion failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
      return 1; \
    } \
  } while (0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

#define ASSERT_EQ_INT(expected, actual) \
  do { \
    int expected_value__ = (expected); \
    int actual_value__ = (actual); \
    if (expected_value__ != actual_value__) { \
      fprintf(stderr, "Assertion failed at %s:%d: expected %d, got %d\n", __FILE__, __LINE__, expected_value__, actual_value__); \
      return 1; \
    } \
  } while (0)

#define ASSERT_EQ_U32(expected, actual) \
  do { \
    unsigned expected_value__ = (unsigned)(expected); \
    unsigned actual_value__ = (unsigned)(actual); \
    if (expected_value__ != actual_value__) { \
      fprintf(stderr, "Assertion failed at %s:%d: expected %u, got %u\n", __FILE__, __LINE__, expected_value__, actual_value__); \
      return 1; \
    } \
  } while (0)

#define ASSERT_EQ_I64(expected, actual) \
  do { \
    long long expected_value__ = (long long)(expected); \
    long long actual_value__ = (long long)(actual); \
    if (expected_value__ != actual_value__) { \
      fprintf(stderr, "Assertion failed at %s:%d: expected %lld, got %lld\n", __FILE__, __LINE__, expected_value__, actual_value__); \
      return 1; \
    } \
  } while (0)

#define ASSERT_STREQ(expected, actual) \
  do { \
    const char *expected_value__ = (expected); \
    const char *actual_value__ = (actual); \
    if (strcmp(expected_value__, actual_value__) != 0) { \
      fprintf(stderr, "Assertion failed at %s:%d: expected \"%s\", got \"%s\"\n", __FILE__, __LINE__, expected_value__, actual_value__); \
      return 1; \
    } \
  } while (0)

#define ASSERT_FLOAT_EQ(expected, actual, epsilon) \
  do { \
    double expected_value__ = (double)(expected); \
    double actual_value__ = (double)(actual); \
    double delta__ = fabs(expected_value__ - actual_value__); \
    if (delta__ > (epsilon)) { \
      fprintf(stderr, "Assertion failed at %s:%d: expected %.6f, got %.6f (delta %.6f)\n", __FILE__, __LINE__, expected_value__, actual_value__, delta__); \
      return 1; \
    } \
  } while (0)

#define ASSERT_CONTAINS(haystack, needle) \
  do { \
    const char *haystack__ = (haystack); \
    const char *needle__ = (needle); \
    if (strstr(haystack__, needle__) == NULL) { \
      fprintf(stderr, "Assertion failed at %s:%d: missing substring \"%s\"\n", __FILE__, __LINE__, needle__); \
      return 1; \
    } \
  } while (0)

#define ASSERT_NOT_CONTAINS(haystack, needle) \
  do { \
    const char *haystack__ = (haystack); \
    const char *needle__ = (needle); \
    if (strstr(haystack__, needle__) != NULL) { \
      fprintf(stderr, "Assertion failed at %s:%d: unexpected substring \"%s\"\n", __FILE__, __LINE__, needle__); \
      return 1; \
    } \
  } while (0)

#define RUN_TEST(fn) \
  do { \
    fprintf(stderr, "[RUN ] %s\n", #fn); \
    if ((fn)() != 0) { \
      fprintf(stderr, "[FAIL] %s\n", #fn); \
      return 1; \
    } \
    fprintf(stderr, "[ OK ] %s\n", #fn); \
  } while (0)
