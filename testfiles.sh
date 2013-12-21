#!/bin/sh

tdir=/tmp/rzip.$$

mkdir $tdir || exit 1

failed() {
    f=$1
    echo "Failed on $f"
    exit 1
}

ratio() {
    echo $2 $1 | awk '{printf "%5.2f\n", $2/$1}'
}

ts1=0
ts2=0

for f in $*; do
    if [ ! -r $f -o ! -f $f ]; then
	continue;
    fi
    bname=`basename $f`
    ./rzip -k $f -o $tdir/$bname || failed $f
    s1=`stat -L -c '%s' $f`
    s2=`stat -L -c '%s' $tdir/$bname`
    r=`ratio $s1 $s2`
    echo $f $s1 $s2 $r
    ./rzip -k -d $tdir/$bname -o $tdir/$bname.2 || failed $f
    if ! cmp $f $tdir/$bname.2; then
	echo "Failed on $f!!"
	exit 1
    fi
    ts1=`expr $ts1 + $s1`
    ts2=`expr $ts2 + $s2`
    rm -f $tdir/$bname.2 $tdir/$bname
done

echo ALL OK
r=`ratio $ts1 $ts2`
echo $ts1 $ts2 $r
rm -rf $tdir
