// Re-ported from Golden-Agent/src/golden_agent/config.py (drifted snapshot,
// 2026-09-02). Mirrors the module's public surface one-to-one.
#include "config.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace {

std::string host_platform() {
#if __APPLE__
    return "darwin";
#else
    return "linux";
#endif
}

// shutil.which(): true if `name` is an executable file somewhere on PATH.
bool which(const std::string& name) {
    const char* pathenv = std::getenv("PATH");
    if (!pathenv) return false;
    std::string path(pathenv);
    size_t start = 0;
    while (true) {
        size_t end = path.find(':', start);
        std::string dir =
            (end == std::string::npos) ? path.substr(start)
                                       : path.substr(start, end - start);
        if (!dir.empty()) {
            std::string cand = dir + "/" + name;
            if (access(cand.c_str(), X_OK) == 0) return true;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

// Path.home() equivalent: $HOME, falling back to /root.
std::string home_dir() {
    const char* h = std::getenv("HOME");
    if (h && *h) return h;
    return "/root";
}

// platform.machine() equivalent (only consulted on darwin in
// _server_download_url; host value used when the caller passes arch="").
std::string host_arch() {
#if defined(__APPLE__)
    struct utsname u;
    if (uname(&u) == 0) return u.machine;
    return "x64";
#else
    return "x64";
#endif
}

// mkdir -p
void mkdir_p(const std::string& path) {
    if (path.empty()) return;
    std::string acc;
    for (size_t i = 0; i < path.size(); i++) {
        acc.push_back(path[i]);
        if (path[i] == '/' || i == path.size() - 1) {
            if (acc == "/") continue;
            struct stat st;
            if (stat(acc.c_str(), &st) == 0) {
                if (!S_ISDIR(st.st_mode)) throw std::runtime_error("not a dir: " + acc);
                continue;
            }
            if (mkdir(acc.c_str(), 0755) != 0 && errno != EEXIST)
                throw std::runtime_error("mkdir failed: " + acc);
        }
    }
}

std::string read_all(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open: " + path);
    std::string out;
    char buf[8192];
    while (f.read(buf, sizeof buf)) out.append(buf, f.gcount());
    out.append(buf, f.gcount());
    return out;
}

bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

}  // namespace

namespace ga {

const char* backend_value(Backend b) {
    switch (b) {
        case Backend::CUDA: return "cuda";
        case Backend::METAL: return "metal";
        case Backend::VULKAN: return "vulkan";
        case Backend::CPU: return "cpu";
    }
    return "cpu";
}

const char* backend_label(Backend b) {
    switch (b) {
        case Backend::CUDA: return "NVIDIA CUDA";
        case Backend::METAL: return "Apple Metal";
        case Backend::VULKAN: return "Vulkan";
        case Backend::CPU: return "CPU only";
    }
    return "CPU only";
}

std::optional<Backend> backend_from_str(const std::string& s) {
    if (s == "cuda") return Backend::CUDA;
    if (s == "metal") return Backend::METAL;
    if (s == "vulkan") return Backend::VULKAN;
    if (s == "cpu") return Backend::CPU;
    return std::nullopt;
}

Backend detect_backend() {
#if __APPLE__
    return Backend::METAL;
#else
    if (which("nvidia-smi")) return Backend::CUDA;
    if (which("vulkaninfo")) return Backend::VULKAN;
    return Backend::CPU;
#endif
}

const std::vector<ModelSpec>& models() {
    static const std::vector<ModelSpec> kModels = [] {
        std::vector<ModelSpec> v;

        ModelSpec tiny;
        tiny.key = "tiny";
        tiny.label = "LFM2.5 2.6B";
        tiny.repo = "LiquidAI/LFM2.5-2.6B-GGUF";
        tiny.filename = "LFM2.5-2.6B-QAD-Q4_0.gguf";
        tiny.size_bytes = 1593894944;
        tiny.n_ctx = 128000;
        tiny.sampling_overrides = {{"temperature", 0.1}, {"top_k", 50.0},
                                   {"repeat_penalty", 1.1}};
        tiny.tool_format = "lfm2";
        tiny.reasoning = "on";
        v.push_back(tiny);

        ModelSpec lite;
        lite.key = "lite";
        lite.label = "Ornith 1.5 9B";
        lite.repo = "AtomicChat/Ornith-1.5-9B-GGUF";
        lite.filename = "Ornith-1.5-9B-AD-Q4_K-IQ4_XS.gguf";
        lite.size_bytes = 5611873024;
        lite.sampling_overrides = {{"temperature", 0.7}, {"top_k", 20.0},
                                   {"repeat_penalty", 1.05}};
        lite.reasoning = "on";
        lite.draft_repo = "audreyt/Ornith-1.5-9B-DFlash-GGUF";
        lite.draft_filename = "ornith1.5-9b-dflash-bf16-projection-Q4_K_M.gguf";
        lite.draft_size_bytes = 765959552;
        lite.spec_type = SPEC_TYPE_DRAFT_DFLASH;
        v.push_back(lite);

        ModelSpec pro;
        pro.key = "pro";
        pro.label = "Qwen3.8 27B";
        pro.repo = "sdkyuan/qwen3.8-27B-qat-q2_0-gguf";
        pro.filename = "qwen38-27b-qat-q2_0.gguf";
        pro.size_bytes = 8759266208;
        pro.sampling_overrides = {{"temperature", 0.7}, {"top_k", 20.0},
                                  {"repeat_penalty", 1.05}};
        pro.reasoning = "on";
        pro.reasoning_effort = "medium";
        pro.draft_repo = "incoai/Qwen3.8-27B-DFlash2-GGUF";
        pro.draft_filename = "Qwen3.8-27B-DFlash2-Q4_K_M.gguf";
        pro.draft_size_bytes = 1143006816;
        pro.spec_type = SPEC_TYPE_DRAFT_DFLASH;
        v.push_back(pro);

        return v;
    }();
    return kModels;
}

const ModelSpec* model_spec(const std::string& key) {
    for (const auto& s : models())
        if (s.key == key) return &s;
    return nullptr;
}

// _server_download_url(platform=None, backend=None)
std::string server_download_url(const std::string& platform,
                                Backend backend,
                                const std::string& arch) {
    std::string plat = platform.empty() ? host_platform() : platform;
    const std::string ver = LLAMA_SERVER_VERSION;
    const std::string base = LLAMA_SERVER_BASE_URL;
    if (plat == "darwin") {
        std::string a = arch.empty() ? host_arch() : arch;
        if (a != "arm64") a = "x64";
        return base + "/" + ver + "/llama-" + ver + "-bin-macos-" + a + ".tar.gz";
    }
    if (plat == "win32") {
        const char* name = "win-cpu-x64";
        if (backend == Backend::CUDA) name = "win-cuda-12.4-x64";
        else if (backend == Backend::VULKAN) name = "win-vulkan-x64";
        return base + "/" + ver + "/llama-" + ver + "-bin-" + name + ".zip";
    }
    // linux (and any other platform string)
    const char* name = "ubuntu-x64";
    if (backend == Backend::VULKAN) name = "ubuntu-vulkan-x64";
    return base + "/" + ver + "/llama-" + ver + "-bin-" + name + ".tar.gz";
}

std::string config_dir() { return home_dir() + "/.golden-agent"; }

std::string config_path() { return config_dir() + "/config.json"; }

std::string inference_path() { return config_dir() + "/inference.json"; }

json::Value load_config() {
    json::Value empty = json::Value::make_object();
    std::string p = config_path();
    if (!file_exists(p)) return empty;
    try {
        return json::parse(read_all(p));
    } catch (...) {
        return empty;
    }
}

void save_config(const json::Value& data) {
    mkdir_p(config_dir());
    std::ofstream f(config_path(), std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("cannot open for write: " + config_path());
    f << json::dumps(data, 2);
}

json::Value default_inference() {
    json::Value out = json::Value::make_object();
    for (const auto& spec : models()) {
        json::Value per = json::Value::make_object();
        per.obj.push_back({"spec_draft_n_max",
                          spec.draft_filename ? json::Value::make_int(7)
                                             : json::Value::make_null()});
        per.obj.push_back(
            {"reasoning_effort",
             spec.reasoning_effort
                 ? json::Value::make_string(*spec.reasoning_effort)
                 : json::Value::make_null()});
        json::Value arr = json::Value::make_array();
        per.obj.push_back({"extra_args", arr});
        out.obj.push_back({spec.key, per});
    }
    return out;
}

json::Value load_inference() {
    std::string p = inference_path();
    if (file_exists(p)) {
        return json::parse(read_all(p));  // lets parse errors propagate, as in Python
    }
    json::Value fresh = default_inference();
    mkdir_p(config_dir());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("cannot open for write: " + p);
    f << json::dumps(fresh, 2);
    return fresh;
}

}  // namespace ga
