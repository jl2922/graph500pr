/* Copyright (C) 2010 The Trustees of Indiana University.                  */
/*                                                                         */
/* Use, modification and distribution is subject to the Boost Software     */
/* License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at */
/* http://www.boost.org/LICENSE_1_0.txt)                                   */
/*                                                                         */
/*  Authors: Jeremiah Willcock                                             */
/*           Andrew Lumsdaine                                              */

#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS
#endif
#include "splittable_mrg.h"
#include "graph_generator.h"
#include "permutation_gen.h"
#include <stdint.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef __MTA__
#include <sys/mta_task.h>
#endif
#ifdef GRAPH_GENERATOR_MPI
#include <mpi.h>
#endif

static inline void* safe_malloc(size_t n) {
  void* p = malloc(n);
  if (!p) {
    fprintf(stderr, "Out of memory trying to allocate %zu byte(s)\n", n);
    abort();
  }
  return p;
}

static inline void* safe_calloc(size_t n, size_t k) {
  void* p = calloc(n, k);
  if (!p) {
    fprintf(stderr, "Out of memory trying to allocate %zu byte(s)\n", n);
    abort();
  }
  return p;
}

/* Get a number in [0, n) in an unbiased way. */
#ifdef __MTA__
#pragma mta inline
#endif
static inline uint_fast64_t random_up_to(mrg_state* st, uint_fast64_t n) {
  /* PRNG returns values in [0, 0x7FFFFFFF) */
  /* Two iters returns values in [0, 0x3FFFFFFF00000001) */
  assert (n > 0 && n <= UINT64_C(0x3FFFFFFF00000001));
  if (n == 1) {
    return 0;
  } else if (n <= UINT64_C(0x7FFFFFFF)) {
    uint_fast64_t acc_value_limit = (UINT64_C(0x7FFFFFFF) / n) * n; /* Round down to multiple of n */
    while (1) {
      uint_fast64_t acc = mrg_get_uint_orig(st);
      if (acc >= acc_value_limit) continue;
      return acc % n;
    }
  } else if (n <= UINT64_C(0x3FFFFFFF00000001)) {
    uint_fast64_t acc_value_limit = (UINT64_C(0x3FFFFFFF00000001) / n) * n; /* Round down to multiple of n */
    while (1) {
      uint_fast64_t acc = mrg_get_uint_orig(st) * UINT64_C(0x7FFFFFFF);
      acc += mrg_get_uint_orig(st); /* Do this separately to get fixed ordering. */
      if (acc >= acc_value_limit) continue;
      return acc % n;
    }
  } else {
    /* Should have been caught before */
    return 0;
  }
}

typedef struct slot_data {
  count_type index, value;
} slot_data;

/* Compare-and-swap; return 1 if successful or 0 otherwise. */
#ifdef __MTA__
#pragma mta inline
static inline int count_type_cas(volatile count_type* p, count_type oldval, count_type newval) {
  count_type val = readfe(p);
  if (val == oldval) {
    writeef(p, newval);
    return 1;
  } else {
    writeef(p, val);
    return 0;
  }
}
#elif defined(GRAPH_GENERATOR_MPI) || defined(GRAPH_GENERATOR_SEQ)
/* Sequential */
static inline int count_type_cas(count_type* p, count_type oldval, count_type newval) {
  if (*p == oldval) {
    *p = newval;
    return 1;
  } else {
    return 0;
  }
}
#else
#error "Need to define count_type_cas() for your system"
#endif

/* This code defines a simple closed-indexing hash table.  It is used to speed
 * up the rand_sort algorithm given below.  Elements with -1 as index are
 * unused; others are used. */

#ifdef __MTA__
#pragma mta inline
#endif
static inline void hashtable_insert(slot_data* ht, count_type ht_size, count_type index, count_type value, count_type hashval) {
  count_type i;
  for (i = hashval; i < ht_size; ++i) {
    if (count_type_cas(&ht[i].index, (count_type)(-1), index)) {
      ht[i].value = value;
      return;
    }
  }
  for (i = 0; i < hashval; ++i) {
    if (count_type_cas(&ht[i].index, (count_type)(-1), index)) {
      ht[i].value = value;
      return;
    }
  }
  assert (!"Should not happen: overflow in hash table");
}

