//#=##=##=#==#=#==#===#+==#+==========+==+=+=+=+=+=++=+++=+++++=-++++=-+++++++++++
//
// Part of the LLFS Project, under Apache License v2.0.
// See https://www.apache.org/licenses/LICENSE-2.0 for license information.
// SPDX short identifier: Apache-2.0
//
//+++++++++++-+-+--+----- --- -- -  -  -   -

#pragma once
#ifndef LLFS_BLOOM_FILTER_HPP
#define LLFS_BLOOM_FILTER_HPP

#include <llfs/data_layout.hpp>
#include <llfs/seq.hpp>

#include <batteries/async/slice_work.hpp>
#include <batteries/async/work_context.hpp>
#include <batteries/async/worker_pool.hpp>
#include <batteries/math.hpp>

#include <batteries/math.hpp>
#include <batteries/seq/loop_control.hpp>
#include <batteries/static_assert.hpp>
#include <batteries/suppress.hpp>

#include <xxhash.h>

#include <cmath>
#include <type_traits>

namespace llfs {

/** \brief Parameters used to build a Bloom filter.
 *
 * If instead of bits-per-item, one wants to set a given false positive (error) rate, then the
 * function optimal_bloom_filter_bit_rate may be used to calculate the required bit rate.
 */
struct BloomFilterParams {
  usize bits_per_item;
};

//=#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------
// Implementation details - NOT FOR EXTERNAL USE
//
namespace detail {

/** \brief A set of randomly chosen (by hardware entropy generator) seeds for up to 64 different
 * hash functions to use for building and querying Bloom filters.
 */
inline constexpr std::array<u64, 64> kBloomFilterHashSeeds = {
    0xce3a9eb8b885d5afull, 0x33d9975b8a739ac6ull, 0xe65d0fff49425f03ull, 0x10bb3a132ec4fabcull,
    0x88d476f6e7f2c53cull, 0xcb4905c588217f44ull, 0x54eb7b8b55ac05d6ull, 0xac0de731d7f3f97cull,
    0x998963e5d908c156ull, 0x0bdf939d3b7c1cd6ull, 0x2cf7007c36b2c966ull, 0xb53c35171f25ccceull,
    0x7d6d2ad5e3ef7ae3ull, 0xe3aaa3bf1dbffd08ull, 0xa81f70b4f8dc0f80ull, 0x1f4887ce81cdf25aull,
    0x6433a69ba9e9d9b1ull, 0xf859167265201651ull, 0xe48c6589be0ff660ull, 0xadd5250ba0e7ac09ull,
    0x833f55b86dee015full, 0xae3b000feb85dceaull, 0x0110cfeb4fe23291ull, 0xf3a5d699ab2ce23cull,
    0x7c3a2b8a1c43942cull, 0x8cb3fb6783724d25ull, 0xe3619c66bf3aa139ull, 0x3fdf358be099c7d9ull,
    0x0c38ccabc94a487full, 0x43e19e80ee4fe6edull, 0x22699c9fc26f20eeull, 0xa559cbafff2cea37ull,
    0xfbed4777b17fb16dull, 0x7197788291858011ull, 0xa9325a240f0d996eull, 0x6782b2e3766f2f76ull,
    0xbc3aca45c9d9dc36ull, 0x7b687762afe92061ull, 0x7b2a7cb985790bcfull, 0xf244ed1bc2b06f7dull,
    0x29acd54ff9cb3809ull, 0xe1926523e6f67949ull, 0x98f964fbc223bb91ull, 0xaab5ee47827c5506ull,
    0x0dab726106a4c8ddull, 0xa88bb10b8e57cdd9ull, 0xbef7ede281a687afull, 0x0e2a6b9bc5b7d6e3ull,
    0x5b6f250b605200c8ull, 0xafe46bbd0e81722full, 0xb5d978e72ac594daull, 0x8c4362498b85fff9ull,
    0xce8cd0d29a933471ull, 0x9c2a28aabd1e71cbull, 0x572c8c1d4ea24d86ull, 0x8fc7dff3afb5fbf7ull,
    0xf378bc6c41606bf9ull, 0xa4c36401cf7a557full, 0x0b0a5bdd27f682afull, 0x3fbe0f66ef4777c1ull,
    0x0ed678ccbd246356ull, 0xc2d3489afc4edcd6ull, 0xc482a884240966c6ull, 0x19b952db37267518ull,
};

// Validate assumption that the number of seeds above is a power of 2.
//
BATT_STATIC_ASSERT_EQ(u64{1} << (batt::log2_ceil(kBloomFilterHashSeeds.size())),
                      kBloomFilterHashSeeds.size());

/** \brief Returns the n-th hash function for the given integer value.
 */
inline u64 get_nth_hash_for_bloom(usize int_value, usize n)
{
  return XXH64(&int_value, sizeof(int_value),
               kBloomFilterHashSeeds[n & (kBloomFilterHashSeeds.size() - 1)]);
}

/** \brief Returns the n-th hash function for the given string value.
 */
inline u64 get_nth_hash_for_bloom(const std::string_view& str, usize n)
{
  return XXH64(str.data(), str.size(),
               kBloomFilterHashSeeds[n & (kBloomFilterHashSeeds.size() - 1)]);
}

/** \brief Returns the n-th hash function for the given value.
 *
 * This is the generic overload of this function; it uses std::hash<T> to calculate a hash value,
 * then hashes that value again using xxhash to obtain the n-th hash function (for Bloom filters).
 */
template <typename T,
          typename = std::enable_if_t<!std::is_convertible_v<const T&, usize> &&
                                      !std::is_convertible_v<const T&, std::string_view>>>
inline u64 get_nth_hash_for_bloom(const T& item, usize n)
{
  return get_nth_hash_for_bloom(std::hash<T>{}(item), n);
}

}  //namespace detail
//
//=#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------

/** \brief Invokes `fn` `count` times, each time with a unique hash function applied to `item`.
 *
 * `fn` may return either `void` or `seq::LoopControl`.  If it returns `seq::LoopControl`, then the
 * returned value is used to decide wither to continue calculating hash functions and calling `fn`,
 * or break out of the loop and return early.
 *
 * \return seq::LoopControl::kBreak if `fn` requested that this function return early; otherwise
 * seq::LoopControl::kContinue, indicating that `fn` was called `count` times.
 */
template <typename T, typename Fn = seq::LoopControl(u64)>
inline seq::LoopControl hash_for_bloom(const T& item, u64 count, Fn&& fn)
{
  for (u64 i = 0; i < count; ++i) {
    if (seq::run_loop_fn(fn, detail::get_nth_hash_for_bloom(item, i)) == seq::LoopControl::kBreak) {
      return seq::LoopControl::kBreak;
    }
  }
  return seq::LoopControl::kContinue;
}

// Calculate the required bit rate for a given target false positive probability.
//
inline double optimal_bloom_filter_bit_rate(double target_false_positive_P)
{
  static const double log_phi_e = 2.0780869212350273;
  return -std::log(target_false_positive_P) * log_phi_e;
}

struct PackedBloomFilter {
  // The size of the filter in 64-bit words, minus 1.  The word_count (== word_count_mask + 1) MUST
  // be a power of 2.
  //
  little_u64 word_count_mask;

