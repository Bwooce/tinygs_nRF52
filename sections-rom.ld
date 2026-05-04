/*
 * Linker fragment: declare the iterable section for the device web UI
 * HTTP service (`tinygs_web`). Required so HTTP_SERVICE_DEFINE's extern
 * `_http_resource_desc_tinygs_web_list_start/end` symbols resolve.
 */
#include <zephyr/linker/iterable_sections.h>

ITERABLE_SECTION_ROM(http_resource_desc_tinygs_web, Z_LINK_ITERABLE_SUBALIGN)
