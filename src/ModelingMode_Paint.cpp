// ModelingMode_Paint.cpp - Image Reference/Paint functions for ModelingMode
// Split from ModelingMode.cpp for better organization

#include "ModelingMode.hpp"

#include <imgui.h>
#include <nfd.h>
#include <stb_image.h>

#include <iostream>
#include <filesystem>
#include <cmath>
#include <algorithm>

using namespace eden;

void ModelingMode::renderImageRefWindow() {
    ImGui::SetNextWindowSize(ImVec2(400, 450), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Image References", &m_ctx.showImageRefWindow)) {
        // Load image button
        if (ImGui::Button("Load Image...")) {
            nfdchar_t* outPath = nullptr;
            nfdfilteritem_t filters[1] = {{"Image", "png,jpg,jpeg,bmp,tga"}};
            if (NFD_OpenDialog(&outPath, filters, 1, nullptr) == NFD_OKAY) {
                int w, h, channels;
                unsigned char* data = stbi_load(outPath, &w, &h, &channels, 4);
                if (data) {
                    // Reserve space to prevent reallocation invalidating existing textures
                    if (m_ctx.cloneSourceImages.size() == m_ctx.cloneSourceImages.capacity()) {
                        m_ctx.cloneSourceImages.reserve(m_ctx.cloneSourceImages.capacity() + 10);
                    }

                    // Create and add the new image
                    m_ctx.cloneSourceImages.emplace_back();
                    auto& img = m_ctx.cloneSourceImages.back();
                    img.filepath = outPath;
                    // Extract filename for display name
                    std::string path = outPath;
                    size_t lastSlash = path.find_last_of("/\\");
                    img.name = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
                    img.width = w;
                    img.height = h;
                    img.pixelData.assign(data, data + w * h * 4);
                    stbi_image_free(data);

                    // Create Vulkan texture for the new image
                    if (m_ctx.createCloneImageTextureCallback) {
                        m_ctx.createCloneImageTextureCallback(img);
                    }

                    std::cout << "Loaded image " << m_ctx.cloneSourceImages.size()
                              << ": " << w << "x" << h
                              << " descriptor=" << img.descriptorSet << std::endl;
                }
                NFD_FreePath(outPath);
            }
        }

        // Image list
        ImGui::SameLine();
        ImGui::TextDisabled("(%d images)", static_cast<int>(m_ctx.cloneSourceImages.size()));

        if (m_ctx.cloneSourceImages.empty()) {
            ImGui::TextDisabled("No images loaded. Click 'Load Image...' to add.");
        } else {
            // Tabs for each loaded image
            if (ImGui::BeginTabBar("ImageTabs")) {
                for (size_t i = 0; i < m_ctx.cloneSourceImages.size(); ++i) {
                    auto& img = m_ctx.cloneSourceImages[i];

                    // Use index-based unique ID for tab
                    ImGui::PushID(static_cast<int>(i));
                    bool open = true;
                    if (ImGui::BeginTabItem(img.name.c_str(), &open)) {
                        m_ctx.imageRefSelectedIndex = static_cast<int>(i);

                        // Image info
                        ImGui::TextDisabled("%dx%d", img.width, img.height);
                        ImGui::SameLine();
                        ImGui::Text("Zoom: %.0f%%", m_ctx.imageRefZoom * 100.0f);
                        ImGui::SameLine();
                        if (ImGui::Button("Reset View")) {
                            m_ctx.imageRefZoom = 1.0f;
                            m_ctx.imageRefPan = glm::vec2(0.0f);
                        }

                        // Perspective correction toggle
                        ImGui::SameLine();
                        bool perspActive = m_perspectiveMode && m_perspectiveImageIdx == static_cast<int>(i);
                        if (ImGui::Checkbox("Perspective", &perspActive)) {
                            if (perspActive) {
                                m_perspectiveMode = true;
                                m_perspectiveImageIdx = static_cast<int>(i);
                                m_perspectiveCornerCount = 0;
                            } else {
                                m_perspectiveMode = false;
                                m_perspectiveCornerCount = 0;
                            }
                        }
                        if (m_perspectiveMode && m_perspectiveImageIdx == static_cast<int>(i)) {
                            ImGui::SameLine();
                            ImGui::TextDisabled("(%d/4 corners)", m_perspectiveCornerCount);
                            if (m_perspectiveCornerCount > 0) {
                                ImGui::SameLine();
                                if (ImGui::Button("Clear")) {
                                    m_perspectiveCornerCount = 0;
                                }
                            }
                            if (m_perspectiveCornerCount == 4) {
                                ImGui::SameLine();
                                if (ImGui::Button("Straighten")) {
                                    // Perform perspective correction and create stamp
                                    createPerspectiveCorrectedStamp(img);
                                    m_perspectiveMode = false;
                                    m_perspectiveCornerCount = 0;
                                }
                            }
                        }

                        ImGui::Separator();

                        // Image display area with pan/zoom
                        ImVec2 canvasSize = ImGui::GetContentRegionAvail();
                        canvasSize.y = std::max(canvasSize.y, 200.0f);

                        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
                        ImDrawList* drawList = ImGui::GetWindowDrawList();

                        // Background
                        drawList->AddRectFilled(canvasPos,
                            ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                            IM_COL32(40, 40, 40, 255));

                        // Calculate image display rect with zoom and pan
                        float imgAspect = static_cast<float>(img.width) / img.height;
                        float canvasAspect = canvasSize.x / canvasSize.y;

                        float displayW, displayH;
                        if (imgAspect > canvasAspect) {
                            displayW = canvasSize.x * m_ctx.imageRefZoom;
                            displayH = displayW / imgAspect;
                        } else {
                            displayH = canvasSize.y * m_ctx.imageRefZoom;
                            displayW = displayH * imgAspect;
                        }

                        float imgX = canvasPos.x + (canvasSize.x - displayW) * 0.5f + m_ctx.imageRefPan.x;
                        float imgY = canvasPos.y + (canvasSize.y - displayH) * 0.5f + m_ctx.imageRefPan.y;

                        // Draw the actual image if texture is loaded
                        if (img.descriptorSet) {
                            // Clip to canvas bounds
                            drawList->PushClipRect(canvasPos,
                                ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), true);

                            drawList->AddImage(
                                (ImTextureID)img.descriptorSet,
                                ImVec2(imgX, imgY),
                                ImVec2(imgX + displayW, imgY + displayH)
                            );

                            drawList->PopClipRect();
                        } else {
                            // Fallback: show placeholder text if texture not ready
                            ImVec2 textPos(canvasPos.x + 10, canvasPos.y + canvasSize.y / 2 - 10);
                            drawList->AddText(textPos, IM_COL32(200, 200, 200, 255),
                                ("Loading: " + std::to_string(img.width) + "x" + std::to_string(img.height)).c_str());
                        }

                        // Border
                        drawList->AddRect(canvasPos,
                            ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                            IM_COL32(80, 80, 80, 255));

                        // Invisible button for interaction (unique ID per tab)
                        ImGui::SetCursorScreenPos(canvasPos);
                        ImGui::PushID(static_cast<int>(i));
                        ImGui::InvisibleButton("##imageCanvas", canvasSize);
                        ImGui::PopID();

                        bool isHovered = ImGui::IsItemHovered();

                        // Pan with MMB
                        if (isHovered && ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
                            ImVec2 delta = ImGui::GetIO().MouseDelta;
                            m_ctx.imageRefPan.x += delta.x;
                            m_ctx.imageRefPan.y += delta.y;
                        }

                        // Zoom with mousewheel
                        if (isHovered) {
                            float wheel = ImGui::GetIO().MouseWheel;
                            if (wheel != 0.0f) {
                                float zoomFactor = 1.0f + wheel * 0.1f;
                                m_ctx.imageRefZoom = std::clamp(m_ctx.imageRefZoom * zoomFactor, 0.1f, 10.0f);
                            }
                        }

                        // Perspective corner placement
                        if (m_perspectiveMode && m_perspectiveImageIdx == static_cast<int>(i) &&
                            isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyAlt) {
                            if (m_perspectiveCornerCount < 4) {
                                ImVec2 mousePos = ImGui::GetMousePos();
                                float relX = std::clamp((mousePos.x - imgX) / displayW, 0.0f, 1.0f);
                                float relY = std::clamp((mousePos.y - imgY) / displayH, 0.0f, 1.0f);
                                m_perspectiveCorners[m_perspectiveCornerCount] = glm::vec2(relX * img.width, relY * img.height);
                                m_perspectiveCornerCount++;
                                std::cout << "Placed perspective corner " << m_perspectiveCornerCount << " at ("
                                          << m_perspectiveCorners[m_perspectiveCornerCount-1].x << ", "
                                          << m_perspectiveCorners[m_perspectiveCornerCount-1].y << ")" << std::endl;
                            }
                        }

                        // Draw perspective corners and quad
                        if (m_perspectiveMode && m_perspectiveImageIdx == static_cast<int>(i) && m_perspectiveCornerCount > 0) {
                            drawList->PushClipRect(canvasPos,
                                ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), true);

                            // Draw placed corners
                            for (int c = 0; c < m_perspectiveCornerCount; c++) {
                                float cx = imgX + (m_perspectiveCorners[c].x / img.width) * displayW;
                                float cy = imgY + (m_perspectiveCorners[c].y / img.height) * displayH;

                                // Corner circle
                                drawList->AddCircleFilled(ImVec2(cx, cy), 8.0f, IM_COL32(255, 100, 0, 255));
                                drawList->AddCircle(ImVec2(cx, cy), 8.0f, IM_COL32(255, 255, 255, 255), 0, 2.0f);

                                // Corner number
                                char numStr[8];
                                snprintf(numStr, sizeof(numStr), "%d", c + 1);
                                drawList->AddText(ImVec2(cx - 4, cy - 6), IM_COL32(255, 255, 255, 255), numStr);
                            }

                            // Draw lines between corners
                            if (m_perspectiveCornerCount >= 2) {
                                for (int c = 0; c < m_perspectiveCornerCount - 1; c++) {
                                    float x1 = imgX + (m_perspectiveCorners[c].x / img.width) * displayW;
                                    float y1 = imgY + (m_perspectiveCorners[c].y / img.height) * displayH;
                                    float x2 = imgX + (m_perspectiveCorners[c+1].x / img.width) * displayW;
                                    float y2 = imgY + (m_perspectiveCorners[c+1].y / img.height) * displayH;
                                    drawList->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(255, 100, 0, 255), 2.0f);
                                }
                            }

                            // Close the quad if all 4 corners placed
                            if (m_perspectiveCornerCount == 4) {
                                float x1 = imgX + (m_perspectiveCorners[3].x / img.width) * displayW;
                                float y1 = imgY + (m_perspectiveCorners[3].y / img.height) * displayH;
                                float x2 = imgX + (m_perspectiveCorners[0].x / img.width) * displayW;
                                float y2 = imgY + (m_perspectiveCorners[0].y / img.height) * displayH;
                                drawList->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(255, 100, 0, 255), 2.0f);

                                // Fill quad with semi-transparent orange
                                ImVec2 quadPts[4];
                                for (int c = 0; c < 4; c++) {
                                    quadPts[c] = ImVec2(
                                        imgX + (m_perspectiveCorners[c].x / img.width) * displayW,
                                        imgY + (m_perspectiveCorners[c].y / img.height) * displayH
                                    );
                                }
                                drawList->AddQuadFilled(quadPts[0], quadPts[1], quadPts[2], quadPts[3], IM_COL32(255, 100, 0, 40));
                            }

                            drawList->PopClipRect();
                        }

                        // Shift+Drag to select rectangle for stamp
                        if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::GetIO().KeyShift) {
                            ImVec2 mousePos = ImGui::GetMousePos();
                            float relX = std::clamp((mousePos.x - imgX) / displayW, 0.0f, 1.0f);
                            float relY = std::clamp((mousePos.y - imgY) / displayH, 0.0f, 1.0f);

                            m_stampSelectStart = glm::vec2(relX * img.width, relY * img.height);
                            m_stampSelectEnd = m_stampSelectStart;
                            m_stampSelectImageIdx = static_cast<int>(i);
                            m_stampSelecting = true;
                        }

                        // Update selection while dragging
                        if (m_stampSelecting && m_stampSelectImageIdx == static_cast<int>(i) && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                            ImVec2 mousePos = ImGui::GetMousePos();
                            float relX = std::clamp((mousePos.x - imgX) / displayW, 0.0f, 1.0f);
                            float relY = std::clamp((mousePos.y - imgY) / displayH, 0.0f, 1.0f);
                            m_stampSelectEnd = glm::vec2(relX * img.width, relY * img.height);
                        }

                        // Finish selection and create stamp
                        if (m_stampSelecting && m_stampSelectImageIdx == static_cast<int>(i) && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                            m_stampSelecting = false;

                            // Calculate selection bounds
                            int x1 = static_cast<int>(std::min(m_stampSelectStart.x, m_stampSelectEnd.x));
                            int y1 = static_cast<int>(std::min(m_stampSelectStart.y, m_stampSelectEnd.y));
                            int x2 = static_cast<int>(std::max(m_stampSelectStart.x, m_stampSelectEnd.x));
                            int y2 = static_cast<int>(std::max(m_stampSelectStart.y, m_stampSelectEnd.y));

                            int selW = x2 - x1;
                            int selH = y2 - y1;

                            if (selW > 2 && selH > 2 && !img.pixelData.empty()) {
                                // Extract pixels from source image
                                m_ctx.stampData.resize(selW * selH * 4);
                                m_ctx.stampWidth = selW;
                                m_ctx.stampHeight = selH;

                                for (int sy = 0; sy < selH; sy++) {
                                    for (int sx = 0; sx < selW; sx++) {
                                        int srcX = x1 + sx;
                                        int srcY = y1 + sy;

                                        if (srcX >= 0 && srcX < img.width && srcY >= 0 && srcY < img.height) {
                                            size_t srcIdx = (srcY * img.width + srcX) * 4;
                                            size_t dstIdx = (sy * selW + sx) * 4;

                                            if (srcIdx + 3 < img.pixelData.size()) {
                                                m_ctx.stampData[dstIdx] = img.pixelData[srcIdx];
                                                m_ctx.stampData[dstIdx + 1] = img.pixelData[srcIdx + 1];
                                                m_ctx.stampData[dstIdx + 2] = img.pixelData[srcIdx + 2];
                                                m_ctx.stampData[dstIdx + 3] = img.pixelData[srcIdx + 3];
                                            }
                                        }
                                    }
                                }

                                // Defer stamp preview update to next frame
                                m_pendingStampPreviewUpdate = true;

                                // Switch to stamp mode
                                m_ctx.useStamp = true;
                                m_ctx.useSmear = false;

                                std::cout << "Created stamp from selection: " << selW << "x" << selH << std::endl;
                            }
                        }

                        // Draw selection rectangle while selecting
                        if (m_stampSelecting && m_stampSelectImageIdx == static_cast<int>(i)) {
                            float rx1 = imgX + (std::min(m_stampSelectStart.x, m_stampSelectEnd.x) / img.width) * displayW;
                            float ry1 = imgY + (std::min(m_stampSelectStart.y, m_stampSelectEnd.y) / img.height) * displayH;
                            float rx2 = imgX + (std::max(m_stampSelectStart.x, m_stampSelectEnd.x) / img.width) * displayW;
                            float ry2 = imgY + (std::max(m_stampSelectStart.y, m_stampSelectEnd.y) / img.height) * displayH;

                            drawList->AddRect(ImVec2(rx1, ry1), ImVec2(rx2, ry2), IM_COL32(255, 255, 0, 255), 0.0f, 0, 2.0f);
                            drawList->AddRectFilled(ImVec2(rx1, ry1), ImVec2(rx2, ry2), IM_COL32(255, 255, 0, 50));
                        }

                        // Alt+Click to sample color from image reference
                        if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::GetIO().KeyAlt) {
                            ImVec2 mousePos = ImGui::GetMousePos();

                            // Convert screen position to image pixel coordinates
                            float relX = (mousePos.x - imgX) / displayW;
                            float relY = (mousePos.y - imgY) / displayH;

                            // Clamp to valid range
                            relX = std::clamp(relX, 0.0f, 1.0f);
                            relY = std::clamp(relY, 0.0f, 1.0f);

                            // Convert to pixel coordinates
                            int px = static_cast<int>(relX * img.width);
                            int py = static_cast<int>(relY * img.height);
                            px = std::clamp(px, 0, img.width - 1);
                            py = std::clamp(py, 0, img.height - 1);

                            // Sample color from image
                            size_t pixelIdx = (py * img.width + px) * 4;
                            if (pixelIdx + 2 < img.pixelData.size()) {
                                m_ctx.paintColor.r = img.pixelData[pixelIdx] / 255.0f;
                                m_ctx.paintColor.g = img.pixelData[pixelIdx + 1] / 255.0f;
                                m_ctx.paintColor.b = img.pixelData[pixelIdx + 2] / 255.0f;

                                std::cout << "Sampled color from " << img.name << " at (" << px << ", " << py << "): "
                                          << "RGB(" << static_cast<int>(m_ctx.paintColor.r * 255) << ", "
                                          << static_cast<int>(m_ctx.paintColor.g * 255) << ", "
                                          << static_cast<int>(m_ctx.paintColor.b * 255) << ")" << std::endl;
                            }
                        }

                        ImGui::EndTabItem();
                    }

                    ImGui::PopID();

                    // Handle tab close - mark for deletion (don't delete during iteration)
                    if (!open) {
                        m_pendingCloneImageDelete = static_cast<int>(i);
                    }
                }
                ImGui::EndTabBar();
            }

            // Note: deletion is deferred to update() to avoid ImGui draw list issues
        }

        ImGui::Separator();

        ImGui::TextDisabled("MMB: Pan | Scroll: Zoom");
        ImGui::TextDisabled("Alt+Click: Clone source | Shift+Drag: Create stamp");
        if (m_ctx.cloneSourceSet && m_ctx.cloneSourceViewIndex >= 0 &&
            m_ctx.cloneSourceViewIndex < static_cast<int>(m_ctx.cloneSourceImages.size())) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Clone source: %s (%.0f, %.0f)",
                m_ctx.cloneSourceImages[m_ctx.cloneSourceViewIndex].name.c_str(),
                m_ctx.cloneSourcePixel.x, m_ctx.cloneSourcePixel.y);
        } else {
            ImGui::TextDisabled("Alt+Click on image to set clone source");
        }
        if (m_perspectiveMode) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Click to place corners (TL, TR, BR, BL order)");
        }
    }
    ImGui::End();
}

