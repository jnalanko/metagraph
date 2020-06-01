#include "transform_annotation.hpp"

#include <fmt/format.h>

#include "common/logger.hpp"
#include "common/unix_tools.hpp"
#include "common/threads/threading.hpp"
#include "annotation/representation/row_compressed/annotate_row_compressed.hpp"
#include "annotation/representation/column_compressed/annotate_column_compressed.hpp"
#include "annotation/representation/annotation_matrix/static_annotators_def.hpp"
#include "annotation/binary_matrix/multi_brwt/clustering.hpp"
#include "annotation/annotation_converters.hpp"
#include "config/config.hpp"
#include "load/load_annotation.hpp"

using mtg::common::logger;
using mtg::common::get_verbose;
using namespace annotate;

typedef MultiLabelEncoded<std::string> Annotator;

static const Eigen::IOFormat CSVFormat(Eigen::StreamPrecision,
                                       Eigen::DontAlignCols, " ", "\n");


template <class AnnotatorTo, class AnnotatorFrom>
void convert(std::unique_ptr<AnnotatorFrom> annotator,
             const Config &config,
             const Timer &timer) {
    logger->trace("Converting annotation to {}...",
                  Config::annotype_to_string(config.anno_type));

    auto target_annotator = convert<AnnotatorTo>(std::move(*annotator));
    annotator.reset();
    logger->trace("Conversion done in {} sec", timer.elapsed());

    logger->trace("Serializing annotation to '{}'...", config.outfbase);
    target_annotator->serialize(config.outfbase);
}


