#pragma once

// Runtime lookup of health-data vendors by id. The concrete vendor classes live
// one-per-file in src/health/vendor/<id>.cpp and are never named outside their own
// translation unit; callers reach them only through open_vendor(). This header
// is the whole public surface of the vendor module alongside vendor.hpp.

#include "health/vendor/vendor.hpp"

#include <memory>
#include <string>
#include <vector>

namespace mirobody { namespace vendor {

// The id of every registered vendor, in matrix order (README.md).
std::vector<std::string> vendor_ids();

// Static metadata for all registered vendors — no credentials, no network.
std::vector<VendorInfo> all_vendor_info();

// Construct the vendor with the given id. Throws VendorError if `id` is unknown.
// The returned client is a stub until its src/health/vendor/<id>.cpp implements the
// transport; metadata via info() works regardless.
std::unique_ptr<Vendor> open_vendor(const std::string& id, const VendorConfig& config);

}}
