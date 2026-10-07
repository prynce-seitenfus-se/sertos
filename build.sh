#!/usr/bin/env bash
set -eu

instrumented=0
for arg in "$@"; do
    case "$arg" in
        -i|--instrumented)
            instrumented=1
            ;;
        -h|--help)
            echo "Usage: $0 [-i|--instrumented]"
            exit 0
            ;;
        *)
            echo "Unknown argument: $arg" >&2
            exit 1
            ;;
    esac
done

core_sources=(
    src/sertos_task.c
    src/sertos_scheduler.c
    src/sertos_stats.c
    src/sertos_sem.c
    src/sertos_mutex.c
    src/sertos_queue.c
    src/sertos_stream_buffer.c
    src/sertos_timer.c
)

module_sources=(
    modules/bitmap/bitmap.c
    modules/crc/crc.c
    modules/fsm/fsm.c
    modules/linked_list/linked_list.c
    modules/memory_pool/memory_pool.c
    modules/ring_buffer/ring_buffer.c
)

includes=(
    -Iinc
    -Iport
    -Imodules/atomic
    -Imodules/ring_buffer
    -Imodules/memory_pool
    -Imodules/linked_list
    -Imodules/bitmap
    -Imodules/crc
    -Imodules/fsm
)

if ! command -v gcc >/dev/null 2>&1 ||
   ! command -v ar >/dev/null 2>&1 ||
   ! command -v size >/dev/null 2>&1; then
    echo "[ERROR] Linux build requires gcc, ar, and size in WSL." >&2
    exit 1
fi

lib_dir=lib/posix
obj_dir=build/posix
if [ "$instrumented" -eq 1 ]; then
    library="$lib_dir/libsertos_posix_instrumented.a"
else
    library="$lib_dir/libsertos_posix.a"
fi

mkdir -p "$lib_dir" "$obj_dir"

echo "[TOOLCHAIN] gcc=$(command -v gcc)"
echo "[TOOLCHAIN] ar=$(command -v ar)"
echo "[TOOLCHAIN] size=$(command -v size)"

objects=()

for source in "${core_sources[@]}"; do
    object="$obj_dir/$(basename "${source%.*}").o"
    echo "[BUILD] $source"
    core_cflags=()
    if [ "$instrumented" -eq 1 ]; then
        core_cflags+=("-finstrument-functions")
    fi
    gcc -O2 -Wall -Wextra -pedantic -std=c99 "${includes[@]}" "${core_cflags[@]}" -c "$source" -o "$object"
    objects+=("$object")
done

other_sources=("${module_sources[@]}" port/posix/port_posix.c)
for source in "${other_sources[@]}"; do
    object="$obj_dir/$(basename "${source%.*}").o"
    echo "[BUILD] $source"
    gcc -O2 -Wall -Wextra -pedantic -std=c99 "${includes[@]}" -c "$source" -o "$object"
    objects+=("$object")
done

ar rcs "$library" "${objects[@]}"

echo "[SUCCESS] Generated: $library"
size -t "$library" | awk '$NF == "(TOTALS)" {
    printf ".text %12d\n.data %12d\n.bss  %12d\nTotal %12d\n", $1, $2, $3, $4
}' || true
