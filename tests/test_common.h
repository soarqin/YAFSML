#ifndef ML_TEST_COMMON_H
#define ML_TEST_COMMON_H
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#define EXPECT_TRUE(cond)  do { if (!(cond)) { fprintf(stderr, "%s:%d EXPECT_TRUE failed: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)
#define EXPECT_EQ(a, b)    do { if ((a) != (b)) { fprintf(stderr, "%s:%d EXPECT_EQ failed: %s != %s\n", __FILE__, __LINE__, #a, #b); return 1; } } while (0)
#define EXPECT_STREQ_W(a, b) do { if (wcscmp((a),(b)) != 0) { fprintf(stderr, "%s:%d EXPECT_STREQ_W failed\n", __FILE__, __LINE__); return 1; } } while (0)
#define EXPECT_STREQ_A(a, b) do { if (strcmp((a),(b)) != 0) { fprintf(stderr, "%s:%d EXPECT_STREQ_A failed\n", __FILE__, __LINE__); return 1; } } while (0)
#define EXPECT_NULL(p)     EXPECT_TRUE((p) == NULL)
#define EXPECT_NOT_NULL(p) EXPECT_TRUE((p) != NULL)
#endif
