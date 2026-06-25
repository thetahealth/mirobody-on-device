#pragma once

// Structural validation for generic FHIR R4 resources.
//
// Mirrors the validation the Android Health Connect medical-records write API
// applies (data-format page), to the extent possible without the full FHIR
// StructureDefinitions (we keep the generic model, so deep per-field type
// checks are out of scope — see the limitations in fhir/README.md):
//
//   - the body must be a JSON object;
//   - resourceType must be present, a string, and well-formed;
//   - id, when present, must be a valid FHIR id;
//   - contained FHIR resources are not allowed;
//   - no top-level member may be JSON null.
//
// Returns an issue list; an empty list means the resource is acceptable. The
// REST layer turns a non-empty list into an OperationOutcome (fhir/rest.cpp).

#include <string>
#include <vector>

namespace rapidjson { class CrtAllocator;
    template <typename> class MemoryPoolAllocator;
    template <typename> struct UTF8;
    template <typename, typename> class GenericValue;
    typedef GenericValue<UTF8<char>, MemoryPoolAllocator<CrtAllocator> > Value; }

namespace mirobody { namespace fhir {

// One validation problem, shaped to map onto an OperationOutcome.issue entry.
struct ValidationIssue {
    std::string severity;     // "error" | "warning"
    std::string code;         // FHIR issue-type code, e.g. "structure", "value"
    std::string diagnostics;  // human-readable detail

    ValidationIssue(const std::string& s, const std::string& c, const std::string& d)
        : severity(s), code(c), diagnostics(d) {}
};

// Validate `res` (a parsed JSON value). On a structurally valid resource the
// returned vector is empty and, when non-null, *out_type / *out_id are filled
// with resourceType and id (id may be left empty if the resource has none).
std::vector<ValidationIssue> validate_resource(const rapidjson::Value& res,
                                               std::string* out_type = nullptr,
                                               std::string* out_id = nullptr);

}}  // namespace mirobody::fhir
