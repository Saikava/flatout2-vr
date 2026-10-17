#!/bin/bash
# bench.sh <name> Key=Value...: set the ini, run the replay benchmark, keep the log and shots under fo2vr/build/bench/<name>
T="$(cd "$(dirname "$0")" && pwd)"   # these tools
B="$T/../../build/bench"   # the benchmarks' logs and shots (not in git)
G="C:/GOG Games/FlatOut 2/scripts"
name=$1; shift
python "$T/setini.py" "$@"
before=$(ls "$G/shots" | wc -l)
powershell -NoProfile -ExecutionPolicy Bypass -File "$T/runreplay.ps1" > /dev/null
mkdir -p "$B/$name"
cp "$G/FO2VR.log" "$B/$name/run.log"
ls -t "$G/shots" | head -n $(( $(ls "$G/shots" | wc -l) - before )) | while read f; do mv "$G/shots/$f" "$B/$name/"; done
echo "== $name: $*"
grep -E "level of detail|grass |benchmark" "$B/$name/run.log" | sed 's/^[0-9:.]* *//' | sort -u
grep "GPU: eyes" "$B/$name/run.log" | tail -3 | sed 's/.*GPU/GPU/'
