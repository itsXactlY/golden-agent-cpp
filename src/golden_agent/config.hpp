// Re-ported from Golden-Agent/src/golden_agent/config.py (drifted snapshot,
// 2026-09-02): 4 backends (CUDA/METAL/VULKAN/CPU), 3 models (tiny/lite/pro),
// rich ModelSpec (repo, n_ctx, reasoning_effort, draft_repo, spec_type),
// inference.json with per-model overrides, llama-server asset URLs.
#pragma once
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "json.hpp"

namespace ga {

// ---- Spec-type constants (config.py) ------------------------------------
constexpr const char* SPEC_TYPE_NONE = "none";
constexpr const char* SPEC_TYPE_DRAFT_DFLASH = "draft-dflash";

// ---- Backend (config.py: class Backend(str, Enum)) ----------------------
enum class Backend { CUDA, METAL, VULKAN, CPU };

// "cuda" | "metal" | "vulkan" | "cpu"
const char* backend_value(Backend b);
// "NVIDIA CUDA" | "Apple Metal" | "Vulkan" | "CPU only"
const char* backend_label(Backend b);
// std::nullopt on unknown name (Python: Backend(s) raises ValueError).
std::optional<Backend> backend_from_str(const std::string& s);

// Mirrors detect_backend(): darwin -> METAL; nvidia-smi on PATH -> CUDA;
// vulkaninfo on PATH -> VULKAN; otherwise CPU.
Backend detect_backend();

// ---- Model catalogue (config.py: ModelSpec + MODELS) --------------------
struct ModelSpec {
    std::string key;
    std::string label;
    std::string repo;
    std::string filename;
    long long size_bytes = 0;
    std::optional<int> n_ctx;
    // empty map == None in Python
    std::map<std::string, double> sampling_overrides;
    std::string tool_format = "qwen3_xml";
    std::string reasoning = "auto";
    std::optional<std::string> reasoning_effort;
    std::optional<std::string> draft_repo;
    std::optional<std::string> draft_filename;
    std::optional<long long> draft_size_bytes;
    std::string spec_type = SPEC_TYPE_NONE;
};

// Python dict insertion order: tiny, lite, pro.
const std::vector<ModelSpec>& models();
// nullptr if the key is unknown.
const ModelSpec* model_spec(const std::string& key);

// ---- Constants -----------------------------------------------------------
constexpr const char* DEFAULT_MODEL_KEY = "lite";
constexpr const char* LLAMA_SERVER_VERSION = "b10621";
constexpr const char* LLAMA_SERVER_BASE_URL =
    "https://github.com/ggml-org/llama.cpp/releases/download";
constexpr int DEFAULT_PORT = 2011;
constexpr const char* DEFAULT_HOST = "127.0.0.1";

// ---- llama-server asset URLs ---------------------------------------------
// Mirrors _server_download_url(platform=None, backend=None).
// `platform` is the sys.platform string ("darwin" / "win32" / "linux");
// pass "" to use this host's platform. `arch` is platform.machine()
// (used only on darwin: "arm64" vs "x64").
std::string server_download_url(const std::string& platform = "",
                                Backend backend = Backend::CPU,
                                const std::string& arch = "");

// ---- Config directory / files --------------------------------------------
// CONFIG_DIR = ~/.golden-agent
std::string config_dir();
// CONFIG_FILE = CONFIG_DIR / "config.json"
std::string config_path();
// INFERENCE_FILE = CONFIG_DIR / "inference.json"
std::string inference_path();

// ---- config.json ---------------------------------------------------------
// load_config(): parsed JSON object, or {} when the file is missing/corrupt.
json::Value load_config();
// save_config(data): mkdir -p CONFIG_DIR, write json.dumps(data, indent=2).
void save_config(const json::Value& data);

// ---- inference.json ------------------------------------------------------
// default_inference(): per-model overrides:
//   {key: {"spec_draft_n_max": 7 if spec has draft else null,
//          "reasoning_effort": spec.reasoning_effort,
//          "extra_args": []}}
json::Value default_inference();
// load_inference(): returns the file's JSON; if absent, writes
// default_inference() and returns that. The file is never overwritten once
// it exists, so user edits survive across runs.
json::Value load_inference();

}  // namespace ga
