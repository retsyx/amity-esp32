# Source this file from a bash shell. All generated project files stay in this repository.
export IDF_PATH="${IDF_PATH:-$HOME/.espressif/v6.1/esp-idf}"
export ESP_MATTER_PATH="${ESP_MATTER_PATH:-$HOME/.espressif/v6.1/esp-matter}"
export IDF_COMPONENT_CACHE_PATH="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/.cache/components"
export PYTHONDONTWRITEBYTECODE=1
. "$IDF_PATH/export.sh"
. "$ESP_MATTER_PATH/export.sh"
