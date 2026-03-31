/**
 * ModelingMode_Ports.cpp — Port authoring UI and gizmo rendering.
 *
 * Ports are vertex-independent connection points with position, forward, and up
 * vectors. Used for pipe/assembly snap systems in the game engine.
 */

#include "ModelingMode.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <iostream>

using eden::SceneObject;

// ── UI Panel ──────────────────────────────────────────────

void ModelingMode::renderPortUI() {
    if (!m_ctx.selectedObject) return;

    ImGui::Separator();
    ImGui::Text("Ports");
    ImGui::SameLine();
    ImGui::Checkbox("Show##ports", &m_showPorts);

    // Port creation
    ImGui::SetNextItemWidth(120);
    ImGui::InputTextWithHint("##portname", "Port name...", m_portNameBuf, sizeof(m_portNameBuf));
    ImGui::SameLine();
    bool disabled = (strlen(m_portNameBuf) == 0);
    if (disabled) ImGui::BeginDisabled();
    if (ImGui::Button("Add Port")) {
        // Place port at mesh center, facing +Z, up = +Y
        glm::vec3 pos(0.0f);
        if (m_ctx.editableMesh.isValid()) {
            // If a face is selected, place at its center with its normal as forward
            auto selFaces = m_ctx.editableMesh.getSelectedFaces();
            if (!selFaces.empty() && m_ctx.selectedObject) {
                uint32_t fi = *selFaces.begin();
                pos = getFaceCenter(m_ctx.selectedObject, fi);
                // Convert from world to local
                glm::mat4 invModel = glm::inverse(m_ctx.selectedObject->getTransform().getMatrix());
                pos = glm::vec3(invModel * glm::vec4(pos, 1.0f));
            }
        }

        EditableMesh::Port port;
        port.name = m_portNameBuf;
        port.position = pos;
        port.forward = glm::vec3(0.0f, 0.0f, 1.0f);
        port.up = glm::vec3(0.0f, 1.0f, 0.0f);

        // If face selected, use face normal as forward
        auto selFaces = m_ctx.editableMesh.getSelectedFaces();
        if (!selFaces.empty() && m_ctx.selectedObject) {
            glm::vec3 worldNormal = getFaceNormal(m_ctx.selectedObject, *selFaces.begin());
            glm::mat4 invModel = glm::inverse(m_ctx.selectedObject->getTransform().getMatrix());
            glm::vec3 localNormal = glm::normalize(glm::vec3(
                glm::transpose(glm::inverse(invModel)) * glm::vec4(worldNormal, 0.0f)));
            port.forward = localNormal;
            // Derive up perpendicular to forward
            glm::vec3 hint = (std::abs(localNormal.y) < 0.9f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
            port.up = glm::normalize(glm::cross(localNormal, glm::cross(hint, localNormal)));
        }

        m_ctx.editableMesh.addPort(port);
        m_selectedPortIndex = static_cast<int>(m_ctx.editableMesh.getPortCount()) - 1;
        syncPortsToSceneObject();
        std::cout << "[Port] Added '" << port.name << "' at ("
                  << port.position.x << "," << port.position.y << "," << port.position.z << ")"
                  << std::endl;
    }
    if (disabled) ImGui::EndDisabled();

    // List existing ports
    const auto& ports = m_ctx.editableMesh.getPorts();
    for (size_t i = 0; i < ports.size(); ++i) {
        bool selected = (m_selectedPortIndex == static_cast<int>(i));
        ImGui::PushID(static_cast<int>(i));

        if (ImGui::Selectable(ports[i].name.c_str(), selected, ImGuiSelectableFlags_AllowOverlap)) {
            m_selectedPortIndex = selected ? -1 : static_cast<int>(i);
        }

        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20);
        if (ImGui::SmallButton("X")) {
            m_ctx.editableMesh.removePort(i);
            if (m_selectedPortIndex == static_cast<int>(i)) m_selectedPortIndex = -1;
            else if (m_selectedPortIndex > static_cast<int>(i)) m_selectedPortIndex--;
            syncPortsToSceneObject();
            ImGui::PopID();
            break;
        }

        ImGui::PopID();
    }

    // Edit selected port properties
    if (m_selectedPortIndex >= 0 && m_selectedPortIndex < static_cast<int>(ports.size())) {
        auto& portVec = const_cast<std::vector<EditableMesh::Port>&>(ports);
        auto& p = portVec[m_selectedPortIndex];
        ImGui::Indent();
        bool changed = false;
        ImGui::Text("Position");
        ImGui::SetNextItemWidth(-1);
        changed |= ImGui::DragFloat3("##portpos", &p.position.x, 0.01f, -100.0f, 100.0f, "%.3f");
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Direction (green arrow)");
        ImGui::SetNextItemWidth(-1);
        changed |= ImGui::SliderFloat3("##portfwd", &p.forward.x, -1.0f, 1.0f, "%.2f");
        if (ImGui::SmallButton("Flip##fwd")) { p.forward = -p.forward; changed = true; } ImGui::SameLine();
        if (ImGui::SmallButton("+X##f")) { p.forward = {1,0,0}; changed = true; } ImGui::SameLine();
        if (ImGui::SmallButton("-X##f")) { p.forward = {-1,0,0}; changed = true; } ImGui::SameLine();
        if (ImGui::SmallButton("+Y##f")) { p.forward = {0,1,0}; changed = true; } ImGui::SameLine();
        if (ImGui::SmallButton("-Y##f")) { p.forward = {0,-1,0}; changed = true; } ImGui::SameLine();
        if (ImGui::SmallButton("+Z##f")) { p.forward = {0,0,1}; changed = true; } ImGui::SameLine();
        if (ImGui::SmallButton("-Z##f")) { p.forward = {0,0,-1}; changed = true; }
        ImGui::TextColored(ImVec4(0.3f, 0.3f, 1.0f, 1.0f), "Sync (blue arrow)");
        ImGui::SetNextItemWidth(-1);
        changed |= ImGui::SliderFloat3("##portup", &p.up.x, -1.0f, 1.0f, "%.2f");
        if (ImGui::SmallButton("Flip##up")) { p.up = -p.up; changed = true; } ImGui::SameLine();
        if (ImGui::SmallButton("+X##u")) { p.up = {1,0,0}; changed = true; } ImGui::SameLine();
        if (ImGui::SmallButton("-X##u")) { p.up = {-1,0,0}; changed = true; } ImGui::SameLine();
        if (ImGui::SmallButton("+Y##u")) { p.up = {0,1,0}; changed = true; } ImGui::SameLine();
        if (ImGui::SmallButton("-Y##u")) { p.up = {0,-1,0}; changed = true; } ImGui::SameLine();
        if (ImGui::SmallButton("+Z##u")) { p.up = {0,0,1}; changed = true; } ImGui::SameLine();
        if (ImGui::SmallButton("-Z##u")) { p.up = {0,0,-1}; changed = true; }
        if (changed) {
            if (glm::length(p.forward) > 0.001f) p.forward = glm::normalize(p.forward);
            if (glm::length(p.up) > 0.001f) p.up = glm::normalize(p.up);
            syncPortsToSceneObject();
        }
        ImGui::Unindent();
    }
}

