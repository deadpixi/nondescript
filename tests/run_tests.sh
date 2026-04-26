#!/bin/sh

rm -f runner
cc -o runner runner.c ../nondescript.c
./runner *.nds

echo
echo "Testing en_US locale for numeric parsing..."
LC_NUMERIC=en_US ./runner locale.nds

echo
echo "Testing fr_FR locale for numeric parsing..."
LC_NUMERIC=fr_FR ./runner locale.nds

echo
echo "Running should-fail tests..."
ok=0
unexpected_pass=0
crashed=0
for f in should_fail/*.nds; do
    ./runner "$f" >/dev/null 2>&1
    rc=$?
    if [ $rc -eq 0 ]; then
        echo "  UNEXPECTED PASS: $f"
        unexpected_pass=$((unexpected_pass + 1))
    elif [ $rc -gt 128 ]; then
        echo "  CRASH (signal $((rc - 128))): $f"
        crashed=$((crashed + 1))
    else
        ok=$((ok + 1))
    fi
done
total=$((ok + unexpected_pass + crashed))
echo "  $ok/$total cleanly failed; $unexpected_pass unexpected passes; $crashed crashes"

if [ $unexpected_pass -gt 0 ] || [ $crashed -gt 0 ]; then
    exit 1
fi
