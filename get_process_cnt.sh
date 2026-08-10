#!/usr/bin/env bash

# Add every NFP device to sample here.
device_ids=(0 1)

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
output_file="${script_dir}/processed_cnt.csv"

if [[ ! -e "$output_file" ]]; then
    printf 'current_timestamp,device_id,int64_value\n' > "$output_file"
fi

for device_id in "${device_ids[@]}"; do
    output="$(sudo nfp-rtsym -n "$device_id" -l 8 -v _pif_act_stats:56)" || {
        printf 'Failed to read process count for device %s\n' "$device_id" >&2
        continue
    }

    # nfp-rtsym prints: <address>: <low-32-bit-word> <high-32-bit-word>
    read -r low_word high_word < <(awk 'NR == 1 { print $2, $3 }' <<< "$output")
    low_word="${low_word#0x}"
    high_word="${high_word#0x}"

    if [[ ! "$low_word" =~ ^[[:xdigit:]]{8}$ || ! "$high_word" =~ ^[[:xdigit:]]{8}$ ]]; then
        printf 'Unexpected nfp-rtsym output for device %s: %s\n' "$device_id" "$output" >&2
        continue
    fi

    # The second value is the high word.  For example, 0x00000570 0x00000000
    # becomes 0x0000000000000570 before conversion to an unsigned int64.
    merged_hex="0x${high_word}${low_word}"
    int64_value="$(printf '%u' "$merged_hex")"

    printf '%s,%s,%s\n' "$(date --iso-8601=seconds)" "$device_id" "$int64_value" >> "$output_file"

    if ! sudo nfp-rtsym -n "$device_id" -l 8 -v _pif_act_stats:56 0; then
        printf 'Failed to clear process count for device %s\n' "$device_id" >&2
    fi
done
