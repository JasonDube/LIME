#pragma once

#include <httplib.h>
#include <string>
#include <fstream>
#include <vector>
#include <iostream>

/**
 * @brief Simple HTTP client for Hunyuan3D-2 API server
 *
 * Wraps cpp-httplib Client for /send and /status/{uid} endpoints.
 * The API server runs separately (api_server.py on port 8081).
 */
class Hunyuan3DClient {
public:
    Hunyuan3DClient(const std::string& host = "localhost", int port = 8081)
        : m_host(host), m_port(port) {}

    /**
     * @brief Check if the Hunyuan server is reachable
     */
    bool isServerRunning() {
        httplib::Client cli(m_host, m_port);
        cli.set_connection_timeout(2);
        auto res = cli.Get("/status/test");
        return res != nullptr;
    }

    /**
     * @brief Start async generation via /send endpoint
     * @param prompt Text prompt (used as text2image input)
     * @param imageBase64 Optional base64-encoded image (if provided, prompt is ignored)
     * @param steps Number of inference steps
     * @param octreeResolution Octree resolution for shape generation
     * @param guidanceScale Guidance scale
     * @param maxFaces Maximum face count for mesh reduction
     * @param texture Whether to generate texture
     * @param seed Random seed
     * @return Job UID string, or empty on failure
     */
    std::string startGeneration(
        const std::string& prompt,
        const std::string& imageBase64,
        int steps = 5,
        int octreeResolution = 256,
        float guidanceScale = 5.0f,
        int maxFaces = 10000,
        bool texture = true,
        int seed = 12345,
        int textureSize = 1024,
        bool removeBackground = true,
        bool multiView = false,
        const std::string& leftBase64 = "",
        const std::string& rightBase64 = "",
        const std::string& backBase64 = ""
    ) {
        httplib::Client cli(m_host, m_port);
        cli.set_connection_timeout(10);
        cli.set_read_timeout(60);
        cli.set_write_timeout(60);

        // Build JSON body
        std::string body = "{";
        if (!imageBase64.empty()) {
            if (multiView) {
                // Multi-view: send as {"image_front": ..., "image_left": ..., etc}
                body += "\"image_front\":\"" + imageBase64 + "\",";
                if (!leftBase64.empty())  body += "\"image_left\":\"" + leftBase64 + "\",";
                if (!rightBase64.empty()) body += "\"image_right\":\"" + rightBase64 + "\",";
                if (!backBase64.empty())  body += "\"image_back\":\"" + backBase64 + "\",";
                body += "\"multiview\":true,";
            } else {
                body += "\"image\":\"" + imageBase64 + "\",";
            }
        } else if (!prompt.empty()) {
            body += "\"text\":\"" + escapeJson(prompt) + "\",";
        }
        body += "\"num_inference_steps\":" + std::to_string(steps) + ",";
        body += "\"octree_resolution\":" + std::to_string(octreeResolution) + ",";
        body += "\"guidance_scale\":" + std::to_string(guidanceScale) + ",";
        body += "\"face_count\":" + std::to_string(maxFaces) + ",";
        body += "\"texture\":" + std::string(texture ? "true" : "false") + ",";
        body += "\"texture_size\":" + std::to_string(textureSize) + ",";
        body += "\"remove_bg\":" + std::string(removeBackground ? "true" : "false") + ",";
        body += "\"seed\":" + std::to_string(seed) + ",";
        body += "\"num_chunks\":8000,";
        body += "\"type\":\"glb\"";
        body += "}";

        std::cout << "[Hunyuan3D] POST /send body size: " << body.size() << " bytes"
                  << ", multiview=" << (multiView ? "true" : "false")
                  << ", has_front=" << (!imageBase64.empty() ? "yes" : "no")
                  << ", has_left=" << (!leftBase64.empty() ? "yes" : "no")
                  << ", has_right=" << (!rightBase64.empty() ? "yes" : "no")
                  << ", has_back=" << (!backBase64.empty() ? "yes" : "no")
                  << std::endl;

        auto res = cli.Post("/send", body, "application/json");
        if (!res || res->status != 200) {
            std::cerr << "[Hunyuan3D] /send failed" << (res ? " status=" + std::to_string(res->status) : " (no response)") << std::endl;
            return "";
        }

        // Parse UID from response: {"uid": "..."}
        auto uidPos = res->body.find("\"uid\"");
        if (uidPos == std::string::npos) return "";
        auto colonPos = res->body.find(':', uidPos);
        auto quoteStart = res->body.find('"', colonPos + 1);
        auto quoteEnd = res->body.find('"', quoteStart + 1);
        if (quoteStart == std::string::npos || quoteEnd == std::string::npos) return "";

        std::string uid = res->body.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
        std::cout << "[Hunyuan3D] Job started: " << uid << std::endl;
        return uid;
    }

    /**
     * @brief Check status of a generation job
     * @param uid Job UID
     * @param outBase64GLB Output: base64-encoded GLB data if completed
     * @return "processing", "completed", or "error"
     */
    std::string checkStatus(const std::string& uid, std::string& outBase64GLB) {
        httplib::Client cli(m_host, m_port);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(120);  // Texture gen can block server for 60+ seconds

        auto res = cli.Get(("/status/" + uid).c_str());
        if (!res) {
            // Server busy or unreachable — not a generation error, just can't poll
            return "processing";
        }
        if (res->status != 200) {
            return "error";
        }

        // Parse status
        if (res->body.find("\"status\":\"error\"") != std::string::npos) {
            return "error";
        }
        if (res->body.find("\"completed\"") != std::string::npos) {
            // Extract base64 model data
            auto key = std::string("\"model_base64\"");
            auto pos = res->body.find(key);
            if (pos != std::string::npos) {
                auto colonPos = res->body.find(':', pos);
                auto quoteStart = res->body.find('"', colonPos + 1);
                auto quoteEnd = res->body.rfind('"');
                if (quoteStart != std::string::npos && quoteEnd > quoteStart) {
                    outBase64GLB = res->body.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
                }
            }
            return "completed";
        }
        return "processing";
    }

