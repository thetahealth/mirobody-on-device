package ai.thetahealth.mirobody.data.circle

import ai.thetahealth.mirobody.data.circle.dto.CircleIdRequest
import ai.thetahealth.mirobody.data.circle.dto.CircleMembersResponse
import ai.thetahealth.mirobody.data.circle.dto.ConversationShare
import ai.thetahealth.mirobody.data.circle.dto.ConversationShareRequest
import ai.thetahealth.mirobody.data.circle.dto.ConversationUnshareRequest
import ai.thetahealth.mirobody.data.circle.dto.CreateCircleRequest
import ai.thetahealth.mirobody.data.circle.dto.HealthSharer
import ai.thetahealth.mirobody.data.circle.dto.HealthSharingRequest
import ai.thetahealth.mirobody.data.circle.dto.InviteRequest
import ai.thetahealth.mirobody.data.circle.dto.MemberRequest
import ai.thetahealth.mirobody.data.circle.dto.NicknameRequest
import ai.thetahealth.mirobody.data.circle.dto.RenameCircleRequest
import ai.thetahealth.mirobody.data.circle.dto.RoleRequest
import ai.thetahealth.mirobody.data.circle.dto.TokenRequest
import ai.thetahealth.mirobody.data.net.ensureOk
import ai.thetahealth.mirobody.data.net.unwrap

class CircleRepository(
    private val api: CircleApi,
) {
    suspend fun members(): CircleMembersResponse = api.members().unwrap()

    suspend fun create(name: String) {
        api.create(CreateCircleRequest(name = name)).ensureOk()
    }

    suspend fun rename(circleId: Long, name: String) {
        api.rename(RenameCircleRequest(careCircleId = circleId, name = name)).ensureOk()
    }

    suspend fun delete(circleId: Long) {
        api.delete(CircleIdRequest(careCircleId = circleId)).ensureOk()
    }

    suspend fun invite(email: String, circleId: Long?) {
        api.invite(InviteRequest(email = email, careCircleId = circleId)).ensureOk()
    }

    suspend fun accept(token: String) {
        api.accept(TokenRequest(token = token)).ensureOk()
    }

    suspend fun decline(token: String) {
        api.decline(TokenRequest(token = token)).ensureOk()
    }

    suspend fun remove(member: Long) {
        api.remove(MemberRequest(member = member)).ensureOk()
    }

    suspend fun setNickname(member: Long, nickname: String) {
        api.setNickname(NicknameRequest(member = member, nickname = nickname)).ensureOk()
    }

    suspend fun setRole(member: Long, role: String) {
        api.setRole(RoleRequest(member = member, role = role)).ensureOk()
    }

    /** access: "off" | "view" | "edit"; per circle (care_circle_id required). */
    suspend fun setHealthSharing(circleId: Long, access: String) {
        api.setHealthSharing(HealthSharingRequest(access = access, careCircleId = circleId)).ensureOk()
    }

    suspend fun healthSharedWithMe(): List<HealthSharer> =
        api.healthSharedWithMe().unwrap().users

    suspend fun shareConversation(conversationId: String, members: List<Long>, access: String) {
        api.shareConversation(
            ConversationShareRequest(conversationId = conversationId, members = members, access = access),
        ).ensureOk()
    }

    suspend fun unshareConversation(conversationId: String, member: Long) {
        api.unshareConversation(
            ConversationUnshareRequest(conversationId = conversationId, member = member),
        ).ensureOk()
    }

    suspend fun conversationShares(conversationId: String): List<ConversationShare> =
        api.conversationShares(conversationId).unwrap().shares
}
