#!/usr/bin/env bash
# Integration test for cloud saves against real RomM and Hoard containers.
# Needs Docker and a JDK 11+ on PATH. Uses ports 18080 (RomM) and 12421 (Hoard)
# and removes the containers and their volumes afterwards.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/../../android/app/src/main/java/com/ylports/mother3recomp/cloud"
WORK="$(mktemp -d)"
trap 'docker compose -f "$HERE/compose.yml" --project-directory "$WORK" down -v >/dev/null 2>&1; rm -rf "$WORK"' EXIT

mkdir -p "$WORK/romm/library/roms/gba" "$WORK/romm/assets" "$WORK/romm/config" "$WORK/hoard-config"
head -c 1048576 /dev/urandom > "$WORK/romm/library/roms/gba/Mother3Test.gba"
(shasum -a 1 "$WORK/romm/library/roms/gba/Mother3Test.gba" 2>/dev/null ||
 sha1sum "$WORK/romm/library/roms/gba/Mother3Test.gba") | cut -d' ' -f1 > "$WORK/rom.sha1"

export ROMM_AUTH_SECRET_KEY="$(openssl rand -hex 32)"
docker compose -f "$HERE/compose.yml" --project-directory "$WORK" up -d
for _ in $(seq 1 100); do
  r=$(curl -s -o /dev/null -w "%{http_code}" http://localhost:18080/api/heartbeat || true)
  h=$(curl -s -o /dev/null -w "%{http_code}" http://localhost:12421/v1/health || true)
  [[ "$r" == 200 && "$h" == 200 ]] && break
  sleep 3
done

# RomM: first admin (CSRF cookie needed for the anonymous call), then a scan.
curl -s -c "$WORK/cj" http://localhost:18080/api/heartbeat >/dev/null
CSRF=$(awk '$6=="romm_csrftoken"{print $7}' "$WORK/cj")
curl -sf -b "$WORK/cj" -X POST http://localhost:18080/api/users -H "x-csrftoken: $CSRF" \
  -H 'Content-Type: application/json' \
  -d '{"username":"admin","email":"admin@example.com","password":"romm-test-pw-123","role":"admin"}' >/dev/null
curl -sf -u admin:romm-test-pw-123 -X POST http://localhost:18080/api/tasks/scan \
  -H 'Content-Type: application/json' -d '{}' >/dev/null

ZSTD=1.5.7-12
curl -sfL "https://repo1.maven.org/maven2/org/json/json/20250517/json-20250517.jar" -o "$WORK/json.jar"
curl -sfL "https://repo1.maven.org/maven2/com/github/luben/zstd-jni/$ZSTD/zstd-jni-$ZSTD.jar" -o "$WORK/zstd.jar"
mkdir "$WORK/out"
javac -d "$WORK/out" -cp "$WORK/json.jar:$WORK/zstd.jar" "$SRC/CloudHttp.java" "$SRC/CloudProvider.java" \
  "$SRC/RommProvider.java" "$SRC/HoardProvider.java" "$SRC/SaveSync.java" "$HERE/CloudIT.java"
java -cp "$WORK/out:$WORK/json.jar:$WORK/zstd.jar" com.ylports.mother3recomp.cloud.CloudIT "$WORK/rom.sha1"
