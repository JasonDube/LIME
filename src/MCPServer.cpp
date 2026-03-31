#include "MCPServer.hpp"
#include <httplib.h>
#include <iostream>
#include <sstream>

// Simple JSON parsing helpers (avoiding external dependency)
namespace {

std::string escapeJson(const std::string& s) {
    std::string result;
    for (char c : s) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c;
        }
    }
    return result;
}

// Skip whitespace
void skipWhitespace(const std::string& s, size_t& pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r')) {
        ++pos;
    }
}

// Parse a JSON string (assumes pos is at opening quote)
std::string parseString(const std::string& s, size_t& pos) {
    if (pos >= s.size() || s[pos] != '"') return "";
    ++pos;
    std::string result;
    while (pos < s.size() && s[pos] != '"') {
        if (s[pos] == '\\' && pos + 1 < s.size()) {
            ++pos;
            switch (s[pos]) {
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                default: result += s[pos]; break;
            }
        } else {
            result += s[pos];
        }
        ++pos;
    }
    if (pos < s.size()) ++pos; // skip closing quote
    return result;
}

// Simple JSON parser for flat objects
MCPParams parseJsonObject(const std::string& json) {
    MCPParams result;

    size_t pos = json.find('{');
    if (pos == std::string::npos) return result;
    ++pos;

    while (pos < json.size()) {
        skipWhitespace(json, pos);
        if (pos >= json.size() || json[pos] == '}') break;

        // Parse key
        if (json[pos] != '"') break;
        std::string key = parseString(json, pos);

        skipWhitespace(json, pos);
        if (pos >= json.size() || json[pos] != ':') break;
        ++pos;
        skipWhitespace(json, pos);

        // Parse value
        if (pos >= json.size()) break;

        if (json[pos] == '"') {
            result[key] = MCPValue(parseString(json, pos));
        } else if (json[pos] == 't' && json.substr(pos, 4) == "true") {
            result[key] = MCPValue(true);
            pos += 4;
        } else if (json[pos] == 'f' && json.substr(pos, 5) == "false") {
            result[key] = MCPValue(false);
            pos += 5;
        } else if (json[pos] == 'n' && json.substr(pos, 4) == "null") {
            result[key] = MCPValue();
            pos += 4;
        } else if (json[pos] == '-' || (json[pos] >= '0' && json[pos] <= '9')) {
            // Number
            size_t numStart = pos;
            bool isFloat = false;
            if (json[pos] == '-') ++pos;
            while (pos < json.size() && ((json[pos] >= '0' && json[pos] <= '9') || json[pos] == '.')) {
                if (json[pos] == '.') isFloat = true;
                ++pos;
            }
            std::string numStr = json.substr(numStart, pos - numStart);
            if (isFloat) {
                result[key] = MCPValue(std::stof(numStr));
            } else {
                result[key] = MCPValue(std::stoi(numStr));
            }
        }

        skipWhitespace(json, pos);
        if (pos < json.size() && json[pos] == ',') ++pos;
    }

    return result;
}

std::string mcpResultToJson(const MCPResult& result) {
    std::ostringstream ss;
    ss << "{";
    bool first = true;
    for (const auto& [key, value] : result) {
        if (!first) ss << ",";
        first = false;
        ss << "\"" << key << "\":";

        if (value.isNull()) {
            ss << "null";
        } else if (std::holds_alternative<bool>(value.data)) {
            ss << (std::get<bool>(value.data) ? "true" : "false");
        } else if (std::holds_alternative<int>(value.data)) {
            ss << std::get<int>(value.data);
        } else if (std::holds_alternative<float>(value.data)) {
            ss << std::get<float>(value.data);
        } else if (std::holds_alternative<std::string>(value.data)) {
            ss << "\"" << escapeJson(std::get<std::string>(value.data)) << "\"";
        }
    }
    ss << "}";
    return ss.str();
}

} // anonymous namespace

MCPServer::MCPServer(int port) : m_port(port) {
}

MCPServer::~MCPServer() {
    stop();
}

void MCPServer::registerTool(const std::string& name,
                             const std::string& description,
                             ToolHandler handler) {
    m_tools[name] = {description, handler};
    std::cout << "[MCP] Registered tool: " << name << std::endl;
}

bool MCPServer::start() {
    if (m_running) return true;

    m_server = std::make_unique<httplib::Server>();

    // List available tools
    m_server->Get("/tools", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_content(toolsToJson(), "application/json");
    });

    // Execute a tool
    m_server->Post("/execute", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string response = handleRequest(req.body);
        res.set_content(response, "application/json");
    });

    // CORS preflight
    m_server->Options(".*", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 204;
    });

    // Health check
    m_server->Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_content("{\"status\":\"ok\",\"server\":\"LIME MCP\"}", "application/json");
    });

    m_running = true;
    m_thread = std::thread(&MCPServer::serverThread, this);

    std::cout << "[MCP] Server starting on port " << m_port << std::endl;
    return true;
}

void MCPServer::stop() {
    if (!m_running) return;

    m_running = false;
    if (m_server) {
        m_server->stop();
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_server.reset();
    std::cout << "[MCP] Server stopped" << std::endl;
}

void MCPServer::serverThread() {
    if (m_server) {
        std::cout << "[MCP] Server listening on http://localhost:" << m_port << std::endl;
        m_server->listen("0.0.0.0", m_port);
    }
}

void MCPServer::processCommands() {
    // Process all pending commands on main thread
    std::queue<MCPCommand> toProcess;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        std::swap(toProcess, m_commandQueue);
    }

    while (!toProcess.empty()) {
        MCPCommand cmd = std::move(toProcess.front());
        toProcess.pop();

        auto it = m_tools.find(cmd.method);
        if (it != m_tools.end()) {
            MCPResult result = it->second.handler(cmd.params);
            if (cmd.callback) {
                cmd.callback(result);
            }
        }
    }
}

std::string MCPServer::handleRequest(const std::string& json) {
    // Parse the request
    MCPParams request = parseJsonObject(json);

    std::string method;
    MCPParams params;

    // Get method name
    auto methodIt = request.find("method");
    if (methodIt != request.end() && std::holds_alternative<std::string>(methodIt->second.data)) {
        method = std::get<std::string>(methodIt->second.data);
    }

    // For now, parse params from the flat request (simple approach)
    // In a full implementation, we'd parse nested "params" object
    params = request;
    params.erase("method");

    // Check if tool exists
    auto toolIt = m_tools.find(method);
    if (toolIt == m_tools.end()) {
        return "{\"error\":\"Unknown method: " + method + "\",\"available_tools\":" + toolsToJson() + "}";
    }

    // Execute synchronously for now (tools should be fast)
    // For slow operations, we'd queue and return a job ID
    try {
        MCPResult result = toolIt->second.handler(params);
        return "{\"success\":true,\"result\":" + mcpResultToJson(result) + "}";
    } catch (const std::exception& e) {
        return "{\"error\":\"" + escapeJson(e.what()) + "\"}";
    }
}

std::string MCPServer::toolsToJson() {
    std::ostringstream ss;
    ss << "[";
    bool first = true;
    for (const auto& [name, info] : m_tools) {
        if (!first) ss << ",";
        first = false;
        ss << "{\"name\":\"" << name << "\",\"description\":\"" << escapeJson(info.description) << "\"}";
    }
    ss << "]";
    return ss.str();
}
