// End-to-end test of the layer, without the game: a real Vulkan device with the layer
// enabled through environment variables (no registry), an index buffer uploaded from
// staging and one in mapped memory, and indexed draws. Checks efmivulkan.log for the
// region hash matches EFMI would make.
//
//   smoketest <folder with efmivulkan.json> <scratch dir>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "hash.h"
#include "texhash.h"

using efmivulkan::Crc32c;

static int g_failed = 0;

static void Expect(const char* name, bool ok) {
    printf("%-60s %s\n", name, ok ? "ok" : "FAIL");
    if (!ok) ++g_failed;
}

#define CHECK(x)                                                        \
    do {                                                                \
        VkResult r_ = (x);                                              \
        if (r_ != VK_SUCCESS) {                                         \
            printf("%s failed: %d (line %d)\n", #x, int(r_), __LINE__); \
            return 2;                                                   \
        }                                                               \
    } while (0)

static std::string ReadAll(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

// Two trivial shaders as SPIR-V words, written by hand: the Windows SDK's dxc has no
// SPIR-V output, and the draws only need a valid pipeline. Ids: 1 main, 2 output,
// 3 void, 4 fn, 5 float, 6 vec4, 7 output pointer, 8 0.0, 9 1.0, 10 vec4(0,0,0,1), 11 label.
#define SPIRV_TAIL                                                                           \
    0x00020013, 3, 0x00030021, 4, 3, 0x00030016, 5, 32, 0x00040017, 6, 5, 4, 0x00040020, 7, 3, 6, \
        0x0004003B, 7, 2, 3, 0x0004002B, 5, 8, 0x00000000, 0x0004002B, 5, 9, 0x3F800000,          \
        0x0007002C, 6, 10, 8, 8, 8, 9, 0x00050036, 3, 1, 0, 4, 0x000200F8, 11, 0x0003003E, 2, 10,  \
        0x000100FD, 0x00010038
// gl_Position = vec4(0, 0, 0, 1)
static const uint32_t kVertexSpirv[] = {
    0x07230203, 0x00010000, 0, 12, 0,
    0x00020011, 1,                                // OpCapability Shader
    0x0003000E, 0, 1,                             // OpMemoryModel Logical GLSL450
    0x0006000F, 0, 1, 0x6E69616D, 0x00000000, 2,  // OpEntryPoint Vertex %1 "main" %2
    0x00040047, 2, 11, 0,                         // OpDecorate %2 BuiltIn Position
    SPIRV_TAIL};
// out vec4 color = vec4(0, 0, 0, 1)
static const uint32_t kFragmentSpirv[] = {
    0x07230203, 0x00010000, 0, 12, 0,
    0x00020011, 1,
    0x0003000E, 0, 1,
    0x0006000F, 4, 1, 0x6E69616D, 0x00000000, 2,  // OpEntryPoint Fragment %1 "main" %2
    0x00030010, 1, 7,                             // OpExecutionMode %1 OriginUpperLeft
    0x00040047, 2, 30, 0,                         // OpDecorate %2 Location 0
    SPIRV_TAIL};

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        printf("usage: smoketest <folder with efmivulkan.json> <scratch dir>\n");
        return 2;
    }
    const std::wstring layerDir = argv[1], scratch = argv[2];
    const std::wstring modsDir = scratch + L"\\Mods";
    CreateDirectoryW(modsDir.c_str(), nullptr);
    CreateDirectoryW((modsDir + L"\\Efmi").c_str(), nullptr);

    // Index data: one buffer uploaded through staging, one in mapped memory.
    std::vector<uint32_t> uploaded(300), mapped(90);
    for (uint32_t i = 0; i < 300; ++i) uploaded[i] = i;
    for (uint32_t i = 0; i < 90; ++i) mapped[i] = 89 - i;
    // EFMI's keys: the XXMI fork's region hash of the indices a draw reads.
    const uint32_t uploadedHash = Crc32c(0, uploaded.data() + 150, 150 * 4);
    const uint32_t mappedHash = Crc32c(0, mapped.data(), 90 * 4);
    const uint32_t posedHash = Crc32c(0, uploaded.data(), 60 * 4);
    const uint32_t crossHash = Crc32c(0, uploaded.data() + 60, 30 * 4);
    // A 64x64 BC7 texture with one mip, and its 3DMigoto hash as a DX11 game would have
    // created it: BC7_UNORM (98), default usage.
    std::vector<uint8_t> bc7(64 * 64);
    for (size_t i = 0; i < bc7.size(); ++i) bc7[i] = static_cast<uint8_t>(i * 7 + 3);
    const uint32_t textureHash = [&] {
        const uint32_t data = efmivulkan::texhash::DataHash(bc7.data(), bc7.size(), 64, 64, false);
        for (const auto& c : efmivulkan::texhash::Candidates(data, VK_FORMAT_BC7_UNORM_BLOCK, 64, 64, 1)) {
            if (c.dxgiFormat == 98 && c.usage == 0) return c.hash;
        }
        return 0u;
    }();
    // A second game texture, 64x64 with 2 mips, and a streamed copy of it: a 32x32 image
    // whose mip 0 is its mip 1, as Unity on Vulkan binds while the full size streams in.
    std::vector<uint8_t> fullMip0(64 * 64), fullMip1(32 * 32);
    for (size_t i = 0; i < fullMip0.size(); ++i) fullMip0[i] = static_cast<uint8_t>(i * 13 + 1);
    for (size_t i = 0; i < fullMip1.size(); ++i) fullMip1[i] = static_cast<uint8_t>(i * 5 + 9);
    const uint32_t fullHash = [&] {
        const uint32_t data = efmivulkan::texhash::DataHash(fullMip0.data(), fullMip0.size(), 64, 64, false);
        for (const auto& c : efmivulkan::texhash::Candidates(data, VK_FORMAT_BC7_UNORM_BLOCK, 64, 64, 2)) {
            if (c.dxgiFormat == 98 && c.usage == 0) return c.hash;
        }
        return 0u;
    }();
    // A mod in EFMI's format, with its own meshes. Three components, three outcomes:
    //   0: drawn by the mod (16-byte vertices, as the test pipeline reads)
    //   1: refused, its vertex buffer has a 12-byte stride
    //   2: left to the game, it needs the merged skeleton (gpu_posed, newer format)
    {
        CreateDirectoryW((modsDir + L"\\Efmi\\Meshes").c_str(), nullptr);
        const uint16_t ib[3] = {0, 1, 2};
        const float vb16[12] = {}, vb12[9] = {};
        std::ofstream(modsDir + L"\\Efmi\\Meshes\\IB.buf", std::ios::binary).write(reinterpret_cast<const char*>(ib), sizeof(ib));
        std::ofstream(modsDir + L"\\Efmi\\Meshes\\VB16.buf", std::ios::binary).write(reinterpret_cast<const char*>(vb16), sizeof(vb16));
        std::ofstream(modsDir + L"\\Efmi\\Meshes\\VB12.buf", std::ios::binary).write(reinterpret_cast<const char*>(vb12), sizeof(vb12));
        // The mod's texture for the game's: a DDS with a DX10 header, BC7 (98), 64x64, one mip.
        uint32_t dds[37] = {0x20534444, 124, 0x1007, 64, 64, 4096, 0, 1};
        dds[19] = 32;          // Pixel format size.
        dds[20] = 4;           // DDPF_FOURCC.
        dds[21] = 0x30315844;  // "DX10".
        dds[32] = 98;          // DXGI_FORMAT_BC7_UNORM.
        dds[33] = 3;           // Texture 2D.
        dds[35] = 1;           // Array size.
        std::vector<uint8_t> pixels(4096, 0x5a);
        char name[64];
        snprintf(name, sizeof(name), "\\Efmi\\T0 t=%08x.dds", textureHash);
        std::ofstream file(modsDir + std::wstring(name, name + strlen(name)), std::ios::binary);
        file.write(reinterpret_cast<const char*>(dds), sizeof(dds));
        file.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
        snprintf(name, sizeof(name), "\\Efmi\\T1 t=%08x.dds", fullHash);
        std::ofstream file1(modsDir + std::wstring(name, name + strlen(name)), std::ios::binary);
        file1.write(reinterpret_cast<const char*>(dds), sizeof(dds));
        file1.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
        // Bone weights and merged-skeleton indices, as gpu_posed mods have them: 16 bytes,
        // R16G16B16A16_UNORM weights then R16G16B16A16_UINT indices. Bones 3 and 1 are used.
        const uint16_t blend[3][8] = {{0xffff, 0, 0, 0, 3, 0, 0, 0}, {0xffff, 0, 0, 0, 1, 0, 0, 0}, {0x8000, 0x7fff, 0, 0, 3, 1, 0, 0}};
        std::ofstream(modsDir + L"\\Efmi\\Meshes\\Blend.buf", std::ios::binary).write(reinterpret_cast<const char*>(blend), sizeof(blend));
        std::ofstream ini(modsDir + L"\\Efmi\\efmi.ini");
        char text[4096];
        snprintf(text, sizeof(text),
                 "[TextureOverride_Component0]\nhash = %08x\nmatch_index_count = 150\n$object_detected = 1\n"
                 "if $mod_enabled && DRAW_TYPE == 4\n    handling = skip\n    run = CommandList_Draw_Component0\nendif\n\n"
                 "[TextureOverride_Component1]\nhash = %08x\nmatch_index_count = 90\n$object_detected = 1\n"
                 "run = CommandList_Draw_Component1\n\n"
                 "[TextureOverride_EntryPoint_Component2]\nhash = %08x\nmatch_index_count = 60\n"
                 "if $mod_enabled && DRAW_TYPE == 4\n    $\\EFMIv1\\component_id = 2\n    $\\EFMIv1\\gpu_posed = 1\n"
                 "    CommandList\\EFMIv1\\Callback_Component_DrawCustom = ref CommandList_Draw_Component2\nendif\n\n"
                 "[CommandList_Draw_Component2]\nib = ref Resource_IB\nvb0 = ref Resource_VB16\nvb2 = ref Resource_Blend\ndrawindexed = 3, 0, 0\n\n"
                 "[Constants]\nglobal $bones_count = 4\n\n"
                 "[CommandListInitializeMergedSkeleton]\n$cmp_id = 2\n"
                 "$Pool_MergedSkeleton_Component_VertexGroupOffsets[$cmp_id] = 0\n"
                 "$Pool_MergedSkeleton_Component_VertexGroupCounts[$cmp_id] = 4\n\n"
                 "[Resource_Blend]\ntype = Buffer\nformat = DXGI_FORMAT_R16G16B16A16_UNORM\nstride = 16\nfilename = Meshes/Blend.buf\n\n"
                 "[CommandList_Draw_Component0]\nib = ref Resource_IB\nvb0 = ref Resource_VB16\nps-t16 = Resource_Texture0\n"
                 "drawindexed = 3, 0, 0\ndrawindexed = 0, 0, 0\n"
                 // Cross-IB, as Mifu's mod: component 0 records its bones under an ID...
                 "cs-t2 = ResourceID_Body\nrun = CustomShader_RecordBones\n\n"
                 "[CustomShader_RecordBones]\ncs-t0 = vs-t0\ncs-u1 = ResourceFakeT0_UAV\ndispatch = 12, 1, 1\n\n"
                 "[CustomShader_RedirectCB1]\ncs-t0 = ResourceDumpedCB1_SRV\ncs-u0 = ResourceFakeCB1_UAV\ndispatch = 4, 1, 1\n\n"
                 // ... and component 3 draws a piece with them.
                 "[TextureOverride_Component3]\nhash = %08x\nmatch_index_count = 30\nrun = CommandList_Draw_Component3\n\n"
                 "[CommandList_Draw_Component3]\nvs-t0 = ResourceFakeT0_SRV\nvs-cb2 = ResourceFakeCB1\ncs-t2 = ResourceID_Body\n"
                 "run = CustomShader_RedirectCB1\nib = ref Resource_IB\nvb0 = ref Resource_VB16\ndrawindexed = 2, 1, 0\n\n"
                 "[CommandList_Draw_Component1]\nib = ref Resource_IB\nvb0 = ref Resource_VB12\ndrawindexed = 3, 0, 0\ndrawindexed = 2, 0, 0\n\n"
                 "[Resource_IB]\ntype = Buffer\nformat = DXGI_FORMAT_R16_UINT\nstride = 6\nfilename = Meshes/IB.buf\n"
                 "[Resource_VB16]\ntype = Buffer\nstride = 16\nfilename = Meshes/VB16.buf\n"
                 "[Resource_VB12]\ntype = Buffer\nstride = 12\nfilename = Meshes/VB12.buf\n\n"
                 "[TextureOverride_Texture0]\nhash = %08x\nthis = Resource_Texture0\n"
                 "[Resource_Texture0]\nfilename = T0 t=%08x.dds\n"
                 "[TextureOverride_Texture1]\nhash = %08x\nthis = Resource_Texture1\n"
                 "[Resource_Texture1]\nfilename = T1 t=%08x.dds\n",
                 uploadedHash, mappedHash, posedHash, crossHash, textureHash, textureHash, fullHash, fullHash);
        ini << text;
    }

    // The layer through the environment: an explicit layer in this folder, acting in
    // this process, logging and reading mods in the scratch folder.
    SetEnvironmentVariableW(L"VK_ADD_LAYER_PATH", layerDir.c_str());
    SetEnvironmentVariableW(L"VK_LAYER_PATH", layerDir.c_str());
    SetEnvironmentVariableW(L"EFMIVULKAN_ANY_PROCESS", L"1");
    SetEnvironmentVariableW(L"EFMIVULKAN_LOG_DIR", scratch.c_str());
    SetEnvironmentVariableW(L"EFMIVULKAN_MODS", modsDir.c_str());

    HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
    if (!loader) {
        printf("no Vulkan loader\n");
        return 2;
    }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));

    const char* layers[] = {"VK_LAYER_EFMIVULKAN_mod_loader"};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = 1;
    ici.ppEnabledLayerNames = layers;
    VkInstance instance;
    CHECK(createInstance(&ici, nullptr, &instance));
