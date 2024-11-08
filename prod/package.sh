#!/bin/bash

# Output file
output="sculpt.c"

# Clear the output file if it exists
> "$output"

# Add header file include (if needed)
echo "// Combined source file for sculpt" >> "$output"
echo "#include \"sculpt.h\"" >> "$output"
echo "" >> "$output"

# List of source files to merge
src_files=(
    "../src/sculpt_header.c"
    "../src/sculpt_conn.c"
    "../src/sculpt_mgr.c"
    "../src/sculpt_util.c"
)

# Iterate over each source file and append it to the output file
for file in "${src_files[@]}"; do
    echo "// Start of $file" >> "$output"
    cat "$file" >> "$output"
    echo -e "\n// End of $file\n" >> "$output"
done

cp ../src/sculpt.h .

echo "Merged source files into $output."

