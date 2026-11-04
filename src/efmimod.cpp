#include "efmimod.h"

#include <algorithm>
#include <cstdlib>
#include <set>

#include "log.h"

namespace efmivulkan {
namespace {

using namespace ini;

constexpr int kMaxRunDepth = 4;

std::string WithoutRef(const std::string& value) {
    const std::string low = Lower(Trim(value));
    return low.rfind("ref ", 0) == 0 ? Trim(low.substr(4)) : low;
}

std::optional<uint32_t> ParseHash(const std::string& value) {
    char* end = nullptr;
    const unsigned long v = std::strtoul(value.c_str(), &end, 16);
    if (end == value.c_str() || value.size() > 8) return std::nullopt;  // 16 digits: a shader hash.
    return static_cast<uint32_t>(v);
}

class Parser {
public:
    Parser(const std::vector<Section>& sections, const std::wstring& dir, EfmiMod& mod) : sections_(sections), dir_(dir), mod_(mod) {}

    // A Resource section's buffer: its index in mod.buffers, or -1 without a file.
    int Buffer(const std::string& name) {
        const auto known = buffers_.find(name);
        if (known != buffers_.end()) return known->second;
        EfmiBuffer b;
        b.name = name;
        std::string format;
        if (const Section* s = FindSection(sections_, name)) {
            for (const std::string& line : s->lines) {
                std::string key, value;
                if (!KeyValue(line, &key, &value)) continue;
                if (key == "filename") b.path = File(value);
                if (key == "stride") b.stride = static_cast<uint32_t>(std::atoi(value.c_str()));
                if (key == "format") format = Lower(value);
            }
        }
        b.format = format;
        if (format.find("r32_uint") != std::string::npos) {
            b.indexBytes = 4;
        } else if (format.find("r16_uint") != std::string::npos) {
            b.indexBytes = 2;
        } else if (b.stride % 3 == 0 && (b.stride == 6 || b.stride == 12)) {
            b.indexBytes = b.stride / 3;  // A triangle's indices.
        }
        int index = -1;
        if (!b.path.empty()) {
            index = static_cast<int>(mod_.buffers.size());
            mod_.buffers.push_back(std::move(b));
        }
        buffers_[name] = index;
        return index;
    }

    int Texture(const std::string& name) {
        const auto known = textures_.find(name);
        if (known != textures_.end()) return known->second;
        int index = -1;
        if (const Section* s = FindSection(sections_, name)) {
            for (const std::string& line : s->lines) {
                std::string key, value;
                if (KeyValue(line, &key, &value) && key == "filename") {
                    index = static_cast<int>(mod_.textures.size());
                    mod_.textures.push_back(File(value));
                }
            }
        }
        textures_[name] = index;
        return index;
    }

    // Lines of a draw list as the toggles leave them, following "run =" into the mod's
    // own command lists. EFMI's own ones (CommandList\EFMIv1\...) are not the mod's.
    void WalkDrawList(const Section& section, Vars& vars, EfmiDrawList& list, int depth) {
        ForActiveLines(section, vars, [&](const std::string& line) {
            std::string key, value;
            if (!KeyValue(line, &key, &value) || Assign(key, value, vars)) return;
            if (key == "cs-t2") {
                boneId_ = RecordId(WithoutRef(value));
            } else if ((key == "vs-t0" || key.rfind("vs-cb", 0) == 0) && Lower(Trim(value)) != "null") {
                otherBones_ = true;
            } else if (key == "run" && Lower(Trim(value)).rfind("customshader", 0) == 0) {
                // A custom shader that reads the draw's bones records them under the current ID.
                if (IsBoneRecorder(Lower(Trim(value))) && boneId_ >= 0) {
                    recorded_.insert(boneId_);
                    if (std::find(list.recordIds.begin(), list.recordIds.end(), boneId_) == list.recordIds.end()) list.recordIds.push_back(boneId_);
                }
            } else if (key == "draw" && Lower(Trim(value)) == "from_caller") {
                list.drawOriginal = true;
            } else if (key == "ib") {
                list.ib = Buffer(WithoutRef(value));
            } else if (key.size() == 3 && key[0] == 'v' && key[1] == 'b' && key[2] >= '0' && key[2] < '0' + kEfmiVbSlots) {
                list.vb[key[2] - '0'] = Buffer(WithoutRef(value));
            } else if (key == "drawindexed" || key == "drawindexedinstanced") {
                // drawindexed = count, first, base
                // drawindexedinstanced = count, INSTANCE_COUNT, first, base, FIRST_INSTANCE
                const std::vector<std::string> args = Split(value, ",");
                const bool instanced = key == "drawindexedinstanced";
                if (args.size() < (instanced ? 4u : 3u)) return;
                // "drawindexed = INDEX_COUNT, FIRST_INDEX, 0": the game's own draw, as
                // "draw = from_caller" (components posed on the CPU, kept as they are).
                if (Lower(Trim(args[0])) == "index_count") {
                    list.drawOriginal = true;
                    return;
                }
                const auto at = [&](size_t i) { return std::atoi(Trim(args[i]).c_str()); };
                EfmiDrawCall d;
                d.indexCount = static_cast<uint32_t>(at(0));
                d.firstIndex = static_cast<uint32_t>(at(instanced ? 2 : 1));
                d.baseVertex = at(instanced ? 3 : 2);
                d.ib = list.ib;
                std::copy(list.vb.begin(), list.vb.end(), d.vb.begin());
                d.shaderFiltered = ini::t_shaderFiltered > 0;
                if (otherBones_) {
                    // Bones of its own: the draw's, recorded and bound back, or another
                    // component's record, or something the layer cannot follow.
                    if (boneId_ < 0) {
                        ++list.otherBonesDraws;
                        return;
                    }
                    d.boneSource = recorded_.count(boneId_) ? -1 : boneId_;
                }
                list.draws.push_back(d);
            } else if (key == "resource\\rabbitfx\\diffuse" || key == "resource\\rabbitfx\\lightmap" || key == "resource\\rabbitfx\\normalmap") {
                // RabbitFX: the library's patched shaders sample these instead of the game's own.
                const int role = key[18] == 'd' ? 0 : key[18] == 'l' ? 1 : 2;  // After "resource\rabbitfx\".
                list.rabbit[role] = Texture(WithoutRef(value));
            } else if (key.rfind("ps-t", 0) == 0 && isdigit(static_cast<unsigned char>(key[4]))) {
                const int texture = Texture(WithoutRef(value));
                if (texture >= 0) list.textures.push_back({std::atoi(key.c_str() + 4), texture});
            } else if (key == "run" && depth < kMaxRunDepth && Lower(value).find('\\') == std::string::npos) {
                if (const Section* next = FindSection(sections_, Lower(value))) WalkDrawList(*next, vars, list, depth + 1);
            }
        });
    }

