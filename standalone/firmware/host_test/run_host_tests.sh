#!/bin/sh
set -e
cd "$(dirname "$0")"
CJSON="${IDF_PATH:?set IDF_PATH to your ESP-IDF folder}/components/json/cJSON"
gcc -std=c11 -Wall -Wextra -Werror -I../main -I"$CJSON" \
    test_host.c ../main/protocol.c ../main/formutil.c ../main/assist_util.c "$CJSON/cJSON.c" -lm -o /tmp/sphere_standalone_host_test
/tmp/sphere_standalone_host_test