#ifdef __MTA__
#pragma mta inline
#endif
static inline int hashtable_count_key(const slot_data* ht, count_type ht_size, count_type index, count_type hashval) {
  int c = 0;
  count_type i;
  for (i = hashval; i < ht_size && ht[i].index != (count_type)(-1); ++i) {
    if (ht[i].index == index) ++c;
  }
  if (i == ht_size) {
    for (i = 0; i < hashval && ht[i].index != (count_type)(-1); ++i) {
      if (ht[i].index == index) ++c;
    }
  }
  return c;
}

/* Return all values with the given index value into result array; return value
 * of function is element count. */
#ifdef __MTA__
#pragma mta inline
#endif
static inline int hashtable_get_values(const slot_data* ht, count_type ht_size, count_type index, count_type hashval, count_type* result) {
  int x = 0;
  count_type i;
  for (i = hashval; i < ht_size && ht[i].index != (count_type)(-1); ++i) {
    if (ht[i].index == index) {
      result[x++] = ht[i].value;
    }
  }
  if (i == ht_size) {
    for (i = 0; i < hashval && ht[i].index != (count_type)(-1); ++i) {
      if (ht[i].index == index) {
        result[x++] = ht[i].value;
      }
    }
  }
  return x;
}

#ifdef __MTA__
#pragma mta inline
#endif
static inline void selection_sort(count_type* a, count_type n) {
  count_type i, j;
  if (n <= 1) return;
  for (i = 0; i + 1 < n; ++i) {
    count_type minpos = i;
    for (j = i + 1; j < n; ++j) {
      if (a[j] < a[minpos]) minpos = j;
    }
    if (minpos != i) {
      count_type t = a[minpos];
      a[minpos] = a[i];
      a[i] = t;
    }
  }
}

/* Fisher-Yates shuffle */
#ifdef __MTA__
#pragma mta inline
#endif
static inline void randomly_permute(count_type* a, count_type n, mrg_state* st) {
  count_type i, j;
  if (n <= 1) return;
  for (i = n - 1; i > 0; --i) {
    j = random_up_to(st, i + 1);
    if (i != j) {
      count_type t = a[i];
      a[i] = a[j];
      a[j] = t;
    }
  }
}

/* Exclusive prefix sum on ints; returns sum of overall input array */
static inline int int_prefix_sum(int* out, const int* in, size_t n) {
  size_t i;
  if (n == 0) return 0;
  out[0] = 0;
  for (i = 1; i < n; ++i) out[i] = out[i - 1] + in[i - 1];
  return out[n - 1] + in[n - 1];
}

/* A variant of the rand_sort algorithm from Cong and Bader ("An Empirical
 * Analysis of Parallel Random Permutation Algorithms on SMPs", Georgia Tech TR
 * GT-CSE-06-06.pdf,
 * <URL:http://smartech.gatech.edu/bitstream/1853/14385/1/GT-CSE-06-06.pdf>).
 * Sorting here is done using a hash table to effectively act as a bucket sort.
 * The rand_sort algorithm was chosen instead of the other algorithms in order
 * to get reproducibility across architectures and processor counts.  That is
 * also the reason for the extra sort immediately before scrambling all
 * elements with the same key, as well as the expensive PRNG operations. */