void ModelingMode::createPerspectiveCorrectedStamp(const CloneSourceImage& img) {
    // Compute bounding box of source quad to determine output size
    float minX = m_perspectiveCorners[0].x, maxX = m_perspectiveCorners[0].x;
    float minY = m_perspectiveCorners[0].y, maxY = m_perspectiveCorners[0].y;
    for (int i = 1; i < 4; i++) {
        minX = std::min(minX, m_perspectiveCorners[i].x);
        maxX = std::max(maxX, m_perspectiveCorners[i].x);
        minY = std::min(minY, m_perspectiveCorners[i].y);
        maxY = std::max(maxY, m_perspectiveCorners[i].y);
    }

    // Output size based on the larger dimension of the bounding box
    int outW = static_cast<int>(maxX - minX);
    int outH = static_cast<int>(maxY - minY);
    outW = std::max(outW, 8);
    outH = std::max(outH, 8);

    // Source corners (the perspective-distorted quad in image coords)
    // Expected order: TL, TR, BR, BL
    float srcPts[4][2] = {
        {m_perspectiveCorners[0].x, m_perspectiveCorners[0].y},
        {m_perspectiveCorners[1].x, m_perspectiveCorners[1].y},
        {m_perspectiveCorners[2].x, m_perspectiveCorners[2].y},
        {m_perspectiveCorners[3].x, m_perspectiveCorners[3].y}
    };

    // Destination corners (rectangle)
    float dstPts[4][2] = {
        {0.0f, 0.0f},
        {static_cast<float>(outW), 0.0f},
        {static_cast<float>(outW), static_cast<float>(outH)},
        {0.0f, static_cast<float>(outH)}
    };

    // Compute homography matrix H that maps dst -> src
    // Using Direct Linear Transform (DLT) method
    // For each point correspondence (x,y) -> (u,v), we have:
    // [ x  y  1  0  0  0 -ux -uy -u ]   [h1]
    // [ 0  0  0  x  y  1 -vx -vy -v ] * [h2] = 0
    //                                   [...]
    //                                   [h9]

    // Build 8x9 matrix A
    float A[8][9];
    for (int i = 0; i < 4; i++) {
        float x = dstPts[i][0], y = dstPts[i][1];
        float u = srcPts[i][0], v = srcPts[i][1];

        A[i*2][0] = x;  A[i*2][1] = y;  A[i*2][2] = 1;
        A[i*2][3] = 0;  A[i*2][4] = 0;  A[i*2][5] = 0;
        A[i*2][6] = -u*x; A[i*2][7] = -u*y; A[i*2][8] = -u;

        A[i*2+1][0] = 0;  A[i*2+1][1] = 0;  A[i*2+1][2] = 0;
        A[i*2+1][3] = x;  A[i*2+1][4] = y;  A[i*2+1][5] = 1;
        A[i*2+1][6] = -v*x; A[i*2+1][7] = -v*y; A[i*2+1][8] = -v;
    }

    // Solve for h using least squares (set h9 = 1)
    // Rearrange to: [A8x8] * [h1..h8] = [col9]
    float A8[8][8];
    float b8[8];
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            A8[i][j] = A[i][j];
        }
        b8[i] = -A[i][8];  // Move the h9 term to RHS (h9 = 1)
    }

    // Gaussian elimination with partial pivoting
    float h[9];
    h[8] = 1.0f;

    // Forward elimination
    for (int k = 0; k < 8; k++) {
        // Find pivot
        int maxRow = k;
        float maxVal = std::abs(A8[k][k]);
        for (int i = k + 1; i < 8; i++) {
            if (std::abs(A8[i][k]) > maxVal) {
                maxVal = std::abs(A8[i][k]);
                maxRow = i;
            }
        }

        // Swap rows
        if (maxRow != k) {
            for (int j = 0; j < 8; j++) std::swap(A8[k][j], A8[maxRow][j]);
            std::swap(b8[k], b8[maxRow]);
        }

        // Eliminate
        for (int i = k + 1; i < 8; i++) {
            if (std::abs(A8[k][k]) < 1e-10f) continue;
            float factor = A8[i][k] / A8[k][k];
            for (int j = k; j < 8; j++) {
                A8[i][j] -= factor * A8[k][j];
            }
            b8[i] -= factor * b8[k];
        }
    }

    // Back substitution
    for (int i = 7; i >= 0; i--) {
        h[i] = b8[i];
        for (int j = i + 1; j < 8; j++) {
            h[i] -= A8[i][j] * h[j];
        }
        if (std::abs(A8[i][i]) > 1e-10f) {
            h[i] /= A8[i][i];
        }
    }

    // Now h[] is our homography matrix (row-major): maps dst coords to src coords
    // H = [h0 h1 h2]
    //     [h3 h4 h5]
    //     [h6 h7 h8]

    // Warp the image
    m_ctx.stampData.resize(outW * outH * 4);
    m_ctx.stampWidth = outW;
    m_ctx.stampHeight = outH;

    for (int dy = 0; dy < outH; dy++) {
        for (int dx = 0; dx < outW; dx++) {
            // Apply homography to get source coordinates
            float w = h[6] * dx + h[7] * dy + h[8];
            if (std::abs(w) < 1e-10f) w = 1e-10f;

            float srcX = (h[0] * dx + h[1] * dy + h[2]) / w;
            float srcY = (h[3] * dx + h[4] * dy + h[5]) / w;

            // Bilinear interpolation
            int x0 = static_cast<int>(std::floor(srcX));
            int y0 = static_cast<int>(std::floor(srcY));
            int x1 = x0 + 1;
            int y1 = y0 + 1;

            float fx = srcX - x0;
            float fy = srcY - y0;

            // Clamp to image bounds
            x0 = std::clamp(x0, 0, img.width - 1);
            y0 = std::clamp(y0, 0, img.height - 1);
            x1 = std::clamp(x1, 0, img.width - 1);
            y1 = std::clamp(y1, 0, img.height - 1);

            // Sample 4 corners
            size_t idx00 = (y0 * img.width + x0) * 4;
            size_t idx01 = (y0 * img.width + x1) * 4;
            size_t idx10 = (y1 * img.width + x0) * 4;
            size_t idx11 = (y1 * img.width + x1) * 4;

            size_t dstIdx = (dy * outW + dx) * 4;

            for (int c = 0; c < 4; c++) {
                float v00 = img.pixelData[idx00 + c];
                float v01 = img.pixelData[idx01 + c];
                float v10 = img.pixelData[idx10 + c];
                float v11 = img.pixelData[idx11 + c];

                float v = v00 * (1-fx) * (1-fy) + v01 * fx * (1-fy) +
                          v10 * (1-fx) * fy + v11 * fx * fy;

                m_ctx.stampData[dstIdx + c] = static_cast<unsigned char>(std::clamp(v, 0.0f, 255.0f));
            }
        }
    }

    // Enable stamp mode
    m_pendingStampPreviewUpdate = true;
    m_ctx.useStamp = true;

    std::cout << "Created perspective-corrected stamp: " << outW << "x" << outH << std::endl;
}

