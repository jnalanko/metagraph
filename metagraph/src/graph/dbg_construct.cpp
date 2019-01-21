#include "dbg_construct.hpp"

#include <type_traits>
#include <ips4o.hpp>

#include "kmer.hpp"
#include "dbg_succinct_chunk.hpp"
#include "utils.hpp"
#include "unix_tools.hpp"
#include "reads_filtering.hpp"
#include "helpers.hpp"

using TAlphabet = KmerExtractor::TAlphabet;

const size_t kMaxKmersChunkSize = 30'000'000;


template <class V>
void try_reserve(V *vector, size_t size, size_t min_size = 0) {
    size = std::max(size, min_size);

    while (size > min_size) {
        try {
            vector->reserve(size);
            return;
        } catch (const std::bad_alloc &exception) {
            size = min_size + (size - min_size) * 2 / 3;
        }
    }
    vector->reserve(min_size);
}

template <class V>
void sort_and_remove_duplicates(V *array,
                                size_t num_threads = 1,
                                size_t offset = 0) {
    ips4o::parallel::sort(array->begin() + offset, array->end(),
                          std::less<typename V::value_type>(),
                          num_threads);
    // remove duplicates
    auto unique_end = std::unique(array->begin() + offset, array->end());
    array->erase(unique_end, array->end());
}

template <typename KMER>
void shrink_kmers(Vector<KMER> *kmers,
                  size_t num_threads,
                  bool verbose,
                  size_t offset = 0) {
    if (verbose) {
        std::cout << "Allocated capacity exceeded, filter out non-unique k-mers..."
                  << std::flush;
    }

    size_t prev_num_kmers = kmers->size();
    sort_and_remove_duplicates(kmers, num_threads, offset);

    if (verbose) {
        std::cout << " done. Number of kmers reduced from " << prev_num_kmers
                                                  << " to " << kmers->size() << ", "
                  << (kmers->size() * sizeof(KMER) >> 20) << "Mb" << std::endl;
    }
}

template <class Array, class Vector>
void extend_kmer_storage(const Array &temp_storage,
                         Vector *kmers,
                         size_t num_threads,
                         bool verbose,
                         std::mutex &mutex_resize,
                         std::shared_timed_mutex &mutex_copy) {
    // acquire the mutex to restrict the number of writing threads
    std::unique_lock<std::mutex> resize_lock(mutex_resize);

    if (kmers->size() + temp_storage.size() > kmers->capacity()) {
        std::unique_lock<std::shared_timed_mutex> reallocate_lock(mutex_copy);

        shrink_kmers(kmers, num_threads, verbose);

        try {
            try_reserve(kmers, kmers->size() + kmers->size() / 2,
                               kmers->size() + temp_storage.size());
        } catch (const std::bad_alloc &exception) {
            std::cerr << "ERROR: Can't reallocate. Not enough memory" << std::endl;
            exit(1);
        }
    }

    size_t offset = kmers->size();
    kmers->resize(kmers->size() + temp_storage.size());

    std::shared_lock<std::shared_timed_mutex> copy_lock(mutex_copy);

    resize_lock.unlock();

    std::copy(temp_storage.begin(),
              temp_storage.end(),
              kmers->begin() + offset);
}

typedef std::function<void(const std::string&)> CallbackString;

template <typename KMER, class KmerExtractor>
void extract_kmers(std::function<void(CallbackString)> generate_reads,
                   size_t k,
                   bool canonical_mode,
                   Vector<KMER> *kmers,
                   const std::vector<TAlphabet> &suffix,
                   size_t num_threads,
                   bool verbose,
                   std::mutex &mutex_resize,
                   std::shared_timed_mutex &mutex_copy,
                   bool remove_redundant = true) {
    static_assert(KMER::kBitsPerChar == KmerExtractor::kLogSigma);

    Vector<KMER> temp_storage;
    temp_storage.reserve(1.1 * kMaxKmersChunkSize);

    generate_reads([&](const std::string &read) {
        KmerExtractor::sequence_to_kmers(read, k, suffix, &temp_storage, canonical_mode);

        if (temp_storage.size() < kMaxKmersChunkSize)
            return;

        if (remove_redundant) {
            sort_and_remove_duplicates(&temp_storage);
        }

        if (temp_storage.size() > 0.9 * kMaxKmersChunkSize) {
            extend_kmer_storage(temp_storage, kmers,
                                num_threads, verbose, mutex_resize, mutex_copy);
            temp_storage.resize(0);
        }
    });

    if (temp_storage.size()) {
        if (remove_redundant) {
            sort_and_remove_duplicates(&temp_storage);
        }
        extend_kmer_storage(temp_storage, kmers,
                            num_threads, verbose, mutex_resize, mutex_copy);
    }
}

