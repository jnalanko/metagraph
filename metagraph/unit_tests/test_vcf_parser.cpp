#include <stdio.h>

#include "gtest/gtest.h"

#include "vcf_parser.hpp"

const std::string test_data_dir = "../unit_tests/data/";
const std::string ref_file = test_data_dir + "test_vcfparse.fa";
const std::string vcf_file1 = test_data_dir + "test_vcfparse_1.vcf";
const std::string vcf_file2 = test_data_dir + "test_vcfparse_2.vcf";

const std::vector<std::string> annots = {
    "AC_AFR", 
    "AC_AMR",
    "AC_ASJ",
    "AC_EAS",
    "AC_FIN",
    "AC_NFE",
    "AC_OTH",
    "AC_SAS"
};

TEST(VcfParse, LoadVCF) {
    vcf_parser vcf;
    ASSERT_TRUE(vcf.init(ref_file.c_str(), vcf_file1.c_str(), 3));
}

TEST(VcfParse, TestKmer) {
    vcf_parser vcf;
    ASSERT_TRUE(vcf.init(ref_file, vcf_file1, 3));
    std::string seq, annot;
    ASSERT_TRUE(vcf.get_seq(annots, &seq, &annot));
    EXPECT_EQ("TGCGCGC", seq);
    EXPECT_EQ("test:A:B:C", annot);
    EXPECT_FALSE(vcf.get_seq(annots, &seq, &annot));
}

TEST(VcfParse, TestKmerEdge) {
    vcf_parser vcf;
    ASSERT_TRUE(vcf.init(ref_file, vcf_file1, 4));
    std::string seq, annot;
    ASSERT_TRUE(vcf.get_seq(annots, &seq, &annot));
    EXPECT_EQ("ATGCGCGCG", seq);
    EXPECT_EQ("test:A:B:C", annot);
    EXPECT_FALSE(vcf.get_seq(annots, &seq, &annot));
}

TEST(VcfParse, TestKmerOverLeftEdge) {
    vcf_parser vcf;
    ASSERT_TRUE(vcf.init(ref_file, vcf_file1, 5));
    std::string seq, annot;
    ASSERT_TRUE(vcf.get_seq(annots, &seq, &annot));
    EXPECT_EQ("ATGCGCGCGC", seq);
    EXPECT_EQ("test:A:B:C", annot);
    EXPECT_FALSE(vcf.get_seq(annots, &seq, &annot));
}

TEST(VcfParse, TestKmerOverBothEdges) {
    vcf_parser vcf;
    ASSERT_TRUE(vcf.init(ref_file, vcf_file1, 16));
    std::string seq, annot;
    ASSERT_TRUE(vcf.get_seq(annots, &seq, &annot));
    EXPECT_EQ("ATGCGCGCGCGCTCTCGCGCA", seq);
    EXPECT_EQ("test:A:B:C", annot);
    EXPECT_FALSE(vcf.get_seq(annots, &seq, &annot));
}

TEST(VcfParse, TestKmerInfoCopyNumber) {
    vcf_parser vcf;
    ASSERT_TRUE(vcf.init(ref_file, vcf_file2, 3));
    std::string seq, annot;
    ASSERT_TRUE(vcf.get_seq(annots, &seq, &annot));
    EXPECT_EQ("TGCCCGC", seq);
    EXPECT_EQ("test:AC_AMR", annot);
    annot="";

    EXPECT_TRUE(vcf.get_seq(annots, &seq, &annot));
    EXPECT_EQ("TGCTTCGC", seq);
    EXPECT_EQ("test", annot);
    annot="";

    EXPECT_TRUE(vcf.get_seq(annots, &seq, &annot));
    EXPECT_EQ("TGCTTTTCGC", seq);
    EXPECT_EQ("test", annot);

    EXPECT_FALSE(vcf.get_seq(annots, &seq, &annot));
}

