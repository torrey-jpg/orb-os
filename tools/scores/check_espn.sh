#!/bin/bash
# Kevin's Orb: which User-Agent (the name a request introduces itself with) does ESPN accept?
# Run from the orb-os folder:  bash tools/scores/check_espn.sh
URL="https://site.api.espn.com/apis/site/v2/sports/football/nfl/scoreboard"
try() {
  printf "%-48s " "$1"
  if [ "$1" = "(none)" ]; then
    curl -sS -m 20 -H "User-Agent:" -o /dev/null -w "HTTP %{http_code}\n" "$URL"
  else
    curl -sS -m 20 -A "$1" -o /dev/null -w "HTTP %{http_code}\n" "$URL"
  fi
}
try "TheOrbOS (ESP32-S3 hobby; +https://zionbrock.com/orb)"
try "ESP32HTTPClient"
try "KevinOrb/1.0"
try "Mozilla/5.0 (compatible; KevinOrb/1.0)"
try "(none)"