  // The number of hash functions used.
  //
  little_u16 hash_count;

  // Align to 64-bit boundary.
  //
  little_u8 reserved_[6];

  // The actual filter array starts here (it will probably be larger than one element...)
  //
  little_u64 words[1];

  //==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -

  // Approximate value of ln(2) * 65536 (fixed point, 16 bits decimal).
  //
  static constexpr u64 kLn2Fixed16 = 45426;

  static u64 word_count_from_bit_count(u64 filter_bit_count)
  {
    return u64{1} << batt::log2_ceil((filter_bit_count + 63) / 64);
  }

  static u64 optimal_hash_count(u64 filter_size_in_bits, u64 item_count)
  {
    static const double ln2 = std::log(2.0);

    const double bit_rate = double(filter_size_in_bits) / double(item_count);

    return std::max<u64>(1, usize(bit_rate * ln2 - 0.5));
  }

  static PackedBloomFilter from_params(const BloomFilterParams& params, usize item_count)
  {
    PackedBloomFilter filter;
    filter.initialize(params, item_count);
    return filter;
  }

  //==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -

  void initialize(const BloomFilterParams& params, usize item_count)
  {
    const usize num_words = word_count_from_bit_count(params.bits_per_item * item_count);
    const usize filter_bit_count = num_words * 64;

    this->word_count_mask = num_words - 1;
    this->hash_count = optimal_hash_count(filter_bit_count, item_count);
  }

  u64 index_from_hash(u64 hash_val) const
  {
    return (hash_val >> 6) & this->word_count_mask.value();
  }

  static constexpr u64 bit_mask_from_hash(u64 hash_val)
  {
    return u64{1} << (hash_val & 63);
  }

  template <typename T>
  bool might_contain(const T& item) const
  {
    return hash_for_bloom(item, this->hash_count, [this](u64 h) {
             if ((this->words[this->index_from_hash(h)].value() & this->bit_mask_from_hash(h)) ==
                 0) {
               return seq::LoopControl::kBreak;
             }
             return seq::LoopControl::kContinue;
           }) == seq::LoopControl::kContinue;
  }

