package ai.thetahealth.mirobody.data.circle

import ai.thetahealth.mirobody.data.circle.dto.CircleIdRequest
import ai.thetahealth.mirobody.data.circle.dto.CircleMembersResponse
import ai.thetahealth.mirobody.data.circle.dto.ConversationShareRequest
import ai.thetahealth.mirobody.data.circle.dto.ConversationSharesResponse
import ai.thetahealth.mirobody.data.circle.dto.ConversationUnshareRequest
import ai.thetahealth.mirobody.data.circle.dto.CreateCircleRequest
import ai.thetahealth.mirobody.data.circle.dto.HealthSharedResponse
import ai.thetahealth.mirobody.data.circle.dto.HealthSharingRequest
import ai.thetahealth.mirobody.data.circle.dto.InviteRequest
import ai.thetahealth.mirobody.data.circle.dto.MemberRequest
import ai.thetahealth.mirobody.data.circle.dto.NicknameRequest
import ai.thetahealth.mirobody.data.circle.dto.RenameCircleRequest
import ai.thetahealth.mirobody.data.circle.dto.RoleRequest
import ai.thetahealth.mirobody.data.circle.dto.TokenRequest
import ai.thetahealth.mirobody.data.net.ApiEnvelope
import kotlinx.serialization.json.JsonElement
import retrofit2.http.Body
import retrofit2.http.GET
import retrofit2.http.POST
import retrofit2.http.Query

// Care-circle + conversation-sharing endpoints. Modern backends only; the
// embedded SQLite server includes them. Auth (bearer JWT) is added by the
// AuthInterceptor. See src/circle/README.md for the contract.
interface CircleApi {
    @GET("/api/circle/members")
    suspend fun members(): ApiEnvelope<CircleMembersResponse>

    @POST("/api/circle/create")
    suspend fun create(@Body body: CreateCircleRequest): ApiEnvelope<JsonElement?>

    @POST("/api/circle/rename")
    suspend fun rename(@Body body: RenameCircleRequest): ApiEnvelope<JsonElement?>

    @POST("/api/circle/delete")
    suspend fun delete(@Body body: CircleIdRequest): ApiEnvelope<JsonElement?>

    @POST("/api/circle/invite")
    suspend fun invite(@Body body: InviteRequest): ApiEnvelope<JsonElement?>

    @POST("/api/circle/accept")
    suspend fun accept(@Body body: TokenRequest): ApiEnvelope<JsonElement?>

    @POST("/api/circle/decline")
    suspend fun decline(@Body body: TokenRequest): ApiEnvelope<JsonElement?>

    @POST("/api/circle/remove")
    suspend fun remove(@Body body: MemberRequest): ApiEnvelope<JsonElement?>

    @POST("/api/circle/nickname")
    suspend fun setNickname(@Body body: NicknameRequest): ApiEnvelope<JsonElement?>

    @POST("/api/circle/role")
    suspend fun setRole(@Body body: RoleRequest): ApiEnvelope<JsonElement?>

    @POST("/api/circle/health-sharing")
    suspend fun setHealthSharing(@Body body: HealthSharingRequest): ApiEnvelope<JsonElement?>

    @GET("/api/circle/health-shared-with-me")
    suspend fun healthSharedWithMe(): ApiEnvelope<HealthSharedResponse>

    @POST("/api/conversation/share")
    suspend fun shareConversation(@Body body: ConversationShareRequest): ApiEnvelope<JsonElement?>

    @POST("/api/conversation/unshare")
    suspend fun unshareConversation(@Body body: ConversationUnshareRequest): ApiEnvelope<JsonElement?>

    @GET("/api/conversation/shares")
    suspend fun conversationShares(@Query("id") conversationId: String): ApiEnvelope<ConversationSharesResponse>
}
