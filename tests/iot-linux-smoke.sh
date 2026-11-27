#!/bin/sh
set -eu
binary=$(realpath "$1")
task_dir=$(mktemp -d /tmp/OpenAoIP-IoT-smoke.XXXXXX)
task_pid=''
cleanup() {
  if [ -n "$task_pid" ] && kill -0 "$task_pid" 2>/dev/null; then kill "$task_pid"; wait "$task_pid" || true; fi
}
trap cleanup EXIT HUP INT TERM
# Explicit test-only ALSA PCM. No physical soundcard and no production fallback are involved.
cat > "$task_dir/alsa.conf" <<'EOF'
pcm.!default {
  type null
  hint { show on description "OpenAoIP smoke-test null PCM" }
}
EOF
export ALSA_CONFIG_PATH="$task_dir/alsa.conf"
"$binary" --version
"$binary" --init --config "$task_dir/iot.json"
test "$(stat -c %a "$task_dir/iot.json")" = 600
"$binary" --check-config --config "$task_dir/iot.json"
"$binary" --audio-devices > "$task_dir/devices.json"
jq -e '.backend == "ALSA" and (.playback | length > 0)' "$task_dir/devices.json" >/dev/null
jq '.autoStart = true | .audio.interface = "127.0.0.1" | .audio.controller = "http://127.0.0.1:39999" | .management.bind = "127.0.0.1"' "$task_dir/iot.json" > "$task_dir/next.json"
chmod 600 "$task_dir/next.json"
mv "$task_dir/next.json" "$task_dir/iot.json"
task_key=$(jq -r '.management.token' "$task_dir/iot.json")
"$binary" --config "$task_dir/iot.json" > "$task_dir/service.log" 2>&1 &
task_pid=$!
task_ready=false
for task_attempt in $(seq 1 80); do
  if curl --silent --max-time 1 --fail -H "Authorization: Bearer $task_key" http://127.0.0.1:32110/v1/status > "$task_dir/status.json" &&
     jq -e '.product == "OpenAoIP IoT" and .running and .audioConnected' "$task_dir/status.json" >/dev/null; then
    task_ready=true; break
  fi
  sleep .1
done
if [ "$task_ready" != true ]; then cat "$task_dir/service.log" >&2; exit 1; fi
task_code=$(curl --silent --output /dev/null --write-out '%{http_code}' http://127.0.0.1:32110/v1/config)
test "$task_code" = 403
curl --silent --fail -H "Authorization: Bearer $task_key" -H 'Content-Type: application/json' \
  -d '{"revision":1,"running":false}' http://127.0.0.1:32110/v1/run > "$task_dir/stopped.json"
jq -e '.autoStart == false and .revision == 2' "$task_dir/stopped.json" >/dev/null
jq -e '.autoStart == false and .revision == 2' "$task_dir/iot.json" >/dev/null
echo 'PASS Linux executable, private initialization, actual ALSA backend/null-PCM callbacks, authenticated management and persistent Stop.'