// ── Metadata Editor ───────────────────────────────────────

void ModelingMode::renderMetadataUI() {
    if (!m_ctx.selectedObject) return;

    ImGui::Separator();
    ImGui::Text("Metadata");

    // Add new key:value
    ImGui::SetNextItemWidth(80);
    ImGui::InputTextWithHint("##metakey", "key", m_metaKeyBuf, sizeof(m_metaKeyBuf));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-50);
    ImGui::InputTextWithHint("##metaval", "value", m_metaValueBuf, sizeof(m_metaValueBuf));
    ImGui::SameLine();
    if (ImGui::SmallButton("Add##meta")) {
        if (strlen(m_metaKeyBuf) > 0 && strlen(m_metaValueBuf) > 0) {
            m_ctx.editableMesh.setMetadata(m_metaKeyBuf, m_metaValueBuf);
            m_metaKeyBuf[0] = '\0';
            m_metaValueBuf[0] = '\0';
        }
    }

    // List existing metadata
    const auto& meta = m_ctx.editableMesh.getMetadata();
    for (const auto& [key, value] : meta) {
        ImGui::BulletText("%s: %s", key.c_str(), value.c_str());
        ImGui::SameLine();
        ImGui::PushID(key.c_str());
        if (ImGui::SmallButton("X##meta")) {
            m_ctx.editableMesh.removeMetadata(key);
            ImGui::PopID();
            break;  // Iterator invalidated
        }
        ImGui::PopID();
    }
}

