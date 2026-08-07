#include "fhir/units/families.hpp"

// C++11 port of mirobody/indicator/fhir/units/families.py. The data tables
// below are a verbatim copy of the Python dicts; keep them in sync. Insertion
// order of UCUM_FAMILY is significant (see families.hpp) so it is encoded as an
// ordered vector rather than a map literal.

namespace mirobody { namespace fhir { namespace units {

const std::vector<std::pair<std::string, std::string> >& ucum_family_ordered() {
    static const std::vector<std::pair<std::string, std::string> > v = {
        // ── Mass concentration (MCnc) ──
        {"g/L", "MCnc"}, {"g/dL", "MCnc"}, {"g/mL", "MCnc"},
        {"mg/L", "MCnc"}, {"mg/dL", "MCnc"}, {"mg/mL", "MCnc"},
        {"ug/L", "MCnc"}, {"ug/dL", "MCnc"}, {"ug/mL", "MCnc"}, {"ug/uL", "MCnc"},
        {"ng/L", "MCnc"}, {"ng/mL", "MCnc"}, {"ng/dL", "MCnc"},
        {"pg/mL", "MCnc"}, {"pg/L", "MCnc"}, {"fg/mL", "MCnc"},

        // ── Substance/molar concentration (SCnc) ──
        {"mol/L", "SCnc"}, {"mol/dL", "SCnc"}, {"mmol/L", "SCnc"},
        {"umol/L", "SCnc"}, {"nmol/L", "SCnc"}, {"pmol/L", "SCnc"}, {"fmol/L", "SCnc"},
        {"meq/L", "SCnc"}, {"mmol/dL", "SCnc"}, {"umol/dL", "SCnc"},
        {"nmol/mL", "SCnc"}, {"umol/mL", "SCnc"}, {"pmol/mL", "SCnc"},

        // ── Substance ratio (SRto) ──
        {"mmol/mol", "SRto"},

        // ── Substance rate (SRat) ──
        {"mmol/d", "SRat"}, {"umol/d", "SRat"}, {"meq/d", "SRat"},
        {"mmol/(24.h)", "SRat"}, {"umol/(24.h)", "SRat"}, {"nmol/(24.h)", "SRat"},
        {"pmol/(24.h)", "SRat"}, {"mmol/(12.h)", "SRat"}, {"mmol/(8.h)", "SRat"},
        {"mmol/(6.h)", "SRat"}, {"umol/(12.h)", "SRat"}, {"umol/(8.h)", "SRat"},

        // ── Mass rate (MRat) ──
        {"mg/d", "MRat"}, {"ug/d", "MRat"}, {"g/d", "MRat"},
        {"mg/(24.h)", "MRat"}, {"ug/(24.h)", "MRat"}, {"g/(24.h)", "MRat"},
        {"ng/(24.h)", "MRat"}, {"g/(6.h)", "MRat"}, {"g/(8.h)", "MRat"},
        {"g/(10.h)", "MRat"}, {"g/(12.h)", "MRat"}, {"mg/(6.h)", "MRat"},
        {"mg/(8.h)", "MRat"}, {"mg/(12.h)", "MRat"}, {"mg/(18.h)", "MRat"},
        {"g/h", "MRat"}, {"mg/h", "MRat"}, {"mg/min", "MRat"}, {"ug/min", "MRat"},
        {"mg/kg/d", "MRat"}, {"mmol/(5.h)", "SRat"}, {"g/(5.h)", "MRat"},
        {"umol/min/g", "CCnt"},

        // ── Mass ratio (MRto) ──
        {"mg/g", "MRto"}, {"ug/g", "MRto"}, {"ng/mg", "MRto"}, {"ug/mg", "MRto"},
        {"g/g", "MRto"}, {"nmol/mg", "MRto"}, {"pmol/mg", "MRto"},
        {"mg/mmol", "MRto"}, {"ug/mmol", "MRto"}, {"umol/mmol", "SRto"},
        {"umol/mol", "SRto"}, {"nmol/mol", "SRto"}, {"nmol/mmol", "SRto"},
        {"pmol/mmol", "SRto"}, {"umol/umol", "SRto"}, {"mmol/mmol", "SRto"},
        {"d/(7.d)", "NRat"}, {"d/(30.d)", "NRat"},

        // ── Catalytic content (CCnt) ──
        {"nmol/h/mg", "CCnt"}, {"nmol/min/mg", "CCnt"}, {"umol/h/mg", "CCnt"},
        {"umol/min/mg", "CCnt"}, {"nmol/h/mL", "CCnc"}, {"U/g", "CCnt"},
        {"umol/10*6", "EntSub"},

        // ── Length ratio (LenRto) ──
        {"[ft_us]/[ft_us]", "LenRto"},

        // ── Sound intensity ──
        {"dB", "RelSoundInt"},

        // ── Frequency (Freq) ──
        {"Hz", "Freq"}, {"kHz", "Freq"}, {"MHz", "Freq"},

        // ── Electric resistance (Resis) ──
        {"Ohm", "Resis"}, {"kOhm", "Resis"},

        // ── Radioactivity (Acty) ──
        {"mCi", "Acty"}, {"uCi", "Acty"}, {"Bq", "Acty"},
        {"kBq", "Acty"}, {"MBq", "Acty"}, {"GBq", "Acty"},

        // ── Energy (Engy) ──
        {"kcal", "Engy"}, {"cal", "Engy"}, {"J", "Engy"}, {"kJ", "Engy"}, {"MJ", "Engy"},

        // ── Energy difference (EngDiff) ──
        {"kJ/mol", "EngDiff"}, {"J/mol", "EngDiff"}, {"kcal/mol", "EngDiff"},

        // ── Energy rate (EngRat) ──
        {"kcal/h", "EngRat"}, {"kcal/d", "EngRat"}, {"kcal/(24.h)", "EngRat"},
        {"kcal/min", "EngRat"}, {"kJ/d", "EngRat"}, {"kJ/(24.h)", "EngRat"},
        {"kJ/min", "EngRat"}, {"kcal/kg/d", "EngRat"},

        // ── Power (Pwr) ──
        {"W", "Pwr"}, {"mW", "Pwr"}, {"kW", "Pwr"}, {"W/kg", "Pwr"},

        // ── Viscosity ──
        {"cP", "Visc"}, {"mPa.s", "Visc"},

        // ── Bethesda units ──
        {"[beth'U]", "Arb"}, {"[beth'U]/mL", "ACnc"},

        // ── Catalytic concentration rate (CCncRat) ──
        {"umol/L/h", "CCncRat"}, {"nmol/L/h", "CCncRat"},

        // ── MET ──
        {"[MET]", "ARat"},

        // ── Catalytic concentration variants ──
        {"umol/h/L", "CCnc"}, {"umol/min/L", "CCnc"},

        // ── Mass content (MCnt) ──
        {"mg/kg", "MCnt"}, {"ug/kg", "MCnt"}, {"ng/kg", "MCnt"},
        {"g/kg", "MCnt"}, {"pg/mg", "MCnt"}, {"ng/g", "MCnt"},

        // ── Substance content (SCnt) ──
        {"mmol/kg", "SCnt"}, {"umol/kg", "SCnt"}, {"nmol/g", "SCnt"},
        {"umol/g", "SCnt"}, {"mmol/g", "SCnt"}, {"meq/kg", "SCnt"},

        // ── Arbitrary concentration (ACnc) ──
        {"[IU]/L", "ACnc"}, {"[IU]/mL", "ACnc"}, {"[IU]/dL", "ACnc"},
        {"m[IU]/L", "ACnc"}, {"m[IU]/mL", "ACnc"}, {"k[IU]/L", "ACnc"},
        {"k[IU]/mL", "ACnc"}, {"u[IU]/mL", "ACnc"}, {"u[IU]/L", "ACnc"},
        {"[arb'U]/mL", "ACnc"}, {"[arb'U]/L", "ACnc"}, {"k[arb'U]/L", "ACnc"},
        {"k[arb'U]/mL", "ACnc"}, {"[arb'U]", "Arb"},

        // ── Catalytic concentration (CCnc) ──
        {"U/L", "CCnc"}, {"U/mL", "CCnc"}, {"mU/L", "CCnc"}, {"mU/mL", "CCnc"},
        {"kU/L", "CCnc"}, {"kat/L", "CCnc"}, {"ukat/L", "CCnc"}, {"nkat/L", "CCnc"},

        // ── Number concentration (NCnc) ──
        {"10*3/uL", "NCnc"}, {"10*6/uL", "NCnc"}, {"10*6/mL", "NCnc"},
        {"10*6/L", "NCnc"}, {"10*9/L", "NCnc"}, {"10*12/L", "NCnc"},
        {"/uL", "NCnc"}, {"/mL", "NCnc"}, {"/L", "NCnc"}, {"/dL", "NCnc"},
        {"/g", "NCnt"}, {"/kg", "NCnt"},

        // ── Bare counts (Num) ──
        {"10*6", "Num"}, {"10*9", "Num"}, {"10*12", "Num"},
        {"{#}", "Num"}, {"{steps}", "Num"}, {"{floors}", "Num"},
        {"{breaths}", "Num"}, {"{beats}", "Num"},

        // ── Score ──
        {"{score}", "Score"},

        // ── Number areic (Naric) ──
        // Bracketed only: bare "HPF"/"LPF" are not UCUM units, so a FHIR
        // consumer validating against UCUM rejects "/HPF". The unbracketed
        // spellings live in tokens.cpp as aliases of these.
        {"/[HPF]", "Naric"}, {"/[LPF]", "Naric"},

        // ── Fractions ──
        {"%", "MFr"}, {"[ppm]", "VFr"}, {"[ppb]", "VFr"}, {"[ppth]", "VFr"},
        {"mL/dL", "VFr"}, {"mL/L", "VFr"},

        // ── Ratios / dimensionless ──
        {"1", "Ratio"},

        // ── Volume (Vol) ──
        {"L", "Vol"}, {"dL", "Vol"}, {"cL", "Vol"}, {"mL", "Vol"},
        {"uL", "Vol"}, {"nL", "Vol"}, {"cm3", "Vol"}, {"mm3", "Vol"},
        {"[foz_us]", "Vol"}, {"[cup_us]", "Vol"}, {"[pt_us]", "Vol"},
        {"[qt_us]", "Vol"}, {"[gal_us]", "Vol"}, {"[tbs_us]", "Vol"}, {"[tsp_us]", "Vol"},

        // ── Volume rate (VRat) ──
        {"L/min", "VRat"}, {"mL/min", "VRat"}, {"mL/h", "VRat"}, {"L/h", "VRat"},
        {"mL/(24.h)", "VRat"}, {"L/(24.h)", "VRat"},
        {"mL/min/{1.73_m2}", "ArVRat"}, {"L/min/m2", "ArVRat"}, {"mL/min/m2", "ArVRat"},
        {"mL/(8.h)", "VRat"}, {"mL/(10.h)", "VRat"}, {"mL/(12.h)", "VRat"},
        {"mL/(6.h)", "VRat"}, {"mL/h/kg", "ArVRat"}, {"L/s", "VRat"}, {"mL/s", "VRat"},
        {"ng/mL/h", "CCnc"}, {"nmol/mL/h", "CCnc"}, {"mL/m2", "ArVol"},
        {"mL/min/kg", "ArVRat"},

        // ── Time durations ──
        {"s", "Time"}, {"ms", "Time"}, {"min", "Time"}, {"h", "Time"},
        {"d", "Time"}, {"wk", "Time"}, {"mo", "Time"}, {"a", "Time"},

        // ── Number rate (NRat) ──
        {"/min", "NRat"}, {"/h", "NRat"}, {"/s", "NRat"}, {"/d", "NRat"},
        {"/wk", "NRat"}, {"/mo", "NRat"}, {"/a", "NRat"},
        {"min/d", "NRat"}, {"h/d", "NRat"}, {"d/wk", "NRat"}, {"h/wk", "NRat"},
        {"min/wk", "NRat"}, {"[MET].min/wk", "ARat"}, {"[MET].h/wk", "ARat"},

        // ── Anthropometric / vital signs ──
        {"kg", "Mass"}, {"g", "Mass"}, {"mg", "Mass"}, {"ug", "Mass"},
        {"ng", "Mass"}, {"fg", "Mass"}, {"[lb_av]", "Mass"}, {"[oz_av]", "Mass"},
        {"cm", "Len"}, {"m", "Len"}, {"mm", "Len"}, {"um", "Len"}, {"km", "Len"},
        {"[in_us]", "Len"}, {"[ft_us]", "Len"}, {"[mi_us]", "Len"}, {"[yd_us]", "Len"},

        // ── Compound / derived ──
        {"kg/m2", "MCnc"}, {"g/m2", "MCnc"},

        // ── Pressure (Pres) ──
        {"mm[Hg]", "Pres"}, {"cm[H2O]", "Pres"}, {"kPa", "Pres"}, {"Pa", "Pres"},
        {"mbar", "Pres"}, {"bar", "Pres"}, {"[psi]", "Pres"},

        // ── Electric potential (Elpot) ──
        {"mV", "Elpot"}, {"uV", "Elpot"}, {"V", "Elpot"},
        {"mV/s", "ElpotRat"}, {"uV.ms", "TmElpot"}, {"uV.s", "TmElpot"},

        // ── Velocity (Vel) ──
        {"cm/s", "Vel"}, {"m/s", "Vel"}, {"mm/s", "Vel"}, {"km/h", "Vel"},

        // ── Area ──
        {"cm2", "Area"}, {"mm2", "Area"}, {"m2", "Area"},

        // ── Areic mass (ArMass) ──
        {"g/cm2", "ArMass"}, {"mg/cm2", "ArMass"}, {"kg/cm2", "ArMass"},

        // ── Areic length (ArLen) ──
        {"cm/m2", "ArLen"}, {"mm/m2", "ArLen"},

        // ── Temperature (Temp) ──
        {"Cel", "Temp"}, {"[degF]", "Temp"}, {"K", "Temp"},

        // ── Angle ──
        {"deg", "Angle"}, {"rad", "Angle"},

        // ── Inverse length (InvLen) ──
        {"[diop]", "InvLen"}, {"[p'diop]", "InvLen"},

        // ── pH ──
        {"[pH]", "LsCnc"},

        // ── Hematology indices ──
        {"fL", "EntVol"}, {"pg", "EntMass"},

        // ── Osmolality ──
        {"mosm/kg", "Osmol"}, {"mosm/L", "Osmolarity"},

        // ── Bare atomic units ──
        {"mmol", "Sub"}, {"umol", "Sub"}, {"nmol", "Sub"}, {"pmol", "Sub"},
        {"fmol", "Sub"}, {"mol", "Sub"}, {"meq", "Sub"}, {"mosm", "Sub"},
        {"osmol", "Sub"}, {"[IU]", "Arb"}, {"m[IU]", "Arb"}, {"k[IU]", "Arb"},
        {"U", "CCnt"}, {"mU", "CCnt"}, {"kU", "CCnt"},
    };
    return v;
}

const std::unordered_map<std::string, std::string>& ucum_family_map() {
    static const std::unordered_map<std::string, std::string> m = [] {
        std::unordered_map<std::string, std::string> mm;
        const std::vector<std::pair<std::string, std::string> >& v = ucum_family_ordered();
        for (std::vector<std::pair<std::string, std::string> >::const_iterator it = v.begin();
             it != v.end(); ++it) {
            mm.insert(*it);  // first wins, matching dict-literal semantics (keys unique anyway)
        }
        return mm;
    }();
    return m;
}

const std::unordered_map<std::string, std::set<std::string> >& ambiguous_units() {
    static const std::unordered_map<std::string, std::set<std::string> > m = {
        {"%", {"MFr", "NFr", "AFr", "VFr", "SFr", "CFr", "LenFr", "RelACnc", "RelRto"}},
        {"mm[Hg]", {"Pres", "PPres"}},
        {"cm[H2O]", {"Pres", "PPres"}},
        {"[ppm]", {"VFr", "MFr", "SFr"}},
        {"deg", {"Angle", "Temp"}},
        {"1", {"Ratio", "NRto", "SRto", "MRto", "VRto", "CRto", "ARto"}},
        {"mg/g", {"MRto", "MCnt"}},
        {"ug/g", {"MRto", "MCnt"}},
        {"ng/g", {"MCnt", "MRto"}},
        {"ng/mg", {"MRto", "MCnt"}},
        {"ug/mg", {"MRto", "MCnt"}},
    };
    return m;
}

std::string unit_family(const std::string& ucum) {
    if (ucum.empty()) return "";
    const std::unordered_map<std::string, std::string>& m = ucum_family_map();
    std::unordered_map<std::string, std::string>::const_iterator it = m.find(ucum);
    return it == m.end() ? std::string() : it->second;
}

std::set<std::string> unit_families(const std::string& ucum) {
    if (ucum.empty()) return std::set<std::string>();
    const std::unordered_map<std::string, std::set<std::string> >& amb = ambiguous_units();
    std::unordered_map<std::string, std::set<std::string> >::const_iterator a = amb.find(ucum);
    if (a != amb.end()) return a->second;
    std::string fam = unit_family(ucum);
    std::set<std::string> out;
    if (!fam.empty()) out.insert(fam);
    return out;
}

}}}  // namespace mirobody::fhir::units
