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

echo
echo "Running REPL tests..."
make -C .. clean nds
ok=0
fail=0

repl_check() {
    name=$1
    input=$2
    expect=$3
    output=$(printf "%b" "$input" | ../nds 2>&1)
    if echo "$output" | grep -qF "$expect"; then
        ok=$((ok + 1))
    else
        echo "  FAIL: $name"
        echo "    expected to find: $expect"
        echo "    got: $output"
        fail=$((fail + 1))
    fi
}

repl_check "single statement" "println(42)\n" "42"
repl_check "two statements share globals" "set x to 5\nprintln(x * 2)\n" "10"
repl_check "multi-line function continues" "function f(n)\nreturn n + 1\nend function\nprintln(f(7))\n" "8"
repl_check "multi-line list literal" "set xs to [1,\n2,\n3]\nprintln(xs)\n" "[1, 2, 3]"
repl_check "real syntax error reported" "set x to + 1\n" "expected expression"
repl_check "REPL recovers from error" "set x to + 1\nprintln(99)\n" "99"
repl_check "EOF inside open block does not crash" "function f()\nreturn 1\n" ">>>"

total=$((ok + fail))
echo "  $ok/$total REPL tests passed"
if [ $fail -gt 0 ]; then
    exit 1
fi
