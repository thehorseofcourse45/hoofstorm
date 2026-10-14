#include "vkcontext.h"
#include <filesystem>
#include <fstream>
#include <cstring>

namespace
{
constexpr uint32_t magic = 0x46535043, version = 1;
constexpr size_t maxCacheBytes = 64u << 20;
struct Header
{
    uint32_t magic, version, vendor, device, driver, pointerBits, bytes, reserved;
    uint8_t uuid[VK_UUID_SIZE];
    uint64_t checksum;
};
static_assert(sizeof(Header) == 56, "cache envelope layout");
uint64_t checksum(const std::vector<uint8_t>& data)
{
    uint64_t hash = 14695981039346656037ull;
    for (uint8_t byte : data) { hash ^= byte; hash *= 1099511628211ull; }
    return hash;
}
std::filesystem::path cachePath(const std::string& utf8)
{
    const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,utf8.data(),int(utf8.size()),nullptr,0);
    if (!count) throw std::runtime_error("invalid UTF-8 cache path");
    std::wstring wide(count,L'\0');
    if (!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,utf8.data(),int(utf8.size()),wide.data(),count))
        throw std::runtime_error("invalid UTF-8 cache path");
    return std::filesystem::path(wide);
}
Header identity(VkPhysicalDevice phys)
{
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    Header h{};
    h.magic=magic; h.version=version; h.vendor=props.vendorID; h.device=props.deviceID;
    h.driver=props.driverVersion; h.pointerBits=sizeof(void*)*8;
    memcpy(h.uuid, props.pipelineCacheUUID, VK_UUID_SIZE);
    return h;
}
bool validData(const Header& h, const Header& expected, const std::vector<uint8_t>& data)
{
    if (h.magic!=magic || h.version!=version || h.vendor!=expected.vendor || h.device!=expected.device ||
        h.driver!=expected.driver || h.pointerBits!=expected.pointerBits || h.reserved ||
        memcmp(h.uuid,expected.uuid,VK_UUID_SIZE) || h.bytes!=data.size() || h.checksum!=checksum(data) ||
        data.size()<sizeof(VkPipelineCacheHeaderVersionOne)) return false;
    VkPipelineCacheHeaderVersionOne raw{};
    memcpy(&raw,data.data(),sizeof raw);
    return raw.headerSize==sizeof raw && raw.headerVersion==VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
        raw.vendorID==expected.vendor && raw.deviceID==expected.device && !memcmp(raw.pipelineCacheUUID,expected.uuid,VK_UUID_SIZE);
}
}

void VkContext::configurePipelineCache(const std::string& file, bool writable)
{
    if (pipelineCacheFile==file && pipelineCacheWritable==writable) return;
    pipelineCacheFile=file; pipelineCacheWritable=writable; pipelineCacheLoadedBytes=0;
    if (file.empty()) return;
    try
    {
        std::ifstream in(cachePath(file), std::ios::binary | std::ios::ate);
        if (!in) return;
        const auto size=in.tellg();
        if (size < std::streamoff(sizeof(Header)) || size > std::streamoff(maxCacheBytes+sizeof(Header))) return;
        in.seekg(0); Header h{}; in.read(reinterpret_cast<char*>(&h),sizeof h);
        std::vector<uint8_t> data(size_t(size)-sizeof h);
        in.read(reinterpret_cast<char*>(data.data()),std::streamsize(data.size()));
        if (!in || !validData(h,identity(phys),data)) return;
        VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        ci.initialDataSize=data.size(); ci.pInitialData=data.data();
        VkPipelineCache loaded=VK_NULL_HANDLE;
        if (vkCreatePipelineCache(device,&ci,nullptr,&loaded)!=VK_SUCCESS) return;
        const auto result=vkMergePipelineCaches(device,pipelineCache,1,&loaded);
        vkDestroyPipelineCache(device,loaded,nullptr);
        if (result==VK_SUCCESS) pipelineCacheLoadedBytes=data.size();
    }
    catch (const std::exception&) { /* A cache miss must not prevent startup. */ }
}

bool VkContext::savePipelineCache() noexcept
{
    if (!pipelineCache || pipelineCacheFile.empty() || !pipelineCacheWritable) return false;
    try
    {
        size_t bytes=0;
        if (vkGetPipelineCacheData(device,pipelineCache,&bytes,nullptr)!=VK_SUCCESS || !bytes || bytes>maxCacheBytes) return false;
        std::vector<uint8_t> data(bytes);
        if (vkGetPipelineCacheData(device,pipelineCache,&bytes,data.data())!=VK_SUCCESS) return false;
        data.resize(bytes);
        Header h=identity(phys); h.bytes=uint32_t(bytes); h.checksum=checksum(data);
        if (!validData(h,identity(phys),data)) return false;
        const auto destination=cachePath(pipelineCacheFile);
        auto temporary=destination;
        temporary+=L".tmp-"+std::to_wstring(GetCurrentProcessId());
        // Each process writes its own file; only a complete replacement can overwrite a good cache.
        bool complete=false;
        {
            std::ofstream out(temporary,std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(&h),sizeof h);
            out.write(reinterpret_cast<const char*>(data.data()),std::streamsize(data.size()));
            out.flush(); complete=bool(out); out.close(); complete=complete && !out.fail();
        }
        if (complete && MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        std::error_code ignored; std::filesystem::remove(temporary,ignored);
    }
    catch (const std::exception&) { /* Read-only or full disks must not prevent shutdown. */ }
    return false;
}
