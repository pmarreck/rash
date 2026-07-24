# Exercise shell function lookup and invocation without external process or I/O noise.
increment() {
	value=$((value + 3))
}

i=0
value=0
while [ "$i" -lt 250000 ]; do
	increment
	i=$((i + 1))
done
[ "$value" -eq 750000 ]
