/* ===========================================================
   #File: dynarray.cpp #
   #Date: 17 Jan 2021 #
   #Revision: 1.0 #
   #Creator: Omid Miresmaeili #
   #Description: C-style dynamic array. This is purely experimental. Use std::vector if you fancy. #
   #Notice: (C) Copyright 2021 by Omid. All Rights Reserved. #
   =========================================================== */
#pragma once

#include "./common.h"

// NOTE(omid):
// 1. After the first pushback the capacity of a dynarray is 8. After that the capacity gets doubled when necessary (capacity < length)
// 2. The first two elements of the dynarray are internal information and represent length and capacity, respectively
// 3. The real array (data) begins at third element (arr_ptr[2])
// 4. The C28182 warning is disabled

#pragma warning (disable: 28182)

#define DYN_ARRAY_INIT(T, arr)                                      \
    do {                                                            \
        size_t * arr_ptr = (size_t *)::malloc(2 * sizeof(size_t));  \
        arr_ptr[0] = 0;                                             \
        arr_ptr[1] = 0;                                             \
        arr = (T *)&arr_ptr[2];                                     \
    } while (0)

#define DYN_ARRAY_DEINIT(arr)                   \
    do {                                        \
        SIMPLE_ASSERT(arr, "invalid array");    \
        size_t * arr_ptr = ((size_t *)(arr)-2); \
        ::free(arr_ptr);                        \
        arr = NULL;                             \
    } while (0)

#define DYN_ARRAY_LENGTH(arr)       (*((size_t *)(arr)-2))
#define DYN_ARRAY_CAPACITY(arr)     (*((size_t *)(arr)-1))

#define DYN_ARRAY_EXPAND(T, arr, size)      \
    do {    \
        SIMPLE_ASSERT(arr, "invalid array");    \
        SIMPLE_ASSERT(size > 0, "invalid expansion size");   \
        size_t * arr_ptr = ((size_t *)(arr)-2);     \
        arr_ptr[1] += size;   \
        arr_ptr = (size_t *)::realloc(arr_ptr, 2 * sizeof(size_t) + arr_ptr[1] * sizeof(*arr));    \
        if (arr_ptr != 0) /*averting C28182 warning*/   \
            (arr) = (T *)&arr_ptr[2];    \
    } while (0)

#define DYN_ARRAY_PUSHBACK(T, arr, value)      \
    do {    \
        SIMPLE_ASSERT(arr, "invalid array");    \
        size_t * arr_ptr = ((size_t *)(arr)-2);     \
        ++arr_ptr[0];   \
        if (arr_ptr[1] < arr_ptr[0]) {      \
            arr_ptr[1] = (0 == arr_ptr[1]) ? 8 : (2 * arr_ptr[1]);   \
            arr_ptr = (size_t *)::realloc(arr_ptr, 2 * sizeof(size_t) + arr_ptr[1] * sizeof(*arr));    \
            SIMPLE_ASSERT(arr_ptr,); \
        }   \
        if (arr_ptr != 0) /*averting C28182 warning*/   \
            (arr) = (T *)&arr_ptr[2];    \
        arr[arr_ptr[0]] = (value);      \
    } while (0)

#pragma warning (default: 28182)