#define INST(fn) auto fn = reinterpret_cast<PFN_vk##fn>(gipa(instance, "vk" #fn))
    INST(EnumeratePhysicalDevices);
    INST(GetPhysicalDeviceQueueFamilyProperties);
    INST(GetPhysicalDeviceMemoryProperties);
    INST(CreateDevice);
    INST(GetDeviceProcAddr);
    INST(DestroyInstance);
#undef INST

    uint32_t count = 1;
    VkPhysicalDevice physical;
    if (EnumeratePhysicalDevices(instance, &count, &physical) < 0 || !count) {
        printf("no GPU\n");
        return 2;
    }
    uint32_t families = 0;
    GetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
    std::vector<VkQueueFamilyProperties> props(families);
    GetPhysicalDeviceQueueFamilyProperties(physical, &families, props.data());
    uint32_t family = 0;
    while (family < families && !(props[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    const float priority = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice device;
    CHECK(CreateDevice(physical, &dci, nullptr, &device));
    // The way Unity gets them: through vkGetDeviceProcAddr, which the layer answers.
#define DEV(fn) auto fn = reinterpret_cast<PFN_vk##fn>(GetDeviceProcAddr(device, "vk" #fn))
    DEV(GetDeviceQueue);
    DEV(CreateBuffer);
    DEV(GetBufferMemoryRequirements);
    DEV(AllocateMemory);
    DEV(BindBufferMemory);
    DEV(MapMemory);
    DEV(CreateCommandPool);
    DEV(AllocateCommandBuffers);
    DEV(BeginCommandBuffer);
    DEV(EndCommandBuffer);
    DEV(CmdCopyBuffer);
    DEV(QueueSubmit);
    DEV(QueueWaitIdle);
    DEV(CreateImage);
    DEV(GetImageMemoryRequirements);
    DEV(BindImageMemory);
    DEV(CreateImageView);
    DEV(CreateRenderPass);
    DEV(CreateFramebuffer);
    DEV(CreateShaderModule);
    DEV(CreatePipelineLayout);
    DEV(CreateGraphicsPipelines);
    DEV(CmdBeginRenderPass);
    DEV(CmdEndRenderPass);
    DEV(CmdBindPipeline);
    DEV(CmdBindIndexBuffer);
    DEV(CmdBindVertexBuffers);
    DEV(CmdCopyBufferToImage);
    DEV(CmdCopyImageToBuffer);
    DEV(CmdPipelineBarrier);
    DEV(CmdDrawIndexed);
    DEV(DeviceWaitIdle);
    DEV(DestroyDevice);
    DEV(CreateSampler);
    DEV(CreateDescriptorSetLayout);
    DEV(CreateDescriptorPool);
    DEV(AllocateDescriptorSets);
    DEV(CreateDescriptorUpdateTemplate);
    DEV(UpdateDescriptorSetWithTemplate);
    DEV(CmdBindDescriptorSets);
#undef DEV
    VkQueue queue;
    GetDeviceQueue(device, family, 0, &queue);

    VkPhysicalDeviceMemoryProperties memory;
    GetPhysicalDeviceMemoryProperties(physical, &memory);
    const auto memoryType = [&](uint32_t bits, VkMemoryPropertyFlags want) {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & want) == want) return i;
        }
        return 0u;
    };
    const auto makeBuffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want, VkBuffer* buffer, VkDeviceMemory* mem) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = usage;
        VkResult r = CreateBuffer(device, &bci, nullptr, buffer);
        if (r) return r;
        VkMemoryRequirements req;
        GetBufferMemoryRequirements(device, *buffer, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = memoryType(req.memoryTypeBits, want);
        r = AllocateMemory(device, &mai, nullptr, mem);
        if (r) return r;
        return BindBufferMemory(device, *buffer, *mem, 0);
    };
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    // The mod index loads on the layer's job thread: let it.
    Sleep(1000);

    VkBuffer staging, indices, mappedIndices;
    VkDeviceMemory stagingMem, indicesMem, mappedMem;
    CHECK(makeBuffer(1200, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host, &staging, &stagingMem));
    CHECK(makeBuffer(1200, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &indices, &indicesMem));
    CHECK(makeBuffer(360, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, host, &mappedIndices, &mappedMem));
    void* p;
    CHECK(MapMemory(device, stagingMem, 0, VK_WHOLE_SIZE, 0, &p));
    memcpy(p, uploaded.data(), 1200);
    CHECK(MapMemory(device, mappedMem, 0, VK_WHOLE_SIZE, 0, &p));
    memcpy(p, mapped.data(), 360);

    // A small render target.
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {16, 16, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImage image;
    CHECK(CreateImage(device, &ii, nullptr, &image));
    VkMemoryRequirements req;
    GetImageMemoryRequirements(device, image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory imageMem;
    CHECK(AllocateMemory(device, &mai, nullptr, &imageMem));
    CHECK(BindImageMemory(device, image, imageMem, 0));
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = ii.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view;
    CHECK(CreateImageView(device, &vci, nullptr, &view));
    VkAttachmentDescription att{};
    att.format = ii.format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments = &att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    VkRenderPass pass;
    CHECK(CreateRenderPass(device, &rpi, nullptr, &pass));
    VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbi.renderPass = pass;
    fbi.attachmentCount = 1;
    fbi.pAttachments = &view;
    fbi.width = 16;
    fbi.height = 16;
    fbi.layers = 1;
    VkFramebuffer framebuffer;
    CHECK(CreateFramebuffer(device, &fbi, nullptr, &framebuffer));

    // A pipeline with no vertex input.
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    VkShaderModule vs, ps;
    smi.codeSize = sizeof(kVertexSpirv);
    smi.pCode = kVertexSpirv;
    CHECK(CreateShaderModule(device, &smi, nullptr, &vs));
    smi.codeSize = sizeof(kFragmentSpirv);
    smi.pCode = kFragmentSpirv;
    CHECK(CreateShaderModule(device, &smi, nullptr, &ps));
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    stages[1].pName = "main";
    // One vertex binding of 16 bytes, which the shader does not read: what the layer
    // compares the mods' strides with.
    // And a per-instance binding, as Unity has: the game's buffer stays bound for it.
    const VkVertexInputBindingDescription bindings[2] = {{0, 16, VK_VERTEX_INPUT_RATE_VERTEX}, {1, 64, VK_VERTEX_INPUT_RATE_INSTANCE}};
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 2;
    vertexInput.pVertexBindingDescriptions = bindings;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0, 0, 16, 16, 0, 1};
    VkRect2D scissor{{0, 0}, {16, 16}};
    VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAtt;
    // One set with a texture and a sampler, as Unity's materials have.
    const VkDescriptorSetLayoutBinding setBindings[3] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dsli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsli.bindingCount = 3;
    dsli.pBindings = setBindings;
    VkDescriptorSetLayout setLayout;
    CHECK(CreateDescriptorSetLayout(device, &dsli, nullptr, &setLayout));
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &setLayout;
    VkPipelineLayout layout;
    CHECK(CreatePipelineLayout(device, &pli, nullptr, &layout));
    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vertexInput;
    gpi.pInputAssemblyState = &assembly;
    gpi.pViewportState = &viewportState;
    gpi.pRasterizationState = &raster;
    gpi.pMultisampleState = &multisample;
    gpi.pColorBlendState = &blend;
    gpi.layout = layout;
    gpi.renderPass = pass;
    VkPipeline pipeline;
    CHECK(CreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline));
    // A skinned pipeline: also bone weights and 8-bit indices, 12 bytes, at binding 2.
    const VkVertexInputBindingDescription skinnedBindings[3] = {
        {0, 16, VK_VERTEX_INPUT_RATE_VERTEX}, {1, 64, VK_VERTEX_INPUT_RATE_INSTANCE}, {2, 12, VK_VERTEX_INPUT_RATE_VERTEX}};
    vertexInput.vertexBindingDescriptionCount = 3;
    vertexInput.pVertexBindingDescriptions = skinnedBindings;
    VkPipeline skinnedPipeline;
    CHECK(CreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, nullptr, &skinnedPipeline));

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.queueFamilyIndex = family;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool;
    CHECK(CreateCommandPool(device, &cpi, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb;
    CHECK(AllocateCommandBuffers(device, &cai, &cb));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;

    // A texture upload, as a game does it: staging copied into mip 0 of a BC7 image.
    VkImageCreateInfo ti{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ti.imageType = VK_IMAGE_TYPE_2D;
    ti.format = VK_FORMAT_BC7_UNORM_BLOCK;
    ti.extent = {64, 64, 1};
    ti.mipLevels = 1;
    ti.arrayLayers = 1;
    ti.samples = VK_SAMPLE_COUNT_1_BIT;
    ti.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImage texture;
    CHECK(CreateImage(device, &ti, nullptr, &texture));
    VkMemoryRequirements texReq;
    GetImageMemoryRequirements(device, texture, &texReq);
    VkMemoryAllocateInfo texAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    texAlloc.allocationSize = texReq.size;
    texAlloc.memoryTypeIndex = memoryType(texReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory textureMem;
    CHECK(AllocateMemory(device, &texAlloc, nullptr, &textureMem));
    CHECK(BindImageMemory(device, texture, textureMem, 0));
    // The second texture, and its streamed copy, from memory the CPU writes.
    const auto makeTexture = [&](uint32_t size, uint32_t mips, VkImage* out) {
        VkImageCreateInfo info = ti;
        info.extent = {size, size, 1};
        info.mipLevels = mips;
        VkResult r = CreateImage(device, &info, nullptr, out);
        if (r) return r;
        VkMemoryRequirements req;
        GetImageMemoryRequirements(device, *out, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkDeviceMemory mem;
        r = AllocateMemory(device, &alloc, nullptr, &mem);
        if (r) return r;
        return BindImageMemory(device, *out, mem, 0);
    };
    VkImage fullTexture, streamedCopy;
    CHECK(makeTexture(64, 2, &fullTexture));
    CHECK(makeTexture(32, 1, &streamedCopy));
    VkBuffer fullStaging;
    VkDeviceMemory fullStagingMem;
    CHECK(makeBuffer(fullMip1.size() + fullMip0.size() + fullMip1.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host, &fullStaging, &fullStagingMem));
    {
        void* q;
        CHECK(MapMemory(device, fullStagingMem, 0, VK_WHOLE_SIZE, 0, &q));
        uint8_t* bytes = static_cast<uint8_t*>(q);
        memcpy(bytes, fullMip1.data(), fullMip1.size());  // The streamed copy's mip 0.
        memcpy(bytes + fullMip1.size(), fullMip0.data(), fullMip0.size());
        memcpy(bytes + fullMip1.size() + fullMip0.size(), fullMip1.data(), fullMip1.size());
    }
    VkBuffer texStaging;
    VkDeviceMemory texStagingMem;
    CHECK(makeBuffer(bc7.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host, &texStaging, &texStagingMem));
    VkBuffer texDevice;
    VkDeviceMemory texDeviceMem;
    CHECK(makeBuffer(bc7.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &texDevice,
                     &texDeviceMem));
    CHECK(MapMemory(device, texStagingMem, 0, VK_WHOLE_SIZE, 0, &p));
    memcpy(p, bc7.data(), bc7.size());

    // The game's set for it, written with a template as Unity does.
    VkImageViewCreateInfo tvci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    tvci.image = texture;
    tvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    tvci.format = ti.format;
    tvci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView textureView;
    CHECK(CreateImageView(device, &tvci, nullptr, &textureView));
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkSampler sampler;
    CHECK(CreateSampler(device, &sci, nullptr, &sampler));
    const VkDescriptorPoolSize poolSizes[3] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 4;
    dpci.poolSizeCount = 3;
    dpci.pPoolSizes = poolSizes;
    VkDescriptorPool descriptorPool;
    CHECK(CreateDescriptorPool(device, &dpci, nullptr, &descriptorPool));
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = descriptorPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &setLayout;
    VkDescriptorSet gameSet;
    CHECK(AllocateDescriptorSets(device, &dsai, &gameSet));
    struct SetData {
        VkDescriptorImageInfo texture;
        VkDescriptorBufferInfo bones, config;
    };
    const VkDescriptorUpdateTemplateEntry entries[3] = {
        {0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, offsetof(SetData, texture), 0},
        {1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, offsetof(SetData, bones), 0},
        {2, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, offsetof(SetData, config), 0}};
    VkDescriptorUpdateTemplateCreateInfo duti{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
    duti.descriptorUpdateEntryCount = 3;
    duti.pDescriptorUpdateEntries = entries;
    duti.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
    duti.descriptorSetLayout = setLayout;
    VkDescriptorUpdateTemplate updateTemplate;
    CHECK(CreateDescriptorUpdateTemplate(device, &duti, nullptr, &updateTemplate));

    // The uploads, as a game does them: staging copied into the index buffer and the texture.
    CHECK(BeginCommandBuffer(cb, &begin));
    VkBufferCopy region{0, 0, 1200};
    CmdCopyBuffer(cb, staging, indices, 1, &region);
    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = texture;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);
    // The texture goes up through a buffer only the GPU sees, as some of Endfield's do: the
    // layer cannot hash it at upload, and has to read it back from a mod draw that binds it.
    VkBufferCopy toDevice{0, 0, bc7.size()};
    CmdCopyBuffer(cb, texStaging, texDevice, 1, &toDevice);
    VkBufferMemoryBarrier copied{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    copied.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    copied.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    copied.srcQueueFamilyIndex = copied.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    copied.buffer = texDevice;
    copied.size = VK_WHOLE_SIZE;
    CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &copied, 0, nullptr);
    VkBufferImageCopy texCopy{};
    texCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    texCopy.imageExtent = {64, 64, 1};
    CmdCopyBufferToImage(cb, texDevice, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &texCopy);
    {
        // The streamed copy goes up first, then the full texture with both mips.
        VkImageMemoryBarrier toDst[2] = {toTransfer, toTransfer};
        toDst[0].image = streamedCopy;
        toDst[1].image = fullTexture;
        toDst[1].subresourceRange.levelCount = 2;
        CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, toDst);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {32, 32, 1};
        CmdCopyBufferToImage(cb, fullStaging, streamedCopy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        VkBufferImageCopy full[2] = {};
        full[0].bufferOffset = fullMip1.size();
        full[0].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        full[0].imageExtent = {64, 64, 1};
        full[1].bufferOffset = fullMip1.size() + fullMip0.size();
        full[1].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1};
        full[1].imageExtent = {32, 32, 1};
        CmdCopyBufferToImage(cb, fullStaging, fullTexture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, full);
    }
    VkImageMemoryBarrier toRead = toTransfer;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toRead);
    CHECK(EndCommandBuffer(cb));
    CHECK(QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    CHECK(QueueWaitIdle(queue));
    // Written after the upload, when the texture is hashed and known.
    // The bones and the instance config, in memory the CPU writes, as Endfield has them:
    // row 5 of the config says this frame's bones start at row 2000 (bone i at 2000 + 3 + 3i).
    VkBuffer gameBones, gameConfig;
    VkDeviceMemory gameBonesMem, gameConfigMem;
    CHECK(makeBuffer(128 << 10, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host, &gameBones, &gameBonesMem));
    CHECK(makeBuffer(64 << 10, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, host, &gameConfig, &gameConfigMem));
    CHECK(MapMemory(device, gameBonesMem, 0, VK_WHOLE_SIZE, 0, &p));
    for (uint32_t row = 0; row < (128 << 10) / 16; ++row) {
        const float value[4] = {float(row), 0, 0, 0};
        memcpy(static_cast<uint8_t*>(p) + row * 16, value, 16);
    }
    CHECK(MapMemory(device, gameConfigMem, 0, VK_WHOLE_SIZE, 0, &p));
    const uint32_t row5[4] = {2000, 2000, 0, 0x3f800000};  // ..., 1.0f, as the game has it.
    memcpy(static_cast<uint8_t*>(p) + 5 * 16, row5, 16);
    const SetData setData{{sampler, textureView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {gameBones, 0, VK_WHOLE_SIZE}, {gameConfig, 0, 64 << 10}};
    UpdateDescriptorSetWithTemplate(device, gameSet, updateTemplate, &setData);
    // A second object with the same character, 10 m away (row 3: position), bones at row 3000.
    VkBuffer gameConfig2;
    VkDeviceMemory gameConfig2Mem;
    CHECK(makeBuffer(64 << 10, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, host, &gameConfig2, &gameConfig2Mem));
    CHECK(MapMemory(device, gameConfig2Mem, 0, VK_WHOLE_SIZE, 0, &p));
    const float position2[4] = {10, 0, 0, 1};
    const uint32_t row5b[4] = {3000, 3000, 0, 0x3f800000};
    memcpy(static_cast<uint8_t*>(p) + 3 * 16, position2, 16);
    memcpy(static_cast<uint8_t*>(p) + 5 * 16, row5b, 16);
    VkDescriptorSet gameSet2;
    CHECK(AllocateDescriptorSets(device, &dsai, &gameSet2));
    const SetData setData2{{sampler, textureView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {gameBones, 0, VK_WHOLE_SIZE}, {gameConfig2, 0, 64 << 10}};
    UpdateDescriptorSetWithTemplate(device, gameSet2, updateTemplate, &setData2);

    // The draws, three times: a mod's buffers and textures load on its first match, its
    // textures go up at the start of the next command buffer, and it draws with them after.
    // A vertex buffer for the game's draws: the 16-byte binding the pipeline declares.
    VkBuffer gameVertices;
    VkDeviceMemory gameVerticesMem;
    CHECK(makeBuffer(4800, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, host, &gameVertices, &gameVerticesMem));
    for (int round = 0; round < 7; ++round) {
    CHECK(BeginCommandBuffer(cb, &begin));
    VkClearValue clear{};
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = pass;
    rbi.framebuffer = framebuffer;
    rbi.renderArea = scissor;
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    CmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const uint32_t dynamicOffset = 0;
    CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &gameSet, 1, &dynamicOffset);
    const VkDeviceSize zero = 0;
    CmdBindVertexBuffers(cb, 0, 1, &gameVertices, &zero);
    CmdBindVertexBuffers(cb, 1, 1, &gameVertices, &zero);
    CmdBindIndexBuffer(cb, indices, 0, VK_INDEX_TYPE_UINT32);
    CmdDrawIndexed(cb, 150, 1, 0, 0, 0);
    CmdDrawIndexed(cb, 150, 1, 150, 0, 0);
    CmdDrawIndexed(cb, 30, 1, 60, 0, 0);
    CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, skinnedPipeline);
    CmdBindVertexBuffers(cb, 2, 1, &gameVertices, &zero);
    CmdDrawIndexed(cb, 60, 1, 0, 0, 0);
    CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &gameSet2, 1, &dynamicOffset);
    CmdDrawIndexed(cb, 60, 1, 0, 0, 0);
    CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    CmdDrawIndexed(cb, 30, 1, 60, 0, 0);  // A second component there: the object is the mod's.
    CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, skinnedPipeline);
    CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &gameSet, 1, &dynamicOffset);
    CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    CmdBindIndexBuffer(cb, mappedIndices, 0, VK_INDEX_TYPE_UINT32);
    CmdDrawIndexed(cb, 90, 1, 0, 0, 0);
    CmdEndRenderPass(cb);
    CHECK(EndCommandBuffer(cb));
    CHECK(QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    CHECK(QueueWaitIdle(queue));
    DeviceWaitIdle(device);
    Sleep(500);  // The first pass loads the mod's buffers on the layer's job thread.
    }

    // The game's texture now holds the mod's pixels (0x5a), copied in place.
    VkBuffer check;
    VkDeviceMemory checkMem;
    CHECK(makeBuffer(bc7.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, host, &check, &checkMem));
    CHECK(BeginCommandBuffer(cb, &begin));
    VkImageMemoryBarrier toSrc = toTransfer;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);
    CmdCopyImageToBuffer(cb, texture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, check, 1, &texCopy);
    CHECK(EndCommandBuffer(cb));
    CHECK(QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    CHECK(QueueWaitIdle(queue));
    CHECK(MapMemory(device, checkMem, 0, VK_WHOLE_SIZE, 0, &p));
    size_t modBytes = 0;
    for (size_t i = 0; i < bc7.size(); ++i) modBytes += static_cast<uint8_t*>(p)[i] == 0x5a;

    Sleep(500);  // The log is written on a background thread.
    const std::string log = ReadAll(scratch + L"\\efmivulkan.log");
    char text[128];
    Expect("layer loaded and logging", log.find("efmivulkan milestone 2: meshes, textures, merged skeleton") != std::string::npos);
    Expect("layer saw the device", log.find("Vulkan device created") != std::string::npos);
    Expect("mod index read", log.find("mods: 1 ini files") != std::string::npos);
    snprintf(text, sizeof(text), "ib MATCH %08x count=150 first=150", uploadedHash);
    Expect("uploaded index buffer: region hash matched", log.find(text) != std::string::npos);
    snprintf(text, sizeof(text), "ib MATCH %08x count=90 first=0", mappedHash);
    Expect("mapped index buffer: region hash matched", log.find(text) != std::string::npos);
    Expect("same index count, other bytes: a candidate, not a match", log.find("count=150 first=0: no mod has this hash") != std::string::npos);
    Expect("milestone 2: the mod parsed", log.find("efmi mods: 1 with 4 components") != std::string::npos);
    Expect("milestone 2: its buffers loaded", log.find("Efmi/efmi.ini: 4 buffers loaded") != std::string::npos);
    Expect("milestone 2: a component drawn by the mod, its per-instance binding left to the game",
           log.find("[textureoverride_component0]: drawn by the mod (1 draws)") != std::string::npos);
    Expect("milestone 2: another stride is refused (no attributes say what the game reads)",
           log.find("[textureoverride_component1]: vertex binding 0 has stride 16 in the game") != std::string::npos);
    Expect("milestone 2b: the merged skeleton read", log.find("Efmi/efmi.ini: merged skeleton of 4 bones, 3 components") != std::string::npos);
    Expect("milestone 2b: bone indices made 8-bit, with a palette", log.find("resource_blend: 3 vertices, 2 bones of the merged skeleton") != std::string::npos);
    Expect("milestone 2b: gpu_posed drawn, posed with the merged skeleton",
           log.find("[textureoverride_entrypoint_component2]: posed with the merged skeleton (2 bones)") != std::string::npos);
    snprintf(text, sizeof(text), "texture MATCH %08x: 64x64, 1 mips, Vulkan format 145, as DXGI format 98, usage default", textureHash);
    Expect("milestone 2b: the game's bones copied at submit (merged 3 = row 2000 + 3 + 3 x 3)",
           log.find("bone 0 (merged 3) starts 2012") != std::string::npos);
    // No SpatialIdentity_IdentifyComponentInstances in this mod: as EFMI, every draw is the
    // mod's and both objects use the skeleton of slot 0 (identification needs frames, which
    // this test, with no swapchain, does not have).
    Expect("milestone 2b: a mod without spatial identification draws every object", log.find("an object not (yet) identified") == std::string::npos);
    Expect("cross-IB: bone records made", log.find("Efmi/efmi.ini: 1 bone records") != std::string::npos);
    Expect("cross-IB: a piece drawn on another component's recorded bones",
           log.find("[textureoverride_component3]: 1 draws on bones recorded by another component (cross-IB), 0 left out") != std::string::npos);
    Expect("milestone 2c: a texture uploaded where the CPU cannot see it, read back, hashed as 3DMigoto would, matched",
           log.find(text) != std::string::npos);
    Expect("milestone 2c: the mod's texture loaded", log.find("Efmi/efmi.ini: 2 of 2 textures loaded") != std::string::npos);
    Expect("milestone 2c: the mod draws with its own texture",
           log.find("[textureoverride_component0]: draws with its own textures") != std::string::npos);
    Expect("no in-place copy (as DX11 EFMI): the game's texture keeps its own pixels", modBytes < bc7.size());
    snprintf(text, sizeof(text), "texture MATCH %08x: a streamed copy, 32x32, that went up first", fullHash);
    Expect("streamed copy: a smaller image of a matched texture matches through its lower mip", log.find(text) != std::string::npos);
    Expect("no errors", log.find("[ERROR]") == std::string::npos);

    DestroyDevice(device, nullptr);
    DestroyInstance(instance, nullptr);
    printf(g_failed ? "%d FAILED\n" : "smoke test passed\n", g_failed);
    return g_failed ? 1 : 0;
}