// Although this function could be parallelized better,
// the experiments show it's already fast enough.
// k is node length
template <typename KMER>
void recover_source_dummy_nodes(size_t k,
                                Vector<KMER> *kmers,
                                size_t num_threads,
                                bool verbose) {
    // remove redundant dummy kmers inplace
    size_t cur_pos = 0;
    size_t dummy_begin = kmers->size();
    size_t num_dummy_parent_kmers = 0;

    for (size_t i = 0; i < dummy_begin; ++i) {
        const KMER &kmer = kmers->at(i);
        // we never add reads shorter than k
        assert(kmer[1] != 0 || kmer[0] != 0 || kmer[k] == 0);

        TAlphabet edge_label;

        // check if it's not a source dummy kmer
        if (kmer[1] > 0 || (edge_label = kmer[0]) == 0) {
            kmers->at(cur_pos++) = kmer;
            continue;
        }

        bool redundant = false;
        for (size_t j = i + 1; j < dummy_begin
                                && KMER::compare_suffix(kmer, kmers->at(j), 1); ++j) {
            if (edge_label == kmers->at(j)[0]) {
                // This source dummy kmer is redundant and has to be erased
                redundant = true;
                break;
            }
        }
        if (redundant)
            continue;

        num_dummy_parent_kmers++;

        // leave this dummy kmer in the list
        kmers->at(cur_pos++) = kmer;

        if (kmers->size() + 1 > kmers->capacity())
            shrink_kmers(kmers, num_threads, verbose, dummy_begin);

        kmers->push_back(kmers->at(i));
        kmers->back().to_prev(k + 1, DBG_succ::kSentinelCode);
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

    std::copy(kmers->begin() + dummy_begin, kmers->end(),
              kmers->begin() + cur_pos);
    kmers->resize(kmers->size() - dummy_begin + cur_pos);
    dummy_begin = cur_pos;

    for (size_t c = 3; c < k + 1; ++c) {
        size_t succ_dummy_begin = dummy_begin;
        dummy_begin = kmers->size();

        for (size_t i = succ_dummy_begin; i < dummy_begin; ++i) {
            if (kmers->size() + 1 > kmers->capacity())
                shrink_kmers(kmers, num_threads, verbose, dummy_begin);

            kmers->push_back(kmers->at(i));
            kmers->back().to_prev(k + 1, DBG_succ::kSentinelCode);
        }
        sort_and_remove_duplicates(kmers, num_threads, dummy_begin);

        if (verbose) {
            std::cout << "Number of dummy k-mers with dummy prefix of length " << c
                      << ": " << kmers->size() - dummy_begin << std::endl;
        }
    }
    ips4o::parallel::sort(kmers->begin(), kmers->end(), std::less<KMER>(), num_threads);
}

/**
 * Initialize graph chunk from a list of sorted kmers.
 */


template <typename KMER, class KmerExtractor>
KmerCollector<KMER, KmerExtractor>
::KmerCollector(size_t k,
                bool canonical_mode,
                Sequence&& filter_suffix_encoded,
                size_t num_threads,
                double memory_preallocated,
                bool verbose)
      : k_(k),
        num_threads_(num_threads),
        thread_pool_(std::max(static_cast<size_t>(1), num_threads_) - 1,
                     std::max(static_cast<size_t>(1), num_threads_)),
        stored_sequences_size_(0),
        verbose_(verbose),
        filter_suffix_encoded_(std::move(filter_suffix_encoded)),
        canonical_mode_(canonical_mode) {
    assert(num_threads_ > 0);
    static_assert(KMER::kBitsPerChar == KmerExtractor::kLogSigma);

    try_reserve(&kmers_, memory_preallocated / sizeof(KMER));
    if (verbose_) {
        std::cout << "Preallocated "
                  << kmers_.capacity() * sizeof(KMER) / (1llu << 30)
                  << "Gb for the k-mer storage"
                  << ", capacity: " << kmers_.capacity() << " k-mers"
                  << std::endl;
    }
}

template <typename KMER, class KmerExtractor>
void KmerCollector<KMER, KmerExtractor>
::add_sequence(const std::string &sequence) {
    if (sequence.size() < k_)
        return;

    // put read into temporary storage
    stored_sequences_size_ += sequence.size();
    sequences_storage_.emplace_back(sequence);

    if (stored_sequences_size_ < kMaxKmersChunkSize)
        return;

    // extract all k-mers from sequences accumulated in the temporary storage
    release_task_to_pool();

    assert(!stored_sequences_size_);
    assert(!sequences_storage_.size());
}

template <typename KMER, class KmerExtractor>
void KmerCollector<KMER, KmerExtractor>
::add_sequences(const std::function<void(CallbackString)> &generate_sequences) {
    thread_pool_.enqueue(extract_kmers<KMER, Extractor>, generate_sequences,
                         k_, canonical_mode_, &kmers_,
                         filter_suffix_encoded_,
                         num_threads_, verbose_,
                         std::ref(mutex_resize_), std::ref(mutex_copy_), true);
}

template <typename KMER, class KmerExtractor>
void KmerCollector<KMER, KmerExtractor>::release_task_to_pool() {
    auto *current_sequences_storage = new std::vector<std::string>();
    current_sequences_storage->swap(sequences_storage_);

    thread_pool_.enqueue(extract_kmers<KMER, Extractor>,
                         [current_sequences_storage](CallbackString callback) {
                             for (auto &&sequence : *current_sequences_storage) {
                                 callback(std::move(sequence));
                             }
                             delete current_sequences_storage;
                         },
                         k_, canonical_mode_, &kmers_, filter_suffix_encoded_,
                         num_threads_, verbose_,
                         std::ref(mutex_resize_), std::ref(mutex_copy_), true);
    stored_sequences_size_ = 0;
}

template <typename KMER, class KmerExtractor>
void KmerCollector<KMER, KmerExtractor>::join() {
    release_task_to_pool();
    thread_pool_.join();

    if (verbose_) {
        std::cout << "Reading data has finished" << std::endl;
        get_RAM();
        std::cout << "Sorting k-mers and appending succinct"
                  << " representation from current bin...\t" << std::flush;
    }
    Timer timer;

    sort_and_remove_duplicates(&kmers_, num_threads_);

    if (verbose_)
        std::cout << timer.elapsed() << "sec" << std::endl;
}

template class KmerCollector<KmerExtractor::Kmer64, KmerExtractor>;
template class KmerCollector<KmerExtractor::Kmer128, KmerExtractor>;
template class KmerCollector<KmerExtractor::Kmer256, KmerExtractor>;
template class KmerCollector<KmerExtractor2Bit::Kmer64, KmerExtractor2Bit>;
template class KmerCollector<KmerExtractor2Bit::Kmer128, KmerExtractor2Bit>;
template class KmerCollector<KmerExtractor2Bit::Kmer256, KmerExtractor2Bit>;


template <class KmerExtractor>
inline std::vector<typename KmerExtractor::TAlphabet>
encode_filter_suffix(const std::string &filter_suffix) {
    std::vector<typename KmerExtractor::TAlphabet> filter_suffix_encoded;
    // TODO: cleanup
    std::transform(
        filter_suffix.begin(), filter_suffix.end(),
        std::back_inserter(filter_suffix_encoded),
        [](char c) {
            return KmerExtractor::encode(c);
        }
    );
    return filter_suffix_encoded;
}

template <class KmerExtractor>
inline std::vector<typename KmerExtractor::TAlphabet>
encode_filter_suffix_boss(const std::string &filter_suffix) {
    std::vector<typename KmerExtractor::TAlphabet> filter_suffix_encoded;
    std::transform(
        filter_suffix.begin(), filter_suffix.end(),
        std::back_inserter(filter_suffix_encoded),
        [](char c) {
            return c == DBG_succ::kSentinel
                            ? DBG_succ::kSentinelCode
                            : KmerExtractor::encode(c);
        }
    );
    return filter_suffix_encoded;
}

template <typename KMER>
SDChunkConstructor<KMER>
::SDChunkConstructor(size_t k,
                     bool canonical_mode,
                     const std::string &filter_suffix,
                     size_t num_threads,
                     double memory_preallocated,
                     bool verbose)
      : kmer_collector_(k,
                        canonical_mode,
                        encode_filter_suffix<Extractor>(filter_suffix),
                        num_threads,
                        memory_preallocated,
                        verbose) {}

template <typename KMER>
DBGBOSSChunkConstructor<KMER>
::DBGBOSSChunkConstructor(size_t k,
                     const std::string &filter_suffix,
                     size_t num_threads,
                     double memory_preallocated,
                     bool verbose)
      : kmer_collector_(k + 1,
                        false,
                        encode_filter_suffix_boss<Extractor>(filter_suffix),
                        num_threads,
                        memory_preallocated,
                        verbose) {
    if (filter_suffix == std::string(filter_suffix.size(), DBG_succ::kSentinel)) {
        kmer_collector_.emplace_back(
            std::vector<TAlphabet>(k + 1, DBG_succ::kSentinelCode), k + 1
        );
    }
}

template <typename KMER>
DBGSD::Chunk* SDChunkConstructor<KMER>
::build_chunk() {
    kmer_collector_.join();
    std::unique_ptr<DBGSD::Chunk> chunk {
        new DBGSD::Chunk(
            [&](const auto &index_callback) {
                kmer_collector_.call_kmers(
                    [&](const auto &kmer) {
                        index_callback(DBGSD::kmer_to_index(kmer));
                    }
                );
            },
            DBGSD::capacity(get_k(), KMER::kBitsPerChar),
            kmer_collector_.size()
        )
    };
    assert(chunk.get());

    return chunk.release();
}

//TODO cleanup
// k is node length
template <typename KMER>
DBG_succ::Chunk* chunk_from_kmers(TAlphabet alph_size,
                                  size_t k,
                                  const KMER *kmers,
                                  uint64_t num_kmers) {
    assert(std::is_sorted(kmers, kmers + num_kmers));

    // the array containing edge labels
    std::vector<TAlphabet> W(1 + num_kmers);
    W[0] = 0;
    // the bit array indicating last outgoing edges for nodes
    std::vector<bool> last(1 + num_kmers, 1);
    last[0] = 0;
    // offsets
    std::vector<uint64_t> F(alph_size, 0);
    F.at(0) = 0;

    size_t curpos = 1;
    TAlphabet lastF = 0;

    for (size_t i = 0; i < num_kmers; ++i) {
        TAlphabet curW = kmers[i][0];
        TAlphabet curF = kmers[i][k];

        assert(curW < alph_size);

        // check redundancy and set last
        if (i + 1 < num_kmers && KMER::compare_suffix(kmers[i], kmers[i + 1])) {
            // skip redundant dummy edges
            if (curW == 0 && curF > 0)
                continue;

            last[curpos] = 0;
        }
        //set W
        if (i > 0) {
            for (size_t j = i - 1; KMER::compare_suffix(kmers[i], kmers[j], 1); --j) {
                if (curW > 0 && kmers[j][0] == curW) {
                    curW += alph_size;
                    break;
                }
                if (j == 0)
                    break;
            }
        }
        W[curpos] = curW;

        while (lastF + 1 < alph_size && curF != lastF) {
            F.at(++lastF) = curpos - 1;
        }
        curpos++;
    }
    while (++lastF < alph_size) {
        F.at(lastF) = curpos - 1;
    }

    W.resize(curpos);
    last.resize(curpos);

    return new DBG_succ::Chunk(k, std::move(W), std::move(last), std::move(F));
}

template <typename KMER>
DBG_succ::Chunk* DBGBOSSChunkConstructor<KMER>
::build_chunk() {
    kmer_collector_.join();

    if (!kmer_collector_.suffix_length()) {
        if (kmer_collector_.verbose()) {
            std::cout << "Reconstructing all required dummy source k-mers...\t"
                      << std::flush;
        }
        Timer timer;

        // kmer_collector stores (DBG_succ::k_ + 1)-mers
        recover_source_dummy_nodes(kmer_collector_.get_k() - 1,
                                   &kmer_collector_.data(),
                                   kmer_collector_.num_threads(),
                                   kmer_collector_.verbose());

        if (kmer_collector_.verbose())
            std::cout << timer.elapsed() << "sec" << std::endl;
    }

    // kmer_collector stores (DBG_succ::k_ + 1)-mers
    DBG_succ::Chunk *result = chunk_from_kmers(
        kmer_collector_.alphabet_size(),
        kmer_collector_.get_k() - 1,
        kmer_collector_.data().data(),
        kmer_collector_.size()
    );

    kmer_collector_.clear();

    return result;
}

DBG_succ* DBGSuccConstructor
::build_graph_from_chunks(const std::vector<std::string> &chunk_filenames,
                          bool verbose) {
    // TODO: move from chunk to here?
    return DBG_succ::Chunk::build_graph_from_chunks(chunk_filenames, verbose);
}

DBGSD* DBGSDConstructor
::build_graph_from_chunks(const std::vector<std::string> &chunk_filenames,
                          bool canonical_mode,
                          bool verbose) {
    if (!chunk_filenames.size())
        return new DBGSD(2);

    std::vector<DBGSD::Chunk> chunks;

    uint64_t cumulative_size = 1;

    for (auto file : chunk_filenames) {
        file = utils::remove_suffix(file, ".dbgsdchunk") + ".dbgsdchunk";

        std::ifstream chunk_in(file, std::ios::binary);

        if (!chunk_in.good()) {
            std::cerr << "ERROR: input file " << file << " corrupted" << std::endl;
            exit(1);
        }

        chunks.emplace_back();
        chunks.back().load(chunk_in);

        assert(chunks.empty() || chunks.back().size() == chunks.front().size());

        cumulative_size += chunks.back().num_set_bits();
    }

    if (verbose)
        std::cout << "Cumulative size of chunks: "
                  << cumulative_size - 1 << std::endl;

    std::unique_ptr<DBGSD> graph{
        new DBGSD(2)
    };

    graph->kmers_ = DBGSD::Chunk(
        [&](const auto &index_callback) {
            index_callback(0);
            for (size_t i = 0; i < chunk_filenames.size(); ++i) {
                if (verbose) {
                    std::cout << "Chunk "
                              << chunk_filenames[i]
                              << " loaded..." << std::flush;
                }

                chunks[i].call_ones(index_callback);

                if (verbose) {
                    std::cout << " concatenated" << std::endl;
                }
            }
        },
        chunks[0].size(), cumulative_size
    );

    graph->canonical_mode_ = canonical_mode;

    graph->k_ = graph->infer_k(graph->kmers_.size(), DBGSD::KmerExtractor::kLogSigma);

    return graph.release();
}


IDBGBOSSChunkConstructor*
IDBGBOSSChunkConstructor
::initialize(size_t k,
             const std::string &filter_suffix,
             size_t num_threads,
             double memory_preallocated,
             bool verbose) {
    using Extractor = KmerExtractor;
    if ((k + 1) * Extractor::kLogSigma <= 64) {
        return new DBGBOSSChunkConstructor<typename Extractor::Kmer64>(
            k, filter_suffix, num_threads, memory_preallocated, verbose
        );
    } else if ((k + 1) * Extractor::kLogSigma <= 128) {
        return new DBGBOSSChunkConstructor<typename Extractor::Kmer128>(
            k, filter_suffix, num_threads, memory_preallocated, verbose
        );
    } else {
        return new DBGBOSSChunkConstructor<typename Extractor::Kmer256>(
            k, filter_suffix, num_threads, memory_preallocated, verbose
        );
    }
}

ISDChunkConstructor*
ISDChunkConstructor
::initialize(size_t k,
             bool canonical_mode,
             const std::string &filter_suffix,
             size_t num_threads,
             double memory_preallocated,
             bool verbose) {
    using Extractor = KmerExtractor2Bit;
    if (k * Extractor::kLogSigma <= 64) {
        return new SDChunkConstructor<typename Extractor::Kmer64>(
            k, canonical_mode, filter_suffix, num_threads, memory_preallocated, verbose
        );
    } else if (k * Extractor::kLogSigma <= 128) {
        return new SDChunkConstructor<typename Extractor::Kmer128>(
            k, canonical_mode, filter_suffix, num_threads, memory_preallocated, verbose
        );
    } else {
        return new SDChunkConstructor<typename Extractor::Kmer256>(
            k, canonical_mode, filter_suffix, num_threads, memory_preallocated, verbose
        );
    }
}

void DBGSDConstructor::build_graph(DBGSD *graph) {
    auto chunk = constructor_->build_chunk();
    graph->k_ = constructor_->get_k();
    graph->canonical_mode_ = constructor_->is_canonical_mode();
    graph->kmers_ = decltype(graph->kmers_)(
        [&](const auto &index_callback) {
            index_callback(0);
            chunk->call_ones(index_callback);
        },
        chunk->size(), chunk->num_set_bits() + 1
    );
    delete chunk;
}

DBGSD::Chunk* DBGSDConstructor::build_chunk() {
    return constructor_->build_chunk();
}
