#include "boss_chunk_construct.hpp"

#include <ips4o.hpp>

#include "unix_tools.hpp"
#include "boss_chunk.hpp"
#include "kmer_collector.hpp"

const static bool kUseDeque = false;


template <class Array>
void sort_and_remove_duplicates(Array *array,
                                size_t num_threads,
                                size_t offset) {
    ips4o::parallel::sort(array->begin() + offset, array->end(),
                          utils::LessFirst<typename Array::value_type>(),
                          num_threads);
    // remove duplicates
    auto unique_end = std::unique(array->begin() + offset, array->end(),
                                  utils::EqualFirst<typename Array::value_type>());
    array->erase(unique_end, array->end());
}

template <typename Array>
void shrink_kmers(Array *kmers,
                  size_t num_threads,
                  bool verbose,
                  size_t offset) {
    if (verbose) {
        std::cout << "Allocated capacity exceeded, filter out non-unique k-mers..."
                  << std::flush;
    }

    size_t prev_num_kmers = kmers->size();
    sort_and_remove_duplicates(kmers, num_threads, offset);

    if (verbose) {
        std::cout << " done. Number of kmers reduced from " << prev_num_kmers
                                                  << " to " << kmers->size() << ", "
                  << (kmers->size() * sizeof(typename Array::value_type) >> 20) << "Mb" << std::endl;
    }
}

template <class Container, typename KMER>
inline KMER& push_back(Container &kmers, const KMER &kmer) {
    if constexpr(utils::is_pair<typename Container::value_type>::value) {
        kmers.emplace_back(kmer, 0);
        return kmers.back().first;
    } else {
        kmers.push_back(kmer);
        return kmers.back();
    }
}

// Although this function could be parallelized better,
// the experiments show it's already fast enough.
// k is node length
template <typename Array>
void recover_source_dummy_nodes(size_t k,
                                Array *kmers,
                                size_t num_threads,
                                bool verbose) {
    using KMER = std::remove_reference_t<decltype(utils::get_first((*kmers)[0]))>;

    size_t dummy_begin = kmers->size();
    size_t num_dummy_parent_kmers = 0;

    for (size_t i = 0; i < dummy_begin; ++i) {
        const KMER &kmer = utils::get_first((*kmers)[i]);
        // we never add reads shorter than k
        assert(kmer[1] != 0 || kmer[0] != 0 || kmer[k] == 0);

        auto node_last_char = kmer[1];
        auto edge_label = kmer[0];
        // check if it's not a source dummy kmer
        if (node_last_char || !edge_label)
            continue;

        num_dummy_parent_kmers++;

        if (kmers->size() + 1 > kmers->capacity())
            shrink_kmers(kmers, num_threads, verbose, dummy_begin);

        push_back(*kmers, kmer).to_prev(k + 1, BOSS::kSentinelCode);
    }
    if (verbose) {
        std::cout << "Number of dummy k-mers with dummy prefix of length 1: "
                  << num_dummy_parent_kmers << std::endl;
    }
    sort_and_remove_duplicates(kmers, num_threads, dummy_begin);

    if (verbose) {
        std::cout << "Number of dummy k-mers with dummy prefix of length 2: "
                  << kmers->size() - dummy_begin << std::endl;
    }

    for (size_t c = 3; c < k + 1; ++c) {
        size_t succ_dummy_begin = dummy_begin;
        dummy_begin = kmers->size();

        for (size_t i = succ_dummy_begin; i < dummy_begin; ++i) {
            if (kmers->size() + 1 > kmers->capacity())
                shrink_kmers(kmers, num_threads, verbose, dummy_begin);

            push_back(*kmers, utils::get_first((*kmers)[i])).to_prev(k + 1, BOSS::kSentinelCode);
        }
        sort_and_remove_duplicates(kmers, num_threads, dummy_begin);

        if (verbose) {
            std::cout << "Number of dummy k-mers with dummy prefix of length " << c
                      << ": " << kmers->size() - dummy_begin << std::endl;
        }
    }
    ips4o::parallel::sort(kmers->begin(), kmers->end(),
                          utils::LessFirst<typename Array::value_type>(),
                          num_threads);
}