/* This version is for sequential machines and the XMT. */
void rand_sort_shared(mrg_state* st, count_type n, count_type* result /* Array of size n */) {
  count_type hash_table_size = 2 * n + 128; /* Must be >n, preferably larger for performance */
  slot_data* ht = (slot_data*)safe_malloc(hash_table_size * sizeof(slot_data));
  count_type i;
#ifdef __MTA__
#pragma mta block schedule
#endif
  for (i = 0; i < hash_table_size; ++i) ht[i].index = (count_type)(-1); /* Unused */
#ifdef __MTA__
#pragma mta assert parallel
#pragma mta block schedule
#endif
  /* Put elements into the hash table with random keys. */
  for (i = 0; i < n; ++i) {
    mrg_state new_st = *st;
    mrg_skip(&new_st, 1, i, 0);
    count_type index = (count_type)random_up_to(&new_st, hash_table_size);
    hashtable_insert(ht, hash_table_size, index, i, index);
  }
  /* Count elements with each key in order to sort them by key. */
  count_type* bucket_counts = (count_type*)safe_calloc(hash_table_size, sizeof(count_type)); /* Uses zero-initialization */
#ifdef __MTA__
#pragma mta assert parallel
#pragma mta block schedule
#endif
  for (i = 0; i < hash_table_size; ++i) {
    /* Count all elements with same index. */
    bucket_counts[i] = hashtable_count_key(ht, hash_table_size, i, i);
  }
  /* bucket_counts replaced by its prefix sum (start of each bucket in output array) */
  count_type* bucket_starts_in_result = bucket_counts;
  count_type running_sum = 0;
#ifdef __MTA__
#pragma mta block schedule
#endif
  for (i = 0; i < hash_table_size; ++i) {
    count_type old_running_sum = running_sum;
    running_sum += bucket_counts[i];
    bucket_counts[i] = old_running_sum;
  }
  assert (running_sum == n);
  bucket_counts = NULL;
#ifdef __MTA__
#pragma mta assert parallel
#pragma mta block schedule
#endif
  for (i = 0; i < hash_table_size; ++i) {
    count_type result_start_idx = bucket_starts_in_result[i];
    count_type* temp = result + result_start_idx;
    /* Gather up all elements with same key. */
    count_type bi = (count_type)hashtable_get_values(ht, hash_table_size, i, i, temp);
    if (bi > 1) {
      /* Selection sort them (for consistency in parallel implementations). */
      selection_sort(temp, bi);
      /* Randomly permute them. */
      mrg_state new_st = *st;
      mrg_skip(&new_st, 1, i, 100);
      randomly_permute(temp, bi, &new_st);
    }
  }
  free(ht); ht = NULL;
  free(bucket_starts_in_result); bucket_starts_in_result = NULL;
}

