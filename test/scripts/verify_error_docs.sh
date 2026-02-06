#!/bin/bash
# Verify all error codes have documentation

set -e

echo "Verifying error documentation coverage..."

# Extract all error codes from dbsp_errors.hpp
error_codes=$(grep -E "= [0-9]{3}," dbsp_errors.hpp | awk '{print $3}' | tr -d ',')

missing=0
for code in $error_codes; do
    # Calculate category
    category=$((code / 100))

    # Check if doc file exists
    doc_file="docs/errors/E${category}xx/DBSP-E$(printf '%03d' $code).md"

    if [ ! -f "$doc_file" ]; then
        echo "❌ Missing documentation: $doc_file (error code $code)"
        ((missing++))
    else
        echo "✓ Found: $doc_file"
    fi
done

if [ $missing -gt 0 ]; then
    echo ""
    echo "Error: $missing error codes are missing documentation"
    exit 1
fi

echo ""
echo "✓ All error codes have documentation"
exit 0
