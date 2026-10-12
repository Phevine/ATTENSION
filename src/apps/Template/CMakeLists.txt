#
# MIT License
# Copyright (c) 2025 Vaclav Mach (Bastl Instruments)
#

# ATTENSION modifications
# Copyright (c) 2026 Chris Beattie
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This identifier applies only to Chris Beattie's copyrightable ATTENSION
# modifications. Original Bastl material remains under the MIT licence above.

# App definition
create_kastle2_app(
    APP_NAME "template"
    APP_OUTPUT_NAME "ATTENSION"
    APP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/AppTemplate.cpp
    DISABLE_SPOKEN_TEST_SAMPLES
)
