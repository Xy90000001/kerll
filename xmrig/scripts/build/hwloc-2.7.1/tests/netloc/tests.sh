#!/usr/bin/env bash
#
# Copyright © 2016-2017 Inria.  All rights reserved.
# See COPYING in top-level directory.
#

# This script needs a test file as argument
# The syntax of this file is the following
# test1:
#   testset: machine1 machine2 # subfolders where tests be run
#   command: ./test1 param1 # command to test
#   needed: dir1/file*txt dir2 # files needed
#   excluded: dir1/file0.txt dir2/file1 # file to exclude
#   checkfiles: dir2/file1 # files used to check validity
#   checkscript: ./check.sh # script to check validity
# test2:
#   ...

export NETLOC_TESTS_PATH="/home/noob/xmrig/scripts/build/hwloc-2.7.1/tests/netloc"
export NETLOC_BUILD_PATH="/home/noob/xmrig/scripts/build/hwloc-2.7.1/tests/netloc"
export NETLOC_UTIL_PATH="/home/noob/xmrig/scripts/build/hwloc-2.7.1/utils/netloc"

origpwd="$(pwd)"

red="\e[31m"
green="\e[32m"
default="\e[0m"

function write_output
{
    color=$1
    shift
    echo -e $color$@$default
}

function quit
{
    write_output $red "Test failed"
    rm -fr $TEMPDIR
}

function getvalues
{
    for i in $@; do
        echo "$params" | $SED -n "s/^[[:space:]]*$i:[[:space:]]*\(.*\)\$/\1/p"
    done | tr '\n' ' '
}

function getCopies
{
    local t=$1
    local copies=$(getvalues copy copy_${t})
    for f in $copies; do
        local name=$(echo $f | $SED 's/\([^=]*\)\(=.*\)\{0,1\}/\1/')
        local ext=$(echo $f | $SED 's/\([^=]*\)\(=.*\)\{0,1\}/\2/')
        if [ -z "$ext" ]; then
            cp -r "$NETLOC_TESTS_PATH/data/$name" "$REFDIR"
            chmod -R u+w $REFDIR
            cp -r "$NETLOC_BUILD_PATH/data/$name" "$REFDIR"
            chmod -R u+w $REFDIR
        elif [ "$ext" = "=txz" ]; then
            # TODO prevent from extracting it again
            $COMPRESS -d --stdout "$NETLOC_TESTS_PATH/data/$name.txz" > "$REFDIR/$name.tar"
            tar -xf "$REFDIR/$name.tar" -C "$REFDIR"
            rm "$REFDIR/$name.tar"
        fi
    done
}


trap 'quit' 0
set -e

compress=xz
hash $compress 2>/dev/null || \
    { echo >&2 "It requires $compress but it's not installed."; exit 1; }
COMPRESS=$(which $compress)
GREP=$(which grep)
SED=$(which sed)
AWK=$(which awk)

# Folder structure like that
# TEMPDIR
# ├── ref <- copy of the data directory
# │   ├── machine1
# │   │   └── file1
# │   └── machine2
# │       └── file1
# └── test
#     └── file1
TEMPDIR=$(mktemp -d -t netloc_tests_XXXXX)
TESTDIR=$TEMPDIR/tests
REFDIR=$TEMPDIR/ref && mkdir $REFDIR
TESTFILE=$REFDIR/$(basename $1); cp $NETLOC_TESTS_PATH/data/$(basename $1) $REFDIR

# Remove comments from the test file
$SED 's/#.*$//g; /^[[:space:]]*$/d' $TESTFILE > $TESTFILE.new
mv -f $TESTFILE.new $TESTFILE

# Get tests
tests=$($SED -n 's/^\([^[:space:]]*\):[[:space:]]*$/\1/p' $TESTFILE)

S=0
F=0

for t in $tests; do
    params_sv=$($AWK '/^'$t':\s*$/{flag=1;next}; /^\S*:\s*$/{flag=0}; {if (flag) print}' $TESTFILE)
    params="$params_sv"
    sets=$(getvalues testset)

    # For each set to test
    for s in $sets; do
        NETLOC_TEST=$s
        params="$(echo "$params_sv" | $SED 's/%/'$NETLOC_TEST'/g')"
        mkdir $TESTDIR
        cd $TESTDIR

        # Copy or extract files if needed
        getCopies $s

        # Copy needed files
        cd "$REFDIR"
        needed=$(getvalues needed)
        if [ -z "needed" ]; then
            cp -r $s $TESTDIR
        else
            files=$(eval "echo \"$needed\"")
            for f in $files; do
                mkdir -p "$TESTDIR/$(dirname "$f")"
                cp -r "$f" "$TESTDIR/$(dirname "$f")"
            done
        fi

        # Remove excluded files
        cd "$TESTDIR"
        excluded=$(getvalues excluded)
        if [ -n "$excluded" ]; then
            files=$(eval "echo \"$excluded\"")
            for f in $files; do
                rm -f "$f"
            done
        fi

        while true; do
            # Remove output files
            cd $TESTDIR
            for cf in $checkfiles; do
                files=$(eval "echo \"$cf\"")
                for f in $files; do
                    rm -f "$f"
                done
            done

            # Execute the command
            cd $TESTDIR
            cmd=$(getvalues command)
            eval " $cmd" > /dev/null || \
                {
                    eval "echo \"Command $cmd failed\""
                    write_output $red "Test $t on $NETLOC_TEST failed!"
                    failed=1
                    break
                }

            # Check files
            cd "$REFDIR"
            checkfiles=$(getvalues checkfiles)
            for cf in $checkfiles; do
                files=$(eval "echo \"$cf\"")
                for f in $files; do
                    cmp "$f" "$TESTDIR/$f" || \
                        {
                            echo "$f: wrong file"
                            eval "echo \"Command was: $cmd\""
                            write_output $red "Test $t on $NETLOC_TEST failed!"
                            failed=1
                            break
                        }
                done
            done
            if [ "$failed" = 1 ]; then break; fi

            # Check program
            cd $TESTDIR
            checkcmd=$(getvalues checkcommand)
            eval " $checkcmd" > /dev/null || \
                {
                    echo "Check command failed: $checkcmd"
                    eval "echo \"Command was: $cmd\""
                    write_output $red "Test $t on $NETLOC_TEST failed!"
                    failed=1
                    break
                }

            break
        done

	cd "$origpwd"
        rm -fr $TESTDIR
        if [ -n "$failed" ]; then
            ((F+=1))
        else
            ((S+=1))
        fi
        failed=
    done
done

if [[ $F -ne O ]]; then
    color=$red
    returncode=1
else
    color=$green
    returncode=0
fi
echo "### Summary ###"
write_output $color "$F tests failed"
write_output $green "$S tests succeeded"
trap 0
rm -fr $TEMPDIR
exit $returncode
