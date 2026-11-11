#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <safetyhook.hpp>
#include "scan.h"

namespace livemap::chat {

struct Config {
    bool enabled = false;
    std::string webhook_url;
    std::string bot_name = "Enshrouded";
};

// Initializes the chat bridge with configuration.
void init(const Config& config);

// Installs the chat_post hook if the site is available.
bool install(const Targets& targets);

// Shuts down the background worker thread.
void shutdown();

// Enqueues an inbound message from Discord or REST API to be posted in-game.
void queue_inbound(std::string sender, std::string message);

// Called on server game tick (in on_player_table) to dequeue and post chat lines.
void on_tick(uintptr_t manager);

// Formats recent messages as a JSON array string.
std::string recent_messages_json();

// Parses incoming JSON payload extracting sender and message fields.
void parse_chat_payload(std::string_view json, std::string& sender, std::string& message);

}  // namespace livemap::chat
