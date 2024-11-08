#!/bin/bash

output="sculpt.c"

> "$output"

echo "// Combined source file for sculpt" >> "$output"
echo "#include \"sculpt.h\"" >> "$output"
echo "" >> "$output"

src_files=(
    "../src/sculpt_header.c"
    "../src/sculpt_conn.c"
    "../src/sculpt_mgr.c"
    "../src/sculpt_util.c"
)

for file in "${src_files[@]}"; do
    echo "// Start of $file" >> "$output"
    cat "$file" >> "$output"
    echo -e "\n// End of $file\n" >> "$output"
done

cp ../src/sculpt.h .

echo "Merged source files into $output."