int transform_annotation(Config *config) {
    assert(config);

    const auto &files = config->fnames;

    const Config::AnnotationType input_anno_type
        = parse_annotation_type(files.at(0));

    if (input_anno_type != Config::ColumnCompressed && files.size() > 1) {
        logger->error("Conversion of multiple annotators is only "
                      "supported for ColumnCompressed");
        exit(1);
    }

    Timer timer;

    /********************************************************/
    /***************** dump labels to text ******************/
    /********************************************************/

    if (config->dump_text_anno) {
        auto annotation = initialize_annotation(files.at(0), *config);

        logger->trace("Loading annotation...");

        if (config->anno_type == Config::ColumnCompressed) {
            if (!annotation->merge_load(files)) {
                logger->error("Cannot load annotations");
                exit(1);
            }
        } else {
            // Load annotation from disk
            if (!annotation->load(files.at(0))) {
                logger->error("Cannot load annotations from file '{}'", files.at(0));
                exit(1);
            }
        }

        logger->trace("Annotation loaded in {} sec", timer.elapsed());
        logger->trace("Dumping annotators...\t");

        if (input_anno_type == Config::ColumnCompressed) {
            assert(dynamic_cast<ColumnCompressed<>*>(annotation.get()));
            dynamic_cast<ColumnCompressed<>*>(
                annotation.get()
            )->dump_columns(config->outfbase, get_num_threads());
        } else if (input_anno_type == Config::BRWT) {
            assert(dynamic_cast<MultiBRWTAnnotator*>(annotation.get()));
            dynamic_cast<MultiBRWTAnnotator*>(
                annotation.get()
            )->dump_columns(config->outfbase, get_num_threads());
        } else {
            throw std::runtime_error("Dumping columns for this type not implemented");
        }

        logger->trace("Dumping done in {} sec", timer.elapsed());

        return 0;
    }

    /********************************************************/
    /***************** rename column labels *****************/
    /********************************************************/

    if (config->rename_instructions_file.size()) {
        tsl::hopscotch_map<std::string, std::string> dict;
        std::ifstream instream(config->rename_instructions_file);
        if (!instream.is_open()) {
            logger->error("Cannot open file '{}'", config->rename_instructions_file);
            exit(1);
        }
        std::string old_name;
        std::string new_name;
        while (instream.good() && !(instream >> old_name).eof()) {
            instream >> new_name;
            if (instream.fail() || instream.eof()) {
                logger->error("Wrong format of the rules for renaming"
                              " annotation columns passed in file '{}'",
                              config->rename_instructions_file);
                exit(1);
            }
            dict[old_name] = new_name;
        }

        auto annotation = initialize_annotation(files.at(0), *config);

        logger->trace("Loading annotation...");

        // TODO: rename columns without loading the full annotation
        if (config->anno_type == Config::ColumnCompressed) {
            if (!annotation->merge_load(files)) {
                logger->error("Cannot load annotations");
                exit(1);
            } else {
                logger->info("Annotation #objects: {}\t#labels: {}",
                             annotation->num_objects(), annotation->num_labels());
            }
        } else {
            // Load annotation from disk
            if (!annotation->load(files.at(0))) {
                logger->error("Cannot load annotations from file '{}'", files.at(0));
                exit(1);
            }
        }

        logger->trace("Annotation loaded in {} sec", timer.elapsed());
        logger->trace("Renaming...");

        //TODO: could be made to work with streaming
        annotation->rename_labels(dict);

        annotation->serialize(config->outfbase);
        logger->trace("Renaming done in {} sec", timer.elapsed());

        return 0;
    }

    /********************************************************/
    /****************** convert annotation ******************/
    /********************************************************/

    if (config->cluster_linkage) {
        if (input_anno_type != Config::ColumnCompressed) {
            logger->error("Column clustering is only supported for ColumnCompressed");
            exit(1);
        }

        logger->trace("Loading annotation and sampling subcolumns of size {}",
                      config->num_rows_subsampled);

        std::vector<uint64_t> row_indexes;
        std::vector<std::unique_ptr<sdsl::bit_vector>> subcolumn_ptrs;
        std::vector<uint64_t> column_ids;
        uint64_t num_rows = 0;
        std::mutex mu;

        ThreadPool subsampling_pool(get_num_threads(), 1);

        // Load columns from disk
        bool success = ColumnCompressed<>::merge_load(files,
            [&](uint64_t i, const std::string &label, auto&& column) {
                subsampling_pool.enqueue([&,i,label,column{std::move(column)}]() {
                    sdsl::bit_vector *subvector;
                    {
                        std::lock_guard<std::mutex> lock(mu);
                        if (row_indexes.empty()) {
                            num_rows = column->size();
                            row_indexes
                                = sample_row_indexes(num_rows,
                                                     config->num_rows_subsampled);
                        } else if (column->size() != num_rows) {
                            logger->error("Size of column {} is {} != {}",
                                          label, column->size(), num_rows);
                            exit(1);
                        }
                        subcolumn_ptrs.emplace_back(new sdsl::bit_vector());
                        subvector = subcolumn_ptrs.back().get();
                        column_ids.push_back(i);
                        logger->trace("Column {}: {}", i, label);
                    }

                    *subvector = sdsl::bit_vector(row_indexes.size(), false);
                    for (size_t j = 0; j < row_indexes.size(); ++j) {
                        if ((*column)[row_indexes[j]])
                            (*subvector)[j] = true;
                    }
                });
            },
            get_num_threads()
        );

        if (!success) {
            logger->error("Cannot load annotations");
            exit(1);
        }

        subsampling_pool.join();

        // arrange the columns in their original order
        std::vector<std::unique_ptr<sdsl::bit_vector>> permuted(subcolumn_ptrs.size());
        permuted.swap(subcolumn_ptrs);
        for (size_t i = 0; i < column_ids.size(); ++i) {
            subcolumn_ptrs[column_ids[i]] = std::move(permuted[i]);
        }

        std::vector<sdsl::bit_vector> subcolumns;
        for (auto &col_ptr : subcolumn_ptrs) {
            subcolumns.push_back(std::move(*col_ptr));
        }

        LinkageMatrix linkage_matrix
                = agglomerative_greedy_linkage(std::move(subcolumns),
                                               get_num_threads());

        std::ofstream out(config->outfbase);
        out << linkage_matrix.format(CSVFormat) << std::endl;

        logger->trace("Linkage matrix is written to {}", config->outfbase);
        return 0;
    }

    if (config->anno_type == input_anno_type) {
        logger->info("Skipping conversion: same input and target type: {}",
                      Config::annotype_to_string(config->anno_type));
        return 0;
    }

    logger->trace("Converting to {} annotator...",
                  Config::annotype_to_string(config->anno_type));

    if (input_anno_type == Config::RowCompressed) {

        std::unique_ptr<const Annotator> target_annotator;

        switch (config->anno_type) {
            case Config::RowFlat: {
                auto annotator = convert<RowFlatAnnotator>(files.at(0));
                target_annotator = std::move(annotator);
                break;
            }
            case Config::RBFish: {
                auto annotator = convert<RainbowfishAnnotator>(files.at(0));
                target_annotator = std::move(annotator);
                break;
            }
            case Config::BinRelWT_sdsl: {
                auto annotator = convert<BinRelWT_sdslAnnotator>(files.at(0));
                target_annotator = std::move(annotator);
                break;
            }
            case Config::BinRelWT: {
                auto annotator = convert<BinRelWTAnnotator>(files.at(0));
                target_annotator = std::move(annotator);
                break;
            }
            default:
                logger->error("Streaming conversion from RowCompressed "
                              "annotation is not implemented for the requested "
                              "target type: {}",
                              Config::annotype_to_string(config->anno_type));
                exit(1);
        }

        logger->trace("Annotation converted in {} sec", timer.elapsed());

        logger->trace("Serializing to '{}'...", config->outfbase);

        target_annotator->serialize(config->outfbase);

        logger->trace("Serialization done in {} sec", timer.elapsed());

    } else if (input_anno_type == Config::ColumnCompressed) {
        auto annotation = initialize_annotation(files.at(0), *config);

        if (config->anno_type != Config::BRWT || !config->infbase.size()) {
            logger->trace("Loading annotation from disk...");
            if (!annotation->merge_load(files)) {
                logger->error("Cannot load annotations");
                exit(1);
            }
            logger->trace("Annotation loaded in {} sec", timer.elapsed());
        }

        std::unique_ptr<ColumnCompressed<>> annotator {
            dynamic_cast<ColumnCompressed<> *>(annotation.release())
        };
        assert(annotator);

        switch (config->anno_type) {
            case Config::ColumnCompressed: {
                assert(false);
                break;
            }
            case Config::RowCompressed: {
                if (config->fast) {
                    RowCompressed<> row_annotator(annotator->num_objects());
                    convert_to_row_annotator(*annotator,
                                             &row_annotator,
                                             get_num_threads());
                    annotator.reset();

                    logger->trace("Annotation converted in {} sec", timer.elapsed());
                    logger->trace("Serializing to '{}'...", config->outfbase);

                    row_annotator.serialize(config->outfbase);

                    logger->trace("Serialization done in {} sec", timer.elapsed());

                } else {
                    convert_to_row_annotator(*annotator,
                                             config->outfbase,
                                             get_num_threads());
                    logger->trace("Annotation converted and serialized in {} sec",
                                  timer.elapsed());
                }
                break;
            }
            case Config::BRWT: {
                auto brwt_annotator = config->infbase.size()
                    ? convert_to_BRWT<MultiBRWTAnnotator>(
                        files, config->infbase,
                        config->parallel_nodes,
                        get_num_threads(),
                        config->tmp_dir.empty()
                            ? std::filesystem::path(config->outfbase).remove_filename()
                            : config->tmp_dir)
                    : (config->greedy_brwt
                        ? convert_to_greedy_BRWT<MultiBRWTAnnotator>(
                            std::move(*annotator),
                            config->parallel_nodes,
                            get_num_threads(),
                            config->num_rows_subsampled)
                        : convert_to_simple_BRWT<MultiBRWTAnnotator>(
                            std::move(*annotator),
                            config->arity_brwt,
                            config->parallel_nodes,
                            get_num_threads()));

                annotator.reset();
                logger->trace("Annotation converted in {} sec", timer.elapsed());

                logger->trace("Serializing to '{}'", config->outfbase);

                brwt_annotator->serialize(config->outfbase);

                break;
            }
            case Config::BinRelWT_sdsl: {
                convert<BinRelWT_sdslAnnotator>(std::move(annotator), *config, timer);
                break;
            }
            case Config::BinRelWT: {
                convert<BinRelWTAnnotator>(std::move(annotator), *config, timer);
                break;
            }
            case Config::RowFlat: {
                convert<RowFlatAnnotator>(std::move(annotator), *config, timer);
                break;
            }
            case Config::RBFish: {
                convert<RainbowfishAnnotator>(std::move(annotator), *config, timer);
                break;
            }
        }

    } else {
        logger->error("Conversion to other representations"
                      " is not implemented for {} annotator",
                      Config::annotype_to_string(input_anno_type));
        exit(1);
    }

    logger->trace("Done");

    return 0;
}

