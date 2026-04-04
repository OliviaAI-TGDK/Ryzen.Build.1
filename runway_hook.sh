#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail

# runway_hook.sh
#
# Dependencies:
#   - bash
#   - curl
#   - python3
#   - file
#
# Env:
#   RUNWAYML_API_SECRET=key_...
#   RUNWAY_API_VERSION=2024-11-06
#   MODEL=gen4.5
#   RATIO=1280:720         # or set 1280:768 if your chosen API version requires it
#   DURATION=5
#   POSITION=first         # first | last
#
# Usage:
#   ./runway_hook.sh /path/to/spool/session-123.bin "A cinematic transformation"
#   ./runway_hook.sh /path/to/image.png "Fog moving through pine trees"
#
# Output files:
#   <input>.runway.input.png      # only when converting a non-image
#   <input>.runway.submit.json
#   <input>.runway.task.json
#   <input>.runway.output.txt

API_KEY="${RUNWAYML_API_SECRET:-}"
API_VERSION="${RUNWAY_API_VERSION:-2024-11-06}"
MODEL="${MODEL:-gen4.5}"
RATIO="${RATIO:-1280:720}"
DURATION="${DURATION:-5}"
POSITION="${POSITION:-first}"

if [[ -z "$API_KEY" ]]; then
  echo "RUNWAYML_API_SECRET is not set" >&2
  exit 1
fi

if [[ $# -lt 2 ]]; then
  echo "usage: $0 INPUT_FILE \"PROMPT TEXT\"" >&2
  exit 1
fi

INPUT_FILE="$1"
PROMPT_TEXT="$2"

if [[ ! -f "$INPUT_FILE" ]]; then
  echo "input file not found: $INPUT_FILE" >&2
  exit 1
fi

MIME="$(file -b --mime-type "$INPUT_FILE" || true)"
BASENAME="$(basename "$INPUT_FILE")"
WORKDIR="$(dirname "$INPUT_FILE")"

PNG_INPUT="$INPUT_FILE.runway.input.png"
SUBMIT_JSON="$INPUT_FILE.runway.submit.json"
TASK_JSON="$INPUT_FILE.runway.task.json"
OUTPUT_TXT="$INPUT_FILE.runway.output.txt"

json_escape() {
  python3 - <<'PY' "$1"
import json, sys
print(json.dumps(sys.argv[1]))
PY
}

make_png_from_binary() {
  local src="$1"
  local out="$2"
  python3 - <<'PY' "$src" "$out"
import math, os, struct, sys, zlib

src, out = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()

# deterministic grayscale square-ish image
n = len(data)
if n == 0:
    data = b"\x00"
    n = 1

width = 256
height = (n + width - 1) // width
rows = []
for y in range(height):
    row = data[y*width:(y+1)*width]
    if len(row) < width:
        row += b"\x00" * (width - len(row))
    rows.append(b"\x00" + row)  # PNG filter type 0

raw = b"".join(rows)

def chunk(tag, payload):
    return (
        struct.pack(">I", len(payload)) +
        tag +
        payload +
        struct.pack(">I", zlib.crc32(tag + payload) & 0xffffffff)
    )

png = b"\x89PNG\r\n\x1a\n"
png += chunk(b'IHDR', struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
png += chunk(b'IDAT', zlib.compress(raw, 9))
png += chunk(b'IEND', b'')

with open(out, "wb") as f:
    f.write(png)
PY
}

START_FILE="$INPUT_FILE"

case "$MIME" in
  image/png|image/jpeg|image/jpg|image/webp)
    START_FILE="$INPUT_FILE"
    ;;
  *)
    make_png_from_binary "$INPUT_FILE" "$PNG_INPUT"
    START_FILE="$PNG_INPUT"
    ;;
esac

UPLOAD_META="$(curl -sS \
  -X POST "https://api.dev.runwayml.com/v1/uploads" \
  -H "Authorization: Bearer ${API_KEY}" \
  -H "Content-Type: application/json" \
  -H "X-Runway-Version: ${API_VERSION}" \
  --data "{\"filename\":\"$(basename "$START_FILE")\",\"type\":\"ephemeral\"}")"

UPLOAD_URL="$(python3 - <<'PY' "$UPLOAD_META"
import json, sys
obj = json.loads(sys.argv[1])
print(obj["uploadUrl"])
PY
)"

RUNWAY_URI="$(python3 - <<'PY' "$UPLOAD_META"
import json, sys
obj = json.loads(sys.argv[1])
print(obj["runwayUri"])
PY
)"

FIELDS_JSON="$(python3 - <<'PY' "$UPLOAD_META"
import json, sys
obj = json.loads(sys.argv[1])
print(json.dumps(obj["fields"]))
PY
)"

python3 - <<'PY' "$UPLOAD_URL" "$FIELDS_JSON" "$START_FILE"
import json, os, subprocess, sys, tempfile

upload_url, fields_json, file_path = sys.argv[1], sys.argv[2], sys.argv[3]
fields = json.loads(fields_json)

cmd = ["curl", "-sS", "-X", "POST", upload_url]
for k, v in fields.items():
    cmd += ["-F", f"{k}={v}"]
cmd += ["-F", f"file=@{file_path}"]
subprocess.check_call(cmd)
PY

PROMPT_JSON="$(json_escape "$PROMPT_TEXT")"

SUBMIT_BODY="$(python3 - <<'PY' "$MODEL" "$RUNWAY_URI" "$POSITION" "$PROMPT_TEXT" "$RATIO" "$DURATION"
import json, sys
model, uri, position, prompt, ratio, duration = sys.argv[1:]
body = {
    "model": model,
    "promptImage": [{"uri": uri, "position": position}],
    "promptText": prompt,
    "ratio": ratio,
    "duration": int(duration),
}
print(json.dumps(body))
PY
)"

curl -sS \
  -X POST "https://api.dev.runwayml.com/v1/image_to_video" \
  -H "Authorization: Bearer ${API_KEY}" \
  -H "Content-Type: application/json" \
  -H "X-Runway-Version: ${API_VERSION}" \
  --data "$SUBMIT_BODY" | tee "$SUBMIT_JSON" >/dev/null

TASK_ID="$(python3 - <<'PY' "$SUBMIT_JSON"
import json, sys
obj = json.load(open(sys.argv[1], "r"))
print(obj["id"])
PY
)"

echo "task_id=$TASK_ID"

while true; do
  curl -sS \
    -X GET "https://api.dev.runwayml.com/v1/tasks/${TASK_ID}" \
    -H "Authorization: Bearer ${API_KEY}" \
    -H "X-Runway-Version: ${API_VERSION}" | tee "$TASK_JSON" >/dev/null

  STATUS="$(python3 - <<'PY' "$TASK_JSON"
import json, sys
obj = json.load(open(sys.argv[1], "r"))
print(obj.get("status", "UNKNOWN"))
PY
)"

  echo "status=$STATUS"

  if [[ "$STATUS" == "SUCCEEDED" ]]; then
    python3 - <<'PY' "$TASK_JSON" "$OUTPUT_TXT"
import json, sys
obj = json.load(open(sys.argv[1], "r"))
out = obj.get("output", [])
with open(sys.argv[2], "w") as f:
    if out:
        for item in out:
            f.write(str(item) + "\n")
    else:
        f.write(json.dumps(obj))
PY
    echo "output_file=$OUTPUT_TXT"
    break
  fi

  if [[ "$STATUS" == "FAILED" || "$STATUS" == "CANCELED" ]]; then
    echo "task failed; see $TASK_JSON" >&2
    exit 1
  fi

  sleep 5
done
