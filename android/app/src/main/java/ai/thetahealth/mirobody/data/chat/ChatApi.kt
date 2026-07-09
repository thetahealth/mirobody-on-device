package ai.thetahealth.mirobody.data.chat

import ai.thetahealth.mirobody.data.chat.dto.ConversationDetail
import ai.thetahealth.mirobody.data.chat.dto.HistoryDeleteRequest
import ai.thetahealth.mirobody.data.chat.dto.HistoryResponse
import ai.thetahealth.mirobody.data.chat.dto.ProviderGroup
import ai.thetahealth.mirobody.data.net.ApiEnvelope
import kotlinx.serialization.json.JsonElement
import retrofit2.http.Body
import retrofit2.http.GET
import retrofit2.http.POST
import retrofit2.http.Query

interface ChatApi {
    @GET("/api/providers")
    suspend fun listProviders(): ApiEnvelope<List<ProviderGroup>>

    @GET("/api/history")
    suspend fun history(
        @Query("page") page: Int = 0,
        @Query("page_size") pageSize: Int = 20,
    ): ApiEnvelope<HistoryResponse>

    @POST("/api/history/delete")
    suspend fun deleteHistory(@Body body: HistoryDeleteRequest): ApiEnvelope<JsonElement?>

    @GET("/api/conversation")
    suspend fun conversation(@Query("id") id: String): ApiEnvelope<ConversationDetail>
}
