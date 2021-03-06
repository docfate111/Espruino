#!/bin/bash
#
# Extracts and orders symbol names so we can see how big they are
#

if [ $# -eq 0 ]
then
  echo "USAGE:"
  echo "scripts/find_big_rom.sh espruino_XXXX.lst"
  exit 1
fi

grep "^0[08]...... [^<]" $1 |  sort -u -k1,1 | sort --key=4 
