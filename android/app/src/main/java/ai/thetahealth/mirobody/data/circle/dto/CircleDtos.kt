package ai.thetahealth.mirobody.data.circle.dto

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

// Wire DTOs for the care-circle routes (/api/circle/*, /api/conversation/*).
// They mirror the web client (htdoc) exactly:
//   - Cross-user references are an opaque `member` handle (care_circle_members.id),
//     never a raw users PK. The server resolves it back with an access check.
//   - The caller's own identity always comes from the JWT, so nothing here sends
//     a user id for "me".
// All these routes use the standard {code,msg,data} ApiEnvelope; the DTOs below
// are the `data` payloads.

// -- /api/circle/members -----------------------------------------------------

@Serializable
data class CircleMember(
    val member: Long = 0,            // opaque handle for this (circle, user) row
    val email: String = "",
    val status: String = "",         // "pending" | "accepted" | "declined"
    val role: Int = 0,               // 0 Member, 1 Maintainer, 2 Owner
    @SerialName("health_access") val healthAccess: Int = 0,   // 0 off, 1 view, 2 edit
    val nickname: String = "",
    val me: Boolean = false,
)

@Serializable
data class Circle(
    @SerialName("circle_id") val circleId: Long = 0,
    val name: String = "",
    @SerialName("my_role") val myRole: Int = 0,
    @SerialName("my_health_access") val myHealthAccess: Int = 0,
    val members: List<CircleMember> = emptyList(),
)

@Serializable
data class CircleInvite(
    @SerialName("circle_id") val circleId: Long = 0,
    val token: String = "",
    @SerialName("owner_email") val ownerEmail: String = "",
    @SerialName("circle_name") val circleName: String = "",
)

@Serializable
data class CircleMembersResponse(
    val circles: List<Circle> = emptyList(),
    val invites: List<CircleInvite> = emptyList(),
)

// -- /api/circle/health-shared-with-me ---------------------------------------

@Serializable
data class HealthSharer(
    val member: Long = 0,            // opaque handle to pass back as the chat `subject`
    val email: String = "",
    val nickname: String = "",
)

@Serializable
data class HealthSharedResponse(
    val users: List<HealthSharer> = emptyList(),
)

// -- /api/conversation/shares ------------------------------------------------

@Serializable
data class ConversationShare(
    val email: String = "",
    val access: String = "view",     // "view" | "edit"
)

@Serializable
data class ConversationSharesResponse(
    val shares: List<ConversationShare> = emptyList(),
)

// -- request bodies ----------------------------------------------------------

@Serializable
data class CreateCircleRequest(val name: String)

@Serializable
data class RenameCircleRequest(
    @SerialName("care_circle_id") val careCircleId: Long,
    val name: String,
)

@Serializable
data class CircleIdRequest(
    @SerialName("care_circle_id") val careCircleId: Long,
)

@Serializable
data class InviteRequest(
    val email: String,
    @SerialName("care_circle_id") val careCircleId: Long? = null,
)

@Serializable
data class TokenRequest(val token: String)

@Serializable
data class MemberRequest(val member: Long)

@Serializable
data class NicknameRequest(
    val member: Long,
    val nickname: String,
)

@Serializable
data class RoleRequest(
    val member: Long,
    val role: String,                // "member" | "maintainer"
)

@Serializable
data class HealthSharingRequest(
    val access: String,              // "off" | "view" | "edit"
    @SerialName("care_circle_id") val careCircleId: Long,
)

@Serializable
data class ConversationShareRequest(
    @SerialName("conversation_id") val conversationId: String,
    val members: List<Long>,
    val access: String,              // "view" | "edit"
)

@Serializable
data class ConversationUnshareRequest(
    @SerialName("conversation_id") val conversationId: String,
    val member: Long,
)
