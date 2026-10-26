#!/bin/bash
# Kevin's Orb: check the OpenAI key in src/secrets.h works for everything the Assistant needs:
# which models the account offers, text-to-speech (you will hear it), speech-to-text (it
# transcribes that same audio back) and a short chat answer. Never prints the key.
# Run from the orb-os folder:  bash tools/assistant/check_openai.sh
KEY=$(sed -n 's/^#define OPENAI_API_KEY "\(.*\)"/\1/p' src/secrets.h 2>/dev/null)
if [ -z "$KEY" ] || [ "$KEY" = "paste-your-key-here" ]; then
  echo "No key yet: paste it into src/secrets.h (between the quotes), save, and run this again."; exit 1
fi
echo "Key found (starts ${KEY:0:7}..., ${#KEY} characters)."
OUT=sim/assistant; mkdir -p $OUT
H=(-H "Authorization: Bearer $KEY")

echo; echo "--- 1. Models on this account"
code=$(curl -sS -m 30 "${H[@]}" -o $OUT/models.json -w "%{http_code}" https://api.openai.com/v1/models)
echo "HTTP $code"
if [ "$code" != "200" ]; then python3 -c "import json;print(json.load(open('$OUT/models.json')).get('error',{}).get('message',''))" 2>/dev/null; exit 1; fi
python3 - "$OUT/models.json" <<'PY'
import json,sys
ids=sorted(m["id"] for m in json.load(open(sys.argv[1]))["data"])
print(len(ids),"models. Relevant ones:")
for i in ids:
    if any(k in i for k in ("transcribe","whisper","tts","mini","luna","nano")) and not any(k in i for k in ("realtime","search","audio-preview","embedding","moderation","image")):
        print("  ",i)
PY
pick() { python3 - "$OUT/models.json" "$@" <<'PY'
import json,sys
ids={m["id"] for m in json.load(open(sys.argv[1]))["data"]}
for c in sys.argv[2:]:
    if c in ids: print(c); break
PY
}
TTS=$(pick gpt-4o-mini-tts tts-1)
STT=$(pick gpt-transcribe gpt-4o-mini-transcribe gpt-4o-transcribe whisper-1)
CHAT=$(pick gpt-6-luna gpt-5-mini gpt-5-nano gpt-4.1-mini gpt-4o-mini)
echo "Using: speech=$TTS  transcribe=$STT  chat=$CHAT"

echo; echo "--- 2. Text to speech ($TTS)"
code=$(curl -sS -m 60 "${H[@]}" -H "Content-Type: application/json" -o $OUT/hello.wav -w "%{http_code}" \
  https://api.openai.com/v1/audio/speech \
  -d "{\"model\":\"$TTS\",\"voice\":\"alloy\",\"input\":\"Good afternoon, Doctor Hollar. Your Orb assistant is ready.\",\"response_format\":\"wav\"}")
echo "HTTP $code, $(wc -c < $OUT/hello.wav | tr -d ' ') bytes"
[ "$code" = "200" ] && command -v afplay >/dev/null && afplay $OUT/hello.wav

echo; echo "--- 3. Speech to text ($STT), on that same audio"
code=$(curl -sS -m 60 "${H[@]}" -o $OUT/stt.json -w "%{http_code}" https://api.openai.com/v1/audio/transcriptions \
  -F model="$STT" -F file=@$OUT/hello.wav)
echo "HTTP $code"; python3 -c "import json;d=json.load(open('$OUT/stt.json'));print('  heard:',d.get('text') or d.get('error',{}).get('message'))"

echo; echo "--- 4. Chat ($CHAT)"
code=$(curl -sS -m 60 "${H[@]}" -H "Content-Type: application/json" -o $OUT/chat.json -w "%{http_code}" \
  https://api.openai.com/v1/chat/completions \
  -d "{\"model\":\"$CHAT\",\"messages\":[{\"role\":\"user\",\"content\":\"In one short sentence: what does a molar do?\"}]}")
echo "HTTP $code"; python3 -c "import json;d=json.load(open('$OUT/chat.json'));print('  answer:',(d.get('choices') or [{}])[0].get('message',{}).get('content') or d.get('error',{}).get('message'))"
echo; echo "Done."
