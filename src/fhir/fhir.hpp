#pragma once

// Shared types and constants for the FHIR R4 module.
//
// mirobody runs an embedded RESTful FHIR R4 endpoint (see fhir/rest.hpp). The
// document pipeline parses indicators + values out of uploaded files, normalizes
// their units (fhir/units), maps them to SNOMED CT / LOINC / RxNorm codes, and
// materializes them as FHIR resources served here. Resources are stored and
// served as generic validated JSON (the "generic/any resource" model): we do
// not hard-model each resource type, only enforce the structural rules below.

#include <string>
#include <vector>

namespace mirobody { namespace fhir {

// FHIR releases mirobody accepts and advertises. R4 (4.0.1) is the default
// fhirVersion in the CapabilityStatement; R4B (4.3.0) is wire-compatible for
// the generic model. Matches the versions the Android Health Connect medical-
// records data-format page accepts.
extern const char* const kFhirVersionR4;   // "4.0.1"
extern const char* const kFhirVersionR4B;  // "4.3.0"
extern const char* const kFhirJsonMime;    // "application/fhir+json"

bool is_supported_fhir_version(const std::string& v);

// The FHIR resource types mirobody recognizes for the CapabilityStatement —
// the 14 from the Health Connect medical-records data-format page. The store
// itself accepts any well-formed resourceType (generic model); this list only
// drives the advertised capability surface.
const std::vector<std::string>& known_resource_types();

// A resourceType is well-formed when it is a non-empty UpperCamelCase token
// (^[A-Z][A-Za-z0-9]*$). Generic model: we do NOT restrict to the known set.
bool is_well_formed_resource_type(const std::string& t);

// A FHIR id is 1-64 chars of [A-Za-z0-9.-] (the R4 `id` primitive). Used both
// for client-supplied ids on PUT and for validating ids inside resource bodies.
bool is_valid_fhir_id(const std::string& id);

}}  // namespace mirobody::fhir
