#!/usr/bin/env bash
# map-keys.sh - map the Cardputer's TCA8418 key numbers, one prompt per key.
#
# It drives the board's own `kbd` console command over USB serial and reads the
# {"key": N, "down": true} lines it prints. No packages needed (stty/cat/grep,
# all coreutils) - the "kbd" here is the firmware command, NOT the Linux kbd pkg.
#
# FIRST close any serial monitor you have open on the port (idf monitor / screen
# / your console) so this script can own it. Then:  bash map-keys.sh
# Press each key as it's asked. Ctrl-C any time; progress is saved as you go.
set -u

PORT="${1:-/dev/ttyACM0}"
OUT="${2:-$(dirname "$0")/cardputer-keymap.txt}"

# The sweep order. Edit freely; press whatever's on your keycaps when asked.
# Ctrl-C to stop early. A key that doesn't exist on your board: just skip it by
# pressing a real one twice - or stop, delete the line, and rerun.
KEYS=(
  "ESC" "BACKTICK  \`" "1" "2" "3" "4" "5" "6" "7" "8" "9" "0" "MINUS  -" "EQUALS  =" "BACKSPACE"
  "TAB" "Q" "W" "E" "R" "T" "Y" "U" "I" "O" "P" "LBRACKET  [" "RBRACKET  ]" "BACKSLASH  \\"
  "FN" "SHIFT" "A" "S" "D" "F" "G" "H" "J" "K" "L" "SEMICOLON  ;" "APOSTROPHE  '" "ENTER"
  "CTRL" "OPT" "ALT" "Z" "X" "C" "V" "B" "N" "M" "COMMA  ," "PERIOD  ." "SLASH  /" "SPACE"
  "LEFT ARROW" "RIGHT ARROW" "UP ARROW" "DOWN ARROW"
)

[ -e "$PORT" ] || { echo "No $PORT - is the board plugged in?"; exit 1; }

# Raw mode; -hupcl so opening/closing can't bounce the board (S3 native USB anyway).
stty -F "$PORT" 115200 raw -echo -hupcl 2>/dev/null \
  || { echo "Can't configure $PORT. A monitor still holding it? Close it and retry."; exit 1; }

exec 3<"$PORT" || { echo "Can't read $PORT (busy?). Close any serial monitor."; exit 1; }
trap 'exec 3<&- 2>/dev/null; echo; echo "Saved -> $OUT"' EXIT

echo "kbd 1800" > "$PORT"        # 30 min of dump; plenty for a full sweep
sleep 0.6                         # let it start + print its ack

# Drain whatever's buffered (boot logs, the kbd ack) so a stale press isn't caught.
while read -t 0.2 -r _ <&3; do :; done

re='"key":[[:space:]]*([0-9]+),[[:space:]]*"down":[[:space:]]*true'
WINDOW_MS="${WINDOW_MS:-700}"     # ms to gather a combo (e.g. Ctrl+key) after the first press

capture() {                       # first key-DOWN, then any more within the window; echo "a+b"
  local line nums=() now end rem rem_ms seen=" " out="" n
  while IFS= read -r line <&3; do                      # block for the first press
    [[ "$line" =~ $re ]] && { nums+=("${BASH_REMATCH[1]}"); break; }
  done
  end=$(( $(date +%s%N) + WINDOW_MS * 1000000 ))       # keep listening this long for a 2nd key
  while now=$(date +%s%N); (( now < end )); do
    rem_ms=$(( (end - now) / 1000000 )); (( rem_ms < 1 )) && rem_ms=1
    printf -v rem '%d.%03d' $(( rem_ms / 1000 )) $(( rem_ms % 1000 ))
    IFS= read -t "$rem" -r line <&3 && { [[ "$line" =~ $re ]] && nums+=("${BASH_REMATCH[1]}"); }
  done
  for n in "${nums[@]}"; do                             # dedup, keep press order, join with +
    [[ "$seen" == *" $n "* ]] && continue
    seen+="$n "; out+="${out:++}$n"
  done
  echo "$out"
}

printf '# Cardputer TCA8418 keymap  %s\n# number(s)  key    (a+b = modifier combo, e.g. Ctrl+key)\n' "$(date)" > "$OUT"
echo "Ready. Press each key when asked. If a symbol needs a modifier (Ctrl for the"
echo "backtick, etc.), just press the combo - both numbers get recorded. Ctrl-C to stop."
echo "(Combo window is ${WINDOW_MS}ms; run with WINDOW_MS=1000 for more time.)"
echo
for label in "${KEYS[@]}"; do
  printf '  press %-18s ... ' "$label"
  num="$(capture)"
  printf '%s\n' "${num:-?}"
  printf '%-9s %s\n' "${num:-?}" "$label" >> "$OUT"
done
echo
echo "Done - full sweep captured."