    /**
     * @brief Fetch new log lines from server since last check
     * @param sinceIndex Index to start from (tracks position)
     * @param outLines New lines appended here
     * @return New total line count, or -1 on error
     */
    int fetchLog(int sinceIndex, std::vector<std::string>& outLines) {
        httplib::Client cli(m_host, m_port);
        cli.set_connection_timeout(2);
        cli.set_read_timeout(5);

        auto res = cli.Get(("/log?since=" + std::to_string(sinceIndex)).c_str());
        if (!res || res->status != 200) return -1;

        const std::string& body = res->body;

        // Parse "total": N
        int total = sinceIndex;
        auto totalPos = body.find("\"total\"");
        if (totalPos != std::string::npos) {
            auto colonPos = body.find(':', totalPos);
            if (colonPos != std::string::npos) {
                total = std::atoi(body.c_str() + colonPos + 1);
            }
        }

        // Parse "lines": [...] — extract quoted strings from the array
        // Strategy: find "lines":[ then extract each quoted string by scanning
        // forward, properly skipping over quoted content (which may contain [ ] chars)
        auto linesKey = body.find("\"lines\"");
        if (linesKey == std::string::npos) return total;
        auto arrOpen = body.find('[', linesKey);
        if (arrOpen == std::string::npos) return total;

        // Walk forward from array open, extracting quoted strings until we hit
        // an unquoted ] which closes the array
        size_t pos = arrOpen + 1;
        while (pos < body.size()) {
            // Skip whitespace and commas
            while (pos < body.size() && (body[pos] == ' ' || body[pos] == ',' || body[pos] == '\n')) pos++;
            if (pos >= body.size()) break;

            // Check for array close (outside any quote)
            if (body[pos] == ']') break;

            // Expect opening quote
            if (body[pos] != '"') { pos++; continue; }

            // Find matching close quote
            size_t q1 = pos;
            size_t q2 = q1 + 1;
            while (q2 < body.size()) {
                if (body[q2] == '"') {
                    // Check not escaped (count preceding backslashes)
                    int bs = 0;
                    size_t c = q2 - 1;
                    while (c > q1 && body[c] == '\\') { bs++; c--; }
                    if (bs % 2 == 0) break;
                }
                q2++;
            }
            if (q2 >= body.size()) break;

            outLines.push_back(body.substr(q1 + 1, q2 - q1 - 1));
            pos = q2 + 1;
        }

        return total;
    }

    /**
     * @brief Base64 encode a file (for sending images)
     */
    static std::string base64EncodeFile(const std::string& filepath) {
        std::ifstream file(filepath, std::ios::binary);
        if (!file.is_open()) return "";

        std::vector<unsigned char> data((std::istreambuf_iterator<char>(file)),
                                         std::istreambuf_iterator<char>());
        return base64Encode(data.data(), data.size());
    }

    /**
     * @brief Decode base64 data and write to file
     */
    static bool base64DecodeToFile(const std::string& base64Data, const std::string& outputPath) {
        std::vector<unsigned char> decoded = base64Decode(base64Data);
        if (decoded.empty()) return false;

        std::ofstream file(outputPath, std::ios::binary);
        if (!file.is_open()) return false;
        file.write(reinterpret_cast<const char*>(decoded.data()), decoded.size());
        return true;
    }

private:
    std::string m_host;
    int m_port;

    static std::string escapeJson(const std::string& s) {
        std::string result;
        for (char c : s) {
            if (c == '"') result += "\\\"";
            else if (c == '\\') result += "\\\\";
            else if (c == '\n') result += "\\n";
            else result += c;
        }
        return result;
    }

    static std::string base64Encode(const unsigned char* data, size_t len) {
        static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string result;
        result.reserve(((len + 2) / 3) * 4);
        for (size_t i = 0; i < len; i += 3) {
            unsigned int n = static_cast<unsigned int>(data[i]) << 16;
            if (i + 1 < len) n |= static_cast<unsigned int>(data[i + 1]) << 8;
            if (i + 2 < len) n |= static_cast<unsigned int>(data[i + 2]);
            result += table[(n >> 18) & 0x3F];
            result += table[(n >> 12) & 0x3F];
            result += (i + 1 < len) ? table[(n >> 6) & 0x3F] : '=';
            result += (i + 2 < len) ? table[n & 0x3F] : '=';
        }
        return result;
    }

    static std::vector<unsigned char> base64Decode(const std::string& encoded) {
        static const int table[256] = {
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
            52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
            -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
            15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
            -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
            41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        };
        std::vector<unsigned char> result;
        result.reserve(encoded.size() * 3 / 4);
        unsigned int val = 0;
        int bits = -8;
        for (unsigned char c : encoded) {
            if (table[c] == -1) continue;
            val = (val << 6) + table[c];
            bits += 6;
            if (bits >= 0) {
                result.push_back(static_cast<unsigned char>((val >> bits) & 0xFF));
                bits -= 8;
            }
        }
        return result;
    }
};
