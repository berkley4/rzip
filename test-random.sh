#! /bin/sh

set -e

# Size to test in MB.
BIG=30

make mkrandom

echo Testing ${BIG}MB of random
./mkrandom $BIG 0 0 > /tmp/rzip.random

# Get starting size
./rzip -k -o /tmp/rzip.random.rz /tmp/rzip.random
BASE_SIZE=`ls -l /tmp/rzip.random.rz | awk '{print $5}'`
rm /tmp/rzip.random /tmp/rzip.random.rz

echo Gain for ${BIG}MB: `expr $BASE_SIZE - $BIG \* 1024 \* 1024`
echo `echo "scale=5; ( $BASE_SIZE - $BIG * 1024 * 1024 ) * 100 / ( $BIG * 1024 * 1024 )" | bc` percent.

for size in 10 100 1000 10000 100000; do
    for dups in 1 4 16; do
	./mkrandom $BIG $size $dups > /tmp/rzip.random
	./rzip -k -o /tmp/rzip.random.rz /tmp/rzip.random
	SIZE=`ls -l /tmp/rzip.random.rz | awk '{print $5}'`
	rm /tmp/rzip.random /tmp/rzip.random.rz
	PERCENT=`echo "scale=5; ( $BASE_SIZE - $SIZE ) * 100 / ( $dups * $size )" | bc`
	echo "Improvement for $dups x $size: `expr $BASE_SIZE - $SIZE` $PERCENT%"
    done
done
	


