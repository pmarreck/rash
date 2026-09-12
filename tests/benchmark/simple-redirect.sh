# Builtin redirect apply + undo, no external process.
# complexity: O(n) over loop count; each iteration is one simple command with `>`.
f=${TMPDIR:-/tmp}/rash-bm-redir
i=0
while [ "$i" -lt 20000 ]; do
	: > "$f"
	i=$((i + 1))
done
[ -f "$f" ]
