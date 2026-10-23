/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Vulkan functions the fake driver is asked for and does not implement and
 * can be stubbed out
 */
#pragma once

#include <set>
#include <string>

const std::set<std::string> &fake_expected_unresolved_names();