template <class KmerExtractor>
inline std::vector<typename KmerExtractor::TAlphabet>
encode_filter_suffix_boss(const std::string &filter_suffix) {
    KmerExtractor kmer_extractor;
    std::vector<typename KmerExtractor::TAlphabet> filter_suffix_encoded;
    std::transform(
        filter_suffix.begin(), filter_suffix.end(),
        std::back_inserter(filter_suffix_encoded),
        [&kmer_extractor](char c) {
            return c == BOSS::kSentinel
                            ? BOSS::kSentinelCode
                            : kmer_extractor.encode(c);
        }
    );
    return filter_suffix_encoded;
}

template <typename KmerStorage>
class BOSSChunkConstructor : public IBOSSChunkConstructor {
    friend IBOSSChunkConstructor;

    template <template <typename KMER> class KmerContainer, typename... Args>
    friend std::unique_ptr<IBOSSChunkConstructor>
    initialize_boss_chunk_constructor(size_t k, const Args& ...args);

  private:
    BOSSChunkConstructor(size_t k,
                         bool canonical_mode = false,
                         const std::string &filter_suffix = "",
                         size_t num_threads = 1,
                         double memory_preallocated = 0,
                         bool verbose = false)
          : kmer_storage_(k + 1,
                          canonical_mode,
                          encode_filter_suffix_boss<KmerExtractor>(filter_suffix),
                          num_threads,
                          memory_preallocated,
                          verbose) {
        if (filter_suffix == std::string(filter_suffix.size(), BOSS::kSentinel)) {
            kmer_storage_.insert_dummy(std::vector<KmerExtractor::TAlphabet>(k + 1, BOSS::kSentinelCode));
        }
    }

    void add_sequence(std::string&& sequence, uint64_t count) {
        kmer_storage_.add_sequence(std::move(sequence), count);
    }

    void add_sequences(std::function<void(CallString)> generate_sequences) {
        kmer_storage_.add_sequences(generate_sequences);
    }

    BOSS::Chunk* build_chunk() {
        auto &kmers = kmer_storage_.data();

        if (!kmer_storage_.suffix_length()) {
            if (kmer_storage_.verbose()) {
                std::cout << "Reconstructing all required dummy source k-mers..."
                          << std::endl;
            }
            Timer timer;

            // kmer_collector stores (BOSS::k_ + 1)-mers
            recover_source_dummy_nodes(kmer_storage_.get_k() - 1,
                                       &kmers,
                                       kmer_storage_.num_threads(),
                                       kmer_storage_.verbose());

            if (kmer_storage_.verbose())
                std::cout << "Dummy source k-mers were reconstructed in "
                          << timer.elapsed() << "sec" << std::endl;
        }

        if constexpr(std::is_same_v<typename KmerStorage::Data,
                                    utils::DequeStorage<typename KmerStorage::Value>>) {
            kmers.shrink_to_fit();
        }

        // kmer_collector stores (BOSS::k_ + 1)-mers
        BOSS::Chunk *result = new BOSS::Chunk(kmer_storage_.alphabet_size(),
                                              kmer_storage_.get_k() - 1,
                                              kmers);
        kmer_storage_.clear();

        return result;
    }

    uint64_t get_k() const { return kmer_storage_.get_k() - 1; }

    KmerStorage kmer_storage_;
};