int merge_annotation(Config *config) {
    assert(config);

    const auto &files = config->fnames;

    if (config->anno_type == Config::ColumnCompressed) {
        ColumnCompressed<> annotation(0, config->num_columns_cached);
        if (!annotation.merge_load(files)) {
            logger->error("Cannot load annotations");
            exit(1);
        }
        annotation.serialize(config->outfbase);
        return 0;
    }

    std::vector<std::unique_ptr<Annotator>> annotators;
    std::vector<std::string> stream_files;

    for (const auto &filename : files) {
        auto anno_file_type = parse_annotation_type(filename);
        if (anno_file_type == Config::AnnotationType::RowCompressed) {
            stream_files.push_back(filename);
        } else {
            auto annotator = initialize_annotation(filename, *config);
            if (!annotator->load(filename)) {
                logger->error("Cannot load annotations from file '{}'", filename);
                exit(1);
            }
            annotators.push_back(std::move(annotator));
        }
    }

    if (config->anno_type == Config::RowCompressed) {
        merge<RowCompressed<>>(std::move(annotators), stream_files, config->outfbase);
    } else if (config->anno_type == Config::RowFlat) {
        merge<RowFlatAnnotator>(std::move(annotators), stream_files, config->outfbase);
    } else if (config->anno_type == Config::RBFish) {
        merge<RainbowfishAnnotator>(std::move(annotators), stream_files, config->outfbase);
    } else if (config->anno_type == Config::BinRelWT_sdsl) {
        merge<BinRelWT_sdslAnnotator>(std::move(annotators), stream_files, config->outfbase);
    } else if (config->anno_type == Config::BinRelWT) {
        merge<BinRelWTAnnotator>(std::move(annotators), stream_files, config->outfbase);
    } else if (config->anno_type == Config::BRWT) {
        merge<MultiBRWTAnnotator>(std::move(annotators), stream_files, config->outfbase);
    } else {
        logger->error("Merging of annotations to '{}' representation is not implemented",
                      config->annotype_to_string(config->anno_type));
        exit(1);
    }

    return 0;
}

int relax_multi_brwt(Config *config) {
    assert(config);

    const auto &files = config->fnames;

    assert(files.size() == 1);
    assert(config->outfbase.size());

    Timer timer;

    auto annotator = std::make_unique<annotate::MultiBRWTAnnotator>();

    logger->trace("Loading annotator...");

    if (!annotator->load(files.at(0))) {
        logger->error("Cannot load annotations from file '{}'", files.at(0));
        exit(1);
    }
    logger->trace("Annotator loaded in {} sec", timer.elapsed());

    logger->trace("Relaxing BRWT tree...");

    annotate::relax_BRWT<annotate::MultiBRWTAnnotator>(annotator.get(),
                                                       config->relax_arity_brwt,
                                                       get_num_threads());

    annotator->serialize(config->outfbase);
    logger->trace("BRWT relaxation done in {} sec", timer.elapsed());

    return 0;
}