#ifdef GRAPH_GENERATOR_MPI
void rand_sort_mpi(MPI_Comm comm, mrg_state* st, count_type n,
                   count_type* result_size_ptr,
                   count_type** result_ptr /* Allocated using safe_malloc() by
                   rand_sort_mpi */) {
  int size, rank;
  MPI_Comm_size(comm, &size);
  MPI_Comm_rank(comm, &rank);

  /* Make MPI data type for slot_data. */
  MPI_Datatype slot_data_type;
  {
    int blocklens[] = {1, 1};
    MPI_Aint temp_base, indices[2];
    slot_data temp;
    MPI_Get_address(&temp, &temp_base);
    MPI_Get_address(&temp.index, &indices[0]);
    MPI_Get_address(&temp.value, &indices[1]);
    indices[0] -= temp_base;
    indices[1] -= temp_base;
    MPI_Datatype old_types[] = {COUNT_MPI_TYPE, COUNT_MPI_TYPE};
    MPI_Type_struct(2, blocklens, indices, old_types, &slot_data_type);
    MPI_Type_commit(&slot_data_type);
  }

  count_type total_hash_table_size = 2 * n + 128; /* Must be >n, preferably larger for performance */

  /* Hash table is distributed by blocks: first (total_hash_table_size % size)
   * are of size (total_hash_table_size / size + 1), rest are of size
   * (total_hash_table_size / size).  This distribution is necessary so that
   * the permutation can easily be assembled at the end of the function. */
  count_type ht_base_block_size = total_hash_table_size / size;
  int ht_block_size_cutoff_rank = total_hash_table_size % size;
  count_type ht_block_size_cutoff_index = ht_block_size_cutoff_rank * (ht_base_block_size + 1);
  count_type ht_my_size = ht_base_block_size + (rank < ht_block_size_cutoff_rank);
  count_type ht_my_start = (rank < ht_block_size_cutoff_rank) ?
                           rank * (ht_base_block_size + 1) :
                           ht_block_size_cutoff_index + (rank - ht_block_size_cutoff_rank) * ht_base_block_size;
  count_type ht_my_end = ht_my_start + ht_my_size;
#define HT_OWNER(e) \
    (((e) < ht_block_size_cutoff_index) ? \
     (e) / (ht_base_block_size + 1) : \
     ht_block_size_cutoff_rank + ((e) - ht_block_size_cutoff_index) / ht_base_block_size)
#define HT_LOCAL(e) ((e) - ht_my_start)

  /* Input elements to scramble are distributed cyclically for simplicity;
   * their distribution does not matter. */
  count_type elt_my_size = (n / size) + (rank < n % size);

  count_type i;

  /* Cache the key-value pairs to avoid PRNG skip operations.  Count the number
   * of pairs going to each destination processor. */
  slot_data* kv_pairs = (slot_data*)safe_malloc(elt_my_size * sizeof(slot_data));
  int* outcounts = (int*)safe_calloc(size, sizeof(int)); /* Relies on zero-init */
  for (i = 0; i < elt_my_size; ++i) {
    mrg_state new_st = *st;
    mrg_skip(&new_st, 1, i * size + rank, 0);
    count_type index = (count_type)random_up_to(&new_st, total_hash_table_size);
    count_type owner = HT_OWNER(index);
    assert (owner < size);
    ++outcounts[owner];
    kv_pairs[i].index = index;
    kv_pairs[i].value = i * size + rank;
  }

  int* outdispls = (int*)safe_malloc(size * sizeof(int));
  int total_outcount = int_prefix_sum(outdispls, outcounts, size);

  slot_data* outdata = (slot_data*)safe_malloc(total_outcount * sizeof(slot_data));
  int* outoffsets = (int*)safe_malloc(size * sizeof(int));
  memcpy(outoffsets, outdispls, size * sizeof(int));

  /* Put the key-value pairs into the output buffer, sorted by destination, to
   * get ready for MPI_Alltoallv. */
  for (i = 0; i < elt_my_size; ++i) {
    count_type index = kv_pairs[i].index;
    count_type owner = HT_OWNER(index);
    outdata[outoffsets[owner]] = kv_pairs[i];
    ++outoffsets[owner];
  }
  free(kv_pairs); kv_pairs = NULL;
  free(outoffsets); outoffsets = NULL;

  int* incounts = (int*)safe_malloc(size * sizeof(int));

  /* Send data counts. */
  MPI_Alltoall(outcounts, 1, MPI_INT, incounts, 1, MPI_INT, comm);

  int* indispls = (int*)safe_malloc(size * sizeof(int));
  int total_incount = int_prefix_sum(indispls, incounts, size);

  slot_data* indata = (slot_data*)safe_malloc(total_incount * sizeof(slot_data));

  /* Send data to put into hash table. */
  MPI_Alltoallv(outdata, outcounts, outdispls, slot_data_type,
                indata, incounts, indispls, slot_data_type,
                comm);

  free(outdata); outdata = NULL;
  free(outcounts); outcounts = NULL;
  free(outdispls); outdispls = NULL;
  free(incounts); incounts = NULL;
  free(indispls); indispls = NULL;
  MPI_Type_free(&slot_data_type);

  /* Create the local part of the hash table. */
  slot_data* ht = (slot_data*)safe_malloc(ht_my_size * sizeof(slot_data));
  for (i = ht_my_start; i < ht_my_end; ++i) {
    ht[HT_LOCAL(i)].index = (count_type)(-1); /* Unused */
  }
  for (i = 0; i < total_incount; ++i) {
    count_type index = indata[i].index, value = indata[i].value;
    assert (HT_OWNER(index) == rank);
    hashtable_insert(ht, ht_my_size, index, value, HT_LOCAL(index));
  }

  free(indata); indata = NULL;

  /* Make the local part of the result.  Most of the rest of this code is
   * similar to the shared-memory/XMT version above. */
  count_type* result = (count_type*)safe_malloc(total_incount * sizeof(count_type));
  *result_ptr = result;
  *result_size_ptr = total_incount;

  count_type* bucket_counts = (count_type*)safe_calloc(ht_my_size, sizeof(count_type)); /* Uses zero-initialization */
  for (i = ht_my_start; i < ht_my_end; ++i) {
    /* Count all elements with same index. */
    bucket_counts[HT_LOCAL(i)] = hashtable_count_key(ht, ht_my_size, i, HT_LOCAL(i));
  }
  /* bucket_counts replaced by its prefix sum (start of each bucket in output array) */
  count_type* bucket_starts_in_result = bucket_counts;
  count_type running_sum = 0;
  for (i = 0; i < ht_my_size; ++i) {
    count_type old_running_sum = running_sum;
    running_sum += bucket_counts[i];
    bucket_counts[i] = old_running_sum;
  }
  assert (running_sum == total_incount);
  bucket_counts = NULL;
  for (i = ht_my_start; i < ht_my_end; ++i) {
    count_type result_start_idx = bucket_starts_in_result[HT_LOCAL(i)];
    count_type* temp = result + result_start_idx;
    /* Gather up all elements with same key. */
    count_type bi = (count_type)hashtable_get_values(ht, ht_my_size, i, HT_LOCAL(i), temp);
    if (bi > 1) {
      /* Selection sort them (for consistency in parallel implementations). */
      selection_sort(temp, bi);
      /* Randomly permute them. */
      mrg_state new_st = *st;
      mrg_skip(&new_st, 1, i, 100);
      randomly_permute(temp, bi, &new_st);
    }
  }
  free(ht); ht = NULL;
  free(bucket_starts_in_result); bucket_starts_in_result = NULL;
}
#undef HT_OWNER
#undef HT_LOCAL
#endif /* GRAPH_GENERATOR_MPI */

