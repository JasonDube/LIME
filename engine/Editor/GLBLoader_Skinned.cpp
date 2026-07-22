// Skinned + animated GLB export for LIME-rigged meshes.
//
// LIME's rig is translation-only (bones don't carry rotation), so the exported
// animation has only `translation` channels per bone. Bind-pose vertex positions
// + per-vertex bone indices/weights + IBMs + per-keyframe bone world positions
// are translated into the standard glTF skin + animation graph that eden's
// SkinnedGLBLoader (and any other glTF runtime) consumes for GPU skinning.

#include "GLBLoader.hpp"

#include <tiny_gltf.h>

#include <glm/gtc/type_ptr.hpp>

#include <cstring>
#include <iostream>
#include <vector>

// stb_image_write implementation is owned by GLBLoader.cpp; just bring in
// the declarations here.
#include <stb_image_write.h>

namespace eden {

static void pngWriteCallbackSkinned(void* context, void* data, int size) {
    auto* buffer = static_cast<std::vector<unsigned char>*>(context);
    auto* bytes = static_cast<const unsigned char*>(data);
    buffer->insert(buffer->end(), bytes, bytes + size);
}

bool GLBLoader::saveSkinnedAnimated(const std::string& filepath,
                                    const std::vector<ModelVertex>& vertices,
                                    const std::vector<uint32_t>& indices,
                                    const std::vector<glm::ivec4>& perVertexBoneIndices,
                                    const std::vector<glm::vec4>& perVertexBoneWeights,
                                    const Skeleton& skeleton,
                                    const std::vector<glm::vec3>& bindBoneWorldPos,
                                    const std::vector<float>& animTimes,
                                    const std::vector<std::vector<glm::vec3>>& animBoneWorldPosPerKey,
                                    const unsigned char* textureData,
                                    int textureWidth,
                                    int textureHeight,
                                    const std::string& meshName,
                                    const std::string& animName) {
    if (vertices.empty() || indices.empty()) {
        std::cerr << "saveSkinnedAnimated: empty mesh\n";
        return false;
    }
    const size_t vertCount = vertices.size();
    const size_t boneCount = skeleton.bones.size();
    if (perVertexBoneIndices.size() != vertCount || perVertexBoneWeights.size() != vertCount) {
        std::cerr << "saveSkinnedAnimated: bone indices/weights size mismatch\n";
        return false;
    }
    if (bindBoneWorldPos.size() != boneCount) {
        std::cerr << "saveSkinnedAnimated: bind bone count mismatch\n";
        return false;
    }
    if (boneCount == 0) {
        std::cerr << "saveSkinnedAnimated: no bones\n";
        return false;
    }

    tinygltf::Model model;

    // ---------- Buffer layout ----------
    // 0: positions (vec3 float)        size = vertCount * 12
    // 1: normals   (vec3 float)
    // 2: uvs       (vec2 float)        size = vertCount * 8
    // 3: colors    (vec4 float)        size = vertCount * 16
    // 4: joints    (uvec4 ushort)      size = vertCount * 8
    // 5: weights   (vec4 float)        size = vertCount * 16
    // 6: indices   (uint32)
    // 7: IBMs      (mat4 float)        size = boneCount * 64
    // 8: anim times (float)            size = animTimes.size() * 4   (only if animTimes non-empty)
    // 9: anim translations interleaved per bone (vec3 float per key per bone)
    //                                  size = boneCount * keyCount * 12  (laid out: bone0 keys, bone1 keys, ...)
    const size_t posLen     = vertCount * sizeof(float) * 3;
    const size_t normLen    = vertCount * sizeof(float) * 3;
    const size_t uvLen      = vertCount * sizeof(float) * 2;
    const size_t colorLen   = vertCount * sizeof(float) * 4;
    const size_t jointsLen  = vertCount * sizeof(uint16_t) * 4;
    const size_t weightsLen = vertCount * sizeof(float) * 4;
    const size_t indexLen   = indices.size() * sizeof(uint32_t);
    const size_t ibmLen     = boneCount * sizeof(float) * 16;
    const size_t keyCount   = animTimes.size();
    const bool hasAnim      = (keyCount > 0 && animBoneWorldPosPerKey.size() == keyCount);
    const size_t timeLen    = hasAnim ? keyCount * sizeof(float) : 0;
    const size_t translLen  = hasAnim ? boneCount * keyCount * sizeof(float) * 3 : 0;

    auto align4 = [](size_t v) { return (v + 3) & ~size_t(3); };
    const size_t off_pos     = 0;
    const size_t off_norm    = align4(off_pos     + posLen);
    const size_t off_uv      = align4(off_norm    + normLen);
    const size_t off_color   = align4(off_uv      + uvLen);
    const size_t off_joints  = align4(off_color   + colorLen);
    const size_t off_weights = align4(off_joints  + jointsLen);
    const size_t off_index   = align4(off_weights + weightsLen);
    const size_t off_ibm     = align4(off_index   + indexLen);
    const size_t off_time    = align4(off_ibm     + ibmLen);
    const size_t off_trans   = align4(off_time    + timeLen);
    const size_t totalSize   = off_trans + translLen;

    tinygltf::Buffer buffer;
    buffer.data.resize(totalSize, 0);
    auto* base = buffer.data.data();

    // Fill vertex attributes.
    glm::vec3 minPos(INFINITY), maxPos(-INFINITY);
    for (size_t i = 0; i < vertCount; ++i) {
        const auto& v = vertices[i];
        minPos = glm::min(minPos, v.position);
        maxPos = glm::max(maxPos, v.position);

        float* p = reinterpret_cast<float*>(base + off_pos)   + i * 3;
        float* n = reinterpret_cast<float*>(base + off_norm)  + i * 3;
        float* t = reinterpret_cast<float*>(base + off_uv)    + i * 2;
        float* c = reinterpret_cast<float*>(base + off_color) + i * 4;
        p[0] = v.position.x; p[1] = v.position.y; p[2] = v.position.z;
        n[0] = v.normal.x;   n[1] = v.normal.y;   n[2] = v.normal.z;
        t[0] = v.texCoord.x; t[1] = v.texCoord.y;
        c[0] = v.color.r;    c[1] = v.color.g;    c[2] = v.color.b;    c[3] = v.color.a;

        // Joints (uvec4 ushort) and weights (vec4 float).
        const auto& bi = perVertexBoneIndices[i];
        const auto& bw = perVertexBoneWeights[i];
        uint16_t* j = reinterpret_cast<uint16_t*>(base + off_joints) + i * 4;
        float*    w = reinterpret_cast<float*>(base + off_weights)   + i * 4;

        // Renormalize weights so they sum to 1.0 — glTF spec recommends it
        // and most loaders assume it.
        float sum = bw.x + bw.y + bw.z + bw.w;
        for (int k = 0; k < 4; ++k) {
            int idx = std::max(0, std::min(static_cast<int>(boneCount) - 1, bi[k]));
            j[k] = static_cast<uint16_t>(idx);
            w[k] = (sum > 1e-6f) ? bw[k] / sum : (k == 0 ? 1.0f : 0.0f);
        }
    }

    // Indices.
    std::memcpy(base + off_index, indices.data(), indexLen);

    // IBMs: translation-only rest pose → IBM = translate(-bindWorldPos).
    for (size_t i = 0; i < boneCount; ++i) {
        glm::mat4 worldBind = glm::translate(glm::mat4(1.0f), bindBoneWorldPos[i]);
        glm::mat4 ibm = glm::inverse(worldBind);
        std::memcpy(base + off_ibm + i * sizeof(float) * 16,
                    glm::value_ptr(ibm), sizeof(float) * 16);
    }

    // Animation: times + per-bone local translations per key.
    if (hasAnim) {
        std::memcpy(base + off_time, animTimes.data(), timeLen);

        // Layout: for bone i, write keyCount vec3s starting at off_trans + i*keyCount*12.
        for (size_t i = 0; i < boneCount; ++i) {
            int parent = skeleton.bones[i].parentIndex;
            for (size_t k = 0; k < keyCount; ++k) {
                const auto& worldArr = animBoneWorldPosPerKey[k];
                glm::vec3 worldThis = (i < worldArr.size()) ? worldArr[i] : bindBoneWorldPos[i];
                glm::vec3 worldParent(0.0f);
                if (parent >= 0 && parent < static_cast<int>(boneCount) && parent < static_cast<int>(worldArr.size())) {
                    worldParent = worldArr[parent];
                }
                glm::vec3 local = worldThis - worldParent;
                float* dst = reinterpret_cast<float*>(base + off_trans
                                                     + i * keyCount * sizeof(float) * 3
                                                     + k * sizeof(float) * 3);
                dst[0] = local.x; dst[1] = local.y; dst[2] = local.z;
            }
        }
    }

    model.buffers.push_back(buffer);

    // ---------- Buffer views & accessors ----------
    auto pushView = [&](size_t offset, size_t length, int target) {
        tinygltf::BufferView v;
        v.buffer = 0;
        v.byteOffset = offset;
        v.byteLength = length;
        if (target) v.target = target;
        model.bufferViews.push_back(v);
        return static_cast<int>(model.bufferViews.size()) - 1;
    };

    int bvPos     = pushView(off_pos,     posLen,     TINYGLTF_TARGET_ARRAY_BUFFER);
    int bvNorm    = pushView(off_norm,    normLen,    TINYGLTF_TARGET_ARRAY_BUFFER);
    int bvUv      = pushView(off_uv,      uvLen,      TINYGLTF_TARGET_ARRAY_BUFFER);
    int bvColor   = pushView(off_color,   colorLen,   TINYGLTF_TARGET_ARRAY_BUFFER);
    int bvJoints  = pushView(off_joints,  jointsLen,  TINYGLTF_TARGET_ARRAY_BUFFER);
    int bvWeights = pushView(off_weights, weightsLen, TINYGLTF_TARGET_ARRAY_BUFFER);
    int bvIndex   = pushView(off_index,   indexLen,   TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
    int bvIbm     = pushView(off_ibm,     ibmLen,     0);
    int bvTime    = hasAnim ? pushView(off_time,  timeLen,   0) : -1;
    int bvTrans   = hasAnim ? pushView(off_trans, translLen, 0) : -1;

    auto pushAccessor = [&](int bv, size_t byteOffset, int compType, size_t count, int type) {
        tinygltf::Accessor a;
        a.bufferView = bv;
        a.byteOffset = byteOffset;
        a.componentType = compType;
        a.count = count;
        a.type = type;
        model.accessors.push_back(a);
        return static_cast<int>(model.accessors.size()) - 1;
    };

    int accPos     = pushAccessor(bvPos,     0, TINYGLTF_COMPONENT_TYPE_FLOAT,          vertCount, TINYGLTF_TYPE_VEC3);
    model.accessors[accPos].minValues = {minPos.x, minPos.y, minPos.z};
    model.accessors[accPos].maxValues = {maxPos.x, maxPos.y, maxPos.z};
    int accNorm    = pushAccessor(bvNorm,    0, TINYGLTF_COMPONENT_TYPE_FLOAT,          vertCount, TINYGLTF_TYPE_VEC3);
    int accUv      = pushAccessor(bvUv,      0, TINYGLTF_COMPONENT_TYPE_FLOAT,          vertCount, TINYGLTF_TYPE_VEC2);
    int accColor   = pushAccessor(bvColor,   0, TINYGLTF_COMPONENT_TYPE_FLOAT,          vertCount, TINYGLTF_TYPE_VEC4);
    int accJoints  = pushAccessor(bvJoints,  0, TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT, vertCount, TINYGLTF_TYPE_VEC4);
    int accWeights = pushAccessor(bvWeights, 0, TINYGLTF_COMPONENT_TYPE_FLOAT,          vertCount, TINYGLTF_TYPE_VEC4);
    int accIndex   = pushAccessor(bvIndex,   0, TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,   indices.size(), TINYGLTF_TYPE_SCALAR);
    int accIbm     = pushAccessor(bvIbm,     0, TINYGLTF_COMPONENT_TYPE_FLOAT,          boneCount, TINYGLTF_TYPE_MAT4);

    int accTime = -1;
    std::vector<int> accBoneTrans(boneCount, -1);
    if (hasAnim) {
        accTime = pushAccessor(bvTime, 0, TINYGLTF_COMPONENT_TYPE_FLOAT, keyCount, TINYGLTF_TYPE_SCALAR);
        // glTF requires min/max on the input (sampler) accessor.
        if (!animTimes.empty()) {
            float mn = animTimes.front(), mx = animTimes.back();
            for (float t : animTimes) { if (t < mn) mn = t; if (t > mx) mx = t; }
            model.accessors[accTime].minValues = {mn};
            model.accessors[accTime].maxValues = {mx};
        }
        for (size_t i = 0; i < boneCount; ++i) {
            accBoneTrans[i] = pushAccessor(bvTrans, i * keyCount * sizeof(float) * 3,
                                           TINYGLTF_COMPONENT_TYPE_FLOAT, keyCount, TINYGLTF_TYPE_VEC3);
        }
    }

    // ---------- Mesh primitive ----------
    tinygltf::Primitive prim;
    prim.attributes["POSITION"]   = accPos;
    prim.attributes["NORMAL"]     = accNorm;
    prim.attributes["TEXCOORD_0"] = accUv;
    prim.attributes["COLOR_0"]    = accColor;
    prim.attributes["JOINTS_0"]   = accJoints;
    prim.attributes["WEIGHTS_0"]  = accWeights;
    prim.indices = accIndex;
    prim.mode = TINYGLTF_MODE_TRIANGLES;

    // ---------- Texture / material (optional) ----------
    int materialIdx = -1;
    if (textureData && textureWidth > 0 && textureHeight > 0) {
        std::vector<unsigned char> pngBytes;
        if (stbi_write_png_to_func(pngWriteCallbackSkinned, &pngBytes,
                                   textureWidth, textureHeight, 4, textureData,
                                   textureWidth * 4)) {
            // Append PNG bytes to the buffer.
            size_t pngOff = align4(buffer.data.size());
            buffer.data.resize(pngOff + pngBytes.size());
            std::memcpy(buffer.data.data() + pngOff, pngBytes.data(), pngBytes.size());
            // Replace buffer (we already pushed an earlier copy).
            model.buffers[0] = buffer;

            tinygltf::BufferView pngView;
            pngView.buffer = 0;
            pngView.byteOffset = pngOff;
            pngView.byteLength = pngBytes.size();
            int bvPng = static_cast<int>(model.bufferViews.size());
            model.bufferViews.push_back(pngView);

            tinygltf::Image img;
            img.bufferView = bvPng;
            img.mimeType = "image/png";
            img.name = meshName + "_texture";
            int imgIdx = static_cast<int>(model.images.size());
            model.images.push_back(img);

            tinygltf::Sampler samp;
            samp.minFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
            samp.magFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
            samp.wrapS = TINYGLTF_TEXTURE_WRAP_REPEAT;
            samp.wrapT = TINYGLTF_TEXTURE_WRAP_REPEAT;
            int sampIdx = static_cast<int>(model.samplers.size());
            model.samplers.push_back(samp);

            tinygltf::Texture tex;
            tex.source = imgIdx;
            tex.sampler = sampIdx;
            int texIdx = static_cast<int>(model.textures.size());
            model.textures.push_back(tex);

            tinygltf::Material mat;
            mat.name = meshName + "_material";
            mat.pbrMetallicRoughness.baseColorTexture.index = texIdx;
            mat.pbrMetallicRoughness.baseColorTexture.texCoord = 0;
            mat.pbrMetallicRoughness.metallicFactor = 0.0;
            mat.pbrMetallicRoughness.roughnessFactor = 1.0;
            materialIdx = static_cast<int>(model.materials.size());
            model.materials.push_back(mat);
            prim.material = materialIdx;
        }
    }

    tinygltf::Mesh mesh;
    mesh.name = meshName;
    mesh.primitives.push_back(prim);
    model.meshes.push_back(mesh);

    // ---------- Nodes ----------
    // Node 0: mesh + skin
    // Nodes 1..boneCount: bone nodes
    tinygltf::Node meshNode;
    meshNode.mesh = 0;
    meshNode.skin = 0;
    meshNode.name = meshName;
    model.nodes.push_back(meshNode);

    const int firstBoneNode = static_cast<int>(model.nodes.size());
    for (size_t i = 0; i < boneCount; ++i) {
        tinygltf::Node bn;
        bn.name = skeleton.bones[i].name.empty() ? ("bone_" + std::to_string(i))
                                                  : skeleton.bones[i].name;
        // Local translation = bind world − parent bind world
        int parent = skeleton.bones[i].parentIndex;
        glm::vec3 worldThis = bindBoneWorldPos[i];
        glm::vec3 worldParent(0.0f);
        if (parent >= 0 && parent < static_cast<int>(boneCount)) {
            worldParent = bindBoneWorldPos[parent];
        }
        glm::vec3 local = worldThis - worldParent;
        bn.translation = {local.x, local.y, local.z};
        model.nodes.push_back(bn);
    }
    // Wire children.
    for (size_t i = 0; i < boneCount; ++i) {
        int parent = skeleton.bones[i].parentIndex;
        if (parent < 0 || parent >= static_cast<int>(boneCount)) continue;
        model.nodes[firstBoneNode + parent].children.push_back(firstBoneNode + static_cast<int>(i));
    }

    // ---------- Skin ----------
    tinygltf::Skin skin;
    skin.name = meshName + "_skin";
    skin.inverseBindMatrices = accIbm;
    skin.joints.reserve(boneCount);
    for (size_t i = 0; i < boneCount; ++i) skin.joints.push_back(firstBoneNode + static_cast<int>(i));
    // skeleton root: pick the first root bone if present.
    for (size_t i = 0; i < boneCount; ++i) {
        if (skeleton.bones[i].parentIndex < 0) { skin.skeleton = firstBoneNode + static_cast<int>(i); break; }
    }
    model.skins.push_back(skin);

    // ---------- Animation ----------
    if (hasAnim) {
        tinygltf::Animation anim;
        anim.name = animName;
        for (size_t i = 0; i < boneCount; ++i) {
            tinygltf::AnimationSampler samp;
            samp.input = accTime;
            samp.output = accBoneTrans[i];
            samp.interpolation = "LINEAR";
            int sampIdx = static_cast<int>(anim.samplers.size());
            anim.samplers.push_back(samp);

            tinygltf::AnimationChannel chan;
            chan.sampler = sampIdx;
            chan.target_node = firstBoneNode + static_cast<int>(i);
            chan.target_path = "translation";
            anim.channels.push_back(chan);
        }
        model.animations.push_back(anim);
    }

    // ---------- Scene ----------
    tinygltf::Scene scene;
    scene.nodes.push_back(0);                       // mesh node
    // Also add bone roots as scene roots so transforms compose correctly.
    for (size_t i = 0; i < boneCount; ++i) {
        if (skeleton.bones[i].parentIndex < 0) {
            scene.nodes.push_back(firstBoneNode + static_cast<int>(i));
        }
    }
    model.scenes.push_back(scene);
    model.defaultScene = 0;

    model.asset.version = "2.0";
    model.asset.generator = "LIME (skinned + animated)";

    tinygltf::TinyGLTF gltf;
    bool ok = gltf.WriteGltfSceneToFile(&model, filepath,
                                        /*embedImages=*/true,
                                        /*embedBuffers=*/true,
                                        /*prettyPrint=*/false,
                                        /*writeBinary=*/true);
    if (!ok) {
        std::cerr << "saveSkinnedAnimated: WriteGltfSceneToFile failed for " << filepath << std::endl;
        return false;
    }
    std::cout << "Saved skinned GLB: " << filepath << " (" << vertCount << " verts, "
              << indices.size() / 3 << " tris, " << boneCount << " bones, "
              << keyCount << " keys)" << std::endl;
    return true;
}

bool GLBLoader::saveSkinnedAnimatedMulti(const std::string& filepath,
                                         const std::vector<ModelVertex>& vertices,
                                         const std::vector<uint32_t>& indices,
                                         const std::vector<glm::ivec4>& perVertexBoneIndices,
                                         const std::vector<glm::vec4>& perVertexBoneWeights,
                                         const Skeleton& skeleton,
                                         const std::vector<glm::vec3>& bindBoneWorldPos,
                                         const std::vector<SkinnedAnimClip>& clips,
                                         const unsigned char* textureData,
                                         int textureWidth,
                                         int textureHeight,
                                         const std::string& meshName) {
    if (vertices.empty() || indices.empty()) { std::cerr << "saveMulti: empty mesh\n"; return false; }
    const size_t vertCount = vertices.size();
    const size_t boneCount = skeleton.bones.size();
    if (perVertexBoneIndices.size() != vertCount || perVertexBoneWeights.size() != vertCount) {
        std::cerr << "saveMulti: bone idx/wt size mismatch\n"; return false;
    }
    if (bindBoneWorldPos.size() != boneCount || boneCount == 0) {
        std::cerr << "saveMulti: bind bone count mismatch\n"; return false;
    }

    tinygltf::Model model;

    // Mesh section sizes (identical to single-clip writer).
    const size_t posLen = vertCount*sizeof(float)*3, normLen = vertCount*sizeof(float)*3;
    const size_t uvLen = vertCount*sizeof(float)*2, colorLen = vertCount*sizeof(float)*4;
    const size_t jointsLen = vertCount*sizeof(uint16_t)*4, weightsLen = vertCount*sizeof(float)*4;
    const size_t indexLen = indices.size()*sizeof(uint32_t), ibmLen = boneCount*sizeof(float)*16;
    auto align4 = [](size_t v){ return (v + 3) & ~size_t(3); };
    const size_t off_pos=0;
    const size_t off_norm=align4(off_pos+posLen), off_uv=align4(off_norm+normLen);
    const size_t off_color=align4(off_uv+uvLen), off_joints=align4(off_color+colorLen);
    const size_t off_weights=align4(off_joints+jointsLen), off_index=align4(off_weights+weightsLen);
    const size_t off_ibm=align4(off_index+indexLen);

    // Per-clip anim sections (time + interleaved per-bone translations + rotations).
    struct ClipOff { size_t offTime, timeLen, offTrans, translLen, offRot, rotLen, keyCount; bool has; };
    std::vector<ClipOff> co(clips.size());
    size_t cursor = align4(off_ibm + ibmLen);
    for (size_t c = 0; c < clips.size(); ++c) {
        size_t kc = clips[c].times.size();
        co[c].has = (kc > 0 && clips[c].boneWorldPosPerKey.size() == kc);
        co[c].keyCount = kc;
        co[c].timeLen   = co[c].has ? kc*sizeof(float) : 0;
        co[c].translLen = co[c].has ? boneCount*kc*sizeof(float)*3 : 0;
        co[c].rotLen    = co[c].has ? boneCount*kc*sizeof(float)*4 : 0;
        co[c].offTime  = cursor; cursor = align4(cursor + co[c].timeLen);
        co[c].offTrans = cursor; cursor = align4(cursor + co[c].translLen);
        co[c].offRot   = cursor; cursor = align4(cursor + co[c].rotLen);
    }
    tinygltf::Buffer buffer;
    buffer.data.resize(cursor, 0);
    auto* base = buffer.data.data();

    // Vertex attributes (identical to single).
    glm::vec3 minPos(INFINITY), maxPos(-INFINITY);
    for (size_t i = 0; i < vertCount; ++i) {
        const auto& v = vertices[i];
        minPos = glm::min(minPos, v.position); maxPos = glm::max(maxPos, v.position);
        float* p=reinterpret_cast<float*>(base+off_pos)+i*3;
        float* n=reinterpret_cast<float*>(base+off_norm)+i*3;
        float* t=reinterpret_cast<float*>(base+off_uv)+i*2;
        float* c2=reinterpret_cast<float*>(base+off_color)+i*4;
        p[0]=v.position.x;p[1]=v.position.y;p[2]=v.position.z;
        n[0]=v.normal.x;n[1]=v.normal.y;n[2]=v.normal.z;
        t[0]=v.texCoord.x;t[1]=v.texCoord.y;
        c2[0]=v.color.r;c2[1]=v.color.g;c2[2]=v.color.b;c2[3]=v.color.a;
        const auto& bi=perVertexBoneIndices[i]; const auto& bw=perVertexBoneWeights[i];
        uint16_t* j=reinterpret_cast<uint16_t*>(base+off_joints)+i*4;
        float* w=reinterpret_cast<float*>(base+off_weights)+i*4;
        float sum=bw.x+bw.y+bw.z+bw.w;
        for (int k=0;k<4;++k){ int idx=std::max(0,std::min((int)boneCount-1,bi[k])); j[k]=(uint16_t)idx; w[k]=(sum>1e-6f)?bw[k]/sum:(k==0?1.0f:0.0f); }
    }
    std::memcpy(base+off_index, indices.data(), indexLen);
    for (size_t i=0;i<boneCount;++i){
        glm::mat4 ibm=glm::inverse(glm::translate(glm::mat4(1.0f), bindBoneWorldPos[i]));
        std::memcpy(base+off_ibm+i*sizeof(float)*16, glm::value_ptr(ibm), sizeof(float)*16);
    }
    // Each clip's anim data. Per key, LIME's native track gives each bone a
    // world head position + world rotation delta from rest. glTF wants
    // parent-LOCAL TRS, so per bone:
    //   localRot = inverse(parentWorldRot) * worldRot
    //   localT   = inverse(parentWorldRot) * (worldHead - parentWorldHead)
    // (rest world rotations are identity here — bone nodes are written with
    // translation only — so the deltas ARE the world rotations). Under FK this
    // composes to W[i] = T(head_i)*R(rot_i), and with the translation-only
    // IBMs the GPU skins v' = rot*(v - bindHead) + head — exactly LIME's
    // native reskin. Missing rotations (legacy clips) fall back to identity,
    // which degenerates to the old translation-only behavior.
    for (size_t c=0;c<clips.size();++c){
        if (!co[c].has) continue;
        std::memcpy(base+co[c].offTime, clips[c].times.data(), co[c].timeLen);
        const bool hasRot = clips[c].boneWorldRotPerKey.size()==co[c].keyCount;
        for (size_t i=0;i<boneCount;++i){
            int parent=skeleton.bones[i].parentIndex;
            for (size_t k=0;k<co[c].keyCount;++k){
                const auto& wa=clips[c].boneWorldPosPerKey[k];
                glm::vec3 wt=(i<wa.size())?wa[i]:bindBoneWorldPos[i];
                glm::vec3 wp(0.0f);
                glm::quat wr(1.0f,0.0f,0.0f,0.0f), pr(1.0f,0.0f,0.0f,0.0f);
                if (hasRot){
                    const auto& ra=clips[c].boneWorldRotPerKey[k];
                    if (i<ra.size()) wr=ra[i];
                    if (parent>=0 && parent<(int)ra.size()) pr=ra[parent];
                }
                if (parent>=0 && parent<(int)boneCount && parent<(int)wa.size()) wp=wa[parent];
                glm::quat prInv=glm::inverse(pr);
                glm::vec3 local=prInv*(wt-wp);
                glm::quat localRot=glm::normalize(prInv*wr);
                float* dst=reinterpret_cast<float*>(base+co[c].offTrans+i*co[c].keyCount*sizeof(float)*3+k*sizeof(float)*3);
                dst[0]=local.x;dst[1]=local.y;dst[2]=local.z;
                float* rdst=reinterpret_cast<float*>(base+co[c].offRot+i*co[c].keyCount*sizeof(float)*4+k*sizeof(float)*4);
                rdst[0]=localRot.x;rdst[1]=localRot.y;rdst[2]=localRot.z;rdst[3]=localRot.w;  // glTF quat = xyzw
            }
        }
    }
    model.buffers.push_back(buffer);

    auto pushView=[&](size_t offset,size_t length,int target){
        tinygltf::BufferView v; v.buffer=0; v.byteOffset=offset; v.byteLength=length;
        if(target)v.target=target; model.bufferViews.push_back(v); return (int)model.bufferViews.size()-1;
    };
    int bvPos=pushView(off_pos,posLen,TINYGLTF_TARGET_ARRAY_BUFFER);
    int bvNorm=pushView(off_norm,normLen,TINYGLTF_TARGET_ARRAY_BUFFER);
    int bvUv=pushView(off_uv,uvLen,TINYGLTF_TARGET_ARRAY_BUFFER);
    int bvColor=pushView(off_color,colorLen,TINYGLTF_TARGET_ARRAY_BUFFER);
    int bvJoints=pushView(off_joints,jointsLen,TINYGLTF_TARGET_ARRAY_BUFFER);
    int bvWeights=pushView(off_weights,weightsLen,TINYGLTF_TARGET_ARRAY_BUFFER);
    int bvIndex=pushView(off_index,indexLen,TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
    int bvIbm=pushView(off_ibm,ibmLen,0);
    auto pushAccessor=[&](int bv,size_t byteOffset,int compType,size_t count,int type){
        tinygltf::Accessor a; a.bufferView=bv; a.byteOffset=byteOffset; a.componentType=compType; a.count=count; a.type=type;
        model.accessors.push_back(a); return (int)model.accessors.size()-1;
    };
    int accPos=pushAccessor(bvPos,0,TINYGLTF_COMPONENT_TYPE_FLOAT,vertCount,TINYGLTF_TYPE_VEC3);
    model.accessors[accPos].minValues={minPos.x,minPos.y,minPos.z};
    model.accessors[accPos].maxValues={maxPos.x,maxPos.y,maxPos.z};
    int accNorm=pushAccessor(bvNorm,0,TINYGLTF_COMPONENT_TYPE_FLOAT,vertCount,TINYGLTF_TYPE_VEC3);
    int accUv=pushAccessor(bvUv,0,TINYGLTF_COMPONENT_TYPE_FLOAT,vertCount,TINYGLTF_TYPE_VEC2);
    int accColor=pushAccessor(bvColor,0,TINYGLTF_COMPONENT_TYPE_FLOAT,vertCount,TINYGLTF_TYPE_VEC4);
    int accJoints=pushAccessor(bvJoints,0,TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT,vertCount,TINYGLTF_TYPE_VEC4);
    int accWeights=pushAccessor(bvWeights,0,TINYGLTF_COMPONENT_TYPE_FLOAT,vertCount,TINYGLTF_TYPE_VEC4);
    int accIndex=pushAccessor(bvIndex,0,TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,indices.size(),TINYGLTF_TYPE_SCALAR);
    int accIbm=pushAccessor(bvIbm,0,TINYGLTF_COMPONENT_TYPE_FLOAT,boneCount,TINYGLTF_TYPE_MAT4);

    std::vector<int> accTime(clips.size(),-1);
    std::vector<std::vector<int>> accBoneTrans(clips.size());
    std::vector<std::vector<int>> accBoneRot(clips.size());
    for (size_t c=0;c<clips.size();++c){
        if (!co[c].has) continue;
        int bvTime=pushView(co[c].offTime,co[c].timeLen,0);
        int bvTrans=pushView(co[c].offTrans,co[c].translLen,0);
        int bvRot=pushView(co[c].offRot,co[c].rotLen,0);
        accTime[c]=pushAccessor(bvTime,0,TINYGLTF_COMPONENT_TYPE_FLOAT,co[c].keyCount,TINYGLTF_TYPE_SCALAR);
        float mn=clips[c].times.front(),mx=clips[c].times.back();
        for(float tt:clips[c].times){ if(tt<mn)mn=tt; if(tt>mx)mx=tt; }
        model.accessors[accTime[c]].minValues={mn}; model.accessors[accTime[c]].maxValues={mx};
        accBoneTrans[c].assign(boneCount,-1);
        accBoneRot[c].assign(boneCount,-1);
        for (size_t i=0;i<boneCount;++i){
            accBoneTrans[c][i]=pushAccessor(bvTrans,i*co[c].keyCount*sizeof(float)*3,TINYGLTF_COMPONENT_TYPE_FLOAT,co[c].keyCount,TINYGLTF_TYPE_VEC3);
            accBoneRot[c][i]=pushAccessor(bvRot,i*co[c].keyCount*sizeof(float)*4,TINYGLTF_COMPONENT_TYPE_FLOAT,co[c].keyCount,TINYGLTF_TYPE_VEC4);
        }
    }

    tinygltf::Primitive prim;
    prim.attributes["POSITION"]=accPos; prim.attributes["NORMAL"]=accNorm;
    prim.attributes["TEXCOORD_0"]=accUv; prim.attributes["COLOR_0"]=accColor;
    prim.attributes["JOINTS_0"]=accJoints; prim.attributes["WEIGHTS_0"]=accWeights;
    prim.indices=accIndex; prim.mode=TINYGLTF_MODE_TRIANGLES;

    if (textureData && textureWidth>0 && textureHeight>0) {
        std::vector<unsigned char> png;
        if (stbi_write_png_to_func(pngWriteCallbackSkinned,&png,textureWidth,textureHeight,4,textureData,textureWidth*4)) {
            size_t pngOff=align4(buffer.data.size());
            buffer.data.resize(pngOff+png.size());
            std::memcpy(buffer.data.data()+pngOff,png.data(),png.size());
            model.buffers[0]=buffer;
            tinygltf::BufferView pv; pv.buffer=0; pv.byteOffset=pngOff; pv.byteLength=png.size();
            int bvPng=(int)model.bufferViews.size(); model.bufferViews.push_back(pv);
            tinygltf::Image img; img.bufferView=bvPng; img.mimeType="image/png"; img.name=meshName+"_texture";
            int imgIdx=(int)model.images.size(); model.images.push_back(img);
            tinygltf::Sampler samp; samp.minFilter=TINYGLTF_TEXTURE_FILTER_LINEAR; samp.magFilter=TINYGLTF_TEXTURE_FILTER_LINEAR;
            samp.wrapS=TINYGLTF_TEXTURE_WRAP_REPEAT; samp.wrapT=TINYGLTF_TEXTURE_WRAP_REPEAT;
            int sampIdx=(int)model.samplers.size(); model.samplers.push_back(samp);
            tinygltf::Texture tex; tex.source=imgIdx; tex.sampler=sampIdx;
            int texIdx=(int)model.textures.size(); model.textures.push_back(tex);
            tinygltf::Material mat; mat.name=meshName+"_material";
            mat.pbrMetallicRoughness.baseColorTexture.index=texIdx;
            mat.pbrMetallicRoughness.baseColorTexture.texCoord=0;
            mat.pbrMetallicRoughness.metallicFactor=0.0; mat.pbrMetallicRoughness.roughnessFactor=1.0;
            model.materials.push_back(mat); prim.material=(int)model.materials.size()-1;
        }
    }

    tinygltf::Mesh mesh; mesh.name=meshName; mesh.primitives.push_back(prim); model.meshes.push_back(mesh);

    tinygltf::Node meshNode; meshNode.mesh=0; meshNode.skin=0; meshNode.name=meshName; model.nodes.push_back(meshNode);
    const int firstBoneNode=(int)model.nodes.size();
    for (size_t i=0;i<boneCount;++i){
        tinygltf::Node bn; bn.name=skeleton.bones[i].name.empty()?("bone_"+std::to_string(i)):skeleton.bones[i].name;
        int parent=skeleton.bones[i].parentIndex; glm::vec3 wp(0.0f);
        if (parent>=0 && parent<(int)boneCount) wp=bindBoneWorldPos[parent];
        glm::vec3 local=bindBoneWorldPos[i]-wp; bn.translation={local.x,local.y,local.z};
        model.nodes.push_back(bn);
    }
    for (size_t i=0;i<boneCount;++i){
        int parent=skeleton.bones[i].parentIndex;
        if (parent<0||parent>=(int)boneCount) continue;
        model.nodes[firstBoneNode+parent].children.push_back(firstBoneNode+(int)i);
    }
    tinygltf::Skin skin; skin.name=meshName+"_skin"; skin.inverseBindMatrices=accIbm;
    skin.joints.reserve(boneCount);
    for (size_t i=0;i<boneCount;++i) skin.joints.push_back(firstBoneNode+(int)i);
    for (size_t i=0;i<boneCount;++i) if (skeleton.bones[i].parentIndex<0){ skin.skeleton=firstBoneNode+(int)i; break; }
    model.skins.push_back(skin);

    // One glTF animation per clip.
    for (size_t c=0;c<clips.size();++c){
        if (!co[c].has) continue;
        tinygltf::Animation anim;
        anim.name = clips[c].name.empty() ? ("clip_"+std::to_string(c)) : clips[c].name;
        for (size_t i=0;i<boneCount;++i){
            tinygltf::AnimationSampler samp; samp.input=accTime[c]; samp.output=accBoneTrans[c][i]; samp.interpolation="LINEAR";
            int si=(int)anim.samplers.size(); anim.samplers.push_back(samp);
            tinygltf::AnimationChannel chan; chan.sampler=si; chan.target_node=firstBoneNode+(int)i; chan.target_path="translation";
            anim.channels.push_back(chan);
            tinygltf::AnimationSampler rsamp; rsamp.input=accTime[c]; rsamp.output=accBoneRot[c][i]; rsamp.interpolation="LINEAR";
            int rsi=(int)anim.samplers.size(); anim.samplers.push_back(rsamp);
            tinygltf::AnimationChannel rchan; rchan.sampler=rsi; rchan.target_node=firstBoneNode+(int)i; rchan.target_path="rotation";
            anim.channels.push_back(rchan);
        }
        model.animations.push_back(anim);
    }

    tinygltf::Scene scene; scene.nodes.push_back(0);
    for (size_t i=0;i<boneCount;++i) if (skeleton.bones[i].parentIndex<0) scene.nodes.push_back(firstBoneNode+(int)i);
    model.scenes.push_back(scene); model.defaultScene=0;
    model.asset.version="2.0"; model.asset.generator="LIME (skinned + multi-anim)";

    tinygltf::TinyGLTF gltf;
    bool ok=gltf.WriteGltfSceneToFile(&model,filepath,true,true,false,true);
    if (!ok){ std::cerr<<"saveMulti: write failed for "<<filepath<<"\n"; return false; }
    std::cout << "Saved multi-clip GLB: " << filepath << " (" << clips.size() << " clips:";
    for (auto& cl : clips) std::cout << " '" << cl.name << "'";
    std::cout << ")" << std::endl;
    return true;
}

} // namespace eden