    // Per draw list: reset before each.
    void Reset() {
        otherBones_ = false;
        boneId_ = -1;
        recorded_.clear();
    }

private:
    int RecordId(const std::string& name) const {
        for (size_t i = 0; i < mod_.boneRecordIds.size(); ++i) {
            if (mod_.boneRecordIds[i] == name) return static_cast<int>(i);
        }
        return -1;
    }

    bool IsBoneRecorder(const std::string& section) const {
        const Section* s = FindSection(sections_, section);
        if (!s) return false;
        for (const std::string& line : s->lines) {
            std::string key, value;
            if (KeyValue(line, &key, &value) && key == "cs-t0" && WithoutRef(value) == "vs-t0") return true;
        }
        return false;
    }

    std::wstring File(std::string value) {
        for (char& c : value) {
            if (c == '/') c = '\\';
        }
        return dir_ + L"\\" + Widen(Trim(value));
    }

    const std::vector<Section>& sections_;
    std::wstring dir_;
    EfmiMod& mod_;
    std::map<std::string, int> buffers_, textures_;

    bool otherBones_ = false;
    int boneId_ = -1;
    std::set<int> recorded_;
};

// The draw list a component section runs: the mod's own command list with draws in it.
const Section* DrawListOf(const std::vector<Section>& sections, const std::string& target) {
    const Section* s = FindSection(sections, target);
    if (!s) return nullptr;
    for (const std::string& line : s->lines) {
        const std::string low = Lower(line);
        if (low.rfind("drawindexed", 0) == 0 || low.rfind("ib ", 0) == 0 || low.rfind("ib=", 0) == 0) return s;
        // CPU-posed components: "draw = from_caller", the game's own draw with the mod's textures.
        if (low.rfind("draw", 0) == 0 && low.find("from_caller") != std::string::npos) return s;
    }
    return nullptr;
}

}  // namespace

std::optional<EfmiMod> ParseEfmiMod(const std::wstring& path, const std::string& relative, const std::wstring& userIni,
                                    const std::map<std::string, int>* toggles) {
    const std::vector<Section> sections = ReadSections(path);
    Vars vars = ReadToggles(sections, relative, userIni, toggles);
    RunPresent(sections, vars);

    EfmiMod mod;
    mod.name = relative;
    mod.ini = path;
    Parser parser(sections, path.substr(0, path.find_last_of(L'\\')), mod);
    // Bone record IDs, in file order so a toggle keeps their numbers.
    for (const Section& s : sections) {
        if (s.name.rfind("customshader", 0) == 0) continue;
        for (const std::string& line : s.lines) {
            std::string key, value;
            if (!KeyValue(line, &key, &value) || key != "cs-t2") continue;
            const std::string id = WithoutRef(value);
            if (id != "null" && std::find(mod.boneRecordIds.begin(), mod.boneRecordIds.end(), id) == mod.boneRecordIds.end()) mod.boneRecordIds.push_back(id);
        }
    }

    for (const Section& s : sections) {
        if (s.name.rfind("textureoverride", 0) != 0) continue;
        EfmiComponent c;
        c.section = s.name;
        bool hasHash = false, hasCount = false;
        for (const std::string& line : s.lines) {
            std::string key, value;
            if (!KeyValue(line, &key, &value)) continue;
            if (key == "hash") {
                if (const auto h = ParseHash(Trim(value))) {
                    c.hash = *h;
                    hasHash = true;
                }
            } else if (key == "match_index_count") {
                c.indexCount = static_cast<uint32_t>(std::atoi(value.c_str()));
                hasCount = true;
            }
        }
        if (!hasHash) continue;
        if (!hasCount) {
            // A texture override: "this = [ref] Resource_X" replaces the game texture.
            for (const std::string& line : s.lines) {
                std::string key, value;
                if (!KeyValue(line, &key, &value) || key != "this") continue;
                const int texture = parser.Texture(WithoutRef(value));
                if (texture >= 0) mod.thisByHash[c.hash] = texture;
            }
            continue;
        }

        Vars v = vars;
        const Section* list = nullptr;
        ForActiveLines(s, v, [&](const std::string& line) {
            std::string key, value;
            if (!KeyValue(line, &key, &value) || Assign(key, value, v)) return;
            if (key == "$\\efmiv1\\gpu_posed") {
                c.gpuPosed = std::atoi(value.c_str()) != 0;
            } else if (key == "$\\efmiv1\\skip_skeleton_override") {
                c.readOnly = std::atoi(value.c_str()) != 0;
            } else if (key == "$\\efmiv1\\component_id") {
                c.componentId = std::atoi(value.c_str());
            } else if (key == "commandlist\\efmiv1\\callback_component_drawcustom" || key == "run") {
                if (const Section* target = DrawListOf(sections, WithoutRef(value))) list = target;
            }
        });
        const auto lod = v.find("$lod_level");
        c.lod = lod == v.end() ? 0 : lod->second;
        parser.Reset();
        if (list) parser.WalkDrawList(*list, v, c.list, 0);
        mod.components.push_back(std::move(c));
    }
    if (mod.components.empty()) return std::nullopt;

    // The merged skeleton: its pools are set line by line, each after "$cmp_id = N".
    const auto bones = vars.find("$bones_count");
    mod.bonesCount = bones == vars.end() ? 0 : static_cast<uint32_t>(std::max(0, bones->second));
    for (const Section& s : sections) {
        int component = -1;
        for (const std::string& line : s.lines) {
            std::string key, value;
            if (!KeyValue(line, &key, &value)) continue;
            if (key == "$cmp_id") {
                component = std::atoi(value.c_str());
                continue;
            }
            if (key == "$\\efmiv1\\identification_min_components") mod.minComponents = std::atoi(value.c_str());
            if (key == "run" && Lower(Trim(value)) == "commandlist\\efmiv1\\spatialidentity_identifycomponentinstances") mod.spatialIdentification = true;
            if (key == "$\\efmiv1\\custom_mesh_scale") mod.meshScale = static_cast<float>(std::atof(value.c_str()));
            const bool offsets = key.rfind("$pool_mergedskeleton_component_vertexgroupoffsets[", 0) == 0;
            const bool counts = key.rfind("$pool_mergedskeleton_component_vertexgroupcounts[", 0) == 0;
            const bool remaps = key.rfind("pool_mergedskeleton_component_lodremaps[", 0) == 0;
            if (component < 0 || component > 255 || !(offsets || counts || remaps)) continue;
            if (mod.skeletonComponents.size() <= size_t(component)) mod.skeletonComponents.resize(component + 1);
            EfmiSkeletonComponent& sc = mod.skeletonComponents[component];
            if (offsets) sc.offset = std::atoi(value.c_str());
            if (counts) sc.count = std::atoi(value.c_str());
            if (remaps) {
                // [$cmp_id * $lod_level_count + L]
                const size_t plus = key.rfind('+');
                const int lod = plus == std::string::npos ? 0 : std::atoi(key.c_str() + plus + 1);
                const std::string target = WithoutRef(value);
                if (lod >= 0 && lod < 4 && target != "null") sc.lodRemap[lod] = parser.Buffer(target);
            }
        }
    }

    for (const std::wstring& texture : mod.textures) {
        const size_t at = texture.find(L"t=", texture.find_last_of(L'\\'));
        mod.textureHashes.push_back(at == std::wstring::npos ? 0 : static_cast<uint32_t>(wcstoul(texture.c_str() + at + 2, nullptr, 16)));
    }
    mod.keys = ParseKeys(sections, relative);
    for (const ModKey& key : mod.keys) {
        const auto it = vars.find(key.var);
        mod.toggles[key.var] = it == vars.end() ? 0 : it->second;
    }
    return mod;
}

}  // namespace efmivulkan
