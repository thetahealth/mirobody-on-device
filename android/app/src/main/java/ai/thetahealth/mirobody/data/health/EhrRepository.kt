package ai.thetahealth.mirobody.data.health

import ai.thetahealth.mirobody.data.net.unwrap

class EhrRepository(private val api: EhrApi) {

    /** Search the EHR provider directory. */
    suspend fun providers(query: String): List<EhrProvider> =
        api.providers(query).unwrap().providers

    /** SMART authorize URL for a tenant's FHIR base, to open in a browser. */
    suspend fun authorizeUrl(fhirBaseUrl: String): String? =
        api.authorize(EhrAuthorizeRequest(fhirBaseUrl)).unwrap().authorizeUrl

    /** Pull Observations from the connected EHR; returns how many were stored. */
    suspend fun sync(): Int = api.sync().unwrap().posted
}
