#!/usr/bin/env bash

set -eu

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 <output_file> <file_or_dir1> [file_or_dir2 ...]" >&2
    exit 1
fi

OUTPUT_FILE="$1"
shift

# Clear or create the destination file
> "$OUTPUT_FILE"

# Process an individual file
append_file() {
    local file_path="$1"

    # Avoid processing the target output file if it exists within the search path
    if [[ -f "$OUTPUT_FILE" && "$file_path" -ef "$OUTPUT_FILE" ]]; then
        return
    fi

    # Ignore common binary/git/build directories or files if needed
    if [[ "$file_path" == *"/.git/"* || "$file_path" == *"/target/"* ]]; then
        return
    fi

    echo "Processing: $file_path"
    {
        echo "// =========================================================================="
        echo "// File: ${file_path}"
        echo "// =========================================================================="
        echo ""
        cat "$file_path"
        echo ""
        echo ""
    } >> "$OUTPUT_FILE"
}

# Main loop over arguments
for target in "$@"; do
    if [ -f "$target" ]; then
        append_file "$target"
    elif [ -d "$target" ]; then
        # Find all regular files in directory tree
        while IFS= read -r -d '' file; do
            append_file "$file"
        done < <(find "$target" -type f -print0)
    else
        echo "Warning: '$target' is neither a valid file nor directory. Skipping." >&2
    fi
done

echo "Successfully concatenated files to '$OUTPUT_FILE'"
