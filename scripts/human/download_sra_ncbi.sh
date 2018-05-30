#!/usr/bin/env bash

ASCP="/cluster/project/grlab/share/modules/packages/aspera/3.6.2/bin/ascp"
ASCP_KEY="/cluster/project/grlab/share/modules/packages/aspera/3.6.2/etc/asperaweb_id_dsa.openssh"
LFTP="/cluster/project/raetsch/lab/01/share/modules/packages/lftp/4.8.3/bin/lftp"

BUFFER="/cluster/project/grlab/projects/tmp_meta/data"

FINAL_DEST="/cluster/work/grlab/projects/metagenome/benchmark_human_metagenome/nobackup/human_gut_sra"


if [ $# -ne 1 ]; then
  echo "Usage: $0 <NCBI_URL>"
  echo "Example: $0 /sra/sra-instant/reads/ByRun/sra/SRR/SRR712/SRR7123282/SRR7123282.sra"
  exit 1
fi

URL="$1"
FILE=$(basename $URL)

# check if the file already exists
#if $LFTP -c "connect -u mikhaika, sftp://leomed; glob --exist $FINAL_DEST/*$FILE"; then
if [ -f $BUFFER/$FILE ] || ssh leomed 'sh -c "if [ -f '"'$FINAL_DEST/$FILE'"' ] ; then exit 0; else exit 1; fi"'; then
  echo "$FILE already exists"
  exit 0
fi

# download file to buffer zone
if ! $ASCP -l 1500000 -T -k 1 -q -i $ASCP_KEY anonftp@ftp.ncbi.nlm.nih.gov:$URL $BUFFER; then
  echo "Loading failed: $FILE"
  exit 0
fi

# move downloaded file to final destination
#$LFTP -c "connect -u mikhaika, sftp://leomed; mput -e -E -O $FINAL_DEST $BUFFER/$FILE"
scp $BUFFER/$FILE leomed:$FINAL_DEST && rm $BUFFER/$FILE
