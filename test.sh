#!/bin/bash
# dsynco tester

TESTS=(diff sync err dryrun)

if test "$#" -lt 1 ; then
  CMDS=( "${TESTS[@]}" )
else
  CMDS=( "$@" )
fi

set -xe

test_init() {
  (
    cd dstest

    mkdir -p left/d right/d

    echo left >left/l
    echo right >right/r

    echo eq >left/d/eq
    cp -a left/d/eq right/d/eq

    echo moved >left/d/mv
    cp -a left/d/mv right/mv

    echo renamed >left/d/renamed
    cp -a left/d/renamed right/renamed2

    echo leftmod >left/d/mod
    sleep .1
    echo rightmod >right/d/mod

    echo mod_date >left/d/mod_date
    echo mod_date >right/d/mod_date
    touch -t 200101010000 "right/d/mod_date"
  )
}

test_diff() {
  test_init
  (
    cd dstest

    ../dsynco -v diff "left" -o "right" | tee LOG
    grep 'diff status: add:2  moved:1  upd:2  del:2  eq:2  total:7/7' LOG
    # Note: 'renamed' & 'renamed2' are counted as add/del

    ../dsynco -v diff "left" -o "right" -a
    ../dsynco -v diff "left" -o "right" -u
    ../dsynco -v diff "left" -o "right" -m
    ../dsynco -v diff "left" -o "right" -d
  )
}

test_sync() {
  test_init
  (
    cd dstest

    ../dsynco -v sync "left" -o "right" -a | tee LOG
    grep 'Result: added:2  moved:0  replaced:0  deleted:0  err:0' LOG

    ../dsynco -v sync "left" -o "right" -d | tee LOG
    grep 'Result: added:0  moved:0  replaced:0  deleted:2  err:0' LOG

    ../dsynco -v sync "left" -o "right" -u | tee LOG
    grep 'Result: added:0  moved:0  replaced:2  deleted:0  err:0' LOG

    ../dsynco -v sync "left" -o "right" -m | tee LOG
    grep 'Result: added:0  moved:1  replaced:0  deleted:0  err:0' LOG
  )
}

test_dryrun() {
  test_init
  (
    cd dstest

    ../dsynco -v sync "left" -o "right" -a -test | tee LOG
    grep 'Result: added:2  moved:0  replaced:0  deleted:0  err:0' LOG
    test ! -f right/l  # left/l was NOT copied

    ../dsynco -v sync "left" -o "right" -m -test | tee LOG
    grep 'Result: added:0  moved:1  replaced:0  deleted:0  err:0' LOG
    test -f right/mv      # source still exists
    test ! -f right/d/mv  # destination was not created

    ../dsynco -v sync "left" -o "right" -d -test | tee LOG
    grep 'Result: added:0  moved:0  replaced:0  deleted:2  err:0' LOG
    test -f right/r
    test -f right/renamed2
  )
}

test_err() {
  (
    cd dstest

    mkdir -p left
    echo left >left/l
    touch right # output is a regular file -> copy fails (ENOTDIR)

    ../dsynco -v sync "left" -o "right" -a | tee LOG
    grep 'Result: added:0  moved:0  replaced:0  deleted:0  err:1' LOG

    # a failed sync must exit non-zero
    if ../dsynco sync "left" -o "right" -a >/dev/null 2>&1 ; then
      echo "FAIL: expected non-zero exit on error"
      exit 1
    fi
  )
}

test_gui() {
  test_init
  (
    cd dstest
    ../dsynco-gui "left" "right"
  )
}

mkdir -p dstest

for cmd in "${CMDS[@]}" ; do
  rm -rf ./dstest/*
  test_$cmd
done

rm -rf ./dstest
echo DONE