// ── Sync to SceneObject ───────────────────────────────────

void ModelingMode::syncPortsToSceneObject() {
    if (!m_ctx.selectedObject) return;
    const auto& ports = m_ctx.editableMesh.getPorts();
    std::vector<SceneObject::StoredPort> stored;
    stored.reserve(ports.size());
    for (const auto& p : ports) {
        stored.push_back({p.name, p.position, p.forward, p.up});
    }
    m_ctx.selectedObject->setPorts(stored);
}

// ── 3D Gizmo Rendering (Vulkan lines) ────────────────────

void ModelingMode::renderPortGizmos3D(VkCommandBuffer cmd, const glm::mat4& viewProj) {
    if (!m_showPorts) return;

    for (auto& obj : m_ctx.sceneObjects) {
        if (!obj->hasPorts() || !obj->isVisible()) continue;

        const auto& ports = obj->getPorts();
        glm::mat4 modelMatrix = obj->getTransform().getMatrix();
        bool isSelected = (obj.get() == m_ctx.selectedObject);

        for (size_t i = 0; i < ports.size(); ++i) {
            const auto& p = ports[i];
            glm::vec3 worldPos = glm::vec3(modelMatrix * glm::vec4(p.position, 1.0f));
            glm::vec3 worldFwd = glm::normalize(glm::vec3(modelMatrix * glm::vec4(p.forward, 0.0f)));
            glm::vec3 worldUp = glm::normalize(glm::vec3(modelMatrix * glm::vec4(p.up, 0.0f)));

            float arrowLen = 0.3f;
            float headLen = arrowLen * 0.2f;

            bool isPortSelected = isSelected && (m_selectedPortIndex == static_cast<int>(i));

            // Forward arrow (always green — brighter when selected)
            glm::vec3 fwdEnd = worldPos + worldFwd * arrowLen;
            glm::vec3 fwdColor = isPortSelected ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(0.0f, 0.6f, 0.0f);
            std::vector<glm::vec3> fwdLines = { worldPos, fwdEnd };
            // Arrow head
            glm::vec3 fwdPerp1 = glm::normalize(glm::cross(worldFwd, worldUp));
            glm::vec3 fwdBase = worldPos + worldFwd * (arrowLen - headLen);
            fwdLines.push_back(fwdEnd); fwdLines.push_back(fwdBase + fwdPerp1 * headLen * 0.5f);
            fwdLines.push_back(fwdEnd); fwdLines.push_back(fwdBase - fwdPerp1 * headLen * 0.5f);
            fwdLines.push_back(fwdEnd); fwdLines.push_back(fwdBase + worldUp * headLen * 0.5f);
            fwdLines.push_back(fwdEnd); fwdLines.push_back(fwdBase - worldUp * headLen * 0.5f);
            m_ctx.modelRenderer.renderLines(cmd, viewProj, fwdLines, fwdColor);

            // Up arrow (always blue — brighter when selected)
            float upLen = arrowLen * 0.6f;
            glm::vec3 upEnd = worldPos + worldUp * upLen;
            glm::vec3 upColor = isPortSelected ? glm::vec3(0.3f, 0.3f, 1.0f) : glm::vec3(0.2f, 0.2f, 0.7f);
            std::vector<glm::vec3> upLines = { worldPos, upEnd };
            glm::vec3 upPerp = glm::normalize(glm::cross(worldUp, worldFwd));
            glm::vec3 upBase = worldPos + worldUp * (upLen - headLen * 0.6f);
            upLines.push_back(upEnd); upLines.push_back(upBase + upPerp * headLen * 0.3f);
            upLines.push_back(upEnd); upLines.push_back(upBase - upPerp * headLen * 0.3f);
            m_ctx.modelRenderer.renderLines(cmd, viewProj, upLines, upColor);

            // Small cross at position (white)
            float crossSize = 0.03f;
            glm::vec3 right = glm::normalize(glm::cross(worldFwd, worldUp));
            std::vector<glm::vec3> crossLines = {
                worldPos - right * crossSize, worldPos + right * crossSize,
                worldPos - worldUp * crossSize, worldPos + worldUp * crossSize,
                worldPos - worldFwd * crossSize, worldPos + worldFwd * crossSize
            };
            m_ctx.modelRenderer.renderLines(cmd, viewProj, crossLines, glm::vec3(1.0f));
        }
    }
}

