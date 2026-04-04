#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail

# eleven_hook.sh
#
# Dependencies:
#   - bash
#   - curl
#   - python3
#   - file
#
# Env:
#   ELEVENLABS_API_KEY=...
#   ELEVEN_MODEL_ID=eleven_v3
#   ELEVEN_VOICE_ID=JBFqnCBsd6RMkjVDRZzb
#   ELEVEN_OUTPUT_FORMAT=mp3_44100_128
#
# Usage:
#   ./eleven_hook.sh --input ./spool/session-123.txt
#   ./eleven_hook.sh --input ./spool/session-123.bin --prompt "Signal recovered. Relay stable."
#   ./eleven_hook.sh --list-voices "George"
#
# Output:
#   <input>.eleven.mp3
#   <input>.eleven.meta.json

API_KEY="${ELEVENLABS_API_KEY:-}"
MODEL_ID="${ELEVEN_MODEL_ID:-eleven_v3}"
VOICE_ID="${ELEVEN_VOICE_ID:-JBFqnCBsd6RMkjVDRZzb}"
OUTPUT_FORMAT="${ELEVEN_OUTPUT_FORMAT:-mp3_44100_128}"

INPUT_FILE=""
PROMPT_TEXT=""
LIST_VOICES=""
OUTPUT_FILE=""
META_FILE=""

if [[ -z "$API_KEY" ]]; then
  echo "ELEVENLABS_API_KEY is not set" >&2
  exit 1
fi

json_escape() {
  python3 - <<'PY' "$1"
import json, sys
print(json.dumps(sys.argv[1]))
PY
}

looks_like_text() {
  local path="$1"
  local mime
  mime="$(file -b --mime-type "$path" || true)"
  case "$mime" in
    text/*|application/json|application/xml|application/javascript|application/x-sh|inode/x-empty)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

usage() {
  cat >&2 <<'EOF'
usage:
  eleven_hook.sh --input FILE [--prompt "TEXT"] [--voice-id ID] [--model-id ID] [--output FILE]
  eleven_hook.sh --list-voices [SEARCH]

examples:
  eleven_hook.sh --input ./spool/session-123.txt
  eleven_hook.sh --input ./spool/session-123.bin --prompt "Signal recovered. Relay stable."
  eleven_hook.sh --list-voices George
EOF
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --input)
      shift
      [[ $# -gt 0 ]] || usage
      INPUT_FILE="$1"
      ;;
    --prompt)
      shift
      [[ $# -gt 0 ]] || usage
      PROMPT_TEXT="$1"
      ;;
    --voice-id)
      shift
      [[ $# -gt 0 ]] || usage
      VOICE_ID="$1"
      ;;
    --model-id)
      shift
      [[ $# -gt 0 ]] || usage
      MODEL_ID="$1"
      ;;
    --output)
      shift
      [[ $# -gt 0 ]] || usage
      OUTPUT_FILE="$1"
      ;;
    --list-voices)
      shift
      if [[ $# -gt 0 && "${1#--}" == "$1" ]]; then
        LIST_VOICES="$1"
      else
        LIST_VOICES=""
        set -- "$@"
      fi
      ;;
    *)
      usage
      ;;
  esac
  shift || true
done

if [[ -n "$LIST_VOICES" || "${1:-}" == "--list-voices" ]]; then
  SEARCH_Q="${LIST_VOICES:-}"
  if [[ -n "$SEARCH_Q" ]]; then
    SEARCH_ENC="$(python3 - <<'PY' "$SEARCH_Q"
import sys, urllib.parse
print(urllib.parse.quote(sys.argv[1]))
PY
)"
    curl -sS "https://api.elevenlabs.io/v1/voices/search?page_size=10&search=${SEARCH_ENC}" \
      -H "xi-api-key: ${API_KEY}" \
      -H "Content-Type: application/json"
  else
    curl -sS "https://api.elevenlabs.io/v1/voices/search?page_size=10" \
      -H "xi-api-key: ${API_KEY}" \
      -H "Content-Type: application/json"
  fi
  exit 0
fi

[[ -n "$INPUT_FILE" ]] || usage
[[ -f "$INPUT_FILE" ]] || { echo "input file not found: $INPUT_FILE" >&2; exit 1; }

if [[ -z "$OUTPUT_FILE" ]]; then
  OUTPUT_FILE="${INPUT_FILE}.eleven.mp3"
fi
META_FILE="${INPUT_FILE}.eleven.meta.json"

TEXT_PAYLOAD=""
if looks_like_text "$INPUT_FILE"; then
  TEXT_PAYLOAD="$(cat "$INPUT_FILE")"
elif [[ -n "$PROMPT_TEXT" ]]; then
  TEXT_PAYLOAD="$PROMPT_TEXT"
else
  echo "input is not text; provide --prompt for ElevenLabs TTS" >&2
  exit 1
fi

BODY="$(python3 - <<'PY' "$TEXT_PAYLOAD" "$MODEL_ID"
import json, sys
text, model_id = sys.argv[1], sys.argv[2]
print(json.dumps({
    "text": text,
    "model_id": model_id
}))
PY
)"

curl -sS -X POST \
  "https://api.elevenlabs.io/v1/text-to-speech/${VOICE_ID}?output_format=${OUTPUT_FORMAT}" \
  -H "xi-api-key: ${API_KEY}" \
  -H "Content-Type: application/json" \
  --data "$BODY" \
  -o "$OUTPUT_FILE"

python3 - <<'PY' "$INPUT_FILE" "$OUTPUT_FILE" "$VOICE_ID" "$MODEL_ID" "$OUTPUT_FORMAT"
import json, os, sys, time
inp, outp, voice, model, fmt = sys.argv[1:]
meta = {
    "schema": "eleven-hook/v1",
    "input_file": inp,
    "output_file": outp,
    "voice_id": voice,
    "model_id": model,
    "output_format": fmt,
    "output_size_bytes": os.path.getsize(outp) if os.path.exists(outp) else 0,
    "timestamp_unix": int(time.time()),
}
print(json.dumps(meta, indent=2))
PY | tee "$META_FILE" >/dev/null

echo "audio_file=$OUTPUT_FILE"
echo "meta_file=$META_FILE"