  template <typename T>
  void insert(const T& item)
  {
    hash_for_bloom(item, this->hash_count, [this](u64 h) {
      this->words[this->index_from_hash(h)] |= this->bit_mask_from_hash(h);
    });
  }

  void clear()
  {
    std::memset(this->words, 0, sizeof(little_u64) * (this->word_count_mask + 1));
  }

  usize word_count() const
  {
    return this->word_count_mask + 1;
  }

  batt::Slice<const little_u64> get_words() const
  {
    return batt::as_slice(this->words, this->word_count());
  }
};

BATT_STATIC_ASSERT_EQ(sizeof(PackedBloomFilter), 24);

inline usize packed_sizeof(const PackedBloomFilter& filter)
{
  return sizeof(PackedBloomFilter) + sizeof(little_u64) * filter.word_count_mask;
}

inline usize packed_sizeof_bloom_filter(const BloomFilterParams& params, usize item_count)
{
  return packed_sizeof(PackedBloomFilter::from_params(params, item_count));
}

template <typename Iter, typename HashFn>
void parallel_build_bloom_filter(batt::WorkerPool& worker_pool, Iter first, Iter last,
                                 const HashFn& hash_fn, PackedBloomFilter* filter)
{
  const batt::WorkSliceParams stage1_params{
      .min_task_size =
          batt::TaskSize{u64(1024 /*?*/ + filter->hash_count - 1) / filter->hash_count},
      .max_tasks = batt::TaskCount{worker_pool.size() + 1},
  };

  const batt::WorkSlicePlan stage1_plan{stage1_params, first, last};

  const batt::InputSize item_count = stage1_plan.input_size;

  BATT_SUPPRESS_IF_GCC("-Wtype-limits")  // Tell GCC we don't care if this is always true...
  BATT_CHECK_GE(item_count, 0);
  BATT_UNSUPPRESS_IF_GCC()

  const batt::TaskCount n_input_shards = stage1_plan.n_tasks;

  const usize filter_size = packed_sizeof(*filter);
  std::unique_ptr<u8[]> temp_memory{new u8[filter_size * n_input_shards]};
  batt::SmallVec<PackedBloomFilter*, 48> temp_filters;
  {
    u8* ptr = temp_memory.get();
    for (usize i = 0; i < n_input_shards; ++i, ptr += filter_size) {
      auto* partial = reinterpret_cast<PackedBloomFilter*>(ptr);
      partial->word_count_mask = filter->word_count_mask;
      partial->hash_count = filter->hash_count;
      temp_filters.emplace_back(partial);
    }
  }

  // Generate the filters for all sliced shards of the input, in parallel.
  {
    batt::ScopedWorkContext work_context{worker_pool};

    BATT_CHECK_OK(slice_work(work_context, stage1_plan,
                             /*gen_work_fn=*/
                             [&](usize task_index, isize task_offset, isize task_size) {
                               auto src_begin = std::next(first, task_offset);
                               return [src = boost::make_iterator_range(
                                           src_begin, std::next(src_begin, task_size)),
                                       dst = temp_filters[task_index], hash_fn] {
                                 dst->clear();
                                 for (const auto& item : src) {
                                   dst->insert(hash_fn(item));
                                 }
                               };
                             }))
        << "work_context must not be closed!";
  }

  // Merge the temporary filters by sliced output shard, in parallel.
  {
    const batt::WorkSliceParams stage2_params{
        .min_task_size = batt::TaskSize{(1024 /*?*/ + n_input_shards - 1) / n_input_shards},
        .max_tasks = batt::TaskCount{worker_pool.size() + 1},
    };

    const batt::WorkSlicePlan stage2_plan{stage2_params, batt::InputSize{filter->word_count()}};

    batt::ScopedWorkContext work_context{worker_pool};

    BATT_CHECK_OK(slice_work(work_context, stage2_plan,
                             /*gen_work_fn=*/
                             [&](usize /*task_index*/, isize task_offset, isize task_size) {
                               return [task_offset, task_size, filter, &temp_filters] {
                                 for (isize i = task_offset; i < task_offset + task_size; ++i) {
                                   filter->words[i] = 0;
                                   for (PackedBloomFilter* partial : temp_filters) {
                                     filter->words[i] |= partial->words[i];
                                   }
                                 }
                               };
                             }))
        << "work_context must not be closed!";
  }
}

}  // namespace llfs

#endif  // LLFS_BLOOM_FILTER_HPP