template <template <typename KMER> class KmerContainer, typename... Args>
static std::unique_ptr<IBOSSChunkConstructor>
initialize_boss_chunk_constructor(size_t k, const Args& ...args) {
    using Extractor = KmerExtractor;

    if ((k + 1) * Extractor::bits_per_char <= 64) {
        return std::unique_ptr<IBOSSChunkConstructor>(
            new BOSSChunkConstructor<KmerContainer<typename Extractor::Kmer64>>(k, args...)
        );
    } else if ((k + 1) * Extractor::bits_per_char <= 128) {
        return std::unique_ptr<IBOSSChunkConstructor>(
            new BOSSChunkConstructor<KmerContainer<typename Extractor::Kmer128>>(k, args...)
        );
    } else {
        return std::unique_ptr<IBOSSChunkConstructor>(
            new BOSSChunkConstructor<KmerContainer<typename Extractor::Kmer256>>(k, args...)
        );
    }
}

template <typename KMER>
using KmerCounterVector = KmerCounter<KMER, KmerExtractor, uint8_t,
                                      Vector<std::pair<KMER, uint8_t>>,
                                      utils::NoCleanup>;
template <typename KMER>
using KmerCounterVectorClean = KmerCounter<KMER, KmerExtractor, uint8_t,
                                           Vector<std::pair<KMER, uint8_t>>,
                                           utils::DummyKmersCleaner>;
template <typename KMER>
using KmerCollectorVector = KmerCollector<KMER, KmerExtractor,
                                          Vector<KMER>,
                                          utils::NoCleanup>;
template <typename KMER>
using KmerCollectorVectorClean = KmerCollector<KMER, KmerExtractor,
                                               Vector<KMER>,
                                               utils::DummyKmersCleaner>;

template <typename KMER>
using KmerCounterDeque = KmerCounter<KMER, KmerExtractor, uint8_t,
                                     utils::DequeStorage<std::pair<KMER, uint8_t>>,
                                     utils::NoCleanup>;
template <typename KMER>
using KmerCounterDequeClean = KmerCounter<KMER, KmerExtractor, uint8_t,
                                          utils::DequeStorage<std::pair<KMER, uint8_t>>,
                                          utils::DummyKmersCleaner>;
template <typename KMER>
using KmerCollectorDeque = KmerCollector<KMER, KmerExtractor,
                                         utils::DequeStorage<KMER>,
                                         utils::NoCleanup>;
template <typename KMER>
using KmerCollectorDequeClean = KmerCollector<KMER, KmerExtractor,
                                              utils::DequeStorage<KMER>,
                                              utils::DummyKmersCleaner>;

std::unique_ptr<IBOSSChunkConstructor>
IBOSSChunkConstructor
::initialize(size_t k,
             bool canonical_mode,
             bool count_kmers,
             const std::string &filter_suffix,
             size_t num_threads,
             double memory_preallocated,
             bool verbose) {

    #define OTHER_ARGS k, canonical_mode, filter_suffix, num_threads, memory_preallocated, verbose

    if (count_kmers) {

        if (filter_suffix.size()) {

            if (!kUseDeque || memory_preallocated > 0) {
                return initialize_boss_chunk_constructor<KmerCounterVector>(OTHER_ARGS);
            } else {
                return initialize_boss_chunk_constructor<KmerCounterDeque>(OTHER_ARGS);
            }
        } else {
            if (!kUseDeque || memory_preallocated > 0) {
                return initialize_boss_chunk_constructor<KmerCounterVectorClean>(OTHER_ARGS);
            } else {
                return initialize_boss_chunk_constructor<KmerCounterDequeClean>(OTHER_ARGS);
            }
        }
    } else {

        if (filter_suffix.size()) {

            if (!kUseDeque || memory_preallocated > 0) {
                return initialize_boss_chunk_constructor<KmerCollectorVector>(OTHER_ARGS);
            } else {
                return initialize_boss_chunk_constructor<KmerCollectorDeque>(OTHER_ARGS);
            }
        } else {
            if (!kUseDeque || memory_preallocated > 0) {
                return initialize_boss_chunk_constructor<KmerCollectorVectorClean>(OTHER_ARGS);
            } else {
                return initialize_boss_chunk_constructor<KmerCollectorDequeClean>(OTHER_ARGS);
            }
        }
    }
}
