#pragma once

#include <string>
#include <functional>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <unordered_map>
#include <variant>

// Forward declare httplib types
namespace httplib {
    class Server;
}

// JSON-like value type for MCP parameters/results
struct MCPValue {
    std::variant<
        std::nullptr_t,
        bool,
        int,
        float,
        std::string,
        std::vector<MCPValue>,
        std::unordered_map<std::string, MCPValue>
    > data;

    MCPValue() : data(nullptr) {}
    MCPValue(bool v) : data(v) {}
    MCPValue(int v) : data(v) {}
    MCPValue(float v) : data(v) {}
    MCPValue(const std::string& v) : data(v) {}
    MCPValue(const char* v) : data(std::string(v)) {}

    bool isNull() const { return std::holds_alternative<std::nullptr_t>(data); }
    bool getBool() const { return std::get<bool>(data); }
    int getInt() const {
        if (std::holds_alternative<int>(data)) return std::get<int>(data);
        if (std::holds_alternative<float>(data)) return static_cast<int>(std::get<float>(data));
        return 0;
    }
    float getFloat() const {
        if (std::holds_alternative<float>(data)) return std::get<float>(data);
        if (std::holds_alternative<int>(data)) return static_cast<float>(std::get<int>(data));
        return 0.0f;
    }
    const std::string& getString() const { return std::get<std::string>(data); }
};

using MCPParams = std::unordered_map<std::string, MCPValue>;
using MCPResult = std::unordered_map<std::string, MCPValue>;

// Command queued for main thread execution
struct MCPCommand {
    std::string method;
    MCPParams params;
    std::function<void(const MCPResult&)> callback;
};

/**
 * MCP Server for LIME editor
 *
 * Runs an HTTP server that accepts JSON-RPC style commands.
 * Commands are queued and executed on the main thread.
 *
 * Usage:
 *   1. Create MCPServer with a port
 *   2. Register tools with registerTool()
 *   3. Call start() to begin listening
 *   4. Call processCommands() each frame from main thread
 *   5. Call stop() on shutdown
 */
class MCPServer {
public:
    using ToolHandler = std::function<MCPResult(const MCPParams&)>;

    MCPServer(int port = 9999);
    ~MCPServer();

    // Register a tool that can be called via MCP
    void registerTool(const std::string& name,
                      const std::string& description,
                      ToolHandler handler);

    // Start the server (launches background thread)
    bool start();

    // Stop the server
    void stop();

    // Process any pending commands (call from main thread each frame)
    void processCommands();

    // Check if server is running
    bool isRunning() const { return m_running; }

    // Get the port
    int getPort() const { return m_port; }

private:
    void serverThread();
    std::string handleRequest(const std::string& json);
    std::string toolsToJson();

    int m_port;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    std::unique_ptr<httplib::Server> m_server;

    // Registered tools
    struct ToolInfo {
        std::string description;
        ToolHandler handler;
    };
    std::unordered_map<std::string, ToolInfo> m_tools;

    // Command queue (thread-safe)
    std::queue<MCPCommand> m_commandQueue;
    std::mutex m_queueMutex;

    // Response storage for sync calls
    std::unordered_map<int, MCPResult> m_responses;
    std::mutex m_responseMutex;
    std::condition_variable m_responseCV;
    int m_nextRequestId = 1;
};