// ── 2D Overlay (port name labels) ────────────────────────

void ModelingMode::renderPortOverlay(float vpX, float vpY, float vpW, float vpH) {
    if (!m_showPorts) return;

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    drawList->PushClipRect(ImVec2(vpX, vpY), ImVec2(vpX + vpW, vpY + vpH), true);

    Camera& activeCamera = m_ctx.getActiveCamera();
    glm::mat4 view = activeCamera.getViewMatrix();
    float aspectRatio = vpW / vpH;
    glm::mat4 proj = activeCamera.getProjectionMatrix(aspectRatio);
    glm::mat4 vp = proj * view;

    ImU32 textColor = IM_COL32(150, 255, 150, 255);
    ImU32 textBg = IM_COL32(0, 0, 0, 180);
    ImU32 selectedColor = IM_COL32(255, 255, 100, 255);

    for (auto& obj : m_ctx.sceneObjects) {
        if (!obj->hasPorts() || !obj->isVisible()) continue;

        const auto& ports = obj->getPorts();
        glm::mat4 modelMatrix = obj->getTransform().getMatrix();
        bool isSelected = (obj.get() == m_ctx.selectedObject);

        for (size_t i = 0; i < ports.size(); ++i) {
            const auto& p = ports[i];
            glm::vec3 worldPos = glm::vec3(modelMatrix * glm::vec4(p.position, 1.0f));
            glm::vec4 clip = vp * glm::vec4(worldPos, 1.0f);
            if (clip.w <= 0.0f) continue;
            glm::vec3 ndc = glm::vec3(clip) / clip.w;
            ImVec2 sp(vpX + (ndc.x + 1.0f) * 0.5f * vpW, vpY + (1.0f - ndc.y) * 0.5f * vpH);

            bool isPortSelected = isSelected && (m_selectedPortIndex == static_cast<int>(i));
            ImU32 color = isPortSelected ? selectedColor : textColor;

            if (!p.name.empty()) {
                ImVec2 textSize = ImGui::CalcTextSize(p.name.c_str());
                ImVec2 textPos(sp.x - textSize.x * 0.5f, sp.y - textSize.y - 4);
                drawList->AddRectFilled(
                    ImVec2(textPos.x - 2, textPos.y - 1),
                    ImVec2(textPos.x + textSize.x + 2, textPos.y + textSize.y + 1),
                    textBg, 2.0f);
                drawList->AddText(textPos, color, p.name.c_str());
            }
        }
    }

    drawList->PopClipRect();
}
