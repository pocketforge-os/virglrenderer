#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu

case_name=${1:-happy}
case "$case_name" in
   happy|happy-unaligned|implicit-unaligned|foreign|wrong-size|\
      wrong-size-unaligned|wrong-offset|wrong-memory-type) ;;
   *) echo "unknown test case: $case_name" >&2; exit 2 ;;
esac

: "${HOST_BUILD:?set HOST_BUILD to the virglrenderer build directory}"
: "${GUEST_ICD:?set GUEST_ICD to virtio_devenv_icd.x86_64.json}"
: "${HOST_ICD:?set HOST_ICD to lvp_icd.x86_64.json}"
: "${HOST_LIBDIR:?set HOST_LIBDIR to the pinned host library directory}"
: "${LOG_DIR:?set LOG_DIR to the output log directory}"
: "${VN_ALLOC_MODE:?set VN_ALLOC_MODE to sync or async}"
case "$VN_ALLOC_MODE" in
   sync) vn_perf=no_async_mem_alloc ;;
   async) vn_perf= ;;
   *) echo "unknown allocation mode: $VN_ALLOC_MODE" >&2; exit 2 ;;
esac

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
server=$HOST_BUILD/vtest/virgl_test_server
render_server=$HOST_BUILD/server/virgl_render_server
client=$HOST_BUILD/tests/venus_vtest_memory
prefix=$LOG_DIR/venus-vtest-memory-$case_name
run_dir=$(mktemp -d)
socket=$run_dir/vtest.sock
server_pid=

cleanup()
{
   if [ -n "$server_pid" ] && kill -0 "$server_pid" 2>/dev/null; then
      kill "$server_pid" 2>/dev/null || true
      wait "$server_pid" 2>/dev/null || true
   fi
   rm -rf -- "$run_dir"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$LOG_DIR" "$(dirname -- "$client")"
cc -std=c11 -Wall -Wextra -Werror "$script_dir/venus_vtest_memory.c" \
   -o "$client" $(pkg-config --cflags --libs vulkan)

if [ ! -x "$server" ] || [ ! -x "$render_server" ]; then
   echo "host vtest or render server is missing" >&2
   exit 2
fi
if [ ! -r "$GUEST_ICD" ] || [ ! -r "$HOST_ICD" ]; then
   echo "guest or host ICD is missing" >&2
   exit 2
fi

multi_client=
if [ "$case_name" = foreign ]; then
   multi_client=--multi-clients
fi

env RENDER_SERVER_EXEC_PATH="$render_server" \
   VK_DRIVER_FILES="$HOST_ICD" \
   LD_LIBRARY_PATH="$HOST_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
   "$server" --no-virgl --venus --no-loop-or-fork $multi_client \
   --socket-path "$socket" >"$prefix.server.log" 2>&1 &
server_pid=$!

ready=0
for unused in $(seq 1 100); do
   if [ -S "$socket" ]; then
      ready=1
      break
   fi
   if ! kill -0 "$server_pid" 2>/dev/null; then
      break
   fi
   sleep 0.05
done
if [ "$ready" -ne 1 ]; then
   wait "$server_pid" || server_status=$?
   server_status=${server_status:-0}
   {
      echo "case=$case_name"
      echo "allocation_mode=$VN_ALLOC_MODE"
      echo "setup=server-not-ready"
      echo "server_exit=$server_status"
   } >"$prefix.summary.log"
   cat "$prefix.summary.log"
   exit 125
fi

set +e
timeout 20s env VK_DRIVER_FILES="$GUEST_ICD" VN_DEBUG=vtest \
   VN_ALLOC_MODE="$VN_ALLOC_MODE" VN_PERF="$vn_perf" \
   VTEST_SOCKET_NAME="$socket" "$client" "$case_name" \
   >"$prefix.client.log" 2>&1
client_status=$?
set -e

set +e
timeout 10s tail --pid="$server_pid" -f /dev/null >/dev/null 2>&1
server_wait_status=$?
if [ "$server_wait_status" -eq 124 ]; then
   kill "$server_pid" 2>/dev/null
fi
wait "$server_pid"
server_status=$?
set -e
server_pid=

stub_matches=$(grep -c "minigbm_allocation is not enabled" \
   "$prefix.server.log" || true)
dispatch_matches=$(grep -c "VTEST_CLIENT_ERROR_COMMAND_DISPATCH" \
   "$prefix.server.log" || true)
free_cs_error_matches=$(grep -c "vkFreeMemory resulted in CS error" \
   "$prefix.server.log" || true)
use_cs_error_matches=$(grep -c "vkBindBufferMemory2 resulted in CS error" \
   "$prefix.server.log" || true)
device_lost_matches=$(grep -c "async use returned VK_ERROR_DEVICE_LOST" \
   "$prefix.client.log" || true)
successful_wait_matches=$(grep -c "async-use reached a successful wait" \
   "$prefix.client.log" || true)
missing_export_matches=$(grep -c \
   "opaque resource was not exported by this renderer" \
   "$prefix.server.log" || true)
size_mismatch_matches=$(grep -c \
   "opaque resource allocation size does not match export" \
   "$prefix.server.log" || true)
case "$case_name" in
   foreign)
      rejection_pattern="failed to import resource: invalid res_id"
      ;;
   wrong-size|wrong-size-unaligned)
      rejection_pattern="opaque resource allocation size does not match export"
      ;;
   wrong-offset)
      rejection_pattern="opaque resource fd offset changed after export"
      ;;
   wrong-memory-type)
      rejection_pattern="opaque resource memory type does not match export"
      ;;
   *)
      rejection_pattern=
      ;;
esac
if [ -n "$rejection_pattern" ]; then
   rejection_matches=$(grep -c "$rejection_pattern" "$prefix.server.log" || true)
else
   rejection_matches=0
fi
{
   echo "case=$case_name"
   echo "allocation_mode=$VN_ALLOC_MODE"
   echo "client_exit=$client_status"
   echo "server_exit=$server_status"
   echo "server_wait_exit=$server_wait_status"
   echo "gbm_stub_matches=$stub_matches"
   echo "dispatch_error_matches=$dispatch_matches"
   echo "free_cs_error_matches=$free_cs_error_matches"
   echo "use_cs_error_matches=$use_cs_error_matches"
   echo "device_lost_matches=$device_lost_matches"
   echo "successful_wait_matches=$successful_wait_matches"
   echo "missing_export_matches=$missing_export_matches"
   echo "size_mismatch_matches=$size_mismatch_matches"
   echo "rejection_matches=$rejection_matches"
} >"$prefix.summary.log"
cat "$prefix.summary.log"

if [ "$case_name" = happy ] || [ "$case_name" = happy-unaligned ] ||
   [ "$case_name" = implicit-unaligned ]; then
   if [ "$client_status" -ne 0 ] || [ "$server_status" -ne 0 ]; then
      exit 1
   fi
elif [ "$VN_ALLOC_MODE" = sync ]; then
   if [ "$client_status" -ne 0 ] || [ "$server_status" -ne 0 ] ||
      [ "$rejection_matches" -lt 1 ]; then
      exit 1
   fi
elif { [ "$client_status" -ne 134 ] && [ "$device_lost_matches" -lt 1 ]; } ||
     [ "$server_status" -ne 0 ] || [ "$rejection_matches" -lt 1 ] ||
     [ "$use_cs_error_matches" -lt 1 ] ||
     [ "$successful_wait_matches" -ne 0 ]; then
   exit 1
fi
