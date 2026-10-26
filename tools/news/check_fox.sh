#!/bin/bash
# Kevin's Orb: fetch Fox News's latest-headlines feed once, report its size, keep a copy.
# Run from the orb-os folder:  bash tools/news/check_fox.sh
URL="https://moxie.foxnews.com/google-publisher/latest.xml"
mkdir -p sim/news
curl -sS -m 30 -A "KevinOrb/1.0" -D sim/news/fox_headers.txt -o sim/news/fox_latest.xml \
     -w "HTTP %{http_code}, %{size_download} bytes, %{time_total} s\n" "$URL"
grep -i -E "^(content-length|transfer-encoding|content-type|content-encoding):" sim/news/fox_headers.txt
echo "items: $(grep -o '<item>' sim/news/fox_latest.xml | wc -l)"
