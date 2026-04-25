#ifndef __TRACYCONFIG_HPP__
#define __TRACYCONFIG_HPP__

#include <string>

#include "TracyUtility.hpp"

namespace tracy
{

struct Config
{
    bool threadedRendering = true;
    bool focusLostLimit = true;
    int targetFps = 60;
    bool drawFrameTargets = false;
    double horizontalScrollMultiplier = 1.0;
    double verticalScrollMultiplier = 1.0;
    bool memoryLimit = false;
    int memoryLimitPercent = 80;
    bool achievements = false;
    bool achievementsAsked = false;
    int dynamicColors = 1;
    bool forceColors = false;
    bool ghostZones = true;
    int shortenName = (int)ShortenName::NoSpaceAndNormalize;
    bool drawSamples = true;
    bool drawContextSwitches = true;
    int plotHeight = 100;
    bool saveUserScale = false;
    float userScale = 1.0f;

    // LLM assistant settings
#ifdef __EMSCRIPTEN__
    bool llm = false;
#else
    bool llm = true;
#endif
    std::string llmAddress = "http://localhost:11434";
    std::string llmModel;
    std::string llmFastModel;
    std::string llmEmbeddingsModel;
    std::string llmUserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/136.0.0.0 Safari/537.36";
    std::string llmSearchIdentifier;
    std::string llmSearchApiKey;
    std::string llmSearchBraveApiKey;
    bool llmSeparateFastModel = true;
    bool llmAnnotateCallstacks = false;
    bool llmLimitToolReplySize = false;
    int llmMaxToolReplySizeValue = 48*1024;

    // Local Node MCP server for the trace integrator (port via TRACY_MCP_HTTP_PORT)
    bool llmIntegratorMcpEnabled = false;
    int llmIntegratorMcpPort = 37512;
    std::string llmIntegratorNode = "node";
    std::string llmIntegratorScript;
    std::string llmIntegratorTracyRoot;
    std::string llmIntegratorBridge;
};

extern Config s_config;

void LoadConfig();
bool SaveConfig();

}

#endif
