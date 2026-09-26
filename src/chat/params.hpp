#pragma once

// Per-command typed parameters (chat interface tier).
//
// A transport produces a generic chat::Packet ({code, params-json}); the
// dispatcher turns that into a typed, validated parameter struct per command via
// `parse(pkt)`, then hands it to the matching Chat method. This keeps the wire
// boundary generic (transports stay dumb) while the dispatcher logic works with
// real named fields instead of stringly-typed params lookups.
//
// Add a command by adding a struct here with a `parse(pkt)` and a case in the
// dispatcher. Files referenced in params (after the dispatcher's upload
// preprocess) are read here into the request's file list.

#include "chat/agent.hpp"   // AgentRequest, AgentFile
#include "chat/chat.hpp"

#include <cstdint>
#include <string>

namespace mirobody { namespace chat {

class Packet;

// kOpChat: run a registered agent for a turn.
struct ChatParams {
    std::string  agent;       // which agent to run
    AgentRequest request;     // the turn handed to Chat::response

    // Build from a packet. `user_id` is the JWT-verified caller. The rest of the
    // turn context -- session_id, timezone, ... -- is read from the packet, where
    // the transport has stamped the header/query values over any body fields.
    // Synthesizes a single user message from "question" when no "messages" array
    // is given.
    static ChatParams parse(const Packet& pkt, std::int64_t user_id);
};

}}
