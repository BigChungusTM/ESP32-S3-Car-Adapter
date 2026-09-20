#!/bin/bash
# Source this file from bash before using idf.py.
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export IDF_PATH="${IDF_PATH:-/Users/lojaws/.platformio/packages/framework-espidf}"
export IDF_TOOLS_PATH="$PROJECT_ROOT/.tools"
source "$IDF_PATH/export.sh"
