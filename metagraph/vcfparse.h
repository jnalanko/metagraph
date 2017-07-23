#ifndef METAGRAPH_VCFPARSE
#define METAGRAPH_VCFPARSE

#include <stdio.h>
#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif
#include <stdint.h>
#include <htslib/vcf_sweep.h>
#include <htslib/faidx.h>
#include "kseq.h"

#ifndef VCFPARSE_T
#define VCFPARSE_T vcfparse
typedef struct __vcfparse {
    faidx_t *ref;
    bcf_sweep_t *sw;
    bcf_hdr_t *hdr;
    bcf1_t *rec;
    uint32_t k;
    uint32_t curi;
    int32_t *curpos;
    const char *curkey;
    char *curalt;
    uint32_t curaltlen;
    kstring_t seq;
    char *kmer1;
    char *kmer3;
    uint32_t kmer1_l;
    uint32_t kmer3_l;
    uint32_t ref_callele_l;
} vcfparse;
static char passfilt[] = "PASS";

static inline void _vcf_clean_kmers(vcfparse* vcfp) {
    if (vcfp->kmer1) {
        free(vcfp->kmer1);
        vcfp->kmer1 = NULL;
    }
    if (vcfp->kmer3) {
        free(vcfp->kmer3);
        vcfp->kmer3 = NULL;
    }
    /*
    if (vcfp->seq.s) {
        free(vcfp->seq.s);
        vcfp->seq.s = NULL;
    }
    */
    /*
    if (vcfp->curalt) {
        free(vcfp->curalt);
        vcfp->curalt = NULL;
    }
    */
}

static inline int vcf_next_line(vcfparse* vcfp) {
    int len;
    if ( (vcfp->rec = bcf_sweep_fwd(vcfp->sw)) ) {
        bcf_unpack(vcfp->rec, BCF_UN_FLT);
        if (vcfp->kmer1)
            _vcf_clean_kmers(vcfp);
        vcfp->curi = 0;
        vcfp->ref_callele_l = strlen(vcfp->rec->d.allele[0]);
        vcfp->curkey = vcfp->hdr->id[BCF_DT_CTG][vcfp->rec->rid].key;
        vcfp->curpos = &(vcfp->rec->pos);
        vcfp->kmer1 = faidx_fetch_seq(vcfp->ref, vcfp->hdr->id[BCF_DT_CTG][vcfp->rec->rid].key, vcfp->rec->pos-vcfp->k, vcfp->rec->pos-1, &len);
        vcfp->kmer3 = faidx_fetch_seq(vcfp->ref, vcfp->hdr->id[BCF_DT_CTG][vcfp->rec->rid].key, vcfp->rec->pos+vcfp->ref_callele_l, vcfp->rec->pos+vcfp->ref_callele_l-1+vcfp->k, &len);
        vcfp->kmer1_l = strlen(vcfp->kmer1);
        vcfp->kmer3_l = strlen(vcfp->kmer3);
        return 1;
    }
    return 0;
}

static inline vcfparse* vcf_init(const char* ref, const char* vcf, int k) {
    vcfparse *vcfp = (vcfparse*)malloc(sizeof(vcfparse));
    //load ref
    vcfp->ref = fai_load(ref);
    if (!vcfp->ref) {
        fprintf(stderr, "Failed to read reference\n");
        exit(1);
    }
    //load vcf
    vcfp->sw = bcf_sweep_init(vcf);
    if (!vcfp->sw) {
        fprintf(stderr, "Failed to read VCF\n");
        exit(1);
    }
    vcfp->k = k;
    vcfp->seq.s = (char*)calloc(1,sizeof(char));
    vcfp->seq.l = 0;
    vcfp->hdr = bcf_sweep_hdr(vcfp->sw);
    vcfp->kmer1 = NULL;
    vcfp->rec = NULL;
    vcfp->curalt = (char*)calloc(1,sizeof(char));
    vcfp->ref_callele_l = 0;
    vcfp->curaltlen = 0;
    if (!vcf_next_line(vcfp)) {
        fprintf(stderr, "Empty VCF file\n");
        exit(1);
    }
    return vcfp;
}

static inline void vcf_print_line(vcfparse* vcfp) {
    if (vcfp->rec) {
        fprintf(stdout, "%s\t%d\t%d\t%d\t%d", 
                vcfp->hdr->id[BCF_DT_CTG][vcfp->rec->rid].key, //contig name
                vcfp->hdr->id[BCF_DT_CTG][vcfp->rec->rid].val->info[0], //contig length
                vcfp->rec->rid, //contig id
                vcfp->rec->pos+1, //1-based coordinate
                vcfp->rec->n_allele //number of alleles
                ); 
    }
}

