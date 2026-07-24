# Exercise Bash arithmetic evaluation without external process or I/O noise.
i=0
total=0
while [ "$i" -lt 250000 ]; do
	total=$((total + i))
	i=$((i + 1))
done
[ "$total" -eq 31249875000 ]