/* Code below this is used for testing the permutation generators. */

#if 0
int main(int argc, char** argv) {
  MPI_Init(&argc, &argv);
  const count_type n = 200000;
  count_type* result = NULL;
  count_type result_size;
  mrg_state st;
  mrg_seed(&st, 1, 2, 3, 4, 5);
  MPI_Barrier(MPI_COMM_WORLD);
  double start = MPI_Wtime();
  rand_sort_mpi(MPI_COMM_WORLD, &st, n, &result_size, &result);
  MPI_Barrier(MPI_COMM_WORLD);
  double time = MPI_Wtime() - start;
#if 0
  count_type i;
  printf("My count = %" PRIcount_type "\n", result_size);
  for (i = 0; i < result_size; ++i) printf("%" PRIcount_type "\n", result[i]);
#endif
  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  if (rank == 0) {
    printf("Shuffle of %" PRIcount_type " element(s) took %f second(s).\n", n, time);
  }
  free(result); result = NULL;
  MPI_Finalize();
  return 0;
}
#endif

#if 0
int main(int argc, char** argv) {
  const count_type n = 5000000;
  count_type* result = (count_type*)safe_malloc(n * sizeof(count_type));
  mrg_state st;
  mrg_seed(&st, 1, 2, 3, 4, 5);
  unsigned long time;
#pragma mta fence
  time = mta_get_clock(0);
  rand_sort_shared(&st, n, result);
#pragma mta fence
  time = mta_get_clock(time);
#if 0
  count_type i;
  for (i = 0; i < n; ++i) printf("%" PRIcount_type "\n", result[i]);
#endif
  printf("Shuffle of %" PRIcount_type " element(s) took %f second(s).\n", n, time * mta_clock_period());
  free(result); result = NULL;
  return 0;
}
#endif