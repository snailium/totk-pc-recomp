#!/bin/sh
# pick a live totk pid
for p in $(pgrep -x totk); do
  st=$(awk '/^State:/{print $2}' /proc/$p/status 2>/dev/null)
  [ "$st" = "Z" ] && continue
  LAST=$p
done
echo "$LAST"
