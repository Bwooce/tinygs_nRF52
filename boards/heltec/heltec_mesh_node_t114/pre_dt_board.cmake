# Copyright (c) 2026 Bruce Fitzsimons
# SPDX-License-Identifier: Apache-2.0

# Suppress "unique_unit_address_if_enabled" — same overlapping nRF
# peripheral nodes (power@40000000 / clock@40000000 / bprot@40000000,
# acl@4001e000 / flash-controller@4001e000) as the nRF52840 DK.
list(APPEND EXTRA_DTC_FLAGS "-Wno-unique_unit_address_if_enabled")