static inline int vcf_get_seq(vcfparse* vcfp) {
    while (vcfp->rec) {
        (vcfp->curi)++;
        if ((vcfp->curi >= vcfp->rec->n_allele) || (!bcf_has_filter(vcfp->hdr, vcfp->rec, passfilt))) {
            vcf_next_line(vcfp);
            continue;
        }
        /*
        if (vcfp->seq.s) {
            free(vcfp->seq.s);
            vcfp->seq.s = NULL;
        }
        */
        char *temp;
        if (vcfp->rec->d.allele[vcfp->curi][0]=='<') {
            //if this is of the form <CN#>, then it's a copy number variation
            //otherwise, crash
            if (vcfp->rec->d.allele[vcfp->curi][1] != 'C' || vcfp->rec->d.allele[vcfp->curi][2] != 'N') {
                fprintf(stderr, "Can't handle this type of variant: %s\n",vcfp->rec->d.allele[vcfp->curi]);
                exit(1);
            }
            //replace <CN#> with # copies of the ref allele
            //fprintf(stderr, "%s\n", vcfp->rec->d.allele[vcfp->curi]);
            vcfp->rec->d.allele[vcfp->curi][strlen(vcfp->rec->d.allele[vcfp->curi])-1]=0;
            //fprintf(stderr, "%s\n", vcfp->rec->d.allele[vcfp->curi]);
            size_t cn = atol(vcfp->rec->d.allele[vcfp->curi]+3);
            /*
            if (vcfp->curalt) {
                free(vcfp->curalt);
            }
            */
            if (vcfp->ref_callele_l * cn > vcfp->curaltlen) {
                temp = (char*)realloc(vcfp->curalt, vcfp->ref_callele_l * cn+1);
                if (temp) {
                    vcfp->curalt = temp;
                } else {
                    free(vcfp->curalt);
                    vcfp->curalt = (char*)malloc(vcfp->ref_callele_l * cn+1);
                }
            }
            vcfp->curalt[0] = 0;
            while (cn--) {
                strcat(vcfp->curalt, vcfp->rec->d.allele[0]);
            }
            //fprintf(stderr,"\n%u\n%s\n%s\n",cn,vcfp->rec->d.allele[0], vcfp->curalt);
        } else {
            if (strlen(vcfp->rec->d.allele[vcfp->curi]) > vcfp->curaltlen) {
                temp = vcfp->curalt = (char*)realloc(vcfp->curalt, strlen(vcfp->rec->d.allele[vcfp->curi])+1);
                if (temp) {
                    vcfp->curalt = temp;
                } else {
                    free(vcfp->curalt);
                    vcfp->curalt = (char*)malloc(strlen(vcfp->rec->d.allele[vcfp->curi])+1);
                }
            }
            strcpy(vcfp->curalt, vcfp->rec->d.allele[vcfp->curi]);
        }
        //vcfp->curaltlen = strlen(vcfp->rec->d.allele[vcfp->curi]);
        vcfp->curaltlen = strlen(vcfp->curalt);
        if (vcfp->kmer1_l+vcfp->curaltlen+vcfp->kmer3_l > vcfp->seq.l) {
            temp = (char*)realloc(vcfp->seq.s, vcfp->kmer1_l+vcfp->curaltlen+vcfp->kmer3_l+1);
            if (temp) {
                vcfp->seq.s = temp;
            } else {
                free(vcfp->seq.s);
                vcfp->seq.s = (char*)malloc(vcfp->kmer1_l+vcfp->curaltlen+vcfp->kmer3_l+1);
            }
        }
        strcpy(vcfp->seq.s, vcfp->kmer1);
        //strcat(vcfp->seq.s, vcfp->rec->d.allele[vcfp->curi]);
        strcat(vcfp->seq.s, vcfp->curalt);
        strcat(vcfp->seq.s, vcfp->kmer3);
        vcfp->seq.l = strlen(vcfp->seq.s);
        //fprintf(stderr, "%s\n", vcfp->seq.s);
        return 1;
    }
    return 0;
}

static inline void vcf_destroy(vcfparse* vcfp) {
    if (vcfp->kmer1)
        _vcf_clean_kmers(vcfp);
    if (vcfp->seq.s)
        free(vcfp->seq.s);
    if (vcfp->curalt)
        free(vcfp->curalt);
    bcf_sweep_destroy(vcfp->sw);
    fai_destroy(vcfp->ref);
    free(vcfp);
}
#endif
#endif
