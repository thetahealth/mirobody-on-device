#include "fhir/resource.hpp"

#include "fhir/fhir.hpp"

#include <rapidjson/document.h>

namespace mirobody { namespace fhir {

std::vector<ValidationIssue> validate_resource(const rapidjson::Value& res,
                                               std::string* out_type,
                                               std::string* out_id) {
    std::vector<ValidationIssue> issues;

    if (!res.IsObject()) {
        issues.push_back(ValidationIssue("error", "structure",
                                         "resource must be a JSON object"));
        return issues;
    }

    // resourceType — present, string, well-formed.
    rapidjson::Value::ConstMemberIterator rt = res.FindMember("resourceType");
    if (rt == res.MemberEnd() || !rt->value.IsString()) {
        issues.push_back(ValidationIssue("error", "required",
                                         "resource is missing a string 'resourceType'"));
    } else {
        std::string type(rt->value.GetString(), rt->value.GetStringLength());
        if (!is_well_formed_resource_type(type)) {
            issues.push_back(ValidationIssue("error", "value",
                                             "'resourceType' is not a valid FHIR type name: " + type));
        } else if (out_type) {
            *out_type = type;
        }
    }

    // id — optional, but must be a valid FHIR id when present.
    rapidjson::Value::ConstMemberIterator idm = res.FindMember("id");
    if (idm != res.MemberEnd()) {
        if (!idm->value.IsString()) {
            issues.push_back(ValidationIssue("error", "value", "'id' must be a string"));
        } else {
            std::string id(idm->value.GetString(), idm->value.GetStringLength());
            if (!is_valid_fhir_id(id)) {
                issues.push_back(ValidationIssue("error", "value",
                                                 "'id' is not a valid FHIR id: " + id));
            } else if (out_id) {
                *out_id = id;
            }
        }
    }

    // Contained resources are not allowed (matches Health Connect).
    if (res.HasMember("contained")) {
        issues.push_back(ValidationIssue("error", "structure",
                                         "contained FHIR resources are not supported"));
    }

    // No top-level member may be JSON null (matches Health Connect's
    // "no JSON null values in top-level fields" rule).
    for (rapidjson::Value::ConstMemberIterator it = res.MemberBegin();
         it != res.MemberEnd(); ++it) {
        if (it->value.IsNull()) {
            std::string name(it->name.GetString(), it->name.GetStringLength());
            issues.push_back(ValidationIssue("error", "value",
                                             "top-level field '" + name + "' is null"));
        }
    }

    return issues;
}

}}  // namespace mirobody::fhir
