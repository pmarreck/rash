# Last-N average window for Rash benchmark logs.
# Sourced by ./bm and tests/cli/bm. Keep BM_WINDOW_PCT / BM_WINDOW_N overridable.

: "${BM_WINDOW_PCT:=15}"
: "${BM_WINDOW_N:=3}"

# Print the average of FIELD over the last BM_WINDOW_N matching rows, or nothing.
bm_avg_last()
{
	local jsonl name machine field
	jsonl=$1
	name=$2
	machine=$3
	field=$4

	if [ ! -s "$jsonl" ]; then
		return 0
	fi
	jq -sr --arg name "$name" --arg machine "$machine" --arg field "$field" --argjson n "$BM_WINDOW_N" '
		map(select(.name == $name and .machine == $machine and (.[$field] | type == "number")))
		| .[-$n:]
		| if length == 0 then empty
		  else (map(.[$field]) | add / length)
		  end
	' "$jsonl"
}

# Percent change from OLD to NEW. Fails the awk process if OLD is 0.
bm_pct_change()
{
	awk -v old="$1" -v new="$2" 'BEGIN {
		if (old + 0 == 0) exit 1
		printf "%.4f", ((new - old) / old) * 100
	}'
}

# Classify a percent change against BM_WINDOW_PCT.
bm_window_class()
{
	awk -v c="$1" -v lim="$BM_WINDOW_PCT" 'BEGIN {
		if (c >= lim) print "regression"
		else if (c <= -lim) print "improvement"
		else print "stable"
	}'
}
